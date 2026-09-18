# disk-recovery-tool

Custom recovery tool for a damaged 8TB disk (exFAT, scrambled sectors).
Three stages, always run in this exact order.

Landing page with download links: https://mbm-group-ab.github.io/disk-recovery-tool/
(see [index.html](index.html), deployed by
`.github/workflows/pages.yml` via GitHub Pages on every push to `main`).

## Build

Dependencies installed via scoop: `cmake`, `mingw`.

```bash
export PATH="/c/Users/mbehd/scoop/apps/mingw/current/bin:$PATH"
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

Outputs: `build/imager/imager.exe`, `build/exfat-parser/exfat_recovery.exe`,
`build/carver/recovery_carver.exe`, `build/map-tool/rescue_map_tool.exe`

MinGW builds are statically linked to their GCC runtimes, so the recovery
workstation does not need matching MinGW DLLs beside the executables.

## Graphical front-end (`recovery_gui.py`)

A cross-platform GUI (Tkinter) that wraps the four executables above,
one tab per stage, and always shows the exact command line it runs (same
philosophy as `recover.ps1`, but graphical).

**Download a build:** grab `DiskRecoveryGUI-windows.exe` / `-macos` / `-linux`
from the [latest release](../../releases/latest) — no Python install needed.

**Or run from source** (any OS with Python 3; Tkinter ships with standard
Python on Windows/macOS, on Linux: `sudo apt install python3-tk`):

```bash
python recovery_gui.py
```

Raw physical-drive access (`\\.\PhysicalDriveN`) only works on Windows,
because the recovery executables use the Win32 API directly. On macOS/Linux
the GUI still builds and runs commands against image files (`.img`/`.rci`)
you already produced elsewhere, and is useful for scanning/recovering from
a copied image.

Windows/macOS/Linux builds are produced automatically by
`.github/workflows/release.yml` (PyInstaller) on every push to `main` — it
bumps a patch version tag and attaches all three executables to that
GitHub Release in one workflow run, so anyone can `git clone` or just
download the executable for their OS.

## Guided menu (`recover.ps1`)

For day-to-day use, the executables are driven through an interactive
PowerShell menu instead of typing every option by hand:

```powershell
# in an Administrator PowerShell, from the repository root
Set-ExecutionPolicy -Scope Process Bypass
.\recover.ps1                 # uses build\ for the exes; -Bin and -WorkDir override
```

The menu lists physical drives (and refuses the system disk as a source), walks
through imaging, scan, named recovery, carving, and status, and prints the exact
command line before each run so it can be repeated by hand. Every run and its
exit code is appended to `recover.log` in the work directory. Nothing in the
menu writes to the source disk. The stages below document the commands the menu
composes, plus options it does not expose.

While the imager runs it rewrites one status line every two seconds:

```
[fast]  37.2%  2.98/8.00 GB  14.8 MB/s  ETA 03:56:12  bad 3 sect (0.01 MB)  errs 5  at 3.01 GB
```

`fast`/`scrape`/`retry` is the current pass, `bad` is confirmed unreadable
sectors, `errs` counts transient read failures, and `at` is the current source
offset. Rate and ETA are computed from this run only, so they stay honest after
`--resume`. The parser prints a similar line while copying file extents.

## Stage 1 — Imaging (always do this first, never work directly on the disk)

Must be run as Administrator (raw physical drive access).

```
imager.exe \\.\PhysicalDriveN  D:\rescue\disk.img
```

When no single destination can hold the image, use a checksummed `.rci` index
and one capacity limit per destination disk. Sizes accept `K`, `M`, `G`, and `T`
(binary multiples):

```
imager.exe \\.\PhysicalDriveN D:\rescue\disk.rci --chunk-size 100G ^
  --chunk-dest D:\recovery-chunks 2T ^
  --chunk-dest E:\recovery-chunks 2T ^
  --chunk-dest F:\recovery-chunks 4T
```

Each part is placed wholly on one destination. Capacity and currently available
space are checked before part creation; the run fails before creating parts if
the declared aggregate capacity is too small.

When the destinations cannot hold the whole source but the source is mostly
empty (a large disk with a few TB of data), add `--lazy-parts`:

```
imager.exe \\.\PhysicalDriveN C:\rescue\disk.rci --chunk-size 4G --lazy-parts ^
  --chunk-dest E:\recovery-chunks 1000G ^
  --chunk-dest \\server\share\recovery-chunks 480G ^
  --chunk-dest C:\recovery-chunks 350G
