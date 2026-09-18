#include "chunked_image_writer.h"

#include "sha256.h"
#include "win32_paths.h"

#include <winioctl.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace recovery {

namespace {

std::string PathToUtf8(const fs::path& path) {
    const std::wstring wide = FullPathWin32(path).wstring();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string output(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        output.data(), length, nullptr, nullptr);
    std::replace(output.begin(), output.end(), '\\', '/');
    return output;
}

fs::path Utf8ToPath(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), length) != length) return {};
    return fs::path(wide);
}

bool WriteHandleAt(HANDLE handle, uint64_t offset, const void* data, size_t size,
                   DWORD& last_error) {
    const auto* input = static_cast<const uint8_t*>(data);
    size_t completed = 0;
    while (completed < size) {
        const DWORD amount = static_cast<DWORD>(std::min<size_t>(
            size - completed, std::numeric_limits<DWORD>::max()));
        const uint64_t position = offset + completed;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD written = 0;
        if (!WriteFile(handle, input + completed, amount, &written, &overlapped) || written != amount) {
            last_error = GetLastError();
            return false;
        }
        completed += written;
    }
    return true;
}

bool AllZero(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (data[i] != 0) return false;
    return true;
}

bool FreeSpaceAt(const fs::path& directory, uint64_t* available, DWORD* error) {
    const fs::path absolute_root = FullPathWin32(directory);
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(absolute_root.c_str(), &free_bytes, nullptr, nullptr)) {
        *error = GetLastError();
        return false;
    }
    *available = free_bytes.QuadPart;
    return true;
}

std::string PartSuffix(uint64_t number) {
    char suffix[48];
    snprintf(suffix, sizeof(suffix), ".part-%06llu.bin", static_cast<unsigned long long>(number));
    return suffix;
}

}  // namespace

ChunkedImageWriter::~ChunkedImageWriter() { Close(); }

void ChunkedImageWriter::Close() {
    for (auto& part : parts_)
        if (part.handle != INVALID_HANDLE_VALUE) CloseHandle(part.handle);
    parts_.clear();
    destinations_.clear();
    destination_used_.clear();
    lazy_parts_ = false;
}

ChunkedImageWriter::Part* ChunkedImageWriter::FindPart(uint64_t number) {
    const uint64_t offset = number * chunk_size_;
    auto it = std::lower_bound(parts_.begin(), parts_.end(), offset,
        [](const Part& part, uint64_t value) { return part.offset < value; });
    if (it == parts_.end() || it->offset != offset) return nullptr;
    return &*it;
}

bool ChunkedImageWriter::PartPathIsProtected(const fs::path& part_path) const {
    if (protected_source_.empty()) return false;
    std::error_code same_error;
    const fs::path normalized_source = FullPathWin32(protected_source_);
    const fs::path normalized_part = FullPathWin32(part_path);
    std::wstring source_text = normalized_source.native();
    std::wstring part_text = normalized_part.native();
    std::transform(source_text.begin(), source_text.end(), source_text.begin(), towlower);
    std::transform(part_text.begin(), part_text.end(), part_text.begin(), towlower);
    const bool same_text = source_text == part_text;
    const bool same_file = fs::exists(normalized_part, same_error) && !same_error &&
                           fs::equivalent(normalized_source, normalized_part, same_error) &&
                           !same_error;
    return same_text || same_file;
}

