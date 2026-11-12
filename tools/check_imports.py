#!/usr/bin/env python
"""Function-level import check of a Windows executable against a versioned allowlist.

Usage:
  python tools/check_imports.py EXE [--arch x86|x64] [--objdump PATH] [--allowlist FILE] [--list]
  python tools/check_imports.py --self-test

Reads the import table with `objdump -p` (GNU binutils; the one of the MinGW-w64 toolchain that
built the executable, for example i686-w64-mingw32-objdump) and compares every imported function
with tools/win2000_imports.json: for each DLL, the functions known to exist on the oldest Windows
the executable is meant for (x86: Windows 2000 SP4; x64: Windows XP x64 / Server 2003 x64).
An import that is not on the list FAILS (exit status 1) and is named exactly as dll!function: the
Windows loader of an older system refuses an executable whose import table names a function its
DLL does not export, and the DLL-name check this replaces could not see that (spike S1, ADR 0002).

The architecture is read from the executable (override with --arch). Entries marked
"confidence": "verify" are accepted but listed: their minimum operating system has not been
confirmed in Microsoft documentation or on a real system.

tools/build.py runs this check on every Windows Legacy executable it builds.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import common

DEFAULT_ALLOWLIST = Path(__file__).resolve().parent / "win2000_imports.json"
ARCHS = ("x86", "x64")
CONFIDENCES = ("documented", "verify")
FORMATS = {"pei-i386": "x86", "pei-x86-64": "x64"}

RESOLVE_HELP = (
    "To resolve: look the function up in the Microsoft documentation (Requirements, 'Minimum "
    "supported client'). If the system the executable is meant for already has it, add it to "
    "%s with that value, a 'used_by' and a 'confidence' ('documented', or 'verify' when "
    "unsure). Otherwise it must not be imported: load it at run time with GetProcAddress, or "
    "satisfy the reference inside the executable as docs/implementation/adr/0002 does for GetThreadId, or change "
    "the library or toolchain that references it. Do not add it just to make the build pass."
)


class AllowlistError(ValueError):
    pass


def load_allowlist(path=DEFAULT_ALLOWLIST):
    """Reads and validates the allowlist file; raises AllowlistError on a malformed entry."""
    data = common.read_json(path)
    for key in ("architectures", "lists"):
        if not isinstance(data.get(key), dict):
            raise AllowlistError("%s: missing object '%s'" % (path, key))
    for arch, spec in data["architectures"].items():
        if arch not in ARCHS:
            raise AllowlistError("%s: unknown architecture '%s'" % (path, arch))
        if not spec.get("floor") or not isinstance(spec.get("lists"), list):
            raise AllowlistError("%s: architecture '%s' needs 'floor' and 'lists'" % (path, arch))
        for name in spec["lists"]:
            if name not in data["lists"]:
                raise AllowlistError("%s: architecture '%s' uses unknown list '%s'"
                                     % (path, arch, name))
    for name, dlls in data["lists"].items():
        for dll, functions in dlls.items():
            if dll != dll.lower() or not dll.endswith(".dll"):
                raise AllowlistError("%s: list '%s': DLL name '%s' must be lowercase *.dll"
                                     % (path, name, dll))
            for function, entry in functions.items():
                where = "%s: %s/%s!%s" % (path, name, dll, function)
                for field in ("min_os", "used_by", "confidence"):
                    if not isinstance(entry.get(field), str) or not entry[field]:
                        raise AllowlistError("%s: missing or empty '%s'" % (where, field))
                if entry["confidence"] not in CONFIDENCES:
                    raise AllowlistError("%s: confidence must be one of %s"
                                         % (where, ", ".join(CONFIDENCES)))
                if entry["confidence"] == "verify" and not entry.get("note"):
                    raise AllowlistError("%s: a 'verify' entry needs a 'note' saying what to "
                                         "verify" % where)
    return data


def allowed_for(data, arch):
    """Returns (floor, {dll: {function: entry}}) for one architecture."""
    if arch not in data["architectures"]:
        raise AllowlistError("no allowlist for architecture '%s'" % arch)
    spec = data["architectures"][arch]
    merged = {}
    for name in spec["lists"]:
        for dll, functions in data["lists"][name].items():
            merged.setdefault(dll, {}).update(functions)
    return spec["floor"], merged


def parse_objdump(text):
    """Parses the output of `objdump -p`; returns (arch or None, {dll: [functions]}, [odd lines]).

    A line of an import table that is not "<address> <hint> <name>" (an import by ordinal, for
    example) is returned in the third element: the caller must treat it as a failure.
    """
    arch = None
    imports = {}
    odd = []
    dll = None
    in_table = False
    for line in text.splitlines():
        m = re.search(r"file format (\S+)", line)
        if m and arch is None:
            arch = FORMATS.get(m.group(1))
        m = re.match(r"\s*DLL Name:\s*(\S+)", line)
        if m:
            dll = m.group(1).lower()
            imports.setdefault(dll, [])
            in_table = False
            continue
        if dll and re.match(r"\s*vma:\s+Hint/Ord", line):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.strip():
            in_table = False
            dll = None
            continue
        m = re.match(r"\s*[0-9a-fA-F]+\s+\d+\s+(\S+)\s*$", line)
        if m:
            imports[dll].append(m.group(1))
        else:
            odd.append("%s: %s" % (dll, line.strip()))
    return arch, imports, odd


def run_objdump(objdump, exe):
    res = subprocess.run([str(objdump), "-p", str(exe)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, universal_newlines=True)
    if res.returncode != 0:
        raise RuntimeError("%s -p %s failed (%d): %s" % (objdump, exe, res.returncode, res.stderr))
    return res.stdout


def check_imports(imports, odd, data, arch, allowlist_name="tools/win2000_imports.json"):
    """Returns (failures, verify): failure messages, and (dll!function, note) pairs for the
    imports whose allowlist entry is marked 'verify'."""
    floor, allowed = allowed_for(data, arch)
    failures = []
    verify = []
    if not imports:
        failures.append("no import table found in the objdump output")
    for dll in sorted(imports):
        known = allowed.get(dll)
        for function in sorted(imports[dll]):
            where = "%s!%s" % (dll, function)
            if known is None:
                failures.append("%s: the DLL '%s' is not on the %s allowlist (%s) "
                                "(a runtime or system DLL the floor system may not have)"
                                % (where, dll, floor, allowlist_name))
            elif function not in known:
                failures.append("%s is not on the %s allowlist (%s)"
                                % (where, floor, allowlist_name))
            elif known[function]["confidence"] == "verify":
                verify.append((where, known[function]["note"]))
    for line in odd:
        failures.append("unparsed import (by ordinal?) %s: cannot be checked" % line)
    return failures, verify


def check_exe(exe, objdump, arch=None, allowlist=DEFAULT_ALLOWLIST, show_list=False, out=None):
    """Checks one executable and prints the result; returns True when it passes."""
    out = out or sys.stdout
    data = load_allowlist(allowlist)
    detected, imports, odd = parse_objdump(run_objdump(objdump, exe))
    arch = arch or detected
    if arch is None:
        print("%s: cannot tell the architecture, pass --arch x86|x64" % exe, file=out)
        return False
    failures, verify = check_imports(imports, odd, data, arch)
    floor = allowed_for(data, arch)[0]
    total = sum(len(f) for f in imports.values())
    if show_list:
        for dll in sorted(imports):
            print("%s (%d): %s" % (dll, len(imports[dll]), " ".join(sorted(imports[dll]))),
                  file=out)
    if verify:
        print("note: %d import(s) rest on allowlist entries marked 'verify' (unconfirmed, see "
              "'note' in tools/win2000_imports.json): %s"
              % (len(verify), ", ".join(w for w, _ in verify)), file=out)
        if show_list:
            for where, note in verify:
                print("  verify %s: %s" % (where, note), file=out)
    if failures:
        for line in failures:
            print("FAIL %s" % line, file=out)
        print(RESOLVE_HELP % "tools/win2000_imports.json", file=out)
        print("%s (%s): %d import(s) outside the %s allowlist" % (exe, arch, len(failures), floor),
              file=out)
        return False
    print("ok: %s (%s): %d imports from %d DLLs, all on the %s allowlist (%d to verify)"
          % (exe, arch, total, len(imports), floor, len(verify)), file=out)
    return True


# ---- self-test ------------------------------------------------------------------------------

FAKE_OBJDUMP = """\
loomora.exe:     file format pei-i386

