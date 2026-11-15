#!/usr/bin/env python
"""Real-typing check of the Web Legacy page (textarea, end-of-life notice) in Firefox 10 in the
Windows 2000 VM (T093, branch feat/web-legacy-fixes). Tooling, not evidence.

Same setup as typing-check.py (VM running and logged on, LOOMORA_WEB_DIST set, the VM is never
started, stopped, snapshotted or restored). It takes exactly ONE screenshot:

  python tools/trials/web-firefox-10-windows-2000-x86/typing-check-textarea.py OUTDIR

Steps: the page without probe mode is opened, Tab to the field (a textarea), Ctrl+A, "Marie" typed
from scancodes (French AZERTY guest layout), one screenshot (field, greeting and the end-of-life
notice), Alt+F4, process count. Writes OUTDIR/typing-check-textarea-log.txt,
OUTDIR/typing-check-textarea-server-requests.txt and OUTDIR/screenshot-firefox10-textarea-eol-notice.png.
"""
import argparse
import importlib.util
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))

import run_vectors  # noqa: E402

spec = importlib.util.spec_from_file_location(
    "w2k_trial", str(ROOT / "tools" / "trials" / "windows-2000-x86" / "trial-driver.py"))
w2k = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w2k)

PLATFORM_ID = "web-firefox-10-windows-2000-x86"
# On the French AZERTY layout of the guest the letter A is the key at scancode 0x10 (the Q of a
# QWERTY keyboard), so Ctrl+A is Ctrl + scancode 0x10; 0x1E would be Ctrl+Q.
TAB, CTRL, A = 0x0F, 0x1D, 0x10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    args = ap.parse_args()
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    log = []

    def say(text):
        print(text, flush=True)
        log.append(text)

    launcher = run_vectors.WebFirefoxVmLauncher(
        argparse.Namespace(platform_id=PLATFORM_ID, exe=None, wsl_distro=None))
    try:
        say("launcher: " + launcher.prepare())

        launcher.open_url(launcher.page_url(""))
        time.sleep(8)
        codes = [TAB, TAB | 0x80, CTRL, A, A | 0x80, CTRL | 0x80]
        codes += w2k.typing_scancodes("Marie", azerty=True)
        launcher.send_scancodes(codes)
        time.sleep(2)
        launcher.guest.vbm(["controlvm", launcher.vm, "screenshotpng",
                            str(out / "screenshot-firefox10-textarea-eol-notice.png")], 60)
        say("one screenshot written to %s" % out)
        left = launcher.close_window()
        say("Alt+F4 after typing: firefox.exe processes still alive after 20 s: %s" % left)
        say("final count: %s" % launcher.ff_count())
        with open(str(out / "typing-check-textarea-server-requests.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(launcher.server.log_text())
    finally:
        launcher.close()
        with open(str(out / "typing-check-textarea-log.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(l + "\n" for l in log))


if __name__ == "__main__":
    main()
