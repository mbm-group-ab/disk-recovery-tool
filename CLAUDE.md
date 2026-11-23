# disk-recovery-tool: rules for agents

Recovery tool for a damaged 8 TB exFAT disk (scrambled sectors). Read `README.md` (build, the three stages,
current status) and `PLAN.md` (phases and their exit criteria) before changing anything.

## Safety first (from PLAN.md)
- **Never write to the damaged source disk.** Every tool opens the source read-only; keep it that way.
- Never touch a real disk yourself (`\\.\PhysicalDriveN`): real-disk access, destructive operations and anything
  that can overwrite existing recovery data (images, rescue maps, recovered folders) need the owner's explicit OK.
  Work on synthetic images in `tests/`.
- Never report a file as complete when any of its source ranges is unreadable (`complete` / `partial` /
  `unreadable` / `metadata-damaged` must stay truthful).
- Rescue-map checkpoints stay atomic, with a last-known-good backup; image data is flushed before its range is
  marked rescued.
- The stages run in order: 1 imaging, 2 exFAT parser (original names), 3 carving only for what stage 2 missed.

## Layout
- C++ (CMake, MinGW via scoop): `imager/`, `exfat-parser/`, `carver/`, `map-tool/`, shared code in `common/`.
- `recovery_gui.py`: Tkinter front-end that always shows the exact command it runs. `recover.ps1`: guided menu.
- `index.html`, `robots.txt`, `sitemap.xml`, `assets/`: landing page, English only (root rule for non-app sites).

## Build and test
```bash
export PATH="/c/Users/mbehd/scoop/apps/mingw/current/bin:$PATH"
cmake -S . -B build -G "MinGW Makefiles" && cmake --build build
ctest --test-dir build --output-on-failure
```
Every change to a tool needs a test in `tests/` (C++ unit tests or the `*_integration.ps1` scripts on synthetic images).

## CI and deploy (public repo, GitHub-hosted runners are fine)
- `release.yml` (Windows only): builds the engine, runs ctest, packages GUI + `engine\` + `manifest.json` +
  `SHA256SUMS.txt` as `DiskRecoveryTool-windows.zip`, and publishes a release only when `VERSION` in
  `recovery_gui.py` has no `v<VERSION>` tag yet. Bump `VERSION` to release; macOS/Linux are not packaged.
- Imager exit codes are part of the GUI contract (3 low space, 4 unresponsive, 5 stop requested via
  `<dest>.stop`, 6 refused existing image/map/report, 7 refused resume of another source (fingerprint or
  rescued-sample mismatch; `--allow-new-source-id` only skips the fingerprint), 8 refused destination on the source
  disk, 9 busy: another run holds `<dest>.lock`; carver/parser also return 9 on a held `<output>\.recovery.lock`).
  The test build fakes disk numbers with `IMAGER_TEST_SOURCE_DISK` / `IMAGER_TEST_DEST_DISK` and injects faults with
  `IMAGER_TEST_FAIL_WRITE_AFTER` / `IMAGER_TEST_CHECKPOINT_BYTES` (never compiled into the release imager). The map is
  saved right after the destination opens; keep it that way (a crash before the first checkpoint must stay resumable).
  `tests/fuzz_tests.cpp` (fixed seed) covers map and `.rci` loading; extend it when a loader changes. GUI texts live in `gui_i18n.py` (EN/SV/FA, keys checked
  by `tests/gui_contract_test.py`); command lines come only from the `build_*_args` functions.
- `deploy-vps.yml`: copies the landing page to the VPS `static-sites` container
  (`disk-recovery-tool.mbm-group.se`). A push runs the checks; the deploy waits for the owner's 🚀 in MBM Control.
  Never start a deploy yourself.
- Docs and comments in English. Git author: Mohammad Behdad Moudi <mbehdadm@gmail.com> (root rule).
