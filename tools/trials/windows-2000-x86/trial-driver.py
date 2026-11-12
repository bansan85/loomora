#!/usr/bin/env python
"""Driver of the Windows 2000 trial of the Windows Legacy UI (spike S1 / T066, T088, T094).

Run from the repository root, with the VM already running and logged on (the driver never
starts, stops, snapshots or restores it and changes no VM setting):

  python validation/evidence/windows-2000-x86/trial-driver.py [step ...]

Steps (default: all, in this order):
  facts    guest-facts.vbs in the guest -> guest-facts-output.txt, vm-environment.json
  exports  copies the system DLLs of the guest to a scratch folder OUTSIDE the repository (Microsoft
           binaries are never committed), reads their export tables and checks the allowlist of
           tools/win2000_imports.json, the import table of loomora.exe and the functions it loads
           at run time -> dll-exports-check.txt, dll-exports-check.json
  runs     10 x '--probe' and 10 x '--probe --probe-input Marie' -> probe-*-run1.json,
           probe-*-runs.jsonl, trial-measurements.json
  memory   cross-checks peak_rss_kb: memtest.exe (psapi API on a known 8 MB allocation) and a WMI
           poll of a running loomora.exe -> memtest-output.txt, wmi-memory-poll.txt
  glyphs   a probe-mode window with a non-ASCII input, photographed -> window-6-nonascii-probe.png
  gui      the window without --probe: screenshots of the guest screen (VBoxManage controlvm
           screenshotpng), keys from the virtual keyboard (keyboardputstring and keyboardputscancode),
           Alt+F4, process check -> window-*.png, gui-run.json
  selftest loomora_selftest and two control programs started by a bat in the guest -> selftest-*.json/txt
  cleanup  deletes the guest folder this trial created

Settings are the environment variables of tools/run_vectors.py (LOOMORA_VM_NAME, LOOMORA_VM_USER,
LOOMORA_VM_PASSWORD, LOOMORA_VM_DIR, LOOMORA_VBOXMANAGE). Needs the x86 executable
out/windows-legacy/x86/loomora.exe (python tools/build.py windows legacy, in WSL).
"""
import argparse
import io
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))

import check_imports  # noqa: E402
import check_probe  # noqa: E402
import common  # noqa: E402
import run_vectors  # noqa: E402

EVIDENCE = Path(__file__).resolve().parent
PLATFORM_ID = "windows-2000-x86"
RUNS = 10
SYSTEM_DLLS = ["kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ntdll.dll",
               "psapi.dll", "msvcrt.dll", "dwmapi.dll"]
# Functions the executable looks up at run time (dynfeatures.cpp, sysinfo.cpp, main.cpp) and the
# feature that is skipped (disabled_features) or the fallback taken when it is absent.
DYNAMIC_TARGETS = [
    ("user32.dll", "SetProcessDPIAware", "absent -> feature 'dpi-awareness' disabled"),
    ("user32.dll", "SystemParametersInfoW", "absent -> feature 'high-contrast-query' disabled"),
    ("advapi32.dll", "RegOpenKeyExW", "used to read AppsUseLightTheme"),
    ("advapi32.dll", "RegQueryValueExW", "used to read AppsUseLightTheme"),
    ("advapi32.dll", "RegCloseKey", "used to read AppsUseLightTheme"),
    ("psapi.dll", "GetProcessMemoryInfo", "absent -> feature 'peak-memory' disabled, peak_rss_kb -1"),
    ("ntdll.dll", "RtlGetVersion", "absent -> GetVersionExW"),
    ("kernel32.dll", "GetVersionExW", "fallback of RtlGetVersion"),
    ("kernel32.dll", "GetNativeSystemInfo", "absent -> GetSystemInfo (no WOW64 on Windows 2000)"),
    ("dwmapi.dll", "DwmSetWindowAttribute", "dark title bar, only when the dark theme is on"),
]

