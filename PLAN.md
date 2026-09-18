# Disk Recovery Tool — Implementation Plan

The tool must not be used on the real 8 TB source disk until Phase 1 is complete
and its synthetic-image tests pass. Work proceeds autonomously phase by phase;
real-disk access, destructive operations, and changes that can overwrite existing
recovery data always require explicit approval.

## Goals

- Never write to the damaged source disk.
- Never report a file as complete when its source extents include unreadable data.
- Support whole-disk images containing GPT/MBR partitions.
- Allow useful recovery without requiring one empty destination as large as the source.
- Make every long-running operation resumable and auditable.

## Phase 1 — Correctness and safety foundation

- [x] Add a shared Win32 random-access reader that supports regular images and raw physical devices.
- [x] Detect GPT and MBR partitions and locate an exFAT volume automatically; support an explicit offset override.
- [x] Validate the primary and backup exFAT boot regions and all shift/count/range fields before allocation or I/O.
- [x] Load the imager rescue map in the parser and propagate good/bad/unknown source ranges to each recovered file.
- [x] Report `complete`, `partial`, `unreadable`, and `metadata-damaged` truthfully.
- [x] Check every directory creation, open, read, write, flush, and close result.
- [x] Make rescue-map checkpoints atomic and keep a last-known-good backup.
- [x] Flush image data before marking its ranges rescued in a durable checkpoint.
- [x] Prevent source/destination aliasing, output traversal, reserved Windows names, and filename collisions.
- [x] Bound cluster chains, file sizes, recursion, and memory usage; detect loops.
- [x] Add synthetic tests for partitioned images, backup boot sectors, bad ranges, corrupt metadata, disk-full/write failures, and resume.

Exit criterion: all automated Phase 1 tests pass and a small disposable exFAT image
round-trips through imaging and named-file recovery without false `complete` results.

## Phase 2 — Scan and selective low-space recovery

- [x] Add `scan` mode that reads metadata only and writes a durable recovery-plan file.
- [x] Inventory live/deleted files, logical sizes, physical extents, and damage intersections.
- [x] Add filters for path, extension, size, live/deleted state, and user priority.
- [x] Add exFAT timestamp parsing and date filters.
- [x] Convert selected files into a de-duplicated extent plan sorted by physical LBA.
- [x] Read selected extents in disk order, then reconstruct the original directory tree.
- [x] Support multiple capacity-limited destinations using actual free-space checks.
- [x] Resume without truncating verified output; resolve collisions deterministically.

Exit criterion: recovering 2 TB of selected live files from an 8 TB source needs
approximately 2 TB plus metadata/index overhead, rather than an 8 TB image.

## Phase 3 — Resilient imaging and chunk storage

- [x] Implement fast forward/reverse copy passes, trimming, scraping, and bounded retries.
- [x] Detect logical/physical sector sizes and required unbuffered-I/O alignment.
- [x] Add configurable timeouts, skip sizes, retry counts, and thermal/user pauses.
- [x] Distinguish transient/device errors from confirmed unreadable sectors.
- [x] Store checksummed chunks with an index and allow chunks across multiple destination disks.
- [x] Add map validation, generation identifiers, command/source identity, atomic backup, and repair tooling.
- [x] Produce machine-readable progress and final integrity reports.

Exit criterion: interrupted imaging resumes safely; already verified chunks are not
reread; bad regions can be retried in later passes; no single 8 TB filesystem is required.

## Phase 4 — Filesystem recovery and validated carving

- [x] Parse the exFAT Allocation Bitmap, active FAT, entry-set checksum, and ValidDataLength.
- [x] Validate NameHash using the active up-case table and handle TexFAT transactional state.
- [x] Rank metadata confidence and resolve primary/backup or duplicated metadata conservatively.
- [x] Make carving respect bad/unknown rescue-map ranges, with explicit partial opt-in.
- [x] Carve only filesystem-unclaimed or explicitly selected regions.
- [x] Replace fixed-size ZIP/MP4 output with format-aware length validation.
- [x] Add streaming validators for JPEG, PNG, PDF, ZIP, MP4, and GIF.
- [x] De-duplicate carved results by extent and content hash.
- [x] Add output caps, deterministic rerun behavior, provenance, and map-integrity status to carved output.
- [x] Add cryptographic content hashes and verified carving resume.

Exit criterion: carving cannot create unbounded garbage output, every artifact records
its source extents and confidence, and reruns do not duplicate verified results.

## Delivery discipline

Each phase is developed in small reviewable commits after the repository has an
initial baseline commit. No command in development targets the real damaged disk.
Tests use generated images and disposable destinations only. README usage examples
are updated only after the corresponding behavior is implemented and tested.
