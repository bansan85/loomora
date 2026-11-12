#!/usr/bin/env python
"""Second run of loomora_selftest on the Windows 2000 VM, after <iostream> was removed from
core/tests/selftest.cpp (spike S1 / T066; first attempt: selftest-output.txt, which did not load).

Run from the repository root, VM already running and logged on (it is never started, stopped,
snapshotted, restored or reconfigured), after building the programs in WSL:

  wsl -d Ubuntu-24.04 --cd <repo> -- sh tools/guest/build_selftest.sh
  python validation/evidence/windows-2000-x86/selftest-driver.py

What it does, with the launcher machinery of tools/run_vectors.py (WindowsVirtualBoxLauncher,
VBoxGuest) and the helpers of trial-driver.py:
  1. tools/check_imports.py (in WSL, with the MinGW-w64 objdump) on out/windows-legacy/guest/
     loomora_selftest.exe (must pass: 0 imports outside the Windows 2000 SP4 allowlist) and on
     loomora_selftest-noshim.exe (control: expected to fail on KERNEL32!GetThreadId)
     -> selftest-noiostream-imports.txt
  2. copies the system DLLs of the guest (C:\\WINNT\\system32) to a scratch folder OUTSIDE the
     repository (Microsoft binaries are never committed), checks that every allowlist function
     (--verify-dlls) and every import of both programs (--against) is exported by them
     -> selftest-noiostream-vm-exports.txt
  3. copies the two programs and a .bat into a NEW guest folder (<LOOMORA_VM_DIR>-selftest, which
     must not exist), runs the .bat (errorlevel and standard output come back through VBoxManage
     guestcontrol run) -> selftest-noiostream-vm-run.txt, selftest-noiostream-vm-run.json
  4. deletes that guest folder (and the host scratch folder); the guest is left as it was found.
Settings are those of tools/run_vectors.py (LOOMORA_VM_NAME, LOOMORA_VM_USER, LOOMORA_VM_PASSWORD,
LOOMORA_VM_DIR, LOOMORA_VBOXMANAGE).
"""
import importlib.util
import io
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))

import check_imports  # noqa: E402
import common  # noqa: E402
import run_vectors  # noqa: E402

EVIDENCE = Path(__file__).resolve().parent
GUEST_OUT = ROOT / "out" / "windows-legacy" / "guest"
PROGRAMS = ["loomora_selftest.exe", "loomora_selftest-noshim.exe"]
SYSTEM_DLLS = ["kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ntdll.dll",
               "psapi.dll", "msvcrt.dll"]
WSL_DISTRO = run_vectors.DEFAULT_WSL_DISTRO
OBJDUMP = "i686-w64-mingw32-objdump"


