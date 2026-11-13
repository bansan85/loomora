#!/usr/bin/env python3
"""Probe series of the Linux Modern trial, run inside WSL (default GDK backend, no env changes).

usage: python3 probe-series.py <exe> <outdir> <suffix> [runs_per_scenario]

Runs <exe> --probe N times (scenario "default") and <exe> --probe --probe-input Marie N times
(scenario "marie"). For each run it records the exit status, the size of standard error, the
number of stdout lines, the wall time of the process, the resource usage of that child alone
(os.wait4: ru_maxrss in kB), the probe line fields and whether a process of the same name is
left after the exit (pgrep -x). Writes into <outdir>:
  probe-default-run1-<suffix>.json   first probe line of the default scenario
  probe-marie-run1-<suffix>.json     first probe line of the Marie scenario
  probe-default-runs-<suffix>.jsonl  every probe line of the default scenario
  probe-marie-runs-<suffix>.jsonl    every probe line of the Marie scenario
  trial-measurements-<suffix>.json   one record per run, plus the executable path, size and sha256
Files are written with LF line endings.
"""
import hashlib, json, os, subprocess, sys, time

exe, outdir, suffix = sys.argv[1:4]
runs = int(sys.argv[4]) if len(sys.argv) > 4 else 10
name = os.path.basename(exe)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def pgrep():
    out = subprocess.run(["pgrep", "-x", name[:15]], stdout=subprocess.PIPE).stdout.decode().split()
    return out


def one(args):
    t0 = time.monotonic()
    p = subprocess.Popen([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.read()
    err = p.stderr.read()
    _, status, ru = os.wait4(p.pid, 0)
    wall_ms = (time.monotonic() - t0) * 1000.0
    rc = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -os.WTERMSIG(status)
    return rc, out, err, wall_ms, ru.ru_maxrss


records = []
lines = {"default": [], "marie": []}
for scenario, args in (("default", ["--probe"]), ("marie", ["--probe", "--probe-input", "Marie"])):
    for i in range(runs):
        rc, out, err, wall_ms, ru_maxrss = one(args)
        text = out.decode("utf-8", "replace")
        line = text.strip().splitlines()[0] if text.strip() else ""
        rec = json.loads(line) if line.startswith("{") else {}
        lines[scenario].append(line)
        records.append({
            "scenario": scenario, "index": i, "returncode": rc,
            "stdout_lines": len(text.strip().splitlines()),
            "stderr_bytes": len(err), "stderr": err.decode("utf-8", "replace"),
            "wall_ms": round(wall_ms, 1),
            "child_ru_maxrss_kb": ru_maxrss,
            "display_ms": rec.get("display_ms"), "peak_rss_kb": rec.get("peak_rss_kb"),
            "message": rec.get("message"), "input": rec.get("input"),
            "greeting_matches": rec.get("greeting_matches"), "displayed": rec.get("displayed"),
            "eol_warning_shown": rec.get("eol_warning_shown"),
            "disabled_features": rec.get("disabled_features"),
            "leftover_after_exit": pgrep(),
        })


def write(path, text):
    with open(os.path.join(outdir, path), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


for scenario in lines:
    write("probe-%s-run1-%s.json" % (scenario, suffix), lines[scenario][0] + "\n")
    write("probe-%s-runs-%s.jsonl" % (scenario, suffix), "\n".join(lines[scenario]) + "\n")
write("trial-measurements-%s.json" % suffix, json.dumps({
    "exe": exe, "exe_sha256": sha256(exe), "exe_size": os.path.getsize(exe),
    "runs": records}, indent=2, ensure_ascii=False) + "\n")
print("done: %d runs" % len(records))
