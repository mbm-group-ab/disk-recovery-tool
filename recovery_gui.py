#!/usr/bin/env python3
"""Disk Recovery Tool - graphical front-end for the compiled recovery executables.

Wraps imager / exfat_recovery / recovery_carver / rescue_map_tool the same way
recover.ps1 does: every action shows the exact command line before it runs, and
output streams live into a log pane. Nothing here writes to a source drive;
the executables themselves open sources read-only.

Command lines are built by the build_*_args functions below; the shown command
and the executed command are the same list, and tests/gui_contract_test.py runs
those lists against the real binaries. Only one job runs at a time.
"""
import json
import os
import platform
import queue
import re
import subprocess
import sys
import threading

from gui_i18n import LANGUAGES, RTL, TEXTS

VERSION = "0.1.0"

APP_DIR = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = platform.system() == "Windows"
FROZEN = getattr(sys, "frozen", False)

EXE = ".exe" if IS_WINDOWS else ""
TOOL_NAMES = {
    "imager": "imager" + EXE,
    "parser": "exfat_recovery" + EXE,
    "carver": "recovery_carver" + EXE,
    "maptool": "rescue_map_tool" + EXE,
}

IMAGER_EXIT_MEANING = {3: "exit_3", 4: "exit_4", 5: "exit_5", 6: "exit_6"}


def search_dirs():
    """Where the engine binaries may live.

    A packaged (frozen) build only looks beside its own executable and in its
    engine\\ folder, never in the working directory, so a stray file in the
    current folder cannot stand in for a recovery tool.
    """
    if FROZEN:
        base = os.path.dirname(os.path.abspath(sys.executable))
        return [base, os.path.join(base, "engine")]
    return [
        APP_DIR,
        os.path.join(APP_DIR, "build"),
        os.path.join(APP_DIR, "build", "imager"),
        os.path.join(APP_DIR, "build", "exfat-parser"),
        os.path.join(APP_DIR, "build", "carver"),
        os.path.join(APP_DIR, "build", "map-tool"),
    ]


def find_tool(key):
    name = TOOL_NAMES[key]
    for d in search_dirs():
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


# ------------------------------------------------------------ command builders

class InputError(ValueError):
    """Invalid user input; .key/.params name the translated message."""

    def __init__(self, key, **params):
        super().__init__(key)
        self.key = key
        self.params = params


_SIZE = re.compile(r"^\s*(\d+)\s*([KMGT]?)(?:i?B)?\s*$", re.IGNORECASE)
_UNITS = {"": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4}


def parse_size(text, field):
    m = _SIZE.match(text or "")
    if not m:
        raise InputError("err_size", field=field, value=text)
    return int(m.group(1)) * _UNITS[m.group(2).upper()]


def _required(value, field):
    value = (value or "").strip()
    if not value:
        raise InputError("err_required", field=field)
    return value


def build_imager_args(source, dest, chunk_size="", chunk_dest="", lazy=False, resume=False):
    args = [_required(source, "source"), _required(dest, "destination")]
    chunk_size, chunk_dest = (chunk_size or "").strip(), (chunk_dest or "").strip()
    if chunk_size or chunk_dest:
        if not resume and not (chunk_size and chunk_dest):
            raise InputError("err_chunk_pair")
        if chunk_size:
            parse_size(chunk_size, "chunk size")
            args += ["--chunk-size", chunk_size]
        if chunk_dest:
            folder, _, capacity = chunk_dest.rpartition(" ")
            if not folder.strip():
                raise InputError("err_chunk_pair")
            parse_size(capacity, "chunk capacity")
            args += ["--chunk-dest", folder.strip(), capacity]
    if lazy:
        args.append("--lazy-parts")
    if resume:
        args.append("--resume")
    return args


def build_scan_args(source, plan):
    return [_required(source, "source"), "--scan", _required(plan, "plan file")]


def build_recover_args(source, out_dir, plan):
    return [_required(source, "source"), _required(out_dir, "output folder"),
            "--from-plan", _required(plan, "plan file")]