def load_trial_driver():
    spec = importlib.util.spec_from_file_location("trial_driver", str(EVIDENCE / "trial-driver.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def say(text=""):
    print(text, flush=True)


def write_text(name, text):
    with open(str(EVIDENCE / name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text.replace("\r\n", "\n"))


def wsl(*args):
    res = subprocess.run(["wsl", "-d", WSL_DISTRO, "--cd", str(ROOT), "--"] + list(args),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return res.returncode, res.stdout.decode("utf-8", "replace")


def main():
    common.setup_utf8_output()
    for name in PROGRAMS:
        if not (GUEST_OUT / name).is_file():
            sys.exit("%s missing: run tools/guest/build_selftest.sh in WSL first" % name)
    trial_module = load_trial_driver()
    trial = trial_module.Trial()
    launcher, guest = trial.launcher, trial.guest
    hashes = {n: common.sha256(GUEST_OUT / n) for n in PROGRAMS}
    sizes = {n: (GUEST_OUT / n).stat().st_size for n in PROGRAMS}
    try:
        info = guest.state()
        if info.get("VMState") != "running":
            sys.exit("VM %r is not running (state %s)" % (launcher.vm, info.get("VMState")))
        before = trial.vm_info()
        say("VM: %s state %s, last state change %s" % (before["name"], before["VMState"],
                                                       before["VMStateChangeTime"]))

        # -- 1. import check against the allowlist (WSL: the objdump of the toolchain) --------
        text = io.StringIO()
        text.write("tools/check_imports.py --arch x86 --list on the programs built by "
                   "tools/guest/build_selftest.sh\n(the allowlist is tools/win2000_imports.json, "
                   "Windows 2000 SP4)\n\n")
        status = {}
        for name in PROGRAMS:
            rc, out = wsl("python3", "tools/check_imports.py", "--arch", "x86", "--list",
                          "--objdump", OBJDUMP, "out/windows-legacy/guest/" + name)
            status[name] = rc
            text.write("== %s (%d bytes, sha256 %s)\n$ python3 tools/check_imports.py --arch x86 "
                       "--list --objdump %s out/windows-legacy/guest/%s\n%s\nexit status %d\n\n"
                       % (name, sizes[name], hashes[name], OBJDUMP, name, out.rstrip(), rc))
        write_text("selftest-noiostream-imports.txt", text.getvalue())
        say("allowlist check exit status: %s" % json.dumps(status))

        # -- 2. exports of the real system DLLs ------------------------------------------------
        scratch_dlls = os.path.join(launcher.scratch, "system32")
        os.makedirs(scratch_dlls)
        for dll in SYSTEM_DLLS:
            if not guest.copyfrom("C:\\WINNT\\system32\\" + dll, os.path.join(scratch_dlls, dll)):
                sys.exit("cannot copy %s from the guest" % dll)
        dll_hashes = {d: common.sha256(os.path.join(scratch_dlls, d)) for d in SYSTEM_DLLS}
        log = io.StringIO()
        log.write("Windows 2000 build 2195 (no service pack), VirtualBox VM: export tables of the "
                  "DLLs copied from C:\\WINNT\\system32 to a folder outside the repository "
                  "(Microsoft binaries are not committed).\nSystem DLL sha256 (compare with "
                  "dll-exports-check.json):\n")
        for dll in SYSTEM_DLLS:
            log.write("  %s %s\n" % (dll_hashes[dll], dll))
        buf = io.StringIO()
        allow_ok = check_imports.verify_dlls(scratch_dlls, "x86",
                                             "Windows 2000 build 2195 in a VirtualBox VM", out=buf)
        log.write("\n== every function of the x86 allowlist is exported by these DLLs "
                  "(check_imports --verify-dlls) ==\n%s\nresult: %s\n"
                  % (buf.getvalue().rstrip(), "every allowlist function is exported" if allow_ok
                     else "SOME ALLOWLIST FUNCTIONS ARE NOT EXPORTED"))
        missing_by_program = {}
        for name in PROGRAMS:
            rc, dump = wsl(OBJDUMP, "-p", "out/windows-legacy/guest/" + name)
            if rc != 0:
                sys.exit("objdump failed on %s: %s" % (name, dump))
            _arch, imports, odd = check_imports.parse_objdump(dump)
            buf = io.StringIO()
            missing = check_imports.check_against_dlls(imports, scratch_dlls, buf)
            missing_by_program[name] = missing
            total = sum(len(v) for v in imports.values())
            log.write("\n== every import of %s against these DLLs (check_imports --against) ==\n"
                      "%s\nresult: %d imports from %d DLLs, %d not exported, %d unparsed\n"
                      % (name, buf.getvalue().rstrip().replace(
                          scratch_dlls, "<folder of the copied DLLs>"),
                         total, len(imports), missing, len(odd)))
        write_text("selftest-noiostream-vm-exports.txt", log.getvalue())
        say("allowlist all exported: %s; imports not exported: %s" % (allow_ok, missing_by_program))

        # -- 3. run on the guest, in a new folder -------------------------------------------------
        guest_dir = launcher.guest_dir + "-selftest"
        rc, _out, _err = guest.guestcontrol("stat", [guest_dir])
        if rc == 0:
            sys.exit("%s already exists in the guest: not touching it" % guest_dir)
        launcher.guest_dir = guest_dir
        guest.mkdir(guest_dir)
        run_out = ""
        try:
            lines = []
            for name in PROGRAMS:
                guest.copyto(GUEST_OUT / name, launcher.guest_path(name))
                lines += ["echo --- %s" % name, name, "echo exitcode=%errorlevel%"]
            bat = "@echo off\r\ncd /d \"%s\"\r\n%s\r\n" % (guest_dir, "\r\n".join(lines))
            guest.copyto(launcher._host_file("selftest2.bat", bat.encode("ascii")),
                         launcher.guest_path("selftest2.bat"))
            _rc, out, err = guest.run_program(launcher.guest_path("selftest2.bat"), 120)
            run_out = out + (("\n[stderr]\n" + err) if err.strip() else "")
        finally:
            # -- 4. clean up the guest ---------------------------------------------------------
            parent = guest_dir.rsplit("\\", 1)[0]
            leaf = guest_dir.rsplit("\\", 1)[1]
            bat_name = "loomora-selftest-cleanup.bat"
            body = "@echo off\r\ncd /d \"%s\"\r\nrmdir /s /q \"%s\"\r\ndel \"%s\\%s\"\r\n" % (
                parent, leaf, parent, bat_name)
            guest.copyto(launcher._host_file(bat_name, body.encode("ascii")),
                         parent + "\\" + bat_name)
            guest.run_program(parent + "\\" + bat_name, 60)
            gone = guest.guestcontrol("stat", [guest_dir])[0] != 0
            bat_gone = guest.guestcontrol("stat", [parent + "\\" + bat_name])[0] != 0
            say("cleanup: guest folder %s, cleanup bat %s"
                % ("removed" if gone else "STILL THERE", "removed" if bat_gone else "STILL THERE"))

        records = {n: {"bytes": sizes[n], "sha256": hashes[n]} for n in PROGRAMS}
        current = None
        for line in run_out.splitlines():
            if line.startswith("--- "):
                current = line[4:].strip()
            elif current and line.startswith("exitcode="):
                records[current]["exit_code"] = int(line.split("=")[1])
            elif current and line.strip():
                records[current].setdefault("output", []).append(line.strip())
        after = trial.vm_info()
        write_text("selftest-noiostream-vm-run.txt", run_out)
        common.write_json(EVIDENCE / "selftest-noiostream-vm-run.json", {
            "system": "Windows 2000 build 2195, no service pack, VirtualBox VM (see vm-environment.json)",
            "guest_product": guest.guest_property("/VirtualBox/GuestInfo/OS/Product"),
            "guest_service_pack": guest.guest_property("/VirtualBox/GuestInfo/OS/ServicePack"),
            "method": "bat in a new guest folder; errorlevel and stdout come back through "
                      "VBoxManage guestcontrol run (launcher machinery of tools/run_vectors.py)",
            "programs": records,
            "system_dll_sha256": dll_hashes,
            "vm_state_before": {"VMState": before["VMState"],
                                "VMStateChangeTime": before["VMStateChangeTime"]},
            "vm_state_after": {"VMState": after["VMState"],
                               "VMStateChangeTime": after["VMStateChangeTime"]},
            "guest_folder_removed": gone, "cleanup_bat_removed": bat_gone,
        })
        say(json.dumps({n: r.get("exit_code") for n, r in records.items()}))
        say(run_out)
    finally:
        trial.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
