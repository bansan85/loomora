#!/usr/bin/env python
"""Build one Loomora deliverable and record it.

Usage:
  python tools/build.py <platform> <variant> [--wsl-distro NAME] [--arch x86_64|i686|all]

  platform  windows | macos | ios | android | linux | web
  variant   legacy | modern

A target builds the UI of that platform together with the C++11 backend, copies the artifact to
out/<platform>-<variant>/ and writes docs/implementation/validation/deliverables/<platform-id>-<variant>.json (the
Deliverable of data-model.md). A target that is not written yet fails with
"not implemented yet (Txxx)".
The records live in the docs submodule: when docs/implementation is not checked out, the build
still runs, a warning is printed and no record is written (the product build never needs it).

Implemented targets:
  linux modern   CMake + Ninja on ui/modern/linux-gtk3 (T058): C++ with gtkmm (the system
                 gtkmm-3.0 packages; the GTK 4 build with gtkmm-4.0 is a CMake option, see
                 ui/modern/linux-gtk3/CMakeLists.txt), the C++11 backend as the static library
                 libloomora. The build goes under $HOME/loomora-build/linux-modern. Checks in
                 compile_commands.json that the backend sources are compiled with -std=c++11 and
                 the UI sources with one UI standard, that the executable needs only the gtkmm /
                 GTK libraries, the C++ runtime and the C library, and records the UI standard and
                 those libraries (libstdc++ and libgcc are linked dynamically, no symbol version
                 ceiling: a Linux binary is built on the machine that runs it, ADR 0007). On a
                 Windows host the
                 build runs inside WSL (distribution --wsl-distro, or $LOOMORA_WSL_DISTRO, default
                 Ubuntu-24.04): the tool re-runs itself there. Needs cmake, ninja, a C and a C++
                 compiler and the gtkmm-3.0 development files (libgtkmm-3.0-dev).

  linux legacy   CMake on ui/legacy/linux-xlib (T084): C++11 and Xlib, the C++11 backend as a
                 static libloomora.a. Built natively ON THE MACHINE THAT RUNS IT (ADR 0007): the
                 host gcc (inside WSL when started on Windows), libstdc++ and libgcc linked
                 dynamically, libX11 linked, and libXft with fontconfig linked when CMake finds
                 them (pkg-config; the UI then draws with Xft). When CMake does NOT find them the UI
                 is built with the core X11 font (fallback xlib-core-font-glyphs, a '?' for a
                 character the font lacks) and the tool prints the packages to install. Two
                 executables, x86_64 and i686 (--arch x86_64|i686|all, default all); i686 is gcc
                 -m32 through cmake/toolchains/linux-gcc-i686.cmake and needs the 32-bit
                 development files (the tool prints the apt packages when libX11 is missing).
                 Records <host platform id>-legacy.json (x86_64) and linux-i686-legacy.json. The
                 tool fails if the executable needs a shared library other than libX11, the C++
                 runtime, the C library and, with Xft, libXft and the libraries it brings; the
                 libraries are recorded and there is no GLIBC / GLIBCXX version ceiling.

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
"linux-ubuntu-24.04-x86_64", the same rule as ui/modern/linux-gtk3/probe.cpp and
ui/legacy/linux-xlib/probe.c use for the probe record's platform_id (keep them in step). The
Linux Legacy native x86_64 record uses the id of the build machine; the native i686 executable
has no platform record yet (T087) and is recorded under the generic id "linux-i686" until it exists.
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


def fallbacks_of(platform_id, extra=()):
    """Ids of docs/implementation/validation/fallbacks.json whose platform_ids name this platform: the fallbacks that
    the build of that platform takes (docs/implementation/adr/0004-fallback-per-case.md), plus the
    ids in `extra` (for a deliverable whose id is not a platform record, which validate_records.py
    does not let a fallback name)."""
    if not common.docs_present():
        return []  # no docs submodule: no fallbacks list (see write_deliverable)
    data = common.read_json(common.VALIDATION / "fallbacks.json")
    ids = [rec["id"] for rec in data if platform_id in rec.get("platform_ids", [])]
    return ids + [f for f in extra if f not in ids]


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

LINUX_MODERN_CORE_STD = "c++11"  # the backend is always C++11, whatever the UI standard is
LINUX_MODERN_UI_STDS = ("c++11", "c++14", "c++17")  # the UI standards CMakeLists.txt may pick


def cmake_compiler(build_dir, language="CXX"):
    """(id, version) of the compiler CMake detected, from CMakeFiles/<version>/CMake<L>Compiler.cmake."""
    files = sorted((build_dir / "CMakeFiles").glob("*/CMake%sCompiler.cmake" % language))
    if not files:
        sys.exit("no CMake%sCompiler.cmake under %s" % (language, build_dir / "CMakeFiles"))
    text = files[-1].read_text(encoding="utf-8")
    cid = re.search(r'set\(CMAKE_%s_COMPILER_ID "([^"]*)"\)' % language, text)
    version = re.search(r'set\(CMAKE_%s_COMPILER_VERSION "([^"]*)"\)' % language, text)
    path = re.search(r'set\(CMAKE_%s_COMPILER "([^"]*)"\)' % language, text)
    if not (cid and version and path):
        sys.exit("cannot read the compiler of %s from %s" % (language, files[-1]))
    name = {"GNU": "gcc", "Clang": "clang"}.get(cid.group(1), cid.group(1).lower())
    return "%s-%s" % (name, version.group(1)), path.group(1)


def check_linux_modern_standards(build_dir):
    """Reads compile_commands.json: every backend source (core/src) must be compiled with
    -std=c++11 and every UI source with one -std=c++NN of LINUX_MODERN_UI_STDS (the standard of the
    UI target, set as a target property, never globally). Returns the UI standard, e.g. "c++17"."""
    commands = common.read_json(build_dir / "compile_commands.json")
    ui_dir = (common.ROOT / "ui" / "modern" / "linux-gtk3").as_posix()
    core_dir = (common.ROOT / "core" / "src").as_posix()
    core_seen, ui_stds = 0, set()
    for entry in commands:
        source = Path(entry["file"]).as_posix()
        stds = re.findall(r"-std=\S+", entry.get("command") or " ".join(entry.get("arguments", [])))
        if source.startswith(core_dir + "/"):
            core_seen += 1
            if stds != ["-std=" + LINUX_MODERN_CORE_STD]:
                sys.exit("%s is compiled with %s, the backend must be -std=%s only (strict, no "
                         "extensions)" % (source, stds or "no -std", LINUX_MODERN_CORE_STD))
        elif source.startswith(ui_dir + "/"):
            if len(stds) != 1 or stds[0][5:] not in LINUX_MODERN_UI_STDS:
                sys.exit("%s is compiled with %s, expected one of %s"
                         % (source, stds or "no -std", ", ".join(LINUX_MODERN_UI_STDS)))
            ui_stds.add(stds[0][5:])
    if core_seen == 0 or len(ui_stds) != 1:
        sys.exit("compile_commands.json: expected the backend sources and one UI standard, found "
                 "%d backend sources and UI standards %s" % (core_seen, sorted(ui_stds)))
    return ui_stds.pop()


# Shared libraries the GTK executable may name: the gtkmm family and the GTK / GLib libraries it
# sits on (GTK 3 with gtkmm-3.0, GTK 4 with gtkmm-4.0), the C++ runtime and the C library of the
# system with its loader. There is no symbol version ceiling.
LINUX_MODERN_ALLOWED_NEEDED = re.compile(
    r"^(lib(gtkmm|gdkmm|atkmm|giomm|glibmm|pangomm|cairomm|sigc|gtk|gdk|gio|glib|gobject|pango|"
    r"pangocairo|cairo|cairo-gobject|atk|gdk_pixbuf|graphene|harfbuzz|gmodule)-[0-9][0-9.]*\.so\.[0-9]+|"
    r"libstdc\+\+\.so\.6|libgcc_s\.so\.1|libm\.so\.6|libc\.so\.6|libdl\.so\.2|libpthread\.so\.0|"
    r"librt\.so\.1|ld-linux[-a-z0-9_.]*\.so\.[0-9]+)$")


def elf_runtime_facts(exe):
    """(direct DT_NEEDED libraries, number of libraries ldd resolves) of an executable; fails when
    one of the direct libraries is not in the expected set (LINUX_MODERN_ALLOWED_NEEDED) or ldd
    reports a missing one."""
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]",
                        capture(["readelf", "-d", exe]))
    ldd = capture(["ldd", exe])
    if "not found" in ldd:
        sys.exit("ldd reports a missing library for %s:\n%s" % (exe, ldd))
    bad = [lib for lib in needed if not LINUX_MODERN_ALLOWED_NEEDED.match(lib)]
    if bad:
        sys.exit("%s needs shared libraries the Linux Modern UI is not meant to need: %s (allowed: "
                 "the gtkmm / GTK family, libstdc++, libgcc_s, libm, the C library and its loader)"
                 % (exe, ", ".join(bad)))
    return needed, len(ldd.splitlines())


def build_linux_modern():
    """Native Linux build of ui/modern/linux-gtk3 (C++ and gtkmm, CMake + Ninja); returns the
    Deliverable record."""
    require_tools(["cmake", "ninja", "pkg-config", "gcc", "g++", "readelf", "objdump", "ldd"])
    # Always under $HOME (never /tmp): the user must be able to find the build in WSL.
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    build_dir = build_root / "linux-modern"
    if (build_dir / "meson-private").is_dir():  # the build directory of the former Meson build
        shutil.rmtree(str(build_dir))
    common.run(["cmake", "-S", common.ROOT, "-B", build_dir, "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "-DLOOMORA_BUILD_TESTS=OFF", "-DLOOMORA_BUILD_LINUX_MODERN=ON"])
    common.run(["cmake", "--build", build_dir, "--target", "loomora_gtk3"])

    exe = build_dir / "loomora-gtk3"
    if not exe.is_file():
        sys.exit("build finished but %s is missing" % exe)
    ui_std = check_linux_modern_standards(build_dir)
    out_dir = common.ROOT / "out" / "linux-modern"
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact = out_dir / exe.name
    shutil.copyfile(str(exe), str(artifact))
    os.chmod(str(artifact), 0o755)

    cpp_id, cxx = cmake_compiler(build_dir, "CXX")
    gtkmm = capture(["pkg-config", "--modversion", "gtkmm-3.0"])
    gtk = capture(["pkg-config", "--modversion", "gtk+-3.0"])
    needed, ldd_count = elf_runtime_facts(exe)

    platform_id = linux_platform_id()
    record = {
        "platform_id": platform_id,
        "ui_variant": "modern",
        "cpp_std": "c++11",
        "compiler": cpp_id,
        "target": capture([cxx, "-dumpmachine"]),
        "output": "native",
        "ui_language": "c++",
        "ui_toolkit": "gtkmm-3.0 %s on gtk+-3.0 %s (%s UI; GTK 3.10 floor, newer features by macro)"
                      % (gtkmm, gtk, ui_std.upper()),
        "ui_compiler": cpp_id,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": fallbacks_of(platform_id),
        "notes": "cmake + ninja (ui/modern/linux-gtk3/CMakeLists.txt, CMAKE_BUILD_TYPE=Release, "
                 "LOOMORA_BUILD_LINUX_MODERN=ON); static libloomora.a built from core/src with "
                 "-std=%s, the UI sources with -std=%s (both checked in compile_commands.json); "
                 "libstdc++, libgcc, gtkmm and the system libraries linked dynamically; built "
                 "natively on the machine it is for (ADR 0007), no symbol version ceiling; "
                 "direct DT_NEEDED (only the gtkmm / GTK family, the C++ runtime and the C "
                 "library are accepted): %s; %d libraries resolved by ldd; artifact copied to "
                 "out/linux-modern/."
                 % (LINUX_MODERN_CORE_STD, ui_std, ", ".join(needed), ldd_count),
    }
    return record, artifact


def in_wsl_or_linux():
    return sys.platform.startswith("linux")


def rerun_in_wsl(args, platform_name, variant, extra=()):
    """Runs this tool again inside WSL; the repository is reached through its Windows path."""
    distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
    require_tools(["wsl"])
    res = subprocess.run(
        ["wsl", "-d", distro, "--cd", str(common.ROOT), "--",
         "python3", "tools/build.py", platform_name, variant] + list(extra))
    if res.returncode != 0:
        sys.exit("the build inside WSL (%s) failed with status %d" % (distro, res.returncode))


def run_linux_modern(args):
    if in_wsl_or_linux():
        record, artifact = build_linux_modern()
    elif sys.platform == "win32":
        rerun_in_wsl(args, "linux", "modern")
        return  # the inner run has written the deliverable record
    else:
        sys.exit("linux modern cannot be built on this host (%s): use Linux or Windows with WSL"
                 % sys.platform)
    write_deliverable(record, artifact)


# ---- linux legacy (T084) -------------------------------------------------------------------

LINUX_LEGACY_ARCHS = ("x86_64", "i686")

# Shared libraries the Xlib executable may name: libX11, the C++ runtime and the C library of the
# system (with its loader, and the libraries glibc before 2.34 keeps apart), plus, when the UI is
# built with Xft, libXft and the libraries it brings (fontconfig, FreeType, Xrender). Nothing else.
LINUX_LEGACY_ALLOWED_NEEDED = re.compile(
    r"^(libX11\.so\.6|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libm\.so\.6|libc\.so\.6|libdl\.so\.2|"
    r"libpthread\.so\.0|librt\.so\.1|ld-linux[-a-z0-9_.]*\.so\.[0-9]+)$")
LINUX_LEGACY_XFT_NEEDED = re.compile(
    r"^(libXft\.so\.2|libfontconfig\.so\.1|libfreetype\.so\.6|libXrender\.so\.1)$")

# What the machine must provide, printed when the build ends without Xft (the text path of the
# fallback xlib-core-font-glyphs, see ui/legacy/linux-xlib/CMakeLists.txt).
XFT_HELP = {
    "x86_64": "sudo apt install libxft-dev libfontconfig-dev",
    "i686": "sudo apt install libxft-dev:i386 libfontconfig-dev:i386",
}

I386_X11_HELP = """\
linux legacy i686: the 32-bit libX11 is not installed, so the i686 executable cannot be linked
(`gcc -m32` itself works). The i686 build is made natively on the machine that runs it (ADR 0007),
which needs its 32-bit development files. On Ubuntu / Debian install them with:
  sudo dpkg --add-architecture i386
  sudo apt update
  sudo apt install gcc-multilib g++-multilib libx11-dev:i386 libxft-dev:i386 libfontconfig-dev:i386
