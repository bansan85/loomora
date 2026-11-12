#!/usr/bin/env python
"""Check that the Loomora backend is strict C++11 behind a clean C API (spike S6, research R1/R2).

Usage: python tools/check_std.py [--build-dir DIR] [--keep] [-v]

For every GCC and Clang found on the host (g++, clang++, clang++-NN), it:
  1. compiles every core/src/*.cpp with -std=c++11 -pedantic-errors -Wall -Wextra -Werror;
  2. compiles core/include/loomora.h as C89 (-std=c89 -pedantic-errors) and as C++98;
  3. builds a shared library with -fvisibility=hidden plus the export list of core/loomora.map
     (core/loomora.exp on macOS; hidden visibility alone does not hide the std templates) and
     fails if `nm -D --defined-only` shows any exported symbol that is not one of the loomora_*
     C functions of the header, or if one of them is missing (ELF and Mach-O hosts; also
     checked with the C++ runtime linked statically, as on the platforms where it is needed);
  4. builds and runs core/tests/boundary_check.cpp, which makes every allocation fail in turn
     and checks that no exception escapes any loomora_* function.

A compiler that is absent is skipped with a message; the tool fails if neither GCC nor Clang
exists, and also if no strict C++11 check could be run at all. A compiler whose standard
library cannot be used with -std=c++11 (clang on Windows uses the MSVC STL, which needs
C++14) is reported as SKIPPED for the C++11 steps: that is a gap, never a pass.

Exit status: 0 when nothing failed and at least one strict C++11 check ran, 1 otherwise.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import common

SOURCES = sorted(glob.glob(str(common.CORE / "src" / "*.cpp")))
HEADER = common.CORE / "include" / "loomora.h"
BOUNDARY = common.CORE / "tests" / "boundary_check.cpp"
STRICT = ["-std=c++11", "-pedantic-errors", "-Wall", "-Wextra", "-Werror"]
EXE = ".exe" if os.name == "nt" else ""

# Symbols that some linkers add to a shared object; none of them is C++ or loomora's.
LINKER_SYMBOLS = frozenset(["_init", "_fini", "_edata", "_end", "__bss_start"])


class Result(object):
    """Collects OK / FAIL / SKIP lines per compiler."""

    def __init__(self):
        self.failures = 0
        self.strict_runs = 0
        self.lines = []

    def ok(self, tag, msg):
        self.lines.append(("OK", tag, msg))
        print("  [ ok ] %s" % msg)

    def fail(self, tag, msg, detail=""):
        self.failures += 1
        self.lines.append(("FAIL", tag, msg))
        print("  [FAIL] %s" % msg)
        if detail:
            for line in detail.strip().splitlines()[:25]:
                print("         " + line)

    def skip(self, tag, msg):
        self.lines.append(("SKIP", tag, msg))
        print("  [skip] %s" % msg)


def sh(cmd, cwd=None):
    """Run a command, return (returncode, combined output)."""
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, universal_newlines=True,
                           encoding="utf-8", errors="replace")
    except OSError as e:
        return 127, str(e)
    return p.returncode, p.stdout


def version_of(exe):
    rc, out = sh([exe, "--version"])
    return out.splitlines()[0].strip() if rc == 0 and out else "unknown version"


def find_compilers():
    """Return [(family, cxx, cc)] for the GCC and Clang drivers found on PATH."""
    found = []
    gxx = shutil.which("g++")
    if gxx:
        found.append(("gcc", gxx, shutil.which("gcc")))
    # clang++ or the highest clang++-NN (Debian/Ubuntu install only the versioned names).
    clang = shutil.which("clang++")
    cc = shutil.which("clang")
    if not clang:
        best = None
        for d in os.environ.get("PATH", "").split(os.pathsep):
            for p in glob.glob(os.path.join(d, "clang++-[0-9]*")):
                m = re.search(r"clang\+\+-(\d+)$", p)
                if m and (best is None or int(m.group(1)) > best[0]):
                    best = (int(m.group(1)), p)
        if best:
            clang = best[1]
            cc = shutil.which("clang-%d" % best[0]) or shutil.which("clang")
    if clang:
        found.append(("clang", clang, cc))
    return found


def exported_functions():
    """The loomora_* functions declared with LOOMORA_API in the public header."""
    text = HEADER.read_text(encoding="utf-8")
    names = re.findall(r"LOOMORA_API\s+[^;()]*?\b(loomora_[a-z0-9_]+)\s*\(", text)
    if not names:
        sys.exit("check_std: no LOOMORA_API declaration found in loomora.h")
    return sorted(set(names))


def defined_symbols(nm, lib):
    """Names exported by a shared object, or None if the format is not supported."""
    if sys.platform.startswith("darwin"):
        rc, out = sh([nm, "-gU", lib])
        strip = True
    elif os.name == "nt":
        return None
    else:
        rc, out = sh([nm, "-D", "--defined-only", lib])
        strip = False
    if rc != 0:
        return None
    names = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:  # "address type name" (ELF) / "address type name" (Mach-O)
            n = parts[-1]
            names.append(n[1:] if strip and n.startswith("_") else n)
    return names


def check_exports(names, expected):
    """Return a list of problems: unexpected exports, missing functions."""
    problems = []
    for n in names:
        if n in LINKER_SYMBOLS:
            continue
        if n not in expected:
            problems.append("exported but not a loomora_* C function: %s" % n)
    for n in expected:
        if n not in names:
            problems.append("declared in loomora.h but not exported: %s" % n)
    return problems


def check_compiler(family, cxx, cc, work, res, verbose):
    tag = family
    print("\n%s: %s (%s)" % (family, version_of(cxx), cxx))
    work.mkdir(parents=True, exist_ok=True)

    # --- 1. strict C++11 on every source ---------------------------------------------------
    probe = work / "stl_probe.cpp"
    probe.write_text("#include <string>\n#include <new>\n#include <algorithm>\nint main() { return 0; }\n")
    rc, out = sh([cxx, "-std=c++11", "-fsyntax-only", probe])
    strict = rc == 0
    if not strict:
        res.skip(tag, "strict C++11 not available: the standard library of this toolchain does "
                      "not accept -std=c++11 (steps 1, 3 and 4 skipped)")
        first = [l for l in out.splitlines() if "error" in l][:1]
        if verbose and first:
            print("         " + first[0])
    else:
        bad = 0
        for src in SOURCES:
            rc, out = sh([cxx] + STRICT + ["-fsyntax-only", src])
            if rc != 0:
                bad += 1
                res.fail(tag, "%s: %s does not compile with %s" % (family, Path(src).name, " ".join(STRICT)), out)
        if not bad:
            res.ok(tag, "%d sources compile with %s" % (len(SOURCES), " ".join(STRICT)))
            res.strict_runs += 1

    # --- 2. the header as C89 and C++98 ----------------------------------------------------
    tu_c = work / "header_c89.c"
    tu_c.write_text('#include "loomora.h"\n'
                    "int main(void) {\n"
                    "  unsigned char b[16];\n"
                    "  return (int)loomora_abi_version() + loomora_greet(\"a\", 1, b, 16) +\n"
                    "         loomora_is_eol(LOOMORA_FAMILY_LINUX, 5, 4) +\n"
                    "         (loomora_eol_warning() == 0) + (loomora_build_compiler() == 0) +\n"
                    "         (int)loomora_probe_record(\"p\", LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE,\n"
                    "                                   \"a\", \"b\", 1, 1, -1, 0, \"\", \"t\", b, 16);\n"
                    "}\n")
    tu_cpp = work / "header_cpp98.cpp"
    tu_cpp.write_text(tu_c.read_text().replace("(void)", "()"))
    inc = "-I" + str(common.CORE / "include")
    if cc:
        flags = ["-std=c89", "-pedantic-errors", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", inc]
        rc, out = sh([cc] + flags + [tu_c])
        if rc == 0:
            res.ok(tag, "loomora.h compiles as C89 (%s)" % Path(cc).name)
        else:
            res.fail(tag, "loomora.h does not compile as C89 with %s" % Path(cc).name, out)
    else:
        res.skip(tag, "no C compiler next to %s: C89 check of loomora.h skipped" % Path(cxx).name)
    rc, out = sh([cxx, "-std=c++98", "-pedantic-errors", "-Wall", "-Wextra", "-Werror",
                  "-fsyntax-only", inc, tu_cpp])
    if rc == 0:
        res.ok(tag, "loomora.h compiles as C++98")
    else:
        res.fail(tag, "loomora.h does not compile as C++98", out)

    if not strict:
        return

    # --- 3. shared library: only loomora_* exported -----------------------------------------
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if os.name == "nt" or not nm:
        res.skip(tag, "export check skipped (%s)" % ("Windows DLL exports are not inspected by nm"
                                                    if os.name == "nt" else "nm not found"))
    else:
        expected = exported_functions()
        suffix = ".dylib" if sys.platform.startswith("darwin") else ".so"
        # -fvisibility=hidden alone leaves the std templates exported (libstdc++ marks
        # namespace std as default visibility), so the export list is enforced at link time.
        if sys.platform.startswith("darwin"):
            exports = ["-Wl,-exported_symbols_list," + str(common.CORE / "loomora.exp")]
        else:
            exports = ["-Wl,--version-script=" + str(common.CORE / "loomora.map")]
        variants = [("dynamic C++ runtime", exports, "libloomora" + suffix)]
        if not sys.platform.startswith("darwin"):
            variants.append(("static C++ runtime", exports + ["-static-libstdc++", "-static-libgcc"],
                             "libloomora_static_rt" + suffix))
        for label, extra, name in variants:
            lib = work / name
            cmd = ([cxx] + STRICT + ["-fPIC", "-fvisibility=hidden", "-fvisibility-inlines-hidden",
                                       "-DLOOMORA_BUILD_SHARED_LIB", "-shared", "-o", lib] + extra + SOURCES)
            rc, out = sh(cmd)
            if rc != 0:
                if extra and ("cannot find" in out or "relocation" in out):
                    res.skip(tag, "shared library with %s could not be linked here "
                                  "(static libstdc++ unusable in a shared object)" % label)
                else:
                    res.fail(tag, "shared library (%s) does not build" % label, out)
                continue
            names = defined_symbols(nm, lib)
            if names is None:
                res.skip(tag, "nm could not read %s" % name)
                continue
            problems = check_exports(names, expected)
            shown = sorted(n for n in names if n not in LINKER_SYMBOLS)
            if problems:
                res.fail(tag, "exports of the shared library (%s)" % label, "\n".join(problems))
            else:
                res.ok(tag, "shared library (%s) exports exactly the %d loomora_* functions: %s"
                       % (label, len(shown), ", ".join(shown)))
            if verbose:
                print("         nm: " + " ".join(sorted(names)))

    # --- 4. no exception escapes the C boundary ---------------------------------------------
    exe = work / ("boundary_check" + EXE)
    rc, out = sh([cxx] + STRICT + ["-o", exe, BOUNDARY] + SOURCES)
    if rc != 0:
        res.fail(tag, "boundary_check does not build", out)
    else:
        rc, out = sh([exe])
        if rc == 0:
            last = out.strip().splitlines()[-1] if out.strip() else ""
            res.ok(tag, "no exception escapes the C API under allocation failure (%s)" % last)
            if verbose:
                print(out)
        else:
            res.fail(tag, "an exception escaped the C API, or boundary_check failed", out)


def main():
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", help="working directory (default: a temporary one)")
    ap.add_argument("--keep", action="store_true", help="keep the working directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    compilers = find_compilers()
    if not compilers:
        print("check_std: neither GCC (g++) nor Clang (clang++) found on PATH: cannot check",
              file=sys.stderr)
        return 1
    for fam in ("gcc", "clang"):
        if not any(c[0] == fam for c in compilers):
            print("note: %s not found on PATH, skipped" % ("g++" if fam == "gcc" else "clang++"))

    base = Path(args.build_dir) if args.build_dir else Path(tempfile.mkdtemp(prefix="loomora_std_"))
    res = Result()
    try:
        for family, cxx, cc in compilers:
            check_compiler(family, cxx, cc, base / family, res, args.verbose)
    finally:
        if not args.keep and not args.build_dir:
            shutil.rmtree(str(base), ignore_errors=True)

    print("\nSummary: %d failure(s), %d compiler(s) with a strict C++11 compile check, %d skipped step(s)"
          % (res.failures, res.strict_runs, sum(1 for l in res.lines if l[0] == "SKIP")))
    if res.strict_runs == 0:
        print("check_std: no strict C++11 check could run on this host (see the skipped steps)",
              file=sys.stderr)
        return 1
    return 1 if res.failures else 0


if __name__ == "__main__":
    sys.exit(main())
