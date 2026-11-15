#!/usr/bin/env python
"""Ten probe runs of the default text "Coucou" of the Web Modern page in headless Chrome, each in a new
throw-away profile (a cold start of the browser), with the figures of the probe record and the wall time
of chrome.exe (start to exit after the DOM dump). Tooling, not evidence: its output is.

  python tools/trials/web-chrome-157-windows-11-x64/default-series.py [--runs N] [--dist DIR]

Uses WebChromeHostLauncher of tools/run_vectors.py (same flags, same server, same checks)."""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import run_vectors  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--dist", help="dist folder (default $LOOMORA_WEB_DIST or the WSL build folder)")
    ap.add_argument("--platform-id", default="web-chrome-157-windows-11-x64")
    opts = ap.parse_args()
    args = type("A", (), {"variant": "modern", "platform_id": opts.platform_id, "exe": opts.dist,
                          "wsl_distro": None})()
    launcher = run_vectors.WebChromeHostLauncher(args)
    print(launcher.prepare())
    rows = []
    try:
        for n in range(1, opts.runs + 1):
            result = launcher.run("Coucou", 60)
            record = json.loads(result.stdout) if result.stdout.strip() else {}
            rows.append((n, record.get("message"), record.get("display_ms"), record.get("peak_rss_kb"),
                         result.page_ms, result.leftover))
            print("run %2d  message %r  display_ms %s  peak_rss_kb %s  chrome.exe start to exit %s ms  "
                  "chrome.exe left %s" % rows[-1])
    finally:
        launcher.close()
    ok = all(r[1] == "Bonjour Coucou" and r[5] == 0 for r in rows)
    for label, index in (("display_ms", 2), ("chrome.exe start to exit ms", 4)):
        values = [r[index] for r in rows if r[index] is not None]
        print("%s: n %d, min %s, median %s, max %s" % (label, len(values), min(values), statistics.median(values),
                                                       max(values)))
    print("series %s" % ("passed" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
