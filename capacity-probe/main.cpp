// capacity_probe: find the largest offset a raw device will actually read from.
//
// USB "fake capacity" drives report a large size to Windows but only have a
// much smaller amount of real flash wired up; firmware on these drives
// either wraps addresses back into the real capacity (silent data
// corruption - reads "succeed" but return wrong/duplicate data) or, as with
// this drive, starts failing reads once you go past the real chip capacity.
//
// This probe binary-searches for the boundary between "reads succeed" and
// "reads fail" so we know how much of the reported size is actually usable
// before spending hours imaging a drive that can't deliver that much data.
//
// Usage:
//   capacity_probe.exe \\.\PhysicalDrive1 [--reported-size N]

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr uint32_t kProbeSize = 4096;  // one read, sector/page aligned

HANDLE OpenSource(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
}

// Returns true if a kProbeSize-byte read at `offset` succeeds and returns
// a full, non-all-zero-by-coincidence block. We only care about success vs.
// failure here, not content.
bool CanReadAt(HANDLE h, uint64_t offset, uint8_t* buf, DWORD* out_error) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, kProbeSize, &got, &ov);
    *out_error = ERROR_SUCCESS;
    if (!ok) {
        *out_error = GetLastError();
        return false;
    }
    return got == kProbeSize;
}