```

A part file is then created only the first time non-zero data is read for its
region, so all-zero regions cost no destination space, and destinations without
sparse-file support (exFAT, network shares) work because each part is a plain
fully-allocated file. Regions with no part file read back as zeros; the rescue
map still records which of them were actually read. The destination list is
stored in the index, so `--resume` needs no `--chunk-dest` arguments; passing a
new list on resume replaces it (for example to add a disk once the others are
full). Readers (`exfat_recovery`, `recovery_carver`) open lazy `.rci` files
directly. On successful completion, each
part receives a SHA-256 value in `disk.rci`. Resume validates finalized hashes
before trusting existing chunks. The exFAT parser can open the `.rci` directly.

- If interrupted or the system crashes, `--resume` picks up right where it left off (the `.map` file next to it tracks progress)
- `--reverse` performs the initial pending-range pass from the end of the disk
- `--retry-bad N` retries bad sectors in alternating forward/reverse passes
- Logical sector size is detected automatically; `--sector N` is an override
- Physical-sector alignment is detected for unbuffered I/O. `--read-size` and
  `--min-read-size` tune the fast/backoff reads; `--retries` bounds each attempt
- `--read-timeout-ms` cancels a hung overlapped read. For a hot drive,
  `--pause-every 100G --pause-seconds 300` schedules cooling pauses
- `--skip-size` controls the fast-pass jump after a failed large read. Deferred
  `*` ranges are then trimmed/scraped in the opposite direction down to sectors
- `--max-consecutive-failures N` stops cleanly (exit code 4) after N reads in a
  row fail, which is what a USB bridge that has dropped off the bus looks like;
  without it the fast pass would race across the whole disk in `--skip-size`
  steps. Power-cycle the drive and `--resume`
- `--min-dest-free SIZE` stops cleanly (exit code 3) at the next checkpoint once
  the destination volume's free space drops below SIZE, so a full system drive
  never hangs Windows; continue later with `--resume`
- `--resume` refuses to run if neither the primary map nor its backup is valid
- New map files carry a generation ID plus normalized source, destination, and
  sector-size identity; resume rejects a same-sized map from another source
- `<destination>.report.json` is atomically refreshed at checkpoints and records
  rescued/bad/pending byte counts, physical alignment, transient error attempts,
  confirmed bad sectors, last Win32 error, and finalization state
  (`--report` overrides it)
- Output: `disk.img` (sparse file) or `disk.rci` plus part files, and the adjacent
  `.map` report of good/bad regions
- Unreadable sectors are skipped and logged, they don't halt the whole operation

After this stage, set the original physical disk aside and work only on `disk.img`.

To validate a map structurally, or reconstruct a corrupt primary from its valid
atomic backup without touching image data:

```
rescue_map_tool.exe validate D:\rescue\disk.img.map 8796093022208
rescue_map_tool.exe repair   D:\rescue\disk.img.map 8796093022208
```

Repair quarantines a bad primary as `.invalid` and preserves the valid `.bak`.

## Stage 2 — Recovery with original names (exFAT metadata parser)

Inventory first without writing recovered file data:

```
exfat_recovery.exe D:\rescue\disk.img --scan D:\rescue\plan.csv
```

The scan reports `planned-complete` or `planned-partial` for each matching file
(`planned-unreadable` and `planned-metadata-damaged` are also possible), records
physical `offset:length` extents and exact damaged-byte counts, and honors the
image rescue map. Set `selected` to `0` for rows you do not want, then run:

```
exfat_recovery.exe D:\rescue\disk.img D:\recovered --from-plan D:\rescue\plan.csv
```

Selected files are processed in physical starting-offset order to reduce seeks
on a weak source. This makes it possible to estimate and limit destination space
before recovery.
The final `confidence` column is `high`, `medium`, or `low`; use of the backup
boot region, a dirty/TexFAT volume, deletion, partial data, and damaged metadata
are downgraded conservatively.

### Lost directories (parent entry wiped)

When a folder is listed but its directory cluster reads back as zeros (CHKDSK or
a failing bridge wiped it), its files are still on disk but nothing points at
them. The parser can find such directories by their entry-set signature:

```
exfat_recovery.exe D:escue\disk.img --find-lost-dirs D:escue\lost-dirs.txt
exfat_recovery.exe D:escue\disk.img --scan D:escue\plan.csv --lost-dirs D:escue\lost-dirs.txt
exfat_recovery.exe D:escue\disk.img D:ecovered --from-plan D:escue\plan.csv --lost-dirs D:escue\lost-dirs.txt
```

The search reads one sector per allocated cluster (add `--all-clusters` when the
allocation bitmap cannot be trusted), skips clusters the live tree already
covers, and lists candidates whose first entry set validates. Found directories
are walked as `LOST.DIR/cluster_N/...`; a lost directory named by another lost
directory is walked once under its parent. The scan prints a per-directory
entry histogram on stderr for folders that yield no files, which is how a
zeroed directory cluster shows up.

### Fake-capacity devices

A USB "8 TB" drive with a few dozen GB of real flash reports success for every
read but returns zeros past its real capacity, so imaging shows no errors and
files there come back zero-filled. Find the boundary (`capacity_probe.exe`, or
sample reads at increasing offsets) and pass it to the parser:

```
exfat_recovery.exe \.\PhysicalDriveN --scan D:escue\plan.csv --real-capacity 48G
exfat_recovery.exe \.\PhysicalDriveN D:ecovered --from-plan D:escue\plan.csv --real-capacity 48G
```

Files whose data lies entirely at or beyond the limit are reported as
`beyond-device-capacity` and no output file is written for them; files that
straddle it are `partial` with the missing bytes counted in `damaged_bytes`.

### Low-space direct mode

The parser's read-only Win32 source layer can inspect a raw device without first
creating an 8 TB image (Administrator access is normally required):

```
exfat_recovery.exe \\.\PhysicalDrive2 --scan D:\rescue\plan.csv
exfat_recovery.exe \\.\PhysicalDrive2 --dest D:\recovered1 2TB --dest E:\recovered2 2TB --from-plan D:\rescue\plan.csv
```

With no rescue map, a raw-device scan reports matching data as `planned-unknown`;
only the subsequent read can establish whether each extent is readable. Direct
mode opens the source with `GENERIC_READ` only, but it is still a second-choice
workflow: a mechanically failing disk should be imaged once when adequate storage
is available. Do not mount, repair, format, or run CHKDSK on the source.

```
exfat_recovery.exe D:\rescue\disk.img D:\recovered [--include-deleted]
```

- Whole-disk GPT/MBR images are supported; the exFAT partition is detected automatically
- Use `--offset BYTES` only when automatic partition detection cannot locate the volume
- If `disk.img.map` exists it is loaded automatically; use `--map PATH` to specify another map
- Parses the exFAT boot sector, FAT, and directory entries directly (no need to mount)
- Recreates files with their **original names and folder structure** in `D:\recovered`
- `--include-deleted` also recovers entries without the "in-use" flag (deleted but not yet overwritten)
- Produces `manifest.csv` in the recovered folder: path, size, status (`complete` / `partial` / `deleted-*`)

## Stage 3 — Carving (only for what stage 2 couldn't recover)

```
recovery_carver.exe D:\rescue\disk.img D:\carved --whole-image [--max-output BYTES] [--map PATH] [--include-partial]
```

- Reads flat images and multi-destination `.rci` indexes
- Refuses an implicit full scan: use `--whole-image` deliberately or provide one
  or more exact `--range OFFSET:LENGTH` regions
- Searches for JPEG, PNG, PDF, and GIF by bounded streaming footer scans
- ZIP requires a consistent EOCD/central-directory layout; MP4 requires bounded
  ISO-BMFF boxes containing `ftyp`, `mdat`, and `moov`. Both are carved to their
  structurally derived length rather than a fixed-size guess
- `--max-output BYTES` places a hard cap on newly carved output
- The adjacent image map is loaded automatically; candidates crossing bad or
  unknown ranges are skipped unless `--include-partial` is explicitly supplied
- `carve_manifest.csv` records source offset, length, status, unrescued bytes,
  deterministic output path, and SHA-256 for every accepted or map-rejected candidate
- `.carve.sha256` sidecars make reruns verified and non-destructive; corrupted
  existing outputs are rejected, and equal content is stored only once
- Does not recover original names or paths (since that metadata is gone) — only `carved_NNNNNN_0xOFFSET.ext`
- Last resort for images/video/pdf/zip that the file system no longer knows anything about

## Current status

- Imager: flat and multi-destination checksummed chunk copy/resume tests pass;
  capacity exhaustion, corrupted-part resume, and source/part aliasing are tested;
  it is not yet approved for the real disk
- exfat_recovery: GPT discovery, boot-backup fallback, named recovery, scan planning,
  and rescue-map damage propagation pass synthetic integration tests
- recovery_carver: boundary-spanning flat/`.rci` reads, exact ZIP/MP4 structures,
  output caps, content deduplication, and verified/corrupted reruns are covered
  by synthetic tests

## Suggested next step

Before running on the real 8TB disk:
1. Create a small exFAT partition (a few hundred MB, e.g. on a USB drive or VHD), put some files on it, delete a couple
2. Test `imager` on it
3. Run `exfat_recovery` on the resulting image and confirm file names come back correctly
4. Then run confidently on the image of the 8TB disk
