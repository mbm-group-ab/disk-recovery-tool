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
- `release.yml`: on every push to `main`, bumps a patch tag and attaches the Windows/macOS/Linux GUI builds
  (PyInstaller) to a GitHub Release.
- `deploy-vps.yml`: copies the landing page to the VPS `static-sites` container
  (`disk-recovery-tool.mbm-group.se`). A push runs the checks; the deploy waits for the owner's 🚀 in MBM Control.
  Never start a deploy yourself.
- Docs and comments in English. Git author: Mohammad Behdad Moudi <mbehdadm@gmail.com> (root rule).