bool ChunkedImageWriter::CreatePartFile(const fs::path& part_path, uint64_t part_size,
                                        HANDLE* out_handle) {
    HANDLE handle = CreateFileW(part_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        last_error_ = GetLastError();
        return false;
    }
#ifndef RECOVERY_REGULAR_FILES_ONLY
    DWORD returned = 0;
    DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
#endif
    LARGE_INTEGER length{};
    length.QuadPart = static_cast<LONGLONG>(part_size);
    if (!SetFilePointerEx(handle, length, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
        last_error_ = GetLastError();
        CloseHandle(handle);
        DeleteFileW(part_path.c_str());
        return false;
    }
    *out_handle = handle;
    return true;
}

bool ChunkedImageWriter::Create(const fs::path& index_path, uint64_t total_size,
                                uint64_t chunk_size,
                                const std::vector<ChunkDestinationSpec>& destinations,
                                const fs::path& protected_source, bool lazy_parts) {
    Close();
    if (total_size == 0 || chunk_size == 0 || destinations.empty()) {
        last_error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    index_path_ = FullPathWin32(index_path);
    protected_source_ = protected_source;
    total_size_ = total_size;
    chunk_size_ = chunk_size;
    lazy_parts_ = lazy_parts;
    destinations_ = destinations;
    destination_used_.assign(destinations.size(), 0);
    for (auto& destination : destinations_)
        destination.root = FullPathWin32(destination.root);

    std::vector<uint64_t> effective_capacity(destinations.size(), 0);
    uint64_t aggregate_capacity = 0;
    for (size_t i = 0; i < destinations_.size(); ++i) {
        std::error_code ec;
        CreateDirectoriesWin32(destinations_[i].root, ec);
        if (ec) {
            last_error_ = ERROR_PATH_NOT_FOUND;
            Close();
            return false;
        }
        uint64_t available = 0;
        if (!FreeSpaceAt(destinations_[i].root, &available, &last_error_)) {
            Close();
            return false;
        }
        effective_capacity[i] = destinations_[i].capacity == 0
            ? available
            : std::min<uint64_t>(destinations_[i].capacity, available);
        aggregate_capacity = aggregate_capacity > UINT64_MAX - effective_capacity[i]
            ? UINT64_MAX : aggregate_capacity + effective_capacity[i];
    }

    if (lazy_parts_) {
        // Parts appear on demand; the caller decides whether the aggregate
        // capacity is acceptable for the expected amount of non-zero data.
        if (aggregate_capacity < chunk_size_) {
            last_error_ = ERROR_DISK_FULL;
            Close();
            return false;
        }
        if (!WriteIndex()) {
            Close();
            return false;
        }
        last_error_ = ERROR_SUCCESS;
        return true;
    }

    if (aggregate_capacity < total_size_) {
        last_error_ = ERROR_DISK_FULL;
        Close();
        return false;
    }
    size_t destination_index = 0;
    for (uint64_t offset = 0, number = 0; offset < total_size; ++number) {
        const uint64_t part_size = std::min(chunk_size, total_size - offset);
        while (destination_index < destinations_.size()) {
            const uint64_t cap = effective_capacity[destination_index];
            if (destination_used_[destination_index] <= cap &&
                part_size <= cap - destination_used_[destination_index]) break;
            ++destination_index;
        }
        if (destination_index == destinations_.size()) {
            last_error_ = ERROR_DISK_FULL;
            Close();
            return false;
        }
        const fs::path part_path = destinations_[destination_index].root /
            (index_path_.stem().string() + PartSuffix(number));
        if (PartPathIsProtected(part_path)) {
            last_error_ = ERROR_ALREADY_EXISTS;
            Close();
            return false;
        }
        HANDLE handle = INVALID_HANDLE_VALUE;
        if (!CreatePartFile(part_path, part_size, &handle)) {
            Close();
            return false;
        }
        parts_.push_back({offset, part_size, FullPathWin32(part_path), handle, {}});
        destination_used_[destination_index] += part_size;
        offset += part_size;
    }
    if (!WriteIndex()) {
        Close();
        return false;
    }
    last_error_ = ERROR_SUCCESS;
    return true;
}

ChunkedImageWriter::Part* ChunkedImageWriter::CreatePart(uint64_t number) {
    const uint64_t offset = number * chunk_size_;
    if (offset >= total_size_) {
        last_error_ = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    const uint64_t part_size = std::min(chunk_size_, total_size_ - offset);
    for (size_t i = 0; i < destinations_.size(); ++i) {
        const uint64_t cap = destinations_[i].capacity;
        if (cap != 0 && (destination_used_[i] > cap || part_size > cap - destination_used_[i]))
            continue;
        std::error_code ec;
        CreateDirectoriesWin32(destinations_[i].root, ec);
        if (ec) continue;
        // Filesystems without sparse support allocate the whole part now, so
        // the free space must hold it in full.
        uint64_t available = 0;
        DWORD error = ERROR_SUCCESS;
        if (!FreeSpaceAt(destinations_[i].root, &available, &error) || available < part_size)
            continue;
        const fs::path part_path = destinations_[i].root /
            (index_path_.stem().string() + PartSuffix(number));
        if (PartPathIsProtected(part_path)) {
            last_error_ = ERROR_ALREADY_EXISTS;
            return nullptr;
        }
        HANDLE handle = INVALID_HANDLE_VALUE;
        if (!CreatePartFile(part_path, part_size, &handle)) {
            if (last_error_ == ERROR_FILE_EXISTS) return nullptr;  // never clobber a stray part
            continue;  // treat any other creation failure as "this destination is unusable"
        }
        Part part{offset, part_size, FullPathWin32(part_path), handle, {}};
        auto position = std::lower_bound(parts_.begin(), parts_.end(), offset,
            [](const Part& existing, uint64_t value) { return existing.offset < value; });
        Part* inserted = &*parts_.insert(position, std::move(part));
        destination_used_[i] += part_size;
        // The index must name the part before any data is trusted to it, so a
        // crash between the two never leaves an orphaned, unlisted file.
        if (!WriteIndex()) {
            CloseHandle(inserted->handle);
            DeleteFileW(inserted->path.c_str());
            parts_.erase(parts_.begin() + (inserted - parts_.data()));
            destination_used_[i] -= part_size;
            return nullptr;
        }
        return inserted;
    }
    last_error_ = ERROR_DISK_FULL;
    return nullptr;
}

bool ChunkedImageWriter::OpenExisting(const fs::path& index_path,
                                      uint64_t expected_total_size,
                                      const std::vector<ChunkDestinationSpec>& destinations) {
    Close();
    index_path_ = FullPathWin32(index_path);
    std::ifstream index(index_path_);
    std::string header, tag;
    if (!std::getline(index, header) || header != "#recovery-chunks 1" ||
        !(index >> tag >> total_size_) || tag != "total" || total_size_ == 0 ||
        total_size_ != expected_total_size) {
        last_error_ = ERROR_BAD_FORMAT;
        return false;
    }

    chunk_size_ = 0;
    uint64_t previous_end = 0;
    while (index >> tag) {
        if (tag == "lazy") {
            int flag = 0;
            if (!(index >> flag) || flag != 1 || !parts_.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            lazy_parts_ = true;
            continue;
        }
        if (tag == "chunk") {
            if (!(index >> chunk_size_) || chunk_size_ == 0 || !parts_.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            continue;
        }
        if (tag == "dest") {
            std::string encoded_root;
            uint64_t capacity = 0;
            if (!(index >> std::quoted(encoded_root) >> capacity) || !parts_.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            const fs::path root = Utf8ToPath(encoded_root);
            if (root.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            destinations_.push_back({FullPathWin32(root), capacity});
            continue;
        }
        uint64_t offset = 0, size = 0;
        std::string encoded_path;
        if (tag != "part" || !(index >> offset >> size >> std::quoted(encoded_path)) ||
            size == 0 || offset < previous_end || offset > total_size_ ||
            size > total_size_ - offset) {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        if (!lazy_parts_ && offset != previous_end) {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        std::string remainder, parsed_hash;
        std::getline(index, remainder);
        if (!remainder.empty()) {
            std::istringstream attributes(remainder);
            std::string attribute, extra;
            if (attributes >> attribute &&
                (attribute != "sha256" || !(attributes >> parsed_hash) || parsed_hash.size() != 64 ||
                 (attributes >> extra))) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
        }
        fs::path part_path = Utf8ToPath(encoded_path);
        if (part_path.empty()) {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        if (!IsAbsoluteWin32(part_path)) part_path = index_path_.parent_path() / part_path;
        part_path = FullPathWin32(part_path);
        HANDLE handle = CreateFileW(part_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            last_error_ = GetLastError();
            Close();
            return false;
        }
        LARGE_INTEGER actual{};
        if (!GetFileSizeEx(handle, &actual) || actual.QuadPart < 0 ||
            static_cast<uint64_t>(actual.QuadPart) != size) {
            last_error_ = ERROR_BAD_LENGTH;
            CloseHandle(handle);
            Close();
            return false;
        }
        if (chunk_size_ == 0) chunk_size_ = size;
        const uint64_t expected_part_size = std::min(chunk_size_, total_size_ - offset);
        if (size != expected_part_size || offset % chunk_size_ != 0) {
            last_error_ = ERROR_BAD_FORMAT;
            CloseHandle(handle);
            Close();
            return false;
        }
        parts_.push_back({offset, size, part_path, handle, parsed_hash});
        previous_end = offset + size;
    }
    if (lazy_parts_) {
        if (chunk_size_ == 0 || (destinations_.empty() && destinations.empty())) {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        if (!destinations.empty()) {
            destinations_ = destinations;
            for (auto& destination : destinations_)
                destination.root = FullPathWin32(destination.root);
        }
        destination_used_.assign(destinations_.size(), 0);
        for (const auto& part : parts_) {
            for (size_t i = 0; i < destinations_.size(); ++i) {
                if (part.path.parent_path() == destinations_[i].root) {
                    destination_used_[i] += part.size;
                    break;
                }
            }
        }
    } else if (parts_.empty() || previous_end != total_size_) {
        last_error_ = ERROR_BAD_FORMAT;
        Close();
        return false;
    }
    index.close();

    // A finalized index is a durable integrity checkpoint. Never resume on top
    // of a part whose recorded content hash no longer matches.
    for (const auto& part : parts_) {
        if (!part.sha256.empty()) {
            const auto actual_hash = Sha256File(part.path);
            if (!actual_hash || *actual_hash != part.sha256) {
                last_error_ = ERROR_CRC;
                Close();
                return false;
            }
        }
    }
    if (lazy_parts_ && !destinations.empty() && !WriteIndex()) {
        Close();
        return false;
    }
    last_error_ = ERROR_SUCCESS;
    return true;
}

bool ChunkedImageWriter::WriteAt(uint64_t offset, const void* data, size_t size) {
    if (offset > total_size_ || size > total_size_ - offset) {
        last_error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    const uint64_t first_number = offset / chunk_size_;
    const uint64_t last_number = size == 0 ? first_number : (offset + size - 1) / chunk_size_;

    // Writing into a finalized part invalidates its recorded hash; the index
    // must say so before the bytes change.
    std::vector<std::pair<Part*, std::string>> invalidated;
    for (uint64_t number = first_number; number <= last_number; ++number) {
        Part* part = FindPart(number);
        if (part && !part->sha256.empty()) {
            invalidated.emplace_back(part, part->sha256);
            part->sha256.clear();
        }
    }
    if (!invalidated.empty() && !WriteIndex()) {
        for (auto& entry : invalidated) entry.first->sha256 = entry.second;
        return false;
    }

    const auto* input = static_cast<const uint8_t*>(data);
    size_t completed = 0;
    while (completed < size) {
        const uint64_t number = (offset + completed) / chunk_size_;
        const uint64_t part_offset = number * chunk_size_;
        const uint64_t part_size = std::min(chunk_size_, total_size_ - part_offset);
        const uint64_t in_part = offset + completed - part_offset;
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            size - completed, part_size - in_part));
        Part* part = FindPart(number);
        if (!part) {
            if (!lazy_parts_) {
                last_error_ = ERROR_INVALID_PARAMETER;
                return false;
            }
            if (AllZero(input + completed, amount)) {
                // No part file means "reads as zeros", which is exactly this data.
                completed += amount;
                continue;
            }
            part = CreatePart(number);
            if (!part) return false;
        }
        if (part->handle == INVALID_HANDLE_VALUE) {
            last_error_ = ERROR_INVALID_HANDLE;
            return false;
        }
        if (!WriteHandleAt(part->handle, in_part, input + completed, amount, last_error_)) return false;
        completed += amount;
    }
    return true;
}

bool ChunkedImageWriter::Flush() {
    for (const auto& part : parts_) {
        if (!FlushFileBuffers(part.handle)) {
            last_error_ = GetLastError();
            return false;
        }
    }
    return true;
}

bool ChunkedImageWriter::WriteIndex() {
    const fs::path temp(index_path_.string() + ".tmp");
    {
        std::ofstream index(temp, std::ios::trunc);
        if (!index.is_open()) return false;
        index << "#recovery-chunks 1\n";
        index << "total " << total_size_ << '\n';
        if (lazy_parts_) {
            index << "lazy 1\n";
            index << "chunk " << chunk_size_ << '\n';
            for (const auto& destination : destinations_)
                index << "dest " << std::quoted(PathToUtf8(destination.root)) << ' '
                      << destination.capacity << '\n';
        }
        for (const auto& part : parts_) {
            index << "part " << part.offset << ' ' << part.size << ' '
                  << std::quoted(PathToUtf8(part.path));
            if (!part.sha256.empty()) index << " sha256 " << part.sha256;
            index << '\n';
        }
        index.flush();
        if (!index.good()) return false;
    }
    HANDLE temp_handle = CreateFileW(temp.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temp_handle == INVALID_HANDLE_VALUE) {
        last_error_ = GetLastError();
        return false;
    }
    const bool flushed = FlushFileBuffers(temp_handle) != FALSE;
    const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
    const bool closed = CloseHandle(temp_handle) != FALSE;
    const DWORD close_error = closed ? ERROR_SUCCESS : GetLastError();
    if (!flushed || !closed) {
        last_error_ = flushed ? close_error : flush_error;
        return false;
    }
    if (!MoveFileExW(temp.c_str(), index_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        last_error_ = GetLastError();
        return false;
    }
    return true;
}

bool ChunkedImageWriter::Finalize() {
    if (!Flush()) return false;
    for (auto& part : parts_) {
        if (part.handle != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(part.handle)) {
                last_error_ = GetLastError();
                return false;
            }
            part.handle = INVALID_HANDLE_VALUE;
        }
    }
    for (auto& part : parts_) {
        const auto hash = Sha256File(part.path);
        if (!hash) {
            last_error_ = ERROR_CRC;
            return false;
        }
        part.sha256 = *hash;
    }
    if (!WriteIndex()) {
        last_error_ = ERROR_WRITE_FAULT;
        return false;
    }
    return true;
}

}  // namespace recovery
