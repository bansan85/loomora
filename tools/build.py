#!/usr/bin/env python
"""Build one Loomora deliverable and record it.

Usage:
  python tools/build.py <platform> <variant> [--wsl-distro NAME]

  platform  windows | macos | ios | android | linux | web
  variant   legacy | modern

A target builds the UI of that platform together with the C++11 backend, copies the artifact to
out/<platform>-<variant>/ and writes docs/implementation/validation/deliverables/<platform-id>-<variant>.json (the
Deliverable of data-model.md). A target that is not written yet fails with
"not implemented yet (Txxx)".
The records live in the docs submodule: when docs/implementation is not checked out, the build
still runs, a warning is printed and no record is written (the product build never needs it).

Implemented targets:
  linux modern   meson + ninja on ui/modern/linux-gtk3 (T058). On a Windows host the build runs
                 inside WSL (distribution --wsl-distro, or $LOOMORA_WSL_DISTRO, default
                 Ubuntu-24.04): the tool re-runs itself there. Needs meson, ninja, a C and a C++
                 compiler and the gtk+-3.0 development files.

  windows legacy CMake + Ninja + MinGW-w64 GCC on ui/legacy/windows-win32 (T075), twice: i686
                 (one executable for Windows 2000 -> 11, toolchain
                 cmake/toolchains/mingw-w64-win2000-i686.cmake) and x86_64 (XP x64 -> 11,
                 mingw-w64-win-x86_64.cmake). Needs both MinGW-w64 compilers on PATH (msvcrt
                 runtime, not UCRT) and stops with install instructions when one is missing. The
                 tool prefixes can be overridden with $LOOMORA_MINGW_PREFIX_X86 and
                 $LOOMORA_MINGW_PREFIX_X64. Checks every imported function of each executable
                 against tools/win2000_imports.json (tools/check_imports.py) and fails on one
                 that is not listed. Writes one record per Windows platform that has a record in
                 docs/implementation/validation/platforms/ (<platform-id>-legacy.json in docs/implementation/validation/deliverables/: the
                 x86 executable for a platform whose arch is x86, the x64 one for x86_64); for an
                 architecture without any platform record it writes windows-<arch>-legacy.json
                 instead, which tools/validate_records.py reports as a deliverable without
                 platform until the platform record exists.

Platform id of a Linux deliverable: "linux-<os-release ID>-<VERSION_ID>-<machine>" such as
"linux-ubuntu-24.04-x86_64", the same rule as ui/modern/linux-gtk3/probe.c uses for the probe
record's platform_id (keep both in step).
Platform id of a Windows Legacy deliverable: the id of a Windows platform record
(docs/implementation/validation/platforms/, for example "windows-2000-x86" or "windows-11-x64"; data-model.md wants one
Deliverable per platform and variant). One executable runs on every Windows of its architecture,
so several platforms share one artifact: each record names the same file with its own size and
sha256. Only while an architecture has no platform record does the tool fall back to the generic
ids "windows-x86" / "windows-x64".
"""
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

import check_imports
import common

PLATFORMS = ("windows", "macos", "ios", "android", "linux", "web")
VARIANTS = ("legacy", "modern")
DEFAULT_WSL_DISTRO = "Ubuntu-24.04"

# Targets that do not exist yet and the task that will add each of them.
PENDING = {
    ("web", "modern"): "T040",
    ("windows", "modern"): "T046",
    ("macos", "modern"): "T049",
    ("ios", "modern"): "T052",
    ("android", "modern"): "T055",
    ("macos", "legacy"): "T077",
    ("ios", "legacy"): "T079",
    ("android", "legacy"): "T082",
    ("linux", "legacy"): "T084",
    ("web", "legacy"): "T086",
}


