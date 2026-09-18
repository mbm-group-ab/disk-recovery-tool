#!/usr/bin/env python3
"""Disk Recovery Tool - graphical front-end for the compiled recovery executables.

Wraps imager / exfat_recovery / recovery_carver / rescue_map_tool the same way
recover.ps1 does: every action shows the exact command line before it runs, and
output streams live into a log pane. Nothing here writes to a source drive;
the executables themselves open sources read-only.
"""
import os
import platform
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_DIR = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = platform.system() == "Windows"

EXE = ".exe" if IS_WINDOWS else ""
TOOL_NAMES = {
    "imager": "imager" + EXE,
    "parser": "exfat_recovery" + EXE,
    "carver": "recovery_carver" + EXE,
    "maptool": "rescue_map_tool" + EXE,
}
SEARCH_DIRS = [
    APP_DIR,
    os.path.join(APP_DIR, "build"),
    os.path.join(APP_DIR, "build", "imager"),
    os.path.join(APP_DIR, "build", "exfat-parser"),
    os.path.join(APP_DIR, "build", "carver"),
    os.path.join(APP_DIR, "build", "map-tool"),
]


def find_tool(key):
    name = TOOL_NAMES[key]
    for d in SEARCH_DIRS:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


class RecoveryGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Disk Recovery Tool")
        self.geometry("880x640")

        self.tools = {k: find_tool(k) for k in TOOL_NAMES}
        self.proc = None

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=8)

        self.image_tab = self._build_image_tab(nb)
        self.scan_tab = self._build_scan_tab(nb)
        self.carve_tab = self._build_carve_tab(nb)
        self.map_tab = self._build_map_tab(nb)
        nb.add(self.image_tab, text="1. Image drive")
        nb.add(self.scan_tab, text="2. Scan / recover files")
        nb.add(self.carve_tab, text="3. Carve")
        nb.add(self.map_tab, text="Map tool")

        log_frame = ttk.LabelFrame(self, text="Command log")
        log_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.log = tk.Text(log_frame, height=12, wrap="none", state="disabled",
                            bg="#111", fg="#ddd", insertbackground="#ddd")
        self.log.pack(fill="both", expand=True)

        missing = [k for k, v in self.tools.items() if not v]
        if missing:
            self._append(
                "Not found: %s\nBuild the project first (see README) or place the "
                "executables next to this script / in build\\.\n" % ", ".join(missing))
        if not IS_WINDOWS:
            self._append(
                "Note: raw physical-drive access (\\\\.\\PhysicalDriveN) is Windows-only. "
                "On macOS/Linux this GUI can still build command lines and run the tools "
                "against image files (.img/.rci).\n")

    # ------------------------------------------------------------ helpers
    def _append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text if text.endswith("\n") else text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _browse_file(self, var, save=False):
        fn = (filedialog.asksaveasfilename if save else filedialog.askopenfilename)()
        if fn:
            var.set(fn)

    def _browse_dir(self, var):
        d = filedialog.askdirectory()
        if d:
            var.set(d)

    def _run(self, tool_key, args):
        exe = self.tools.get(tool_key)
        if not exe:
            messagebox.showerror("Missing tool", "%s was not found. Build the project first."
                                  % TOOL_NAMES[tool_key])
            return
        cmd = [exe] + args
        self._append("\n$ " + " ".join('"%s"' % a if " " in a else a for a in cmd))

        def worker():
            try:
                self.proc = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1)
                for line in self.proc.stdout:
                    self._append(line.rstrip("\n"))
                rc = self.proc.wait()
                self._append("[exit code %d]" % rc)
            except OSError as e:
                self._append("[failed to launch: %s]" % e)
            finally:
                self.proc = None

        threading.Thread(target=worker, daemon=True).start()

    # ------------------------------------------------------------ tab: image
    def _build_image_tab(self, parent):
        f = ttk.Frame(parent, padding=10)
        self.img_source = tk.StringVar()
        self.img_dest = tk.StringVar()
        self.img_chunk = tk.StringVar()
        self.img_lazy = tk.BooleanVar()
        self.img_resume = tk.BooleanVar()

        r = 0
        ttk.Label(f, text="Source (\\\\.\\PhysicalDriveN or existing image)").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.img_source, width=60).grid(row=r, column=1, sticky="we")
        r += 1
        ttk.Label(f, text="Destination image (.img or .rci)").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.img_dest, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_file(self.img_dest, save=True)).grid(row=r, column=2)
        r += 1
        ttk.Label(f, text="Chunk size (e.g. 100G, blank = single file)").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.img_chunk, width=20).grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Checkbutton(f, text="--lazy-parts (skip all-zero regions)", variable=self.img_lazy).grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Checkbutton(f, text="--resume", variable=self.img_resume).grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Button(f, text="Run imager", command=self._run_imager).grid(row=r, column=1, pady=10, sticky="w")
        f.columnconfigure(1, weight=1)
        return f

    def _run_imager(self):
        args = [self.img_source.get(), self.img_dest.get()]
        if self.img_chunk.get():
            args += ["--chunk-size", self.img_chunk.get()]
        if self.img_lazy.get():
            args.append("--lazy-parts")
        if self.img_resume.get():
            args.append("--resume")
        self._run("imager", args)

    # ------------------------------------------------------------ tab: scan
    def _build_scan_tab(self, parent):
        f = ttk.Frame(parent, padding=10)
        self.scan_source = tk.StringVar()
        self.scan_plan = tk.StringVar()
        self.scan_out = tk.StringVar()

        r = 0
        ttk.Label(f, text="Source image / .rci").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.scan_source, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_file(self.scan_source)).grid(row=r, column=2)
        r += 1
        ttk.Label(f, text="Plan file (.csv)").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.scan_plan, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_file(self.scan_plan, save=True)).grid(row=r, column=2)
        r += 1
        ttk.Button(f, text="Run scan", command=self._run_scan).grid(row=r, column=1, pady=6, sticky="w")
        r += 1
        ttk.Separator(f).grid(row=r, column=0, columnspan=3, sticky="we", pady=8)
        r += 1
        ttk.Label(f, text="Recover to folder").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.scan_out, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_dir(self.scan_out)).grid(row=r, column=2)
        r += 1
        ttk.Button(f, text="Run recovery", command=self._run_recover).grid(row=r, column=1, pady=6, sticky="w")
        f.columnconfigure(1, weight=1)
        return f

    def _run_scan(self):
        self._run("parser", [self.scan_source.get(), "--scan", self.scan_plan.get()])

    def _run_recover(self):
        self._run("parser", [self.scan_source.get(), self.scan_out.get(),
                              "--from-plan", self.scan_plan.get()])

    # ------------------------------------------------------------ tab: carve
    def _build_carve_tab(self, parent):
        f = ttk.Frame(parent, padding=10)
        self.carve_source = tk.StringVar()
        self.carve_out = tk.StringVar()

        r = 0
        ttk.Label(f, text="Source image / .rci").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.carve_source, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_file(self.carve_source)).grid(row=r, column=2)
        r += 1
        ttk.Label(f, text="Output folder").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.carve_out, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_dir(self.carve_out)).grid(row=r, column=2)
        r += 1
        ttk.Button(f, text="Run carver", command=self._run_carve).grid(row=r, column=1, pady=10, sticky="w")
        f.columnconfigure(1, weight=1)
        return f

    def _run_carve(self):
        self._run("carver", [self.carve_source.get(), "--out", self.carve_out.get()])

    # ------------------------------------------------------------ tab: map tool
    def _build_map_tab(self, parent):
        f = ttk.Frame(parent, padding=10)
        self.map_file = tk.StringVar()
        self.map_total_bytes = tk.StringVar()

        r = 0
        ttk.Label(f, text="Rescue map (.map)").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.map_file, width=60).grid(row=r, column=1, sticky="we")
        ttk.Button(f, text="Browse...", command=lambda: self._browse_file(self.map_file)).grid(row=r, column=2)
        r += 1
        ttk.Label(f, text="Source total bytes").grid(row=r, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.map_total_bytes, width=20).grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Button(f, text="Validate", command=lambda: self._run(
            "maptool", ["validate", self.map_file.get(), self.map_total_bytes.get()])).grid(row=r, column=1, pady=6, sticky="w")
        r += 1
        ttk.Button(f, text="Repair", command=lambda: self._run(
            "maptool", ["repair", self.map_file.get(), self.map_total_bytes.get()])).grid(row=r, column=1, sticky="w")
        f.columnconfigure(1, weight=1)
        return f


if __name__ == "__main__":
    RecoveryGUI().mainloop()
