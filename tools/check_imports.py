#!/usr/bin/env python
"""Function-level import check of a Windows executable against a versioned allowlist.

Usage:
  python tools/check_imports.py EXE [--arch x86|x64] [--objdump PATH] [--allowlist FILE] [--list]
                                    [--against DIR [--no-allowlist]]
  python tools/check_imports.py --verify-dlls DIR [--arch x86|x64] [--system TEXT]
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

--verify-dlls DIR is the other direction: it reads the export table of the real system DLLs
copied from a trial system into DIR (kernel32.dll, user32.dll, ...; Microsoft binaries stay
outside the repository) and fails if a function of the allowlist of --arch is not exported by
them. An entry with confidence "verified-on-vm" has been confirmed this way on a real system
(named in the top-level "verified_systems" of the allowlist file).

tools/build.py runs this check on every Windows Legacy executable it builds.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

import common

DEFAULT_ALLOWLIST = Path(__file__).resolve().parent / "win2000_imports.json"
ARCHS = ("x86", "x64")
CONFIDENCES = ("documented", "verify", "verified-on-vm")
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
                if entry["confidence"] == "verified-on-vm":
                    if entry.get("verified_on") not in data.get("verified_systems", {}):
                        raise AllowlistError("%s: a 'verified-on-vm' entry needs 'verified_on' "
                                             "naming a key of the top-level 'verified_systems'"
                                             % where)
                    if not set(entry.get("verified_archs", ARCHS)) <= set(ARCHS):
                        raise AllowlistError("%s: 'verified_archs' must be among %s"
                                             % (where, ", ".join(ARCHS)))
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
        # binutils before 2.39 prints "vma: Hint/Ord Member-Name Bound-To", later ones
        # "vma: Ordinal Hint Member-Name Bound-To"
        if dll and re.match(r"\s*vma:\s+(Hint/Ord|Ordinal\s+Hint)", line):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.strip():
            in_table = False
            dll = None
            continue
        # "<address> <hint> <name>" (older), "<address> <ordinal or <none>> <hint hex> <name>" (newer)
        m = (re.match(r"\s*[0-9a-fA-F]+\s+\d+\s+(\S+)\s*$", line) or
             re.match(r"\s*[0-9a-fA-F]+\s+<none>\s+[0-9a-fA-F]+\s+(\S+)\s*$", line))
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
        if not imports[dll]:
            # a DLL named in the table whose functions were not parsed: an unknown objdump layout
            # must fail, not pass with "0 imports" (binutils 2.39 changed the layout)
            failures.append("%s: no imported function parsed (unknown objdump layout?)" % dll)
        for function in sorted(imports[dll]):
            where = "%s!%s" % (dll, function)
            if known is None:
                failures.append("%s: the DLL '%s' is not on the %s allowlist (%s) "
                                "(a runtime or system DLL the floor system may not have)"
                                % (where, dll, floor, allowlist_name))
            elif function not in known:
                failures.append("%s is not on the %s allowlist (%s)"
                                % (where, floor, allowlist_name))
            else:
                entry = known[function]
                # verified-on-vm counts only for the architectures the verification covered
                unconfirmed = entry["confidence"] == "verify" or (
                    entry["confidence"] == "verified-on-vm" and
                    arch not in entry.get("verified_archs", ARCHS))
                if unconfirmed:
                    verify.append((where, entry.get("note") or "not verified for " + arch))
    for line in odd:
        failures.append("unparsed import (by ordinal?) %s: cannot be checked" % line)
    return failures, verify


def check_against_dlls(imports, directory, out):
    """Every imported function must be exported by the DLL of that name in `directory` (system
    DLLs copied from a trial system). Returns the number of imports that are not."""
    missing = 0
    for dll in sorted(imports):
        path = Path(directory) / dll
        if not path.is_file():
            print("MISSING DLL %s: not in %s (%d imports cannot be confirmed)"
                  % (dll, directory, len(imports[dll])), file=out)
            missing += len(imports[dll])
            continue
        names, _by_ordinal = read_exports(path)
        gone = sorted(f for f in imports[dll] if f not in names)
        for f in gone:
            print("NOT EXPORTED %s!%s by the %s in %s" % (dll, f, dll, directory), file=out)
        missing += len(gone)
        print("%s: %d imported functions, %d exported by the system DLL (%d exports)"
              % (dll, len(imports[dll]), len(imports[dll]) - len(gone), len(names)), file=out)
    return missing


def check_exe(exe, objdump, arch=None, allowlist=DEFAULT_ALLOWLIST, show_list=False, out=None,
              against=None, use_allowlist=True):
    """Checks one executable and prints the result; returns True when it passes. With `against`
    (a folder of system DLLs) every import must also be exported by them; with
    use_allowlist=False only that check is made."""
    out = out or sys.stdout
    if not use_allowlist:
        _arch, imports, odd = parse_objdump(run_objdump(objdump, exe))
        missing = check_against_dlls(imports, against, out) + len(odd)
        print("%s: %d imports checked against the DLLs in %s: %d not exported"
              % (exe, sum(len(f) for f in imports.values()), against, missing), file=out)
        return missing == 0
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
    if against:
        missing = check_against_dlls(imports, against, out)
        print("%s: %d imports checked against the system DLLs in %s: %d not exported"
              % (exe, total, against, missing), file=out)
        return missing == 0
    return True


# ---- export tables of real system DLLs (--verify-dlls) -----------------------------------------

def read_exports(path):
    """Names exported by a PE DLL (a forwarded export counts: the loader resolves it).

    Returns (set of names, number of exports by ordinal only). A small PE reader, so no
    toolchain is needed to read a DLL copied from a trial system."""
    with open(str(path), "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise ValueError("%s: not a PE file (no MZ header)" % path)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("%s: not a PE file (no PE signature)" % path)
    n_sections, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    dir_off = opt + (96 if magic == 0x10B else 112 if magic == 0x20B else -1)
    if dir_off < opt:
        raise ValueError("%s: unknown optional header magic 0x%x" % (path, magic))
    export_rva, export_size = struct.unpack_from("<II", data, dir_off)
    if export_rva == 0:
        return set(), 0
    sections = []
    sec = opt + opt_size
    for i in range(n_sections):
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, sec + 8 + i * 40)
        sections.append((vaddr, max(vsize, rsize), raw))

    def offset(rva):
        for vaddr, size, raw in sections:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + raw
        raise ValueError("%s: RVA 0x%x is in no section" % (path, rva))

    exp = offset(export_rva)
    n_functions, n_names, _functions_rva, names_rva, ordinals_rva = struct.unpack_from(
        "<IIIII", data, exp + 20)
    names = set()
    names_off = offset(names_rva) if n_names else 0
    for i in range(n_names):
        name_rva, = struct.unpack_from("<I", data, names_off + 4 * i)
        start = offset(name_rva)
        end = data.index(b"\0", start)
        names.add(data[start:end].decode("ascii"))
    return names, max(0, n_functions - n_names)


def verify_dlls(directory, arch, system, out=None):
    """Checks that every function of the allowlist of `arch` is exported by the DLL of that name
    found in `directory`. Returns True when all are."""
    out = out or sys.stdout
    data = load_allowlist()
    floor, allowed = allowed_for(data, arch)
    missing_total = 0
    checked = 0
    exports = {}
    for dll in sorted(allowed):
        path = Path(directory) / dll
        if not path.is_file():
            print("MISSING DLL %s: not in %s (%d allowlist functions cannot be confirmed)"
                  % (dll, directory, len(allowed[dll])), file=out)
            missing_total += len(allowed[dll])
            continue
        names, _by_ordinal = read_exports(path)
        exports[dll] = names
        gone = sorted(f for f in allowed[dll] if f not in names)
        checked += len(allowed[dll]) - len(gone)
        for f in gone:
            print("NOT EXPORTED %s!%s (min_os %s, %s)"
                  % (dll, f, allowed[dll][f]["min_os"], allowed[dll][f]["used_by"]), file=out)
        missing_total += len(gone)
        print("%s: %d exports, %d of %d allowlist functions confirmed"
              % (dll, len(names), len(allowed[dll]) - len(gone), len(allowed[dll])), file=out)
    print("%s, allowlist %s: %d function(s) confirmed exported, %d not confirmed"
          % (system or directory, floor, checked, missing_total), file=out)
    return missing_total == 0


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


FAKE_OBJDUMP_NEW = """loomora.exe:     file format pei-i386