def build_carve_args(source, out_dir, whole_image=True, ranges="", max_output="",
                     map_path="", include_partial=False):
    """Matches recovery_carver: <image> <output_dir> (--whole-image | --range O:L...)."""
    args = [_required(source, "source"), _required(out_dir, "output folder")]
    if whole_image:
        args.append("--whole-image")
    else:
        parts = [p.strip() for p in (ranges or "").split(",") if p.strip()]
        if not parts:
            raise InputError("err_ranges")
        for part in parts:
            offset, sep, length = part.partition(":")
            if not sep:
                raise InputError("err_range", value=part)
            try:
                o = parse_size(offset, "offset")
                n = parse_size(length, "length")
            except InputError:
                raise InputError("err_range", value=part) from None
            if n == 0:
                raise InputError("err_range", value=part)
            args += ["--range", "%d:%d" % (o, n)]
    if (max_output or "").strip():
        args += ["--max-output", str(parse_size(max_output, "max output"))]
    if (map_path or "").strip():
        args += ["--map", map_path.strip()]
    if include_partial:
        args.append("--include-partial")
    return args


def build_map_args(operation, map_file, total_bytes):
    total = _required(total_bytes, "total bytes")
    if not total.isdigit():
        raise InputError("err_number", field="total bytes")
    return [operation, _required(map_file, "map"), total]


def display_command(cmd):
    return " ".join('"%s"' % a if (" " in a or not a) else a for a in cmd)


# ------------------------------------------------------------ settings

def settings_path():
    base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "DiskRecoveryTool", "settings.json")


