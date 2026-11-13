#!/usr/bin/env python
"""Function-level import tables of the Windows Legacy executables before and after docs/adr/0004.

  python validation/evidence/windows-11-x64/adr-0004/compare-imports.py BEFORE_DIR

BEFORE_DIR holds x86/loomora.exe and x64/loomora.exe built from the commit before the ADR (a clean
build of 3867bfc); the executables of this tree are out/windows-legacy/{x86,x64}/loomora.exe. The
import table of each is read with the MinGW-w64 objdump in WSL (tools/check_imports.py parses it),
the two sets of dll!function are compared, and tools/check_imports.py is run on every Windows
executable of the build (the UI and the two self-tests) against the allowlist of its architecture.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools"))
import check_imports  # noqa: E402
import common  # noqa: E402

TOOLS = {"x86": "i686-w64-mingw32-", "x64": "x86_64-w64-mingw32-"}


def wsl_path(path):
    path = Path(path).resolve()
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]


def objdump(arch, exe):
    res = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", TOOLS[arch] + "objdump", "-p",
                          wsl_path(exe)], stdout=subprocess.PIPE)
    return res.stdout.decode("utf-8", "replace")


def main():
    common.setup_utf8_output()
    before = Path(sys.argv[1])
    for arch in ("x86", "x64"):
        old = check_imports.parse_objdump(objdump(arch, before / arch / "loomora.exe"))[1]
        new_exe = ROOT / "out" / "windows-legacy" / arch / "loomora.exe"
        new = check_imports.parse_objdump(objdump(arch, new_exe))[1]
        old_set = {(d, f) for d, fs in old.items() for f in fs}
        new_set = {(d, f) for d, fs in new.items() for f in fs}
        print("%s loomora.exe: %d imports before, %d after (sha256 before %s, after %s); "
              "added %s, removed %s" % (arch, len(old_set), len(new_set),
                                        common.sha256(before / arch / "loomora.exe"),
                                        common.sha256(new_exe), sorted(new_set - old_set),
                                        sorted(old_set - new_set)))
    for arch, rel in (("x86", "x86/loomora.exe"), ("x64", "x64/loomora.exe"),
                      ("x86", "guest/loomora_selftest.exe"),
                      ("x64", "guest/loomora_selftest-x64.exe")):
        exe = ROOT / "out" / "windows-legacy" / rel
        res = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--cd", str(ROOT), "--", "python3",
                              "tools/check_imports.py", "--arch", arch, "--objdump",
                              TOOLS[arch] + "objdump", "out/windows-legacy/" + rel],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        first = res.stdout.decode("utf-8", "replace").strip().splitlines()[0]
        print("check_imports %s %s (%d bytes, sha256 %s): exit %d, %s"
              % (arch, exe.relative_to(ROOT).as_posix(), exe.stat().st_size, common.sha256(exe),
                 res.returncode, first))


if __name__ == "__main__":
    main()