(libxft-dev:i386 and libfontconfig-dev:i386 are optional: without them the i686 UI is built with
the core X11 font, the fallback xlib-core-font-glyphs.) Then run the tool again."""


def can_link_i386_x11(cc):
    """True when a 32-bit program using libX11 links (the i386 libX11 development files exist)."""
    src = "#include <X11/Xlib.h>\nint main(void) { return XOpenDisplay(0) != 0; }\n"
    res = subprocess.run([cc, "-m32", "-x", "c", "-", "-o", os.devnull, "-lX11"],
                         input=src, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    return res.returncode == 0


def elf_needed(exe):
    """The direct DT_NEEDED libraries of an ELF executable on this machine."""
    return re.findall(r"^\s*NEEDED\s+(\S+)", capture(["objdump", "-p", exe]), re.MULTILINE)


def cmake_cache_value(build_dir, name):
    """Value of a CMakeCache.txt entry (any type), or None."""
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        m = re.match(r"%s:[A-Z]+=(.*)$" % re.escape(name), line)
        if m:
            return m.group(1)
    return None


def check_linux_legacy_elf(exe, needed, xft):
    """Fails when the executable needs a shared library the Linux Legacy UI is not meant to need:
    libX11, the C++ runtime and the C library, and libXft with its libraries when it is built
    with Xft (which must then name libXft; without Xft it must not). There is no symbol version
    ceiling: the program is built on the machine it runs on."""
    allowed = [LINUX_LEGACY_ALLOWED_NEEDED] + ([LINUX_LEGACY_XFT_NEEDED] if xft else [])
    bad = [lib for lib in needed if not any(rx.match(lib) for rx in allowed)]
    if bad:
        sys.exit("%s needs shared libraries the Linux Legacy UI must not need: %s (allowed: "
                 "libX11, libstdc++, libgcc_s, libm, the C library and its loader%s)"
                 % (exe, ", ".join(bad), ", libXft, fontconfig, FreeType, Xrender" if xft else ""))
    if "libX11.so.6" not in needed:
        sys.exit("%s does not name libX11.so.6" % exe)
    if xft and "libXft.so.2" not in needed:
        sys.exit("%s was configured with Xft but does not name libXft.so.2" % exe)
    if not xft and "libXft.so.2" in needed:
        sys.exit("%s was configured without Xft but names libXft.so.2" % exe)


# CMake options of the Linux Legacy UI build: -O2 as the Makefile this CMake project replaced, no
# backend tests (they need the Catch2 submodule), and a missing libX11 is an error, not a silently
# skipped target. Xft is left to LOOMORA_XLIB_XFT=AUTO: used when CMake finds it.
LINUX_LEGACY_CMAKE_ARGS = [
    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_FLAGS_RELEASE=-O2", "-DCMAKE_CXX_FLAGS_RELEASE=-O2",
    "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++", "-DLOOMORA_BUILD_TESTS=OFF",
    "-DLOOMORA_XLIB_REQUIRED=ON",
]

LINUX_LEGACY_TOOLKIT_BASE = "Xlib (libX11, no toolkit); %s; C++11 with RAII classes for the Xlib%s handles, POSIX"
LINUX_LEGACY_TOOLKIT_XFT = ("text drawn with libXft and fontconfig, linked (the core X11 font "
                            "fixed only when Xft opens no font)")
LINUX_LEGACY_TOOLKIT_CORE = ("text drawn with the core X11 font fixed (built without libXft: "
                             "CMake did not find it)")


def build_linux_legacy_arch(arch, build_root):
    """Native build of ui/legacy/linux-xlib for one architecture with CMake (the C++ runtime of
    the system, dynamically; Xft when CMake finds it); returns (record, artifact)."""
    build_dir = build_root / ("linux-legacy-" + arch)
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    toolchain = []
    if arch == "i686":
        toolchain = ["-DCMAKE_TOOLCHAIN_FILE=" + str(
            common.ROOT / "cmake" / "toolchains" / "linux-gcc-i686.cmake")]
    common.run(["cmake", "-S", common.ROOT, "-B", build_dir] + generator + LINUX_LEGACY_CMAKE_ARGS
               + toolchain)
    text_path = cmake_cache_value(build_dir, "LOOMORA_XLIB_TEXT_PATH")
    if text_path not in ("xft", "core-font"):
        sys.exit("linux legacy %s: cannot read the text path chosen by CMake (LOOMORA_XLIB_TEXT_PATH "
                 "in %s)" % (arch, build_dir / "CMakeCache.txt"))
    xft = text_path == "xft"
    if not xft:
        print("NOTE: linux legacy %s: CMake did not find libXft and fontconfig, the UI is built with "
              "the core X11 font (fallback xlib-core-font-glyphs). For the Xft text path install "
              "their development files, e.g.: %s" % (arch, XFT_HELP[arch]))
    common.run(["cmake", "--build", build_dir, "--target", "loomora_xlib"])
    exe = build_dir / "loomora-xlib"
    if not exe.is_file():
        sys.exit("build finished but %s is missing" % exe)

    needed = elf_needed(exe)
    check_linux_legacy_elf(exe, needed, xft)
    ldd_lines = capture(["ldd", exe]).splitlines()
    if any("not found" in line for line in ldd_lines):
        sys.exit("ldd reports a missing library for %s:\n%s" % (exe, "\n".join(ldd_lines)))

    out_dir = common.ROOT / "out" / "linux-legacy" / arch
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact = out_dir / exe.name
    shutil.copyfile(str(exe), str(artifact))
    os.chmod(str(artifact), 0o755)

    gxx_version = capture(["g++", "-dumpfullversion"])
    gcc_version = capture(["gcc", "-dumpfullversion"])
    glibc = "glibc " + capture(["ldd", "--version"]).splitlines()[0].split()[-1]
    host_id = linux_platform_id()
    host_triple = capture(["g++", "-dumpmachine"])
    if arch == "x86_64":
        platform_id = host_id
        target = host_triple
    else:
        platform_id = "linux-i686"  # no platform record yet (T087)
        target = "i686-linux-gnu (gcc -m32 on %s)" % host_triple
    cmake_version = capture(["cmake", "--version"]).split()[2]
    xft_info = cmake_cache_value(build_dir, "LOOMORA_XLIB_XFT_INFO") or ""
    if xft:
        text_note = ("text drawn with Xft (%s, found by pkg-config and linked; no fallback)" % xft_info)
    else:
        text_note = ("CMake did NOT find libXft and fontconfig: the UI is built with the core "
                     "X11 font path, which draws a '?' for a character the font lacks and reports "
                     "glyph-substitution in the probe line (fallback xlib-core-font-glyphs; "
                     "install %s for the Xft path)" % XFT_HELP[arch].replace("sudo apt install ", ""))
    fallbacks = fallbacks_of(platform_id)
    if not xft and "xlib-core-font-glyphs" not in fallbacks:
        fallbacks.append("xlib-core-font-glyphs")  # taken by this build, whatever the platform id
    record = {
        "platform_id": platform_id,
        "ui_variant": "legacy",
        "cpp_std": "c++11",
        "compiler": "gcc-%s" % gxx_version,
        "target": target,
        "output": "native",
        "ui_language": "c++",
        "ui_toolkit": LINUX_LEGACY_TOOLKIT_BASE % (
            LINUX_LEGACY_TOOLKIT_XFT if xft else LINUX_LEGACY_TOOLKIT_CORE, " and Xft" if xft else ""),
        "ui_compiler": "gcc-%s" % gcc_version,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": fallbacks,
        "notes": "cmake %s (ui/legacy/linux-xlib/CMakeLists.txt, %s, Release with -O2) on %s "
                 "(%s); the UI is C++11 (-std=c++11, -Wall -Wextra -pedantic); static "
                 "libloomora.a built from core/src with -std=c++11; libstdc++ and libgcc_s "
                 "linked dynamically; %s; shared libraries needed (objdump -p): %s; %d libraries "
                 "resolved by ldd. BUILT NATIVELY ON THE MACHINE IT IS FOR (ADR 0007), so no "
                 "glibc or libstdc++ version ceiling is checked and it was not run on any other "
                 "system. Artifact copied to out/linux-legacy/%s/, sha256 %s."
                 % (cmake_version, "x86_64" if arch == "x86_64" else
                    "i686 through cmake/toolchains/linux-gcc-i686.cmake (gcc -m32)", host_id,
                    glibc, text_note, ", ".join(needed), len(ldd_lines), arch,
                    common.sha256(artifact)),
    }
    return record, artifact


def run_linux_legacy(args):
    if sys.platform == "win32":
        # the inner run, in WSL, writes the records
        rerun_in_wsl(args, "linux", "legacy", ["--arch", args.arch])
        return
    if not in_wsl_or_linux():
        sys.exit("linux legacy cannot be built on this host (%s): use Linux or Windows with WSL"
                 % sys.platform)
    require_tools(["cmake", "gcc", "g++", "objdump", "ldd"])
    if platform.machine() != "x86_64":
        sys.exit("linux legacy: this tool builds both architectures on an x86_64 host (gcc -m32 "
                 "for i686); this machine is %s" % platform.machine())
    print("NOTE: linux legacy builds natively on this machine (gcc %s): the executable is meant to "
          "run here, a Linux binary is always built on the machine that runs it (ADR 0007)."
          % capture(["gcc", "-dumpfullversion"]))
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    wanted = LINUX_LEGACY_ARCHS if args.arch == "all" else (args.arch,)
    failed = []
    for arch in wanted:
        if arch == "i686" and not can_link_i386_x11("gcc"):
            print(I386_X11_HELP)
            failed.append(arch)
            continue
        record, artifact = build_linux_legacy_arch(arch, build_root)
        write_deliverable(record, artifact)
    if failed:
        sys.exit("linux legacy: not built: %s (see above)" % ", ".join(failed))


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
    ("linux", "legacy"): run_linux_legacy,
    ("windows", "legacy"): run_windows_legacy,
}


def main(argv=None):
    common.setup_utf8_output()
    parser = argparse.ArgumentParser(description="Build one Loomora deliverable.")
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("variant", choices=VARIANTS)
    parser.add_argument("--wsl-distro", help="WSL distribution used on a Windows host "
                        "(default $LOOMORA_WSL_DISTRO or %s)" % DEFAULT_WSL_DISTRO)
    parser.add_argument("--arch", choices=LINUX_LEGACY_ARCHS + ("all",), default="all",
                        help="linux legacy only: the architecture to build (default all)")
    args = parser.parse_args(argv)

    key = (args.platform, args.variant)
    if key in TARGETS:
        TARGETS[key](args)
        return 0
    task = PENDING.get(key, "no task")
    sys.exit("build target %s-%s: not implemented yet (%s)" % (args.platform, args.variant, task))


if __name__ == "__main__":
    sys.exit(main())