The Import Tables (interpreted .idata section contents)
 vma:            Hint    Time      Forward  DLL       First
                 Table   Stamp     Chain    Name      Thunk
 0014d000	0014d03c 00000000 00000000 0014d8cc 0014d19c

	DLL Name: KERNEL32.dll
	vma:     Ordinal  Hint  Member-Name  Bound-To
	0014d19c  <none>  0092  CloseHandle
	0014d1a0  <none>  0384  InitializeConditionVariable

"""


def fake_dll(names):
    """A minimal PE32 DLL whose export table holds `names` plus one nameless (ordinal) export."""
    text = b"".join(n.encode("ascii") + b"\0" for n in names)
    base = 0x1000  # RVA of the section, file offset 0x200
    head = 40 + 4 * (len(names) + 1) + 4 * len(names) + 2 * len(names)
    name_rvas = []
    cursor = base + head
    for n in names:
        name_rvas.append(cursor)
        cursor += len(n) + 1
    exp = struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, 0, 1, len(names) + 1, len(names),
                      base + 40, base + 40 + 4 * (len(names) + 1),
                      base + 40 + 4 * (len(names) + 1) + 4 * len(names))
    exp += b"".join(struct.pack("<I", base + 0x500) for _ in range(len(names) + 1))
    exp += b"".join(struct.pack("<I", r) for r in name_rvas)
    exp += b"".join(struct.pack("<H", i) for i in range(len(names)))
    body = exp + text
    dos = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x14C, 1, 0, 0, 0, 224, 0x2102)
    opt = struct.pack("<H", 0x10B) + b"\0" * 94 + struct.pack("<II", base, len(body)) + b"\0" * 120
    section = b".edata\0\0" + struct.pack("<IIIIIIHHI", len(body), base, len(body), 0x200, 0, 0, 0, 0, 0)
    image = dos + coff + opt + section
    return image + b"\0" * (0x200 - len(image)) + body


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

    arch, imports_new, odd_new = parse_objdump(FAKE_OBJDUMP_NEW)
    expect(imports_new == {"kernel32.dll": ["CloseHandle", "InitializeConditionVariable"]} and
           not odd_new, "import table in the layout of binutils 2.39 and later parsed")
    failures, _ = check_imports(imports_new, odd_new, data, "x86")
    expect(len(failures) == 1 and "InitializeConditionVariable" in failures[0],
           "x86: a Vista-only function in the new objdump layout is rejected: %s" % failures)
    failures, _ = check_imports({"kernel32.dll": []}, [], data, "x86")
    expect(len(failures) == 1 and "no imported function parsed" in failures[0],
           "a DLL without a parsed function fails instead of passing with 0 imports")
    failures, verify = check_imports(imports, odd, data, "x86")
    expect(len(failures) == 1 and "kernel32.dll!GetThreadId" in failures[0],
           "x86: the Vista-only kernel32.dll!GetThreadId is rejected, and only it: %s" % failures)
    expect(not any("TryEnterCriticalSection" in v[0] for v in verify),
           "x86: an entry verified-on-vm for x86 is not reported as unconfirmed")

    failures, verify = check_imports(imports, odd, data, "x64")
    expect(not failures, "x64: GetThreadId is accepted (XP x64 floor): %s" % failures)
    expect(any("TryEnterCriticalSection" in v[0] for v in verify) and
           any("GetThreadId" in v[0] for v in verify),
           "x64: a verify entry, and an entry verified on x86 only, are reported, not rejected")

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

    import os
    import tempfile
    handle, path = tempfile.mkstemp(suffix=".dll")
    try:
        os.write(handle, fake_dll(["CloseHandle", "TryEnterCriticalSection"]))
        os.close(handle)
        names, by_ordinal = read_exports(path)
        expect(names == {"CloseHandle", "TryEnterCriticalSection"} and by_ordinal == 1,
               "export table of a fabricated DLL read: %s, %s" % (sorted(names), by_ordinal))
    finally:
        os.remove(path)

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
    parser.add_argument("--verify-dlls", metavar="DIR",
                        help="read the export tables of the system DLLs in DIR and check every "
                        "allowlist function of --arch (default x86) against them")
    parser.add_argument("--against", metavar="DIR",
                        help="with EXE: also require every import to be exported by the system "
                        "DLLs in DIR (read from the export tables)")
    parser.add_argument("--no-allowlist", action="store_true",
                        help="with --against: skip the allowlist (for a helper program)")
    parser.add_argument("--system", default="", help="name of the system DIR comes from "
                        "(printed by --verify-dlls)")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in self-test on a fabricated import list and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify_dlls:
        try:
            return 0 if verify_dlls(args.verify_dlls, args.arch or "x86", args.system) else 1
        except (AllowlistError, ValueError, OSError) as exc:
            sys.exit("check_imports: %s" % exc)
    if not args.exe:
        parser.error("an executable is required (or --self-test)")
    try:
        ok = check_exe(args.exe, args.objdump, args.arch, args.allowlist, args.list,
                       against=args.against, use_allowlist=not args.no_allowlist)
    except (AllowlistError, RuntimeError, OSError) as exc:
        sys.exit("check_imports: %s" % exc)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
