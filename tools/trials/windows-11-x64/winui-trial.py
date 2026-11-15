"""Trials of the Windows Modern UI (WinUI 3) on a Windows host. Tooling, not evidence: the script that
produced the logs stored under validation/evidence/winui3/windows-11-x64/.

  python tools/trials/windows-11-x64/winui-trial.py series  EXE [--runs N] [--out FILE]
  python tools/trials/windows-11-x64/winui-trial.py console EXE [--out FILE]
  python tools/trials/windows-11-x64/winui-trial.py resolution FOLDER [--out FILE]
  python tools/trials/windows-11-x64/winui-trial.py rollforward FOLDER [--major N] [--root DIR] [--out FILE]

series       N runs (default 10) of `EXE --probe` from a scratch copy of the published folder, one
             process after the other: prints the probe line of each run, then the statistics of
             display_ms and peak_rss_kb, and checks after each run that no process is left.
console      the AttachConsole path: the UI is started with no redirection from a process that owns a
             console, and the probe line is read back from the console screen buffer.
resolution   which Microsoft.NETCore.App the folder loads on this machine, as published and with
             DOTNET_ROLL_FORWARD=LatestMajor (host trace).
rollforward  [--major 10|11] runs the published folder of a .NET 8 build from a PRIVATE .NET root
             that holds only one .NET runtime (10 by default; the files are copied from the installed
             one and given through DOTNET_ROOT of the child process only, no global setting is
             touched):
             1. as it is, 2. with DOTNET_ROLL_FORWARD=LatestMajor. Starts the apphost
             Loomora.WinUI.exe (the muxer `dotnet app.dll` cannot be used: MRT looks for the resource
             file by the name of the process, so XAML loading fails under dotnet.exe). A framework
             error makes the apphost of a WinExe show a message box: its text is read and the
             process is killed. Reports the exit code, the dialog and the probe line.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def alive(name):
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name, "/NH"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT).stdout.decode("oem", "replace")
    return out.count(name)


def series(args):
    exe = Path(args.exe).resolve()
    lines = []
    scratch = tempfile.mkdtemp(prefix="loomora-winui-series-")
    try:
        folder = os.path.join(scratch, "app")
        shutil.copytree(str(exe.parent), folder)
        for n in range(args.runs):
            probe_file = os.path.join(folder, "loomora-probe.json")
            if os.path.exists(probe_file):
                os.remove(probe_file)
            started = time.time()
            res = subprocess.run([os.path.join(folder, exe.name), "--probe"], cwd=folder,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            line = res.stdout.decode("utf-8").strip()
            lines.append((line, res.returncode, round(time.time() - started, 2), alive(exe.name)))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    out = []
    for line, code, wall, left in lines:
        out.append(line)
    records = [json.loads(l[0]) for l in lines]
    summary = {
        "runs": len(lines),
        "exit_codes": sorted(set(l[1] for l in lines)),
        "leftover_processes_after_each_run": sorted(set(l[3] for l in lines)),
        "wall_seconds": [l[2] for l in lines],
        "display_ms": [r["display_ms"] for r in records],
        "display_ms_median": statistics.median(r["display_ms"] for r in records),
        "peak_rss_kb": [r["peak_rss_kb"] for r in records],
        "greeting_matches_all": all(r["greeting_matches"] for r in records),
    }
    text = "\n".join(out) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8", newline="\n")
    print(text, end="")
    print(json.dumps(summary, indent=1))


def private_root(root, major):
    """A .NET root with the muxer, the host resolver and the .NET <major> runtime only."""
    installed = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "dotnet"
    runtimes = sorted((installed / "shared" / "Microsoft.NETCore.App").iterdir(),
                      key=lambda p: [int(x) for x in p.name.split(".")])
    ten = [p for p in runtimes if p.name.startswith("%d." % major)][-1]
    fxr = sorted((installed / "host" / "fxr").iterdir(),
                 key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)[:3]])[-1]
    if root.exists():
        shutil.rmtree(str(root))
    (root / "host" / "fxr").mkdir(parents=True)
    (root / "shared" / "Microsoft.NETCore.App").mkdir(parents=True)
    shutil.copy2(str(installed / "dotnet.exe"), str(root / "dotnet.exe"))
    shutil.copytree(str(fxr), str(root / "host" / "fxr" / fxr.name))
    shutil.copytree(str(ten), str(root / "shared" / "Microsoft.NETCore.App" / ten.name))
    return ten.name, fxr.name


WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def dialog_texts(pid):
    """Texts (title and static controls) of the visible top-level windows of a process: the message
    box that the apphost of a WinExe shows when it cannot find the framework."""
    user32 = ctypes.WinDLL("user32")
    found = []

    def text_of(hwnd):
        buf = ctypes.create_unicode_buffer(2048)
        user32.GetWindowTextW(hwnd, buf, 2048)
        return buf.value

    def on_child(hwnd, _):
        t = text_of(hwnd)
        if t:
            found.append(t)
        return True

    def on_top(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append("[window] " + text_of(hwnd))
            user32.EnumChildWindows(hwnd, WNDENUMPROC(on_child), 0)
        return True

    user32.EnumWindows(WNDENUMPROC(on_top), 0)
    return found


def run_apphost(exe, cwd, env, wait=25):
    """Starts the apphost; returns (exit code or None, standard output + error, dialog texts).
    A process still alive after `wait` seconds with a dialog is killed (the dialog is the error)."""
    proc = subprocess.Popen([exe, "--probe"], cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    started = time.time()
    dialogs = []
    while proc.poll() is None and time.time() - started < wait:
        time.sleep(0.5)
        dialogs = [d for d in dialog_texts(proc.pid) if "Loomora" not in d or "framework" in d.lower()]
        if any("framework" in d.lower() or ".NET" in d for d in dialogs):
            break
    code = proc.poll()
    if code is None:
        proc.kill()
    out = proc.communicate()[0].decode("utf-8", "replace").strip()
    return code, out, dialogs


def rollforward(args):
    folder = Path(args.folder).resolve()
    root = Path(args.root).resolve()
    runtime, fxr = private_root(root, args.major)
    scratch = tempfile.mkdtemp(prefix="loomora-winui-roll-")
    report = []
    try:
        app = os.path.join(scratch, "app")
        shutil.copytree(str(folder), app)
        cfg = json.load(open(os.path.join(app, "Loomora.WinUI.runtimeconfig.json")))
        report.append("published folder: %s; runtimeconfig framework %s, rollForward %s"
                      % (folder.name, cfg["runtimeOptions"]["framework"],
                         cfg["runtimeOptions"].get("rollForward", "(not set: Minor, the default)")))
        report.append("private .NET root %s holds only: Microsoft.NETCore.App %s, hostfxr %s"
                      % (root, runtime, fxr))
        config_path = os.path.join(app, "Loomora.WinUI.runtimeconfig.json")
        for label, extra, strip in (
                ("1. as published (runtimeconfig as it is)", {}, False),
                ("2. runtimeconfig without rollForward (the default policy)", {}, True),
                ("3. runtimeconfig without rollForward + DOTNET_ROLL_FORWARD=LatestMajor",
                 {"DOTNET_ROLL_FORWARD": "LatestMajor"}, True)):
            config = json.load(open(os.path.join(str(folder), "Loomora.WinUI.runtimeconfig.json")))
            if strip:
                config["runtimeOptions"].pop("rollForward", None)
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(config, f, indent=2)
            env = dict(os.environ, DOTNET_ROOT=str(root), DOTNET_ROOT_X64=str(root),
                       DOTNET_MULTILEVEL_LOOKUP="0")
            env.pop("DOTNET_ROLL_FORWARD", None)
            env.update(extra)
            probe_file = os.path.join(app, "loomora-probe.json")
            if os.path.exists(probe_file):
                os.remove(probe_file)
            code, text, dialogs = run_apphost(os.path.join(app, "Loomora.WinUI.exe"), app, env)
            report.append("--- %s: exit code %s" % (label, "none (killed, process still alive with a dialog)"
                                                    if code is None else "%d (0x%08X)" % (code, code & 0xFFFFFFFF)))
            if dialogs:
                report.append("dialog: " + " | ".join(dialogs))
            report.append(text if text else "(no standard output)")
            if os.path.exists(probe_file):
                report.append("loomora-probe.json: " + open(probe_file, encoding="utf-8").read().strip())
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    out = "\n".join(report) + "\n"
    if args.out:
        Path(args.out).write_text(out, encoding="utf-8", newline="\n")
    print(out, end="")


def resolution(args):
    """Which Microsoft.NETCore.App the published folder loads on THIS machine (all installed .NET
    runtimes visible), as published and with DOTNET_ROLL_FORWARD=LatestMajor, from the host trace
    (COREHOST_TRACE)."""
    folder = Path(args.folder).resolve()
    report = []
    installed = subprocess.run(["dotnet", "--list-runtimes"], stdout=subprocess.PIPE).stdout.decode("utf-8", "replace")
    report.append("installed runtimes (dotnet --list-runtimes):\n" + installed.strip())
    scratch = tempfile.mkdtemp(prefix="loomora-winui-res-")
    try:
        app = os.path.join(scratch, "app")
        shutil.copytree(str(folder), app)
        cfg = json.load(open(os.path.join(app, "Loomora.WinUI.runtimeconfig.json")))["runtimeOptions"]
        report.append("runtimeconfig of %s: framework %s, rollForward %s"
                      % (folder.name, cfg["framework"], cfg.get("rollForward", "(not set)")))
        for label, extra in (("as published", {}), ("DOTNET_ROLL_FORWARD=LatestMajor", {"DOTNET_ROLL_FORWARD": "LatestMajor"})):
            env = dict(os.environ, COREHOST_TRACE="1", COREHOST_TRACE_VERBOSITY="3")
            env.pop("DOTNET_ROLL_FORWARD", None)
            env.update(extra)
            res = subprocess.run([os.path.join(app, "Loomora.WinUI.exe"), "--probe"], cwd=app, env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            text = res.stdout.decode("utf-8", "replace")
            loaded = re.findall(r"Loaded library from .*Microsoft\.NETCore\.App\\([0-9.]+)\\hostpolicy\.dll", text)
            report.append("%s: exit code %d; runtime loaded: %s" % (
                label, res.returncode, ", ".join(sorted(set(loaded))) or "(not found in the trace)"))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    out = "\n".join(report) + "\n"
    if args.out:
        Path(args.out).write_text(out, encoding="utf-8", newline="\n")
    print(out, end="")


def console(args):
    """The AttachConsole path: this process gets its own console (AllocConsole) and starts the UI
    with NO redirection, as a command interpreter does; a GUI program then has no standard output
    handle, attaches to the parent console and writes the probe line to CONOUT$. The line is read
    back from the screen buffer of the console."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.FreeConsole()
    if not k32.AllocConsole():
        raise SystemExit("AllocConsole failed")
    exe = Path(args.exe).resolve()
    scratch = tempfile.mkdtemp(prefix="loomora-winui-console-")
    try:
        app = os.path.join(scratch, "app")
        shutil.copytree(str(exe.parent), app)
        proc = subprocess.Popen([os.path.join(app, exe.name), "--probe"], cwd=app,
                                stdin=None, stdout=None, stderr=None)
        code = proc.wait(timeout=60)

        class COORD(ctypes.Structure):
            _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

        class SMALL_RECT(ctypes.Structure):
            _fields_ = [("L", ctypes.c_short), ("T", ctypes.c_short), ("R", ctypes.c_short), ("B", ctypes.c_short)]

        class INFO(ctypes.Structure):
            _fields_ = [("size", COORD), ("cursor", COORD), ("attr", wt.WORD), ("window", SMALL_RECT), ("max", COORD)]

        handle = k32.CreateFileW("CONOUT$", 0xC0000000, 3, None, 3, 0, None)
        info = INFO()
        k32.GetConsoleScreenBufferInfo(ctypes.c_void_p(handle), ctypes.byref(info))
        lines = []
        for y in range(info.cursor.Y + 1):
            buf = ctypes.create_unicode_buffer(info.size.X + 1)
            read = wt.DWORD()
            k32.ReadConsoleOutputCharacterW(ctypes.c_void_p(handle), buf, info.size.X, COORD(0, y), ctypes.byref(read))
            lines.append(buf.value.rstrip())
        probe_file = os.path.join(app, "loomora-probe.json")
        file_line = open(probe_file, encoding="utf-8").read().strip() if os.path.exists(probe_file) else ""
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    text = "".join(l for l in lines)
    report = ["exit code %d" % code, "console buffer (%d rows, rows joined: the line is longer than the console width):" % len(lines),
              "\n".join(l for l in lines if l), "",
              "probe line found in the console: %s" % (file_line != "" and file_line.replace(" ", "") in text.replace(" ", "")),
              "loomora-probe.json: " + file_line]
    out = "\n".join(report) + "\n"
    if args.out:
        Path(args.out).write_text(out, encoding="utf-8", newline="\n")
    print(out, end="")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("series")
    s.add_argument("exe")
    s.add_argument("--runs", type=int, default=10)
    s.add_argument("--out")
    c = sub.add_parser("console")
    c.add_argument("exe")
    c.add_argument("--out")
    q = sub.add_parser("resolution")
    q.add_argument("folder")
    q.add_argument("--out")
    r = sub.add_parser("rollforward")
    r.add_argument("folder")
    r.add_argument("--root", default=str(Path(__file__).resolve().parents[3] / "build" / "dotnet10-only"))
    r.add_argument("--major", type=int, default=10, help="the one .NET major kept in the private root")
    r.add_argument("--out")
    args = ap.parse_args()
    {"series": series, "rollforward": rollforward, "resolution": resolution, "console": console}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