def capture(cmd, cwd=None):
    """Run a command and return its standard output (stripped); fails with a clear message."""
    res = subprocess.run(
        [str(c) for c in cmd],
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if res.returncode != 0:
        sys.exit("command failed (%d): %s\n%s" % (res.returncode, " ".join(map(str, cmd)), res.stderr))
    return res.stdout.strip()


def require_tools(names):
    missing = [n for n in names if shutil.which(n) is None]
    if missing:
        sys.exit("missing tool(s) on this machine: %s" % ", ".join(missing))


def fallbacks_of(platform_id):
    """Ids of docs/implementation/validation/fallbacks.json whose platform_ids name this platform: the fallbacks that
    the build of that platform takes (docs/implementation/adr/0004-fallback-per-case.md)."""
    if not common.docs_present():
        return []  # no docs submodule: no fallbacks list (see write_deliverable)
    data = common.read_json(common.VALIDATION / "fallbacks.json")
    return [rec["id"] for rec in data if platform_id in rec.get("platform_ids", [])]


def id_part(text):
    """One platform-id component: lowercase, anything outside [a-z0-9._-] becomes '-'."""
    return re.sub(r"[^a-z0-9._-]", "-", text.lower())


def linux_platform_id():
    values = {}
    for path in ("/etc/os-release", "/usr/lib/os-release"):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    key, sep, value = line.strip().partition("=")
                    if sep:
                        values[key] = value.strip("\"' ")
            break
    parts = ["linux"]
    for key in ("ID", "VERSION_ID"):
        if values.get(key):
            parts.append(id_part(values[key]))
    parts.append(id_part(platform.machine()))
    return "-".join(parts)


# ---- linux modern (T058) -------------------------------------------------------------------

def build_linux_modern():
    """Native Linux build of ui/modern/linux-gtk3; returns the Deliverable record."""
    require_tools(["meson", "ninja", "pkg-config", "gcc", "g++"])
    src = common.ROOT / "ui" / "modern" / "linux-gtk3"
    # Always under $HOME (never /tmp): the user must be able to find the build in WSL.
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    build_dir = build_root / "linux-modern"
    if (build_dir / "build.ninja").exists():
        common.run(["meson", "setup", "--reconfigure", build_dir, src])
    else:
        common.run(["meson", "setup", build_dir, src])
    common.run(["ninja", "-C", build_dir])

    exe = build_dir / "loomora-gtk3"
    if not exe.is_file():
        sys.exit("build finished but %s is missing" % exe)
    out_dir = common.ROOT / "out" / "linux-modern"
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact = out_dir / exe.name
    shutil.copyfile(str(exe), str(artifact))
    os.chmod(str(artifact), 0o755)

    compilers = json.loads(capture(["meson", "introspect", build_dir, "--compilers"]))["host"]
    cpp, cc = compilers["cpp"], compilers["c"]
    cpp_id = "%s-%s" % (cpp["id"], cpp["version"])
    cc_id = "%s-%s" % (cc["id"], cc["version"])
    cxx = cpp["exelist"][0]
    gtk = capture(["pkg-config", "--modversion", "gtk+-3.0"])

    platform_id = linux_platform_id()
    record = {
        "platform_id": platform_id,
        "ui_variant": "modern",
        "cpp_std": "c++11",
        "compiler": cpp_id,
        "target": capture([cxx, "-dumpmachine"]),
        "output": "native",
        "ui_language": "c",
        "ui_toolkit": "gtk+-3.0 %s (3.10 API subset)" % gtk,
        "ui_compiler": cc_id,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": fallbacks_of(platform_id),
        "notes": "meson + ninja (ui/modern/linux-gtk3/meson.build); static libloomora.a built "
                 "from core/src with -std=c++11; libstdc++, libgcc, GTK 3 and the system "
                 "libraries linked dynamically (the meson option static_cxx_runtime links the "
                 "C++ runtime statically for a distribution that lacks it, ADR 0004); artifact "
                 "copied to out/linux-modern/.",
    }
    return record, artifact


def in_wsl_or_linux():
    return sys.platform.startswith("linux")


def run_linux_modern(args):
    if in_wsl_or_linux():
        record, artifact = build_linux_modern()
    elif sys.platform == "win32":
        # Re-run this tool inside WSL; the repository is reached through its Windows path.
        distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
        require_tools(["wsl"])
        res = subprocess.run(
            ["wsl", "-d", distro, "--cd", str(common.ROOT), "--",
             "python3", "tools/build.py", "linux", "modern"])
        if res.returncode != 0:
            sys.exit("the build inside WSL (%s) failed with status %d" % (distro, res.returncode))
        return  # the inner run has written the deliverable record
    else:
        sys.exit("linux modern cannot be built on this host (%s): use Linux or Windows with WSL"
                 % sys.platform)
    write_deliverable(record, artifact)


# ---- windows legacy (T075) -----------------------------------------------------------------

# (platform id, toolchain file, default tool prefix, environment variable overriding the prefix,
#  range of Windows versions the executable is meant for)
WINDOWS_LEGACY_ARCHS = (
    ("windows-x86", "mingw-w64-win2000-i686.cmake", "i686-w64-mingw32-",
     "LOOMORA_MINGW_PREFIX_X86", "Windows 2000 SP4 -> 11"),
    ("windows-x64", "mingw-w64-win-x86_64.cmake", "x86_64-w64-mingw32-",
     "LOOMORA_MINGW_PREFIX_X64", "Windows XP x64 -> 11"),
)

# The imports of each executable are checked function by function against
# tools/win2000_imports.json by tools/check_imports.py (x86: the Windows 2000 SP4 allowlist; x64:
# the Windows XP x64 one). A DLL (a UCRT api-ms-win-crt-* import, libstdc++-6.dll, libgcc_s_*.dll,
# libwinpthread-1.dll, ...) or a function that is not listed fails the build: the toolchain, its
# flags or the code pulled in something the oldest supported Windows does not have (spike S1,
# docs/implementation/adr/0002).

MINGW_INSTALL_HELP = """\
windows legacy needs the MinGW-w64 GCC cross compilers, both of them, on PATH:
  i686 (Windows 2000 -> 11): i686-w64-mingw32-gcc, -g++, -windres, -objdump
  x86_64 (XP x64 -> 11):     x86_64-w64-mingw32-gcc, -g++, -windres, -objdump
plus cmake and ninja. The compilers MUST use the msvcrt.dll runtime (not UCRT, which Windows
2000 does not have); the win32 thread model is preferred.
  Windows host: unpack the two WinLibs "GCC + MinGW-w64" builds (https://winlibs.com, runtime
    "MSVCRT"; the Win32 package is i686, the Win64 package is x86_64) and put both bin\\
    directories on PATH.
  WSL / Linux (Ubuntu): sudo apt install g++-mingw-w64-i686 g++-mingw-w64-x86-64 cmake ninja-build
    then, for the win32 thread model, select the -win32 variant of each compiler with
    sudo update-alternatives --config i686-w64-mingw32-g++ (and gcc, and the x86_64 ones), and run
    wsl -d Ubuntu-24.04 -- python3 tools/build.py windows legacy
Other tool prefixes: set LOOMORA_MINGW_PREFIX_X86 / LOOMORA_MINGW_PREFIX_X64 (for example to a
full path ending in "i686-w64-mingw32-")."""


def mingw_tools(prefix):
    return [prefix + name for name in ("gcc", "g++", "windres", "objdump")]


def gcc_info(cxx):
    """(full version, thread model) of a GCC executable."""
    res = subprocess.run([cxx, "-v"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True)
    thread = re.search(r"^Thread model: (\S+)", res.stdout, re.MULTILINE)
    try:
        version = capture([cxx, "-dumpfullversion"])
    except SystemExit:
        version = capture([cxx, "-dumpversion"])
    return version, thread.group(1) if thread else "unknown"


def imported_functions(objdump, exe):
    """{dll (lowercase): [functions]} of the import table of an executable."""
    return check_imports.parse_objdump(capture([objdump, "-p", exe]))[1]


# _WIN32_WINNT of each executable, set by its toolchain file: the API level of its floor.
WINNT_LEVEL = {"x86": "0x0500", "x64": "0x0502"}


def build_windows_legacy_arch(platform_id, toolchain, prefix, span, build_root):
    """Cross-builds loomora.exe for one architecture; returns (record, artifact)."""
    arch = platform_id.split("-", 1)[1]
    build_dir = build_root / ("windows-legacy-" + arch)
    toolchain_file = common.ROOT / "cmake" / "toolchains" / toolchain
    common.run(["cmake", "-S", common.ROOT, "-B", build_dir, "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain_file),
                "-DLOOMORA_MINGW_PREFIX=" + prefix, "-DLOOMORA_BUILD_TESTS=OFF"])
    common.run(["cmake", "--build", build_dir, "--target", "loomora_win32"])

    exe = build_dir / "ui" / "legacy" / "windows-win32" / "loomora.exe"
    if not exe.is_file():
        sys.exit("build finished but %s is missing" % exe)

    allowlist_arch = "x86" if arch == "x86" else "x64"
    if not check_imports.check_exe(exe, prefix + "objdump", allowlist_arch):
        sys.exit("%s: the import check failed (see above); nothing was recorded for %s"
                 % (exe, platform_id))
    imports = imported_functions(prefix + "objdump", exe)
    dlls = sorted(imports)
    function_count = sum(len(f) for f in imports.values())
    floor = check_imports.allowed_for(check_imports.load_allowlist(), allowlist_arch)[0]

    out_dir = common.ROOT / "out" / "windows-legacy" / arch
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact = out_dir / "loomora.exe"
    shutil.copyfile(str(exe), str(artifact))

    has_get_thread_id = "GetThreadId" in imports.get("kernel32.dll", [])
    if arch == "x86":
        getthreadid = ("KERNEL32!GetThreadId (Vista and later) is not imported: the reference "
                       "from libgcc gthr-win32.o is satisfied by "
                       "ui/legacy/windows-win32/getthreadid_shim.cpp (ADR 0002)")
    else:
        state = "is imported" if has_get_thread_id else "is not imported"
        getthreadid = ("KERNEL32!GetThreadId %s (XP x64 has it, so the toolchain output is "
                       "unchanged, ADR 0002)" % state)

    cxx = prefix + "g++"
    version, thread_model = gcc_info(cxx)
    record = {
        "platform_id": platform_id,
        "ui_variant": "legacy",
        "cpp_std": "c++11",
        "compiler": "gcc-%s-mingw-w64" % version,
        "target": capture([cxx, "-dumpmachine"]),
        "output": "native",
        "ui_language": "c++",
        "ui_toolkit": "Win32 API (user32, gdi32), EDIT + STATIC controls, _WIN32_WINNT=%s, "
                      "no MFC, Common Controls 6 visual styles through the manifest (ignored "
                      "before Windows XP)" % WINNT_LEVEL[arch],
        "ui_compiler": "mingw-w64-gcc-%s" % version,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": [],  # filled per platform by windows_legacy_records
        "notes": "cmake + ninja (ui/legacy/windows-win32/CMakeLists.txt) with "
                 "cmake/toolchains/%s; static libloomora.a built from core/src with -std=c++11; "
                 "-static, libstdc++ and libgcc linked statically; GCC thread model %s; imported "
                 "DLLs: %s; %d imported functions, each on the %s allowlist of "
                 "tools/win2000_imports.json (tools/check_imports.py; entries marked verify are "
                 "unconfirmed, entries marked verified-on-vm were read in the export tables of "
                 "the real system named in the file); %s; meant for %s, not verified on any of them by this build (the "
                 "run on Windows 2000 is spike S1 / T066); artifact copied to "
                 "out/windows-legacy/%s/."
                 % (toolchain, thread_model, ", ".join(dlls), function_count, floor, getthreadid,
                    span, arch),
    }
    return record, artifact


def run_windows_legacy(args):
    archs = []
    missing = []
    for platform_id, toolchain, default_prefix, env_var, span in WINDOWS_LEGACY_ARCHS:
        prefix = os.environ.get(env_var) or default_prefix
        missing += [t for t in mingw_tools(prefix) if shutil.which(t) is None]
        archs.append((platform_id, toolchain, prefix, span))
    missing += [t for t in ("cmake", "ninja") if shutil.which(t) is None]
    if missing:
        sys.exit("windows legacy: missing tool(s) on this machine: %s\n\n%s"
                 % (", ".join(missing), MINGW_INSTALL_HELP))
    if sys.platform == "win32":
        build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (common.ROOT / "build"))
    else:  # WSL / Linux: always under $HOME, never /tmp or the Windows tree
        build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    built = {}  # architecture ("x86", "x64") -> (generic record, artifact)
    for platform_id, toolchain, prefix, span in archs:
        built[platform_id.split("-", 1)[1]] = build_windows_legacy_arch(
            platform_id, toolchain, prefix, span, build_root)
    for record, artifact in windows_legacy_records(built, windows_platforms()):
        write_deliverable(record, artifact)


# Architecture of a platform record (data-model.md, Platform.arch) -> architecture of the
# executable built for it.
PLATFORM_ARCH_TO_BUILD = {"x86": "x86", "x86_64": "x64"}


def windows_platforms():
    """{platform id: record} of the Windows platforms of docs/implementation/validation/platforms/."""
    found = {}
    if not common.docs_present():
        return found  # no docs submodule: no platform records
    for path in sorted((common.VALIDATION / "platforms").glob("*.json")):
        rec = common.read_json(path)
        if isinstance(rec, dict) and rec.get("family") == "windows" and rec.get("id"):
            found[rec["id"]] = rec
    return found


def windows_legacy_records(built, platforms):
    """The Deliverable records of the Windows Legacy build: [(record, artifact)].

    One record per Windows platform that has a platform record, naming the executable of that
    platform's architecture (same file, size and sha256 as the generic record, and the same
    build facts); a 64-bit platform also names the i686 executable, which runs there under WOW64.
    An architecture without any platform record gets the generic record (windows-x86 /
    windows-x64), which the validator reports as a deliverable without platform."""
    out = []
    covered = set()
    for platform_id, platform_rec in sorted(platforms.items()):
        arch = PLATFORM_ARCH_TO_BUILD.get(platform_rec.get("arch"))
        if arch not in built:
            continue
        covered.add(arch)
        generic, artifact = built[arch]
        record = dict(generic)
        record["platform_id"] = platform_id
        record["fallbacks"] = fallbacks_of(platform_id)
        notes = ("Deliverable of platform %s (validation/platforms/%s.json). %s; artifact sha256 "
                 "%s." % (platform_id, platform_id, generic["notes"], common.sha256(artifact)))
        if arch == "x64" and "x86" in built:
            x86_record, x86_artifact = built["x86"]
            notes += (" The i686 executable of the same build (%s, %d bytes, sha256 %s, "
                      "out/windows-legacy/x86/) also runs on this 64-bit system under WOW64; the "
                      "verdict of this platform says what was run."
                      % (x86_artifact.name, x86_record["artifact"]["size"],
                         common.sha256(x86_artifact)))
        record["notes"] = notes
        out.append((record, artifact))
    for arch, (generic, artifact) in sorted(built.items()):
        if arch not in covered:
            out.append((generic, artifact))
    return out


def write_deliverable(record, artifact):
    name = "%s-%s.json" % (record["platform_id"], record["ui_variant"])
    path = common.VALIDATION / "deliverables" / name
    print("artifact: %s (%d bytes)" % (artifact.relative_to(common.ROOT).as_posix(),
                                       record["artifact"]["size"]))
    if not common.docs_present():
        print("WARNING: deliverable record %s NOT written: %s"
              % (name, common.docs_missing_message("the record is skipped, the build itself is complete")))
        return
    common.write_json(path, record)
    print("deliverable record: %s" % path.relative_to(common.ROOT).as_posix())


TARGETS = {
    ("linux", "modern"): run_linux_modern,
    ("windows", "legacy"): run_windows_legacy,
}


def main(argv=None):
    common.setup_utf8_output()
    parser = argparse.ArgumentParser(description="Build one Loomora deliverable.")
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("variant", choices=VARIANTS)
    parser.add_argument("--wsl-distro", help="WSL distribution used on a Windows host "
                        "(default $LOOMORA_WSL_DISTRO or %s)" % DEFAULT_WSL_DISTRO)
    args = parser.parse_args(argv)

    key = (args.platform, args.variant)
    if key in TARGETS:
        TARGETS[key](args)
        return 0
    task = PENDING.get(key, "no task")
    sys.exit("build target %s-%s: not implemented yet (%s)" % (args.platform, args.variant, task))


if __name__ == "__main__":
    sys.exit(main())
