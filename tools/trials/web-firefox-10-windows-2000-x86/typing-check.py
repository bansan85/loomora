#!/usr/bin/env python
"""Real-typing check of the Web Legacy page in Firefox 10 in the Windows 2000 VM (T093, spike S5).

Run from the repository root with the VM running and logged on (it never starts, stops, snapshots
or restores it) and the dist folder of `python tools/build.py web legacy` reachable (LOOMORA_WEB_DIST,
see tools/run_vectors.py, WebFirefoxVmLauncher):

  python tools/trials/web-firefox-10-windows-2000-x86/typing-check.py OUTDIR

Steps (screenshots through VBoxManage controlvm screenshotpng, keys through keyboardputscancode):
  1. probe run with input=Marie (record posted by the page, post=1), Alt+F4, process count
  2. the page without probe mode: screenshot of the default "Coucou" greeting, Tab to the field,
     Ctrl+A, the letters of "Marie" typed from scancodes (the guest layout is French AZERTY), the
     screenshot of the greeting that follows, Alt+F4, process count
Writes OUTDIR/typing-check-log.txt, OUTDIR/server-requests.txt and the PNG files (the
screenshots are NOT evidence by default: the caller decides which one is kept).
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
TAB, CTRL, A = 0x0F, 0x1D, 0x1E


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

        # 1. probe run with a typed-in value of the URL
        result = launcher.run(u"Marie", 120)
        say("probe run input=Marie: error=%s exit=%s leftover=%s browser start to record %s ms"
            % (result.error, result.returncode, result.leftover, result.page_ms))
        say("record: " + result.stdout.strip())

        # 2. the page with no probe parameter, then typing
        launcher.open_url(launcher.page_url(""))
        time.sleep(8)
        shot = lambda name: launcher.guest.vbm(["controlvm", launcher.vm, "screenshotpng",
                                                 str(out / name)], 60)
        shot("firefox10-1-default.png")
        codes = [TAB, TAB | 0x80, CTRL, A, A | 0x80, CTRL | 0x80]
        codes += w2k.typing_scancodes("Marie", azerty=True)
        launcher.send_scancodes(codes)
        time.sleep(2)
        shot("firefox10-2-typed-marie.png")
        say("screenshots firefox10-1-default.png and firefox10-2-typed-marie.png written to %s" % out)
        left = launcher.close_window()
        say("Alt+F4 after typing: firefox.exe processes still alive after 20 s: %s" % left)
        say("final count: %s" % launcher.ff_count())
        with open(str(out / "server-requests.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(launcher.server.log_text())
    finally:
        launcher.close()
        with open(str(out / "typing-check-log.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(l + "\n" for l in log))


if __name__ == "__main__":
    main()