The Import Tables (interpreted .idata section contents)
 vma:            Hint    Time      Forward  DLL       First
                 Table   Stamp     Chain    Name      Thunk
 00046000\t00046064 00000000 00000000 00046858 000461d8

\tDLL Name: KERNEL32.dll
\tvma:  Hint/Ord Member-Name Bound-To
\t4635e\t  136  CloseHandle
\t4636c\t  275  GetThreadId
\t46374\t  300  TryEnterCriticalSection

 00046014\t00046064 00000000 00000000 00046858 000461d8

\tDLL Name: USER32.dll
\tvma:  Hint/Ord Member-Name Bound-To
\t4634c\t  631  MessageBoxW

"""


def self_test():
    problems = []

    def expect(condition, what):
        if not condition:
            problems.append(what)

    data = load_allowlist()  # also validates the shape of the real file
    arch, imports, odd = parse_objdump(FAKE_OBJDUMP)
    expect(arch == "x86", "architecture read from the 'file format' line")
    expect(imports == {"kernel32.dll": ["CloseHandle", "GetThreadId", "TryEnterCriticalSection"],
                       "user32.dll": ["MessageBoxW"]} and not odd,
           "fabricated import table parsed into dll -> functions")

    failures, verify = check_imports(imports, odd, data, "x86")
    expect(len(failures) == 1 and "kernel32.dll!GetThreadId" in failures[0],
           "x86: the Vista-only kernel32.dll!GetThreadId is rejected, and only it: %s" % failures)
    expect(any("TryEnterCriticalSection" in v[0] for v in verify),
           "x86: an allowlist entry marked verify is reported, not rejected")

    failures, _ = check_imports(imports, odd, data, "x64")
    expect(not failures, "x64: GetThreadId is accepted (XP x64 floor): %s" % failures)

    failures, _ = check_imports({"ucrtbase.dll": ["malloc"]}, [], data, "x86")
    expect(len(failures) == 1 and "ucrtbase.dll!malloc" in failures[0],
           "a DLL that is not on the list is rejected")

    failures, _ = check_imports({"kernel32.dll": ["CloseHandle"]}, ["kernel32.dll: 4634c  <ord>"],
                                data, "x86")
    expect(len(failures) == 1 and "unparsed import" in failures[0],
           "an import by ordinal (unparsed line) fails instead of passing unseen")

    failures, _ = check_imports({}, [], data, "x86")
    expect(len(failures) == 1, "an empty import table fails")

    expect("GetThreadId" not in data["lists"]["common"].get("kernel32.dll", {}),
           "the common (x86) list does not contain GetThreadId")

    for line in problems:
        print("self-test FAIL: %s" % line)
    if problems:
        return 1
    print("self-test ok: Vista-only GetThreadId rejected on x86, accepted on x64; unknown DLL, "
          "ordinal import and empty table rejected; verify entries reported; allowlist file valid")
    return 0


def main(argv=None):
    common.setup_utf8_output()
    parser = argparse.ArgumentParser(description="Function-level import check of a Windows exe.")
    parser.add_argument("exe", nargs="?", help="the executable to check")
    parser.add_argument("--arch", choices=ARCHS,
                        help="allowlist to use (default: read from the executable)")
    parser.add_argument("--objdump", default="objdump",
                        help="GNU objdump of the toolchain (default: objdump)")
    parser.add_argument("--allowlist", default=str(DEFAULT_ALLOWLIST), help="allowlist JSON file")
    parser.add_argument("--list", action="store_true", help="also print the imports of each DLL")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in self-test on a fabricated import list and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.exe:
        parser.error("an executable is required (or --self-test)")
    try:
        ok = check_exe(args.exe, args.objdump, args.arch, args.allowlist, args.list)
    except (AllowlistError, RuntimeError, OSError) as exc:
        sys.exit("check_imports: %s" % exc)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