def load_settings():
    try:
        with open(settings_path(), encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(data):
    path = settings_path()
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(tmp, path)
    except OSError:
        pass


# ------------------------------------------------------------ GUI

def main():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    class RecoveryGUI(tk.Tk):
        def __init__(self):
            super().__init__()
            self.geometry("920x680")
            self.settings = load_settings()
            self.lang = self.settings.get("language", "en")
            if self.lang not in TEXTS:
                self.lang = "en"
            self.tools = {k: find_tool(k) for k in TOOL_NAMES}
            self.job = None  # {"proc", "tool", "stop_file"} while a tool runs
            self.lines = queue.Queue()
            self.vars = {}
            self.action_buttons = []
            self.protocol("WM_DELETE_WINDOW", self._on_close)
            self._build()
            self.after(100, self._drain)

        # -------------------------------------------------- text helpers
        def t(self, key, **params):
            text = TEXTS[self.lang].get(key) or TEXTS["en"][key]
            return text.format(**params) if params else text

        def var(self, name, kind=tk.StringVar, default=None):
            if name not in self.vars:
                self.vars[name] = kind(value=default) if default is not None else kind()
            return self.vars[name]

        # -------------------------------------------------- layout
        def _build(self):
            for child in self.winfo_children():
                child.destroy()
            self.action_buttons = []
            rtl = self.lang in RTL
            self.side = "e" if rtl else "w"
            self.title("%s %s" % (self.t("title"), VERSION))

            menubar = tk.Menu(self)
            lang_menu = tk.Menu(menubar, tearoff=0)
            for code, name in LANGUAGES.items():
                lang_menu.add_command(label=name, command=lambda c=code: self._set_language(c))
            menubar.add_cascade(label=self.t("menu_language"), menu=lang_menu)
            help_menu = tk.Menu(menubar, tearoff=0)
            help_menu.add_command(label=self.t("about"), command=self._about)
            menubar.add_cascade(label=self.t("menu_help"), menu=help_menu)
            self.config(menu=menubar)

            nb = ttk.Notebook(self)
            nb.pack(fill="both", expand=True, padx=8, pady=8)
            tabs = [
                (self._build_image_tab(nb), "tab_image"),
                (self._build_scan_tab(nb), "tab_scan"),
                (self._build_carve_tab(nb), "tab_carve"),
                (self._build_map_tab(nb), "tab_map"),
            ]
            for frame, key in (reversed(tabs) if rtl else tabs):
                nb.add(frame, text=self.t(key))

            log_frame = ttk.LabelFrame(self, text=self.t("log"))
            log_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))
            bar = ttk.Frame(log_frame)
            bar.pack(fill="x")
            self.stop_button = ttk.Button(bar, text=self.t("stop"), command=self._stop,
                                          state="disabled")
            self.stop_button.pack(side="left" if not rtl else "right", padx=4, pady=4)
            self.log = tk.Text(log_frame, height=12, wrap="none", state="disabled",
                               bg="#111", fg="#ddd", insertbackground="#ddd")
            self.log.pack(fill="both", expand=True)

            missing = [TOOL_NAMES[k] for k, v in self.tools.items() if not v]
            if missing:
                self._append(self.t("missing_tools", names=", ".join(missing)))
            if not IS_WINDOWS:
                self._append(self.t("posix_note"))
            self._set_busy(self.job is not None)

        def _row(self, f, r, label_key, var_name, browse=None):
            rtl = self.lang in RTL
            label_col, entry_col, button_col = (2, 1, 0) if rtl else (0, 1, 2)
            ttk.Label(f, text=self.t(label_key), anchor=self.side,
                      justify="right" if rtl else "left").grid(
                row=r, column=label_col, sticky=self.side, padx=4, pady=2)
            ttk.Entry(f, textvariable=self.var(var_name), width=60).grid(
                row=r, column=entry_col, sticky="we", pady=2)
            if browse:
                ttk.Button(f, text=self.t("browse"), command=browse).grid(row=r, column=button_col)

        def _check(self, f, r, label_key, var_name):
            ttk.Checkbutton(f, text=self.t(label_key),
                            variable=self.var(var_name, tk.BooleanVar)).grid(
                row=r, column=1, sticky=self.side)

        def _action(self, f, r, label_key, command):
            b = ttk.Button(f, text=self.t(label_key), command=command)
            b.grid(row=r, column=1, pady=8, sticky=self.side)
            self.action_buttons.append(b)

        def _file(self, name, save=False):
            fn = (filedialog.asksaveasfilename if save else filedialog.askopenfilename)()
            if fn:
                self.var(name).set(fn)

        def _dir(self, name):
            d = filedialog.askdirectory()
            if d:
                self.var(name).set(d)

        # -------------------------------------------------- tabs
        def _build_image_tab(self, parent):
            f = ttk.Frame(parent, padding=10)
            self._row(f, 0, "img_source", "img_source")
            self._row(f, 1, "img_dest", "img_dest", lambda: self._file("img_dest", save=True))
            self._row(f, 2, "img_chunk", "img_chunk")
            self._row(f, 3, "img_chunk_dest", "img_chunk_dest")
            self._check(f, 4, "img_lazy", "img_lazy")
            self._check(f, 5, "img_resume", "img_resume")
            self._action(f, 6, "img_run", self._run_imager)
            f.columnconfigure(1, weight=1)
            return f

        def _build_scan_tab(self, parent):
            f = ttk.Frame(parent, padding=10)
            self._row(f, 0, "scan_source", "scan_source", lambda: self._file("scan_source"))
            self._row(f, 1, "scan_plan", "scan_plan", lambda: self._file("scan_plan", save=True))
            self._action(f, 2, "scan_run", lambda: self._start(
                "parser", lambda: build_scan_args(self.var("scan_source").get(),
                                                  self.var("scan_plan").get())))
            ttk.Separator(f).grid(row=3, column=0, columnspan=3, sticky="we", pady=8)
            self._row(f, 4, "scan_out", "scan_out", lambda: self._dir("scan_out"))
            self._action(f, 5, "recover_run", lambda: self._start(
                "parser", lambda: build_recover_args(self.var("scan_source").get(),
                                                     self.var("scan_out").get(),
                                                     self.var("scan_plan").get())))
            f.columnconfigure(1, weight=1)
            return f

        def _build_carve_tab(self, parent):
            f = ttk.Frame(parent, padding=10)
            self._row(f, 0, "carve_source", "carve_source", lambda: self._file("carve_source"))
            self._row(f, 1, "carve_out", "carve_out", lambda: self._dir("carve_out"))
            whole = self.var("carve_whole", tk.BooleanVar, True)
            ttk.Radiobutton(f, text=self.t("carve_whole"), variable=whole, value=True).grid(
                row=2, column=1, sticky=self.side)
            ttk.Radiobutton(f, text=self.t("carve_range"), variable=whole, value=False).grid(
                row=3, column=1, sticky=self.side)
            ttk.Entry(f, textvariable=self.var("carve_ranges"), width=60).grid(
                row=4, column=1, sticky="we")
            self._row(f, 5, "carve_max", "carve_max")
            self._row(f, 6, "carve_map", "carve_map", lambda: self._file("carve_map"))
            self._check(f, 7, "carve_partial", "carve_partial")
            self._action(f, 8, "carve_run", lambda: self._start(
                "carver", lambda: build_carve_args(
                    self.var("carve_source").get(), self.var("carve_out").get(),
                    whole.get(), self.var("carve_ranges").get(), self.var("carve_max").get(),
                    self.var("carve_map").get(), self.var("carve_partial", tk.BooleanVar).get())))
            f.columnconfigure(1, weight=1)
            return f

        def _build_map_tab(self, parent):
            f = ttk.Frame(parent, padding=10)
            self._row(f, 0, "map_file", "map_file", lambda: self._file("map_file"))
            self._row(f, 1, "map_total", "map_total")
            for r, (key, op) in enumerate((("map_validate", "validate"), ("map_repair", "repair")), 2):
                self._action(f, r, key, lambda op=op: self._start(
                    "maptool", lambda: build_map_args(op, self.var("map_file").get(),
                                                      self.var("map_total").get())))
            f.columnconfigure(1, weight=1)
            return f

        def _run_imager(self):
            dest = self.var("img_dest").get().strip()
            resume = self.var("img_resume", tk.BooleanVar).get()
            if dest and not resume and os.path.exists(dest):
                messagebox.showwarning(self.t("invalid_title"), self.t("img_exists", path=dest))
                return
            self._start("imager", lambda: build_imager_args(
                self.var("img_source").get(), dest, self.var("img_chunk").get(),
                self.var("img_chunk_dest").get(), self.var("img_lazy", tk.BooleanVar).get(),
                resume), stop_file=dest + ".stop" if dest else None)

        # -------------------------------------------------- job control
        def _set_busy(self, busy):
            for b in self.action_buttons:
                b.configure(state="disabled" if busy else "normal")
            self.stop_button.configure(state="normal" if busy else "disabled")

        def _start(self, tool, build, stop_file=None):
            if self.job is not None:
                messagebox.showinfo(self.t("title"), self.t("busy"))
                return
            exe = self.tools.get(tool)
            if not exe:
                messagebox.showerror(self.t("missing_tool_title"),
                                     self.t("missing_tool", name=TOOL_NAMES[tool]))
                return
            try:
                args = build()
            except InputError as e:
                params = dict(e.params)
                messagebox.showerror(self.t("invalid_title"), self.t(e.key, **params))
                return
            cmd = [exe] + args
            self._append("\n$ " + display_command(cmd))
            if stop_file and os.path.exists(stop_file):
                try:
                    os.remove(stop_file)  # a stale request from an earlier run
                except OSError:
                    pass
            flags = subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, text=True, bufsize=1,
                                        errors="replace", creationflags=flags)
            except OSError as e:
                self._append(self.t("launch_failed", error=e))
                return
            self.job = {"proc": proc, "tool": tool, "stop_file": stop_file}
            self._set_busy(True)

            def reader():
                for line in proc.stdout:
                    self.lines.put(("line", line.rstrip("\n")))
                self.lines.put(("exit", proc.wait()))

            threading.Thread(target=reader, daemon=True).start()

        def _drain(self):
            # Tk widgets are only touched here, on the event loop.
            try:
                while True:
                    kind, value = self.lines.get_nowait()
                    if kind == "line":
                        self._append(value)
                    else:
                        tool = self.job["tool"] if self.job else ""
                        key = IMAGER_EXIT_MEANING.get(value) if tool == "imager" else None
                        self._append(self.t("exit_code", code=value,
                                            meaning=self.t(key) if key else ""))
                        self.job = None
                        self._set_busy(False)
            except queue.Empty:
                pass
            self.after(100, self._drain)

        def _stop(self, ask=True):
            job = self.job
            if job is None:
                return
            if job["tool"] == "imager" and job["stop_file"]:
                try:
                    with open(job["stop_file"], "w", encoding="utf-8"):
                        pass
                    self._append(self.t("stop_imager"))
                    return
                except OSError:
                    pass
            if ask and not messagebox.askyesno(self.t("stop"), self.t("stop_confirm")):
                return
            try:
                job["proc"].terminate()
            except OSError:
                pass

        def _on_close(self):
            if self.job is not None:
                if not messagebox.askyesno(self.t("title"), self.t("close_confirm")):
                    return
                job = self.job
                self._stop(ask=False)
                try:
                    # Give the imager time to reach its checkpoint, then end it.
                    job["proc"].wait(timeout=45 if job["tool"] == "imager" else 5)
                except subprocess.TimeoutExpired:
                    job["proc"].terminate()
            self.destroy()

        # -------------------------------------------------- misc
        def _append(self, text):
            self.log.configure(state="normal")
            self.log.insert("end", text if text.endswith("\n") else text + "\n")
            self.log.see("end")
            self.log.configure(state="disabled")

        def _set_language(self, code):
            if code == self.lang:
                return
            self.lang = code
            self.settings["language"] = code
            save_settings(self.settings)
            log_text = self.log.get("1.0", "end-1c")
            self._build()
            self.log.configure(state="normal")
            self.log.delete("1.0", "end")
            self.log.insert("end", log_text)
            self.log.configure(state="disabled")

        def _about(self):
            messagebox.showinfo(self.t("about"), self.t("about_text", version=VERSION))

    RecoveryGUI().mainloop()


if __name__ == "__main__":
    main()
