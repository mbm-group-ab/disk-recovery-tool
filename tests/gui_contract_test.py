"""Contract test: the GUI's command builders against the real binaries.

Runs the exact argument lists recovery_gui.py would run (no Tk window) on a
synthetic image in a temporary folder, and checks the translations.
Usage: python gui_contract_test.py --carver PATH --imager PATH
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import recovery_gui as gui  # noqa: E402
from gui_i18n import TEXTS  # noqa: E402

FAILURES = []


def check(cond, message):
    if not cond:
        FAILURES.append(message)
        print("FAIL:", message)


def run(exe, args):
    proc = subprocess.run([exe] + args, capture_output=True, text=True, errors="replace")
    return proc.returncode, proc.stdout + proc.stderr


def test_translations():
    placeholder = re.compile(r"{(\w+)}")
    en = TEXTS["en"]
    for lang, texts in TEXTS.items():
        check(set(texts) == set(en), "%s keys differ from en: %s" % (lang, set(en) ^ set(texts)))
        for key, value in en.items():
            if key in texts:
                check(set(placeholder.findall(texts[key])) == set(placeholder.findall(value)),
                      "%s/%s placeholders differ" % (lang, key))


def test_builders():
    check(gui.parse_size("200G", "x") == 200 * 1024 ** 3, "parse_size 200G")
    for bad in ("", "abc", "-1", "1.5G"):
        try:
            gui.parse_size(bad, "x")
            check(False, "parse_size accepted %r" % bad)
        except gui.InputError:
            pass
    args = gui.build_carve_args("img", "out", whole_image=False, ranges="0:1M, 2M:512K",
                                max_output="1G", map_path="img.map", include_partial=True)
    check(args == ["img", "out", "--range", "0:1048576", "--range", "2097152:524288",
                   "--max-output", str(1024 ** 3), "--map", "img.map", "--include-partial"],
          "carve args: %r" % args)
    check("--out" not in gui.build_carve_args("img", "out"), "old --out flag is gone")
    for kwargs in ({"whole_image": False, "ranges": ""}, {"whole_image": False, "ranges": "5"}):
        try:
            gui.build_carve_args("img", "out", **kwargs)
            check(False, "carve accepted %r" % kwargs)
        except gui.InputError:
            pass
    try:
        gui.build_imager_args("src", "dst", chunk_size="1G")
        check(False, "imager accepted chunk size without folder")
    except gui.InputError:
        pass
    check(gui.build_imager_args("s", "d", "1G", r"E:\parts dir 2T") ==
          ["s", "d", "--chunk-size", "1G", "--chunk-dest", r"E:\parts dir", "2T"],
          "imager chunk args")


def make_image(path):
    data = bytearray(4 * 1024 * 1024)
    jpeg = b"\xff\xd8\xff\xe0" + b"\x00\x10JFIF\x00" + bytes(200) + b"\xff\xd9"
    data[1024 * 1024:1024 * 1024 + len(jpeg)] = jpeg
    with open(path, "wb") as f:
        f.write(data)


def test_binaries(carver, imager, root):
    image = os.path.join(root, "source.img")
    make_image(image)

    rc, out = run(carver, gui.build_carve_args(image, os.path.join(root, "whole")))
    check(rc == 0, "carver whole-image via GUI args exited %d: %s" % (rc, out[-400:]))
    rc, out = run(carver, gui.build_carve_args(image, os.path.join(root, "range"),
                                               whole_image=False, ranges="1M:1M",
                                               max_output="10M"))
    check(rc == 0, "carver range via GUI args exited %d: %s" % (rc, out[-400:]))

    dest = os.path.join(root, "copy.img")
    rc, out = run(imager, gui.build_imager_args(image, dest) + ["--sector", "512"])
    check(rc == 0, "imager via GUI args exited %d: %s" % (rc, out[-400:]))
    rc, out = run(imager, gui.build_imager_args(image, dest) + ["--sector", "512"])
    check(rc == 6 and gui.IMAGER_EXIT_MEANING.get(rc) == "exit_6",
          "imager rerun without resume should be refused (6), got %d" % rc)
    rc, out = run(imager, gui.build_imager_args(image, dest, resume=True) + ["--sector", "512"])
    check(rc == 0, "imager resume via GUI args exited %d: %s" % (rc, out[-400:]))
    for code in (7, 8):
        key = gui.IMAGER_EXIT_MEANING.get(code)
        check(key == "exit_%d" % code, "imager exit %d has no GUI meaning" % code)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--carver", required=True)
    parser.add_argument("--imager", required=True)
    opts = parser.parse_args()
    test_translations()
    test_builders()
    root = tempfile.mkdtemp(prefix="disk_recovery_gui_contract_")
    try:
        test_binaries(opts.carver, opts.imager, root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        return 1
    print("gui contract: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