# US scancodes (set 1) of the physical keys; the guest keyboard layout decides what they type.
US_SCANCODES = {"q": 0x10, "w": 0x11, "e": 0x12, "r": 0x13, "t": 0x14, "y": 0x15, "u": 0x16,
                "i": 0x17, "o": 0x18, "p": 0x19, "a": 0x1E, "s": 0x1F, "d": 0x20, "f": 0x21,
                "g": 0x22, "h": 0x23, "j": 0x24, "k": 0x25, "l": 0x26, ";": 0x27, "z": 0x2C,
                "x": 0x2D, "c": 0x2E, "v": 0x2F, "b": 0x30, "n": 0x31, "m": 0x32}
AZERTY_TO_PHYSICAL = {"a": "q", "q": "a", "z": "w", "w": "z", "m": ";"}  # letters that move
SHIFT = 0x2A
HOME, END = (0xE0, 0x47), (0xE0, 0x4F)  # extended keys
ALT, F4 = 0x38, 0x3E


def say(text=""):
    print(text, flush=True)


def write_text(name, text):
    text = text.replace("\r\n", "\n")
    with open(str(EVIDENCE / name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def key(code):
    """Press and release of one scancode (an extended key is a tuple)."""
    codes = list(code) if isinstance(code, tuple) else [code]
    release = codes[:-1] + [codes[-1] | 0x80]
    return codes + release


def typing_scancodes(text, azerty):
    """Scancodes that type `text` (letters) from a US-scancode keyboard on a guest whose layout is
    French AZERTY (azerty=True: a, q, z, w and m sit on other physical keys) or US."""
    out = []
    for ch in text:
        low = ch.lower()
        physical = AZERTY_TO_PHYSICAL.get(low, low) if azerty else low
        code = US_SCANCODES[physical]
        if ch != low:
            out += [SHIFT] + key(code) + [SHIFT | 0x80]
        else:
            out += key(code)
    return out


class Trial(object):
    def __init__(self):
        args = argparse.Namespace(platform_id=PLATFORM_ID, arch=None, wsl_distro=None)
        self.launcher = run_vectors.WindowsVirtualBoxLauncher(args)
        self.guest = self.launcher.guest
        self.vm = self.launcher.vm
        self.exe = self.launcher.exe
        self.scratch_dlls = None

    # -- helpers ---------------------------------------------------------------------------
    def prepare(self):
        say("launcher: " + self.launcher.prepare())

    def vm_info(self):
        info = self.guest.state()
        keep = ("name", "ostype", "memory", "vram", "cpus", "chipset", "firmware", "hpet",
                "storagecontrollertype0", "nic1", "VMState", "VMStateChangeTime",
                "GuestAdditionsVersion", "GuestAdditionsRunLevel", "CurrentSnapshotName")
        return {k: info.get(k) for k in keep}

    def run_script(self, name, text_lf):
        """Runs a VBScript from the evidence folder in the guest and returns its output."""
        data = text_lf.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii")
        self.guest.copyto(self.launcher._host_file(name, data), self.launcher.guest_path(name))
        bat = "%s.bat" % name
        body = "@echo off\r\ncd /d \"%s\"\r\ncscript //nologo \"%s\"\r\n" % (
            self.launcher.guest_dir, self.launcher.guest_path(name))
        self.guest.copyto(self.launcher._host_file(bat, body.encode("ascii")),
                          self.launcher.guest_path(bat))
        _rc, out, err = self.guest.run_program(self.launcher.guest_path(bat), 120)
        return out + (("\n[stderr]\n" + err) if err.strip() else "")

    def procs(self):
        out = self.run_script("guest-procs.vbs", (EVIDENCE / "guest-procs.vbs").read_text("ascii"))
        count = None
        for line in out.splitlines():
            if line.startswith("PROCESSES "):
                try:
                    count = int(line.split()[1])
                except ValueError:
                    count = None
        return count, out.strip()

    def vbm(self, *args):
        rc, out, err = self.guest.vbm(list(args), 120)
        if rc != 0:
            raise run_vectors.VBoxError("VBoxManage %s failed: %s" % (args[0], err.strip()[:300]))
        return out

    def screenshot(self, name):
        path = str(EVIDENCE / name)
        self.vbm("controlvm", self.vm, "screenshotpng", path)
        return {"path": "validation/evidence/windows-2000-x86/" + name,
                "sha256": common.sha256(path), "bytes": os.path.getsize(path)}

    def scancodes(self, codes):
        self.vbm("controlvm", self.vm, "keyboardputscancode", *["%02x" % c for c in codes])

    # -- steps -----------------------------------------------------------------------------
    def step_facts(self):
        out = self.run_script("guest-facts.vbs", (EVIDENCE / "guest-facts.vbs").read_text("ascii"))
        write_text("guest-facts-output.txt", out)
        info = self.vm_info()
        info["vboxmanage_version"] = self.guest.vbm(["--version"], 30)[1].strip()
        info["host"] = platform.platform()
        for name in ("Product", "Release", "ServicePack", "Version"):
            info["guest_property_OS_" + name] = self.guest.guest_property(
                "/VirtualBox/GuestInfo/OS/" + name)
        common.write_json(EVIDENCE / "vm-environment.json", info)
        say("facts: %d lines; VM %s %s MB, state %s" % (len(out.splitlines()), info["name"],
                                                        info["memory"], info["VMState"]))

    def step_exports(self):
        # A plain folder, not tempfile.mkdtemp (its owner-only ACL makes the files VBoxManage
        # copies into it unreadable); the launcher scratch is removed at the end.
        self.scratch_dlls = os.path.join(self.launcher.scratch, "system32")
        os.makedirs(self.scratch_dlls)
        try:
            for dll in SYSTEM_DLLS:
                self.guest.copyfrom("C:\\WINNT\\system32\\" + dll,
                                    os.path.join(self.scratch_dlls, dll))
            present = {d: os.path.isfile(os.path.join(self.scratch_dlls, d)) for d in SYSTEM_DLLS}
            log = io.StringIO()

            def both(text=""):
                print(text)
                log.write(text + "\n")

            both("Windows 2000 build 2195 (no service pack), VirtualBox VM: export tables read from "
                 "C:\\WINNT\\system32 (copies kept outside the repository)")
            both("DLLs found: " + ", ".join("%s=%s" % (d, "yes" if p else "NO") for d, p in
                                            sorted(present.items())))
            both()
            both("== 1. every function of the x86 allowlist (tools/win2000_imports.json) ==")
            buf = io.StringIO()
            allowlist_ok = check_imports.verify_dlls(
                self.scratch_dlls, "x86", "Windows 2000 build 2195 in a VirtualBox VM", out=buf)
            both(buf.getvalue().rstrip())
            both("result: %s" % ("every allowlist function is exported" if allowlist_ok
                                 else "SOME ALLOWLIST FUNCTIONS ARE NOT EXPORTED"))
            both()

            both("== 2. every import of out/windows-legacy/x86/loomora.exe against the DLLs ==")
            wsl_exe = self.wsl_path(self.exe)
            dump = subprocess.run(
                ["wsl", "-d", run_vectors.DEFAULT_WSL_DISTRO, "--", "i686-w64-mingw32-objdump",
                 "-p", wsl_exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True)
            if dump.returncode != 0:
                raise RuntimeError("objdump failed: " + dump.stderr)
            _arch, imports, odd = check_imports.parse_objdump(dump.stdout)
            buf = io.StringIO()
            missing = check_imports.check_against_dlls(imports, self.scratch_dlls, buf)
            both(buf.getvalue().rstrip())
            total = sum(len(v) for v in imports.values())
            both("result: %d imports from %d DLLs, %d not exported, %d unparsed"
                 % (total, len(imports), missing, len(odd)))
            both()

            both("== 3. functions looked up at run time (LoadLibrary / GetProcAddress) ==")
            dynamic = []
            for dll, function, consequence in DYNAMIC_TARGETS:
                exported = None
                if present.get(dll):
                    names, _ = check_imports.read_exports(Path(self.scratch_dlls) / dll)
                    exported = function in names
                dynamic.append({"dll": dll, "function": function, "dll_present": present.get(dll),
                                "exported": exported, "when_absent": consequence})
                both("%-14s %-24s dll %-7s exported %-7s %s"
                     % (dll, function, "present" if present.get(dll) else "ABSENT",
                        {True: "yes", False: "NO", None: "n/a"}[exported], consequence))
            write_text("dll-exports-check.txt", log.getvalue())

            dlls = {}
            for dll in SYSTEM_DLLS:
                path = os.path.join(self.scratch_dlls, dll)
                if os.path.isfile(path):
                    names, by_ordinal = check_imports.read_exports(Path(path))
                    dlls[dll] = {"bytes": os.path.getsize(path), "sha256": common.sha256(path),
                                 "named_exports": len(names), "exports_by_ordinal_only": by_ordinal}
            data = check_imports.load_allowlist()
            _floor, allowed = check_imports.allowed_for(data, "x86")
            common.write_json(EVIDENCE / "dll-exports-check.json", {
                "system": "Windows 2000 build 2195, no service pack, VirtualBox VM",
                "dlls": dlls,
                "allowlist_x86_functions": {d: sorted(f) for d, f in sorted(allowed.items())},
                "allowlist_all_exported": allowlist_ok,
                "exe_imports": {d: sorted(f) for d, f in sorted(imports.items())},
                "exe_imports_not_exported": missing,
                "dynamic_targets": dynamic,
            })
        finally:
            shutil.rmtree(self.scratch_dlls, ignore_errors=True)

    @staticmethod
    def wsl_path(path):
        p = str(path).replace("\\", "/")
        return "/mnt/" + p[0].lower() + p[2:]

    def step_runs(self):
        self.vm_state_before = self.vm_info()
        results = {}
        series = (("default", ["--probe"], None), ("marie", ["--probe", "--probe-input", "Marie"],
                                                  "Marie"))
        for label, exe_args, expected in series:
            rows = []
            for i in range(1, RUNS + 1):
                result = self.launcher.run_exe(exe_args, 60)
                problems = []
                record = {}
                if result.error:
                    problems.append("launch: " + result.error)
                else:
                    line, problem = check_probe.find_probe_line(result.stdout)
                    if problem:
                        problems.append(problem)
                    else:
                        problems += check_probe.check_probe_line(line, expected)
                        record = json.loads(line)
                        record["_line"] = line
                    if result.returncode != 0:
                        problems.append("exit status %s" % result.returncode)
                    if result.leftover:
                        problems.append("%d process(es) left" % result.leftover)
                rows.append({"run": i, "exit_status": result.returncode,
                             "guest_wall_ms": result.guest_wall_ms,
                             "host_elapsed_ms": int(result.elapsed * 1000),
                             "leftover_loomora_exe": result.leftover, "problems": problems,
                             "display_ms": record.get("display_ms"),
                             "peak_rss_kb": record.get("peak_rss_kb"),
                             "message": record.get("message"),
                             "platform_id": record.get("platform_id"),
                             "greeting_matches": record.get("greeting_matches"),
                             "eol_warning_shown": record.get("eol_warning_shown"),
                             "disabled_features": record.get("disabled_features"),
                             "probe_line": record.get("_line")})
                say("%s run %2d: exit %s display_ms %s peak_rss_kb %s wall %s ms left %s %s" % (
                    label, i, result.returncode, record.get("display_ms"),
                    record.get("peak_rss_kb"), result.guest_wall_ms, result.leftover,
                    "OK" if not problems else "PROBLEMS " + "; ".join(problems)))
            results[label] = rows
            lines = [r["probe_line"] for r in rows if r["probe_line"]]
            write_text("probe-%s-runs.jsonl" % label, "".join(l + "\n" for l in lines))
            if lines:
                write_text("probe-%s-run1.json" % label, lines[0] + "\n")

        def stats(values):
            values = [v for v in values if v is not None]
            if not values:
                return None
            return {"n": len(values), "min": min(values), "median": statistics.median(values),
                    "max": max(values)}

        summary = {"series": {}, "executable": {
            "path": "out/windows-legacy/x86/loomora.exe", "bytes": self.exe.stat().st_size,
            "sha256": common.sha256(self.exe)}}
        for label, rows in results.items():
            summary["series"][label] = {
                "runs": len(rows),
                "all_ok": all(not r["problems"] for r in rows),
                "exit_statuses": sorted(set(r["exit_status"] for r in rows if r["exit_status"] is not None)),
                "display_ms": stats([r["display_ms"] for r in rows]),
                "peak_rss_kb": stats([r["peak_rss_kb"] for r in rows]),
                "guest_wall_ms": stats([r["guest_wall_ms"] for r in rows]),
                "leftover_loomora_exe": sorted(set(r["leftover_loomora_exe"] for r in rows)),
                "messages": sorted(set(r["message"] for r in rows)),
                "platform_ids": sorted(set(r["platform_id"] for r in rows)),
                "eol_warning_shown": sorted(set(r["eol_warning_shown"] for r in rows)),
                "disabled_features": sorted(set(",".join(r["disabled_features"] or [])
                                                for r in rows))}
        summary["runs"] = {k: [{x: y for x, y in r.items() if x != "probe_line"} for r in v]
                           for k, v in results.items()}
        summary["vm_before"] = self.vm_state_before
        summary["vm_after"] = self.vm_info()
        summary["note"] = ("guest_wall_ms is measured inside the guest by runw.exe with "
                           "GetTickCount (10 to 16 ms resolution) from CreateProcessW to the exit, "
                           "and includes the 2 s of probe mode; host_elapsed_ms also includes the "
                           "VBoxManage calls and is not a measurement of the program")
        common.write_json(EVIDENCE / "trial-measurements.json", summary)
        say("runs: %s" % json.dumps({k: {x: v[x] for x in ("all_ok", "display_ms", "peak_rss_kb")}
                                     for k, v in summary["series"].items()}))

    def step_memory(self):
        memtest = ROOT / "out" / "windows-legacy" / "guest" / "memtest.exe"
        out_parts = []
        if memtest.is_file():
            self.launcher._sync(memtest, "memtest.exe")
            bat = "@echo off\r\ncd /d \"%s\"\r\nmemtest.exe\r\n" % self.launcher.guest_dir
            self.guest.copyto(self.launcher._host_file("memtest.bat", bat.encode("ascii")),
                              self.launcher.guest_path("memtest.bat"))
            _rc, out, _err = self.guest.run_program(self.launcher.guest_path("memtest.bat"), 60)
            out_parts.append("memtest.exe (source memtest.c, sha256 %s): GetProcessMemoryInfo of a "
                             "process that touches 8 MB with VirtualAlloc\n%s"
                             % (common.sha256(memtest), out))
        else:
            out_parts.append("memtest.exe not built: skipped")
        write_text("memtest-output.txt", "".join(out_parts))
        pid = self.launcher.start_exe(["--probe"])
        out = self.run_script("guest-memory-poll.vbs",
                              (EVIDENCE / "guest-memory-poll.vbs").read_text("ascii"))
        probe_copy = os.path.join(self.launcher.scratch, "poll-probe.json")
        line = ""
        if self.guest.copyfrom(self.launcher.guest_path("loomora-probe.json"), probe_copy):
            with open(probe_copy, "rb") as f:
                line = f.read().decode("utf-8", "replace").strip()
        write_text("wmi-memory-poll.txt",
                   "loomora.exe --probe started with pid %d; WMI (Win32_Process) polled every "
                   "300 ms from another process (all counters in bytes):\n%s\nprobe line written by that run:\n%s\n" % (pid, out, line))
        say("memory: memtest and WMI poll written")

    def step_gui(self):
        log = {"steps": []}
        before, procs_out = self.procs()
        log["processes_before"] = {"count": before, "output": procs_out}
        say("processes before: %s" % before)
        pid = self.launcher.start_exe([])
        log["start"] = {"pid": pid, "command": "loomora.exe (no arguments, no --probe)"}
        time.sleep(4)
        count, out = self.procs()
        log["steps"].append({"step": "after start", "processes": count, "output": out,
                             "screenshot": self.screenshot("window-1-initial.png")})
        say("started pid %s, processes %s" % (pid, count))

        # The virtual keyboard sends US scancodes; the guest layout is read from guest-facts.
        layout = ""
        facts = (EVIDENCE / "guest-facts-output.txt")
        if facts.is_file():
            for line in facts.read_text("utf-8").splitlines():
                if line.startswith("reg.HKCU\\Keyboard Layout\\Preload\\1="):
                    layout = line.split("=", 1)[1].strip()
        log["guest_keyboard_layout_preload1"] = layout

        # 1. keyboardputstring (assumes a US layout): shows what a layout mismatch does.
        self.scancodes(key(HOME) + [SHIFT] + key(END) + [SHIFT | 0x80])
        time.sleep(1)
        log["steps"].append({"step": "Home then Shift+End (selection of the prefilled text)",
                             "screenshot": self.screenshot("window-2-selected.png")})
        self.vbm("controlvm", self.vm, "keyboardputstring", "Marie")
        time.sleep(1.5)
        log["steps"].append({"step": "VBoxManage controlvm keyboardputstring Marie",
                             "screenshot": self.screenshot("window-3-keyboardputstring.png")})

        # 2. scancodes of the physical keys that type "Marie" on the guest layout.
        codes = key(HOME) + [SHIFT] + key(END) + [SHIFT | 0x80]
        if layout.lower().endswith("40c"):  # French AZERTY
            codes += typing_scancodes("Marie", azerty=True)
            log["typed_with"] = "scancodes of the physical keys that type M a r i e on a French (AZERTY) layout"
        else:
            codes += typing_scancodes("Marie", azerty=False)
            log["typed_with"] = "scancodes of the US keys M a r i e"
        self.scancodes(codes)
        time.sleep(1.5)
        log["steps"].append({"step": "Home, Shift+End, then M a r i e as physical keys",
                             "screenshot": self.screenshot("window-4-typed-marie.png")})

        # 3. close with Alt+F4 and confirm the process is gone.
        self.scancodes([ALT, F4, F4 | 0x80, ALT | 0x80])
        time.sleep(3)
        count, out = self.procs()
        log["steps"].append({"step": "Alt+F4", "processes": count, "output": out,
                             "screenshot": self.screenshot("window-5-closed.png")})
        log["processes_after"] = count
        common.write_json(EVIDENCE / "gui-run.json", log)
        say("gui: processes after close: %s" % count)

    def step_glyphs(self):
        """What the fonts of this system draw for a non-ASCII input (the probe text check only
        compares strings): a probe-mode window is photographed while it is open (2 s)."""
        text = "Zoé Hélène çà 日本語 שלום \U0001F600"
        pid = self.launcher.start_exe(["--probe", "--probe-input", text])
        time.sleep(0.8)
        shot = self.screenshot("window-6-nonascii-probe.png")
        time.sleep(3)
        count, out = self.procs()
        common.write_json(EVIDENCE / "glyphs-run.json", {
            "input": text, "input_code_points": [hex(ord(c)) for c in text], "pid": pid,
            "screenshot": shot, "processes_after": count})
        say("glyphs: screenshot taken, processes after %s" % count)

    def step_selftest(self):
        """loomora_selftest (core/tests/selftest.cpp, built by tools/guest/build_selftest.sh) and two
        control programs (loader-control.sh): does the loader of this system start them? Run from
        a bat in the guest, whose errorlevel and output come back. The programs print one line
        when they run; a program the loader refuses prints nothing."""
        names = ["loomora_selftest.exe", "loomora_selftest-noshim.exe", "plain.exe", "vista.exe"]
        lines = []
        for name in names:
            host = ROOT / "out" / "windows-legacy" / "guest" / name
            if not host.is_file():
                raise run_vectors.VBoxError("%s missing: run tools/guest/build_selftest.sh and "
                                            "validation/evidence/windows-2000-x86/loader-control.sh"
                                            % name)
            self.launcher._sync(host, name)
            lines += ["echo --- %s" % name, name, "echo errorlevel=%errorlevel%"]
        bat = "@echo off\r\ncd /d \"%s\"\r\n%s\r\n" % (self.launcher.guest_dir, "\r\n".join(lines))
        path = self.launcher._host_file("selftest.bat", bat.encode("ascii"))
        self.guest.copyto(path, self.launcher.guest_path("selftest.bat"))
        _rc, out, err = self.guest.run_program(self.launcher.guest_path("selftest.bat"), 120)
        records = {n: {"sha256": common.sha256(ROOT / "out" / "windows-legacy" / "guest" / n)}
                   for n in names}
        current = None
        for line in out.splitlines():
            if line.startswith("--- "):
                current = line[4:].strip()
            elif current and line.startswith("errorlevel="):
                records[current]["errorlevel"] = int(line.split("=")[1])
            elif current and line.strip():
                records[current].setdefault("output", []).append(line.strip())
        write_text("selftest-output.txt", out + (("\n[stderr]\n" + err) if err.strip() else ""))
        common.write_json(EVIDENCE / "selftest-run.json", records)
        say("selftest: " + json.dumps({n: r.get("errorlevel") for n, r in records.items()}))

    def step_cleanup(self):
        parent = self.launcher.guest_dir.rsplit("\\", 1)[0]
        name = os.path.basename(self.launcher.guest_dir.replace("\\", "/"))
        bat_name = "loomora-trial-cleanup.bat"
        body = ("@echo off\r\ncd /d \"%s\"\r\nrmdir /s /q \"%s\"\r\ndel \"%s\\%s\"\r\n"
                % (parent, name, parent, bat_name))
        path = self.launcher._host_file(bat_name, body.encode("ascii"))
        self.guest.copyto(path, parent + "\\" + bat_name)
        self.guest.run_program(parent + "\\" + bat_name, 60)
        rc, out, err = self.guest.guestcontrol("stat", [self.launcher.guest_dir])
        gone = rc != 0
        rc2, _o, _e = self.guest.guestcontrol("stat", [parent + "\\" + bat_name])
        say("cleanup: folder %s %s; cleanup bat %s" % (
            self.launcher.guest_dir, "removed" if gone else "STILL THERE",
            "removed" if rc2 != 0 else "STILL THERE"))

    def close(self):
        self.launcher.close()


def main(argv=None):
    common.setup_utf8_output()
    parser = argparse.ArgumentParser(description="Windows 2000 trial driver")
    parser.add_argument("steps", nargs="*", default=[],
                        help="facts exports runs memory gui glyphs selftest cleanup (default: all but cleanup)")
    args = parser.parse_args(argv)
    steps = args.steps or ["facts", "exports", "runs", "memory", "gui", "glyphs", "selftest"]
    trial = Trial()
    try:
        trial.prepare()
        for step in steps:
            say("== %s ==" % step)
            getattr(trial, "step_" + step)()
    finally:
        trial.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