bool ParseU64(const std::wstring& text, uint64_t& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    value = wcstoull(text.c_str(), &end, 10);
    return end != text.c_str();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: capacity_probe.exe <source> [--reported-size N]\n");
        return 1;
    }
    std::wstring source_path = argv[1];
    uint64_t reported_size = 0;
    bool have_check_offset = false;
    uint64_t check_offset = 0;
    uint64_t check_length = 65536;
    bool sample_mode = false;
    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--reported-size" && i + 1 < argc) {
            if (!ParseU64(argv[++i], reported_size)) {
                fwprintf(stderr, L"invalid --reported-size\n");
                return 1;
            }
        } else if (arg == L"--check-offset" && i + 1 < argc) {
            if (!ParseU64(argv[++i], check_offset)) {
                fwprintf(stderr, L"invalid --check-offset\n");
                return 1;
            }
            have_check_offset = true;
        } else if (arg == L"--sample") {
            sample_mode = true;
        } else if (arg == L"--check-length" && i + 1 < argc) {
            if (!ParseU64(argv[++i], check_length)) {
                fwprintf(stderr, L"invalid --check-length\n");
                return 1;
            }
        }
    }

    HANDLE h = OpenSource(source_path);
    if (h == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"cannot open source %ls (Windows error %lu)\n",
                 source_path.c_str(), GetLastError());
        return 1;
    }

    if (sample_mode) {
        // Read 1 MiB at a set of offsets and classify each as data / all-zero /
        // failed, then binary-search the boundary where data turns into zeros.
        // Fake-capacity drives that "succeed" but return zeros past the real
        // flash are invisible to the success/failure probe below.
        const uint32_t kSample = 1024 * 1024;
        uint8_t* sbuf = static_cast<uint8_t*>(_aligned_malloc(kSample, 4096));
        auto classify = [&](uint64_t off, DWORD* perr) -> int {  // 0 fail, 1 zero, 2 data
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            *perr = ERROR_SUCCESS;
            if (!ReadFile(h, sbuf, kSample, &got, &ov) || got == 0) { *perr = GetLastError(); return 0; }
            for (DWORD i = 0; i < got; ++i) if (sbuf[i] != 0) return 2;
            return 1;
        };
        const wchar_t* names[] = {L"READ FAILED", L"ALL ZERO (no data)", L"HAS DATA"};
        const uint64_t GB = 1024ull * 1024 * 1024;
        double pts[] = {0.5, 1, 5, 10, 20, 30, 40, 44, 45, 46, 47, 48, 49, 50, 55, 60, 100, 500, 1024, 1536, 2048, 4096, 7000};
        uint64_t last_data = 0, first_zero = 0; bool have_zero = false;
        for (double g : pts) {
            uint64_t off = static_cast<uint64_t>(g * GB);
            off -= off % 4096;
            DWORD e = 0; int c = classify(off, &e);
            if (c == 0) wprintf(L"%8.1f GB : %ls (Windows error %lu)\n", g, names[c], e);
            else wprintf(L"%8.1f GB : %ls\n", g, names[c]);
            if (c == 2) last_data = off;
            if (c == 1 && !have_zero) { first_zero = off; have_zero = true; }
        }
        if (have_zero && last_data < first_zero) {
            uint64_t lo = last_data, hi = first_zero;
            while (hi - lo > kSample) {
                uint64_t mid = lo + (hi - lo) / 2; mid -= mid % 4096;
                DWORD e = 0; int c = classify(mid, &e);
                if (c == 2) lo = mid; else hi = mid;
            }
            wprintf(L"\nlast offset with real data: ~%.3f GB (%llu bytes)\n",
                    lo / (double)GB, static_cast<unsigned long long>(lo));
            wprintf(L"everything after that reads as zeros -> real capacity is ~%.1f GB\n", hi / (double)GB);
        }
        _aligned_free(sbuf);
        CloseHandle(h);
        return 0;
    }

    if (have_check_offset) {
        // Sweep sector-by-sector across [check_offset, check_offset+check_length)
        // and report exactly which sectors succeed or fail, with retries, so a
        // narrow bad region can be pinpointed rather than only known to exist.
        wprintf(L"checking offsets [%llu, %llu) in %u-byte steps:\n",
                static_cast<unsigned long long>(check_offset),
                static_cast<unsigned long long>(check_offset + check_length), kProbeSize);
        alignas(4096) uint8_t sweep_buf[kProbeSize];
        uint64_t good_count = 0, bad_count = 0;
        for (uint64_t off = check_offset - (check_offset % kProbeSize);
             off < check_offset + check_length; off += kProbeSize) {
            DWORD err = ERROR_SUCCESS;
            bool ok = false;
            for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
                ok = CanReadAt(h, off, sweep_buf, &err);
            }
            if (ok) {
                ++good_count;
            } else {
                ++bad_count;
                wprintf(L"  offset %llu: FAILED after 3 attempts (Windows error %lu)\n",
                        static_cast<unsigned long long>(off), err);
            }
        }
        wprintf(L"\n%llu sectors OK, %llu sectors failed\n",
                static_cast<unsigned long long>(good_count),
                static_cast<unsigned long long>(bad_count));
        CloseHandle(h);
        return bad_count > 0 ? 1 : 0;
    }

    if (reported_size == 0) {
        GET_LENGTH_INFORMATION len_info{};
        DWORD returned = 0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len_info,
                            sizeof(len_info), &returned, nullptr)) {
            reported_size = static_cast<uint64_t>(len_info.Length.QuadPart);
        } else {
            fwprintf(stderr, L"could not query device length; pass --reported-size\n");
            CloseHandle(h);
            return 1;
        }
    }

    alignas(4096) uint8_t buf[kProbeSize];

    // Sanity check: can we read at all, at offset 0?
    DWORD err = ERROR_SUCCESS;
    if (!CanReadAt(h, 0, buf, &err)) {
        wprintf(L"offset 0 read FAILED (Windows error %lu) - drive is not readable at all "
                L"right now (connection/power issue, or drive is dead)\n", err);
        CloseHandle(h);
        return 1;
    }
    wprintf(L"offset 0: OK\n");

    // Binary search the last good offset in [0, reported_size).
    uint64_t good = 0;
    uint64_t bad = reported_size;
    int iterations = 0;
    while (bad - good > kProbeSize) {
        uint64_t mid = good + (bad - good) / 2;
        mid -= mid % kProbeSize;
        if (mid <= good) mid = good + kProbeSize;
        if (CanReadAt(h, mid, buf, &err)) {
            good = mid;
        } else {
            bad = mid;
        }
        if (++iterations % 8 == 0) {
            wprintf(L"  probing... good=%llu bad=%llu (range %.2f GB)\n",
                    static_cast<unsigned long long>(good),
                    static_cast<unsigned long long>(bad),
                    (bad - good) / (1024.0 * 1024.0 * 1024.0));
        }
    }

    wprintf(L"\nreported size:      %llu bytes (%.2f GB)\n",
            static_cast<unsigned long long>(reported_size),
            reported_size / (1024.0 * 1024.0 * 1024.0));
    wprintf(L"last readable byte:  %llu (%.2f GB)\n",
            static_cast<unsigned long long>(good),
            good / (1024.0 * 1024.0 * 1024.0));
    wprintf(L"first failing offset:%llu (%.2f GB)\n",
            static_cast<unsigned long long>(bad),
            bad / (1024.0 * 1024.0 * 1024.0));
    if (bad < reported_size) {
        wprintf(L"\n==> drive reports %.2f GB but only ~%.2f GB is actually readable.\n"
                L"==> this is consistent with a FAKE-CAPACITY device (real flash is "
                L"smaller than advertised).\n"
                L"==> re-run recovery tools with --source-size ~%llu instead of the "
                L"reported size.\n",
                reported_size / (1024.0 * 1024.0 * 1024.0),
                good / (1024.0 * 1024.0 * 1024.0),
                static_cast<unsigned long long>(good));
    } else {
        wprintf(L"\n==> entire reported capacity is readable; the earlier failures were "
                L"transient (connection/power), not a capacity issue.\n");
    }

    CloseHandle(h);
    return 0;
}
