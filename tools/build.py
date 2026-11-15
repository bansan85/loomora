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

  web legacy     CMake + Ninja with Emscripten on ui/legacy/web (T085, T086), WSL / Linux only (on a
                 Windows host the tool re-runs itself in WSL): the C++11 backend built twice, as
                 WebAssembly with the current Emscripten (cmake/toolchains/emscripten-wasm.cmake,
                 the emsdk of $LOOMORA_EMSDK, default ~/emsdk, activated with emsdk_env.sh) and as
                 JavaScript with Emscripten 1.39.20 fastcomp (emscripten-js-fastcomp.cmake, the emsdk
                 of $LOOMORA_EMSDK_FASTCOMP, default ~/emsdk-fastcomp, activated with EM_CONFIG and
                 PATH), each with its self-test run by node (ctest). The page and its script are
                 copied with the three module files into the folder
                 $HOME/loomora-build/web-legacy/dist/ (index.html, app.js, loomora.js,
                 loomora-wasm.js, loomora.wasm), outside the repository tree; that folder is the
                 deliverable. Checks that app.js and the JavaScript build parse as ECMAScript 3
                 (tools/trials/web-node/es-level.js, acorn of the fastcomp emsdk). Writes one
                 deliverable record per Web platform of WEB_PLATFORM_PATH that has a platform
                 record (a browser on an operating system, T093), naming the files of its path.

  web modern     CMake + Ninja with Emscripten and npm + TypeScript on ui/modern/web-ts (T038-T040),
                 WSL / Linux only (on a Windows host the tool re-runs itself in WSL): the C++11
                 backend built as WebAssembly with the current Emscripten (the same toolchain and
                 emsdk as web legacy, self-test run by node), the sources of ui/modern/web-ts copied
                 into $HOME/loomora-build/web-modern/ts/ (so that node_modules never enters the
                 repository tree), `npm ci` (package-lock.json, exact versions) and `tsc`, then the
                 folder $HOME/loomora-build/web-modern/dist/ (index.html, main.js, native.js,
                 loomora.js, loomora.wasm), outside the repository tree. It writes NO deliverable
                 record: no Web platform record for a current browser exists yet (the Chrome trial,
                 T063, adds it); the run of the page under node is
                 `python tools/run_vectors.py web-node-wasm modern`.

  android legacy CMake + Ninja with cmake/toolchains/android-ndk-r15c.cmake (T029, T081, T082) for
                 the JNI glue bridge/jni/loomora_jni.cpp and the C++11 backend, once per ABI
                 (armeabi, armeabi-v7a, x86: API 9 sysroot; arm64-v8a: API 21 sysroot), plus
                 loomora_selftest per ABI (spike S3), then ui/legacy/android-views/build_apk.py
                 (javac, d8 --min-api 9, aapt, zipalign, apksigner v1) writes ONE APK holding
                 the four libraries: out/android-legacy/loomora.apk (selftests in
                 out/android-legacy/selftest/<abi>/). Runs on the Windows host, not in WSL; needs
                 the Android SDK (LOOMORA_ANDROID_SDK or ANDROID_HOME: platforms/android-9,
                 build-tools with d8, aapt, zipalign, apksigner), the NDK r15c
                 (LOOMORA_ANDROID_NDK) and a JDK (JAVA_HOME). It checks each library: DT_NEEDED
                 limited to the libraries of Android 2.3 (libc, libm, libdl, liblog, libstdc++),
                 only Java_* exported, no undefined symbol (the link uses --no-undefined against
                 the API 9 libraries), -std=c++11 for the backend. Writes one record per Android
                 platform of docs/implementation/validation/platforms/ (the same APK for all).

  android modern gradlew assembleRelease on ui/modern/android-compose (T053, T054, T055): Kotlin and
                 Jetpack Compose, AGP 9, compileSdk and targetSdk 37, minSdk 24; Gradle's
                 externalNativeBuild runs CMake 3.22.1 with NDK r30 (libc++ static) for the JNI
                 glue bridge/jni/loomora_jni.cpp and the C++11 backend, ABIs armeabi-v7a, arm64-v8a,
                 x86, x86_64, into ONE APK signed (v2/v3) with a throwaway key that the tool makes
                 with keytool in the build directory: out/android-modern/loomora.apk. Runs on the
                 Windows host; needs the Android SDK (platforms/android-37.0, build-tools, cmake
                 3.22.1, ndk/30.0.16248370), a JDK (JAVA_HOME) and network access for the Gradle
                 wrapper. It checks each library of the APK (DT_NEEDED limited to libc, libm,
                 libdl; only Java_* exported; 16 KB LOAD alignment), -std=c++11 for the backend,
                 apksigner verify and zipalign -P 16. --min-sdk N builds with another minSdk
                 (spike S8) and writes no record. Writes one record per Android platform of API 24
                 or later of docs/implementation/validation/platforms/.

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
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import check_imports
import common

PLATFORMS = ("windows", "macos", "ios", "android", "linux", "web")
VARIANTS = ("legacy", "modern")
DEFAULT_WSL_DISTRO = "Ubuntu-24.04"

# Targets that do not exist yet and the task that will add each of them.
PENDING = {
    ("macos", "modern"): "T049",
    ("ios", "modern"): "T052",
    ("macos", "legacy"): "T077",
    ("ios", "legacy"): "T079",
}


def gcc_full_version(cxx):
    """Full version of a GCC executable: -dumpfullversion, or -dumpversion for GCC before 7 (4.8 of
    CentOS 5 has no -dumpfullversion)."""
    try:
        return capture([cxx, "-dumpfullversion"])
    except SystemExit:
        return capture([cxx, "-dumpversion"])


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
    else:
        # No os-release (CentOS 6): /etc/redhat-release, "CentOS release 6.10 (Final)", same rule
        # as PlatformId() of ui/legacy/linux-xlib/probe.cpp (fallback linux-platform-id-redhat-release).
        if os.path.exists("/etc/redhat-release"):
            with open("/etc/redhat-release", encoding="utf-8") as f:
                m = re.match(r"(\S+) release (\S+)", f.readline())
            if m:
                values["ID"], values["VERSION_ID"] = m.group(1), m.group(2)
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

    gxx_version = gcc_full_version("g++")
    gcc_version = gcc_full_version("gcc")
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
          % gcc_full_version("gcc"))
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
    version = gcc_full_version(cxx)
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
    try:
        shown = artifact.relative_to(common.ROOT).as_posix()
    except ValueError:  # the Web dist/ folder is in the build folder, outside the repository
        shown = str(artifact)
    print("artifact: %s (%d bytes)" % (shown, record["artifact"]["size"]))
    if not common.docs_present():
        print("WARNING: deliverable record %s NOT written: %s"
              % (name, common.docs_missing_message("the record is skipped, the build itself is complete")))
        return
    common.write_json(path, record)
    print("deliverable record: %s" % path.relative_to(common.ROOT).as_posix())


# ---- android legacy (T029, T081, T082) -----------------------------------------------------

ANDROID_LEGACY_ABIS = ("armeabi", "armeabi-v7a", "x86", "arm64-v8a")
# Libraries a library of the APK may need: the ones every Android has since 2.3.
ANDROID_ALLOWED_NEEDED = ("libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so")
# ABI -> (GCC toolchain directory of the NDK, tool prefix); the same table as the toolchain file.
ANDROID_GCC = {
    "armeabi": ("arm-linux-androideabi-4.9", "arm-linux-androideabi-"),
    "armeabi-v7a": ("arm-linux-androideabi-4.9", "arm-linux-androideabi-"),
    "x86": ("x86-4.9", "i686-linux-android-"),
    "arm64-v8a": ("aarch64-linux-android-4.9", "aarch64-linux-android-"),
}

ANDROID_INSTALL_HELP = """\
android legacy needs, on the Windows host (no WSL):
  LOOMORA_ANDROID_NDK   the Android NDK r15c directory (r16b starts at android-14 and ndk-build
                        raises API 9 to 14: only r15c has platforms/android-9, see ADR 0016)
  LOOMORA_ANDROID_SDK   (or ANDROID_HOME) the Android SDK with platforms/android-9 and build-tools
                        that have d8, aapt, zipalign and apksigner (sdkmanager "build-tools/36.0.0"
                        "platforms/android-9")
  JAVA_HOME             a JDK 8 or later (javac, keytool)
  cmake and ninja on PATH"""


def android_ndk_dir():
    ndk = os.environ.get("LOOMORA_ANDROID_NDK")
    if not ndk or not Path(ndk).is_dir():
        sys.exit("android legacy: LOOMORA_ANDROID_NDK is not set to an existing directory\n\n%s"
                 % ANDROID_INSTALL_HELP)
    props = Path(ndk) / "source.properties"
    revision = ""
    if props.is_file():
        m = re.search(r"Pkg\.Revision\s*=\s*(\S+)", props.read_text())
        revision = m.group(1) if m else ""
    if not revision.startswith("15."):
        sys.exit("android legacy: %s is NDK %s, not r15c (15.2.x)\n\n%s"
                 % (ndk, revision or "of unknown version", ANDROID_INSTALL_HELP))
    return Path(ndk), revision


def android_host_tag():
    return "windows-x86_64" if sys.platform == "win32" else "linux-x86_64"


def android_tool(ndk, abi, name):
    gcc_dir, prefix = ANDROID_GCC[abi]
    exe = ".exe" if sys.platform == "win32" else ""
    return str(ndk / "toolchains" / gcc_dir / "prebuilt" / android_host_tag() / "bin"
               / (prefix + name + exe))


def nm_symbols(ndk, abi, lib, flag):
    """Names of the dynamic symbols of a library listed by `nm -D <flag>`, with their type letter."""
    out = []
    for line in capture([android_tool(ndk, abi, "nm"), "-D", flag, lib]).splitlines():
        fields = line.split()
        if len(fields) >= 2:
            out.append((fields[-2], fields[-1]))
    return out


def android_check_library(ndk, abi, lib):
    """Facts about a built libloomora_jni.so; stops on a library or symbol it must not have."""
    needed = re.findall(r"Shared library: \[([^\]]+)\]",
                        capture([android_tool(ndk, abi, "readelf"), "-d", lib]))
    bad = [n for n in needed if n not in ANDROID_ALLOWED_NEEDED]
    if bad:
        sys.exit("%s: needs %s, which Android 2.3 does not provide (allowed: %s)"
                 % (lib, ", ".join(bad), ", ".join(ANDROID_ALLOWED_NEEDED)))
    exported = [name for kind, name in nm_symbols(ndk, abi, lib, "--defined-only") if kind == "T"]
    other = [e for e in exported if not e.startswith("Java_fr_bansan_loomora_probe_Native_")]
    if other:
        sys.exit("%s: exports %s besides the JNI functions" % (lib, ", ".join(other)))
    undefined = [name for kind, name in nm_symbols(ndk, abi, lib, "--undefined-only")
                 if kind in ("U", "w")]
    return needed, exported, undefined


def check_android_standards(build_dir):
    """The backend and the glue are compiled with -std=c++11 (compile_commands.json)."""
    commands = common.read_json(build_dir / "compile_commands.json")
    seen = 0
    for entry in commands:
        file = entry["file"].replace("\\", "/")
        if "/core/src/" in file or file.endswith("/bridge/jni/loomora_jni.cpp"):
            seen += 1
            if "-std=c++11" not in entry["command"]:
                sys.exit("%s is not compiled with -std=c++11" % file)
    if seen == 0:
        sys.exit("no backend source found in compile_commands.json")


def run_android_legacy(args):
    missing = [t for t in ("cmake", "ninja") if shutil.which(t) is None]
    if missing:
        sys.exit("android legacy: missing tool(s): %s\n\n%s"
                 % (", ".join(missing), ANDROID_INSTALL_HELP))
    ndk, revision = android_ndk_dir()
    sdk = os.environ.get("LOOMORA_ANDROID_SDK") or os.environ.get("ANDROID_HOME") or ""
    env_sdk = dict(os.environ, LOOMORA_ANDROID_SDK=sdk) if sdk else os.environ
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (common.ROOT / "build"))
    toolchain_file = common.ROOT / "cmake" / "toolchains" / "android-ndk-r15c.cmake"
    out_dir = common.ROOT / "out" / "android-legacy"
    libs = {}
    facts = {}
    for abi in ANDROID_LEGACY_ABIS:
        build_dir = build_root / ("android-legacy-" + abi)
        common.run(["cmake", "-S", common.ROOT, "-B", build_dir, "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain_file),
                    "-DLOOMORA_ANDROID_NDK=" + str(ndk), "-DANDROID_ABI=" + abi,
                    "-DLOOMORA_BUILD_TESTS=ON", "-DLOOMORA_BUILD_CATCH2_TESTS=OFF"])
        common.run(["cmake", "--build", build_dir])
        check_android_standards(build_dir)
        lib = build_dir / "ui" / "legacy" / "android-views" / "libloomora_jni.so"
        selftest = build_dir / "core" / "tests" / "loomora_selftest"
        for path in (lib, selftest):
            if not path.is_file():
                sys.exit("build finished but %s is missing" % path)
        facts[abi] = android_check_library(ndk, abi, lib)
        libs[abi] = lib
        target_dir = out_dir / "selftest" / abi
        target_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(str(selftest), str(target_dir / "loomora_selftest"))
        print("%s: needs %s; exports %d JNI function(s); %d undefined symbol(s), none missing "
              "from the API %d libraries (linked with --no-undefined)"
              % (abi, ", ".join(facts[abi][0]), len(facts[abi][1]), len(facts[abi][2]),
                 21 if abi == "arm64-v8a" else 9))
    apk = out_dir / "loomora.apk"
    cmd = [sys.executable, common.ROOT / "ui" / "legacy" / "android-views" / "build_apk.py",
           "--out", apk, "--workdir", build_root / "android-legacy-apk",
           "--keystore", build_root / "android-legacy-debug.keystore"]
    for abi in ANDROID_LEGACY_ABIS:
        cmd += ["--lib", "%s=%s" % (abi, libs[abi]), "--strip",
                "%s=%s" % (abi, android_tool(ndk, abi, "strip"))]
    res = subprocess.run([str(c) for c in cmd], env=env_sdk)
    if res.returncode != 0:
        sys.exit("build_apk.py failed (%d)" % res.returncode)
    clang = ndk / "toolchains" / "llvm" / "prebuilt" / android_host_tag() / "bin" / (
        "clang++.exe" if sys.platform == "win32" else "clang++")
    first_line = capture([clang, "--version"]).splitlines()[0]
    m = re.search(r"clang version (\S+)", first_line)
    clang_version = m.group(1) if m else first_line
    for record in android_legacy_records(apk, revision, clang_version, facts):
        write_deliverable(record, apk)


# Platform record arch -> the ABIs of the APK that run on it, and the target text.
ANDROID_PLATFORM_ABIS = {
    "x86": ("x86", "x86 (i686-none-linux-android, API 9 sysroot)"),
    "arm": ("armeabi, armeabi-v7a", "armeabi (armv5te-none-linux-androideabi) and armeabi-v7a "
            "(armv7-none-linux-androideabi), API 9 sysroot"),
    "arm64": ("arm64-v8a", "arm64-v8a (aarch64-none-linux-android, API 21 sysroot)"),
}


def android_platforms():
    """{platform id: record} of the Android platforms of docs/implementation/validation/platforms/."""
    found = {}
    if not common.docs_present():
        return found
    for path in sorted((common.VALIDATION / "platforms").glob("*.json")):
        rec = common.read_json(path)
        if isinstance(rec, dict) and rec.get("family") == "android" and rec.get("id"):
            found[rec["id"]] = rec
    return found


def android_legacy_records(apk, revision, clang_version, facts):
    """One Deliverable record per Android platform record: the same APK, named by each."""
    sha = common.sha256(apk)
    libs = "; ".join("%s: needs %s, %d JNI export(s)" % (abi, ", ".join(f[0]), len(f[1]))
                     for abi, f in sorted(facts.items()))
    for platform_id, platform_rec in sorted(android_platforms().items()):
        abis, target = ANDROID_PLATFORM_ABIS.get(platform_rec.get("arch"), (None, None))
        if abis is None:
            continue
        yield {
            "platform_id": platform_id,
            "ui_variant": "legacy",
            "cpp_std": "c++11",
            "compiler": "clang-%s-ndk-r15c" % clang_version,
            "target": target,
            "output": "native",
            "ui_language": "java",
            "ui_toolkit": "Android framework views (Activity, EditText, TextView, TextWatcher), "
                          "no AndroidX, built in code without resources; minSdkVersion 9, "
                          "targetSdkVersion 23; JNI glue bridge/jni/loomora_jni.cpp",
            "ui_compiler": "javac -source 8 -target 8 against android-9/android.jar, d8 --min-api 9 "
                           "(dex 035), aapt, zipalign, apksigner v1 only",
            "artifact": {"name": apk.name, "size": apk.stat().st_size},
            "disabled_features": [],
            "fallbacks": fallbacks_of(platform_id),
            "notes": "cmake + ninja with cmake/toolchains/android-ndk-r15c.cmake (NDK %s, Clang %s, "
                     "unified headers, --sysroot platforms/android-9, gnustl linked statically, no "
                     "ndk-build); static libloomora.a built from core/src with -std=c++11 and "
                     "libloomora_jni.so per ABI (%s); ONE universal APK holding libraries of the "
                     "four ABIs, named by every Android platform record; the ABIs of this "
                     "platform: %s; artifact sha256 %s; artifact copied to out/android-legacy/."
                     % (revision, clang_version, libs, abis, sha),
        }


# ---- android modern (T053, T054, T055) ------------------------------------------------------

ANDROID_MODERN_ABIS = ("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
ANDROID_MODERN_NDK = "30.0.16248370"
ANDROID_MODERN_ALLOWED_NEEDED = ("libc.so", "libm.so", "libdl.so")  # c++_static: no libc++_shared
ANDROID_MODERN_MIN_API = 24
# Platform record arch -> the ABI of the APK that runs on it.
ANDROID_MODERN_PLATFORM_ABI = {"x86": "x86", "x86_64": "x86_64", "arm": "armeabi-v7a",
                               "arm64": "arm64-v8a"}

ANDROID_MODERN_INSTALL_HELP = """\
android modern needs, on the Windows host (no WSL):
  LOOMORA_ANDROID_SDK   (or ANDROID_HOME) the Android SDK with platforms/android-37.0, build-tools
                        36.0.0, cmake/3.22.1 and ndk/30.0.16248370 (sdkmanager "platforms;android-37.0"
                        "build-tools;36.0.0" "cmake;3.22.1" "ndk;30.0.16248370")
  JAVA_HOME             a JDK 17 or later (JDK 25 was run with Gradle 9.9.1 and AGP 9.5.1)
  Gradle                the wrapper of ui/modern/android-compose (gradlew), which downloads Gradle
                        and the AndroidX / AGP artifacts from Google Maven and Maven Central"""


def android_modern_versions(project):
    """Pinned versions: gradle/libs.versions.toml and gradle/wrapper/gradle-wrapper.properties."""
    toml = (project / "gradle" / "libs.versions.toml").read_text(encoding="utf-8")
    versions = dict(re.findall(r'^(\w+)\s*=\s*"([^"]+)"', toml, re.M))
    props = (project / "gradle" / "wrapper" / "gradle-wrapper.properties").read_text(encoding="utf-8")
    m = re.search(r"gradle-([0-9.]+)-bin\.zip", props)
    versions["gradle"] = m.group(1) if m else "unknown"
    return versions


def android_modern_keystore(path, java_home):
    """A throwaway signing key, made once outside the repository (nothing secret, never committed)."""
    if path.is_file():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    keytool = Path(java_home) / "bin" / ("keytool.exe" if sys.platform == "win32" else "keytool")
    common.run([keytool, "-genkeypair", "-keystore", path, "-storepass", "android", "-keypass",
                "android", "-alias", "loomora", "-keyalg", "RSA", "-keysize", "2048", "-validity",
                "10000", "-dname", "CN=Loomora throwaway,O=Loomora,C=FR"])


def android_modern_check_library(llvm_bin, abi, lib):
    """Facts about a libloomora_jni.so of the APK; stops on a library, symbol or alignment it must not have."""
    exe = ".exe" if sys.platform == "win32" else ""
    needed = re.findall(r"Shared library: \[([^\]]+)\]",
                        capture([llvm_bin / ("llvm-readelf" + exe), "-d", lib]))
    bad = [n for n in needed if n not in ANDROID_MODERN_ALLOWED_NEEDED]
    if bad:
        sys.exit("%s (%s): needs %s (allowed: %s)"
                 % (lib, abi, ", ".join(bad), ", ".join(ANDROID_MODERN_ALLOWED_NEEDED)))
    defined = []
    undefined = []
    for line in capture([llvm_bin / ("llvm-nm" + exe), "-D", lib]).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2] == "T":
            defined.append(fields[-1])
        elif len(fields) >= 2 and fields[-2] in ("U", "w"):
            undefined.append(fields[-1])
    other = [e for e in defined if not e.startswith("Java_fr_bansan_loomora_probe_Native_")]
    if other:
        sys.exit("%s (%s): exports %s besides the JNI functions" % (lib, abi, ", ".join(other)))
    aligns = re.findall(r"LOAD\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+"
                        r"\s+\S+\s+(0x[0-9a-f]+)", capture([llvm_bin / ("llvm-readelf" + exe), "-lW", lib]))
    if not aligns or any(int(a, 16) < 16384 for a in aligns):
        sys.exit("%s (%s): a LOAD segment is aligned below 16 KB (%s)" % (lib, abi, aligns))
    return needed, defined, undefined


def run_android_modern(args):
    sdk_text = os.environ.get("LOOMORA_ANDROID_SDK") or os.environ.get("ANDROID_HOME") or \
        os.environ.get("ANDROID_SDK_ROOT") or ""
    if not sdk_text or not Path(sdk_text).is_dir():
        sys.exit("android modern: LOOMORA_ANDROID_SDK (or ANDROID_HOME) is not set to an existing "
                 "directory\n\n%s" % ANDROID_MODERN_INSTALL_HELP)
    sdk = Path(sdk_text)
    ndk = sdk / "ndk" / ANDROID_MODERN_NDK
    java_home = os.environ.get("JAVA_HOME")
    if not java_home or not Path(java_home).is_dir():
        sys.exit("android modern: JAVA_HOME is not set to an existing JDK\n\n%s"
                 % ANDROID_MODERN_INSTALL_HELP)
    needed_dirs = [ndk, sdk / "cmake" / "3.22.1", sdk / "platforms" / "android-37.0"]
    missing = [str(p) for p in needed_dirs if not p.is_dir()]
    if missing:
        sys.exit("android modern: missing %s\n\n%s" % (", ".join(missing), ANDROID_MODERN_INSTALL_HELP))
    project = common.ROOT / "ui" / "modern" / "android-compose"
    versions = android_modern_versions(project)
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (common.ROOT / "build"))
    keystore = build_root / "android-modern-throwaway.keystore"
    android_modern_keystore(keystore, java_home)
    env = dict(os.environ, ANDROID_HOME=str(sdk), LOOMORA_ANDROID_SDK=str(sdk))
    gradlew = project / ("gradlew.bat" if sys.platform == "win32" else "gradlew")
    cmd = [gradlew, "assembleRelease", "-Ploomora.keystore=" + str(keystore),
           "-Ploomora.storepass=android", "-Ploomora.keyalias=loomora"]
    if args.min_sdk:
        cmd.append("-Ploomora.minSdk=%d" % args.min_sdk)
    res = subprocess.run([str(c) for c in cmd], cwd=str(project), env=env)
    if res.returncode != 0:
        sys.exit("gradlew assembleRelease failed (%d)" % res.returncode)
    built = project / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
    if not built.is_file():
        sys.exit("gradle finished but %s is missing (not signed?)" % built)
    out_dir = common.ROOT / "out" / "android-modern"
    out_dir.mkdir(parents=True, exist_ok=True)
    apk = out_dir / ("loomora-minsdk%d.apk" % args.min_sdk if args.min_sdk else "loomora.apk")
    shutil.copyfile(str(built), str(apk))
    if args.min_sdk:
        print("minSdk %d build (spike S8): %s, %d bytes; no deliverable record is written"
              % (args.min_sdk, apk, apk.stat().st_size))
        return

    # Checks on what was really built: the compile commands, then the libraries inside the APK.
    for abi in ANDROID_MODERN_ABIS:
        found = sorted((project / "app" / ".cxx").glob("RelWithDebInfo/*/%s/compile_commands.json" % abi))
        if not found:
            sys.exit("no compile_commands.json for %s under %s" % (abi, project / "app" / ".cxx"))
        check_android_standards(found[-1].parent)
    llvm_bin = ndk / "toolchains" / "llvm" / "prebuilt" / android_host_tag() / "bin"
    facts = {}
    with tempfile.TemporaryDirectory() as scratch:
        with zipfile.ZipFile(str(apk)) as z:
            names = z.namelist()
            for abi in ANDROID_MODERN_ABIS:
                member = "lib/%s/libloomora_jni.so" % abi
                if member not in names:
                    sys.exit("%s is not in the APK" % member)
                target = Path(scratch) / abi / "libloomora_jni.so"
                target.parent.mkdir(parents=True)
                target.write_bytes(z.read(member))
                facts[abi] = android_modern_check_library(llvm_bin, abi, target)
                print("%s: needs %s; exports %d JNI function(s); %d undefined symbol(s); LOAD "
                      "segments aligned to 16 KB or more; -std=c++11 for the backend and the glue"
                      % (abi, ", ".join(facts[abi][0]), len(facts[abi][1]), len(facts[abi][2])))
            others = sorted(n for n in names if n.startswith("lib/") and not n.endswith("/libloomora_jni.so"))
            print("other native libraries in the APK: %s" % (", ".join(others) or "none"))
    bt = sorted((sdk / "build-tools").iterdir())[-1]
    exe = ".exe" if sys.platform == "win32" else ""
    print(capture([bt / ("apksigner" + (".bat" if sys.platform == "win32" else "")), "verify",
                   "--verbose", "--min-sdk-version", str(ANDROID_MODERN_MIN_API), apk]))
    print(capture([bt / ("zipalign" + exe), "-c", "-P", "16", "-v", "4", apk]).splitlines()[-1])
    clang = llvm_bin / ("clang++" + exe)
    m = re.search(r"clang version (\S+)", capture([clang, "--version"]).splitlines()[0])
    clang_version = m.group(1) if m else "unknown"
    for record in android_modern_records(apk, clang_version, versions, facts, java_home):
        write_deliverable(record, apk)


def android_api_of(platform_rec):
    m = re.search(r"API (\d+)", platform_rec.get("os_version", ""))
    return int(m.group(1)) if m else 0


def android_modern_records(apk, clang_version, versions, facts, java_home):
    """One Deliverable record per Android platform record of API 24 or later (the Modern floor)."""
    sha = common.sha256(apk)
    libs = "; ".join("%s: needs %s, %d JNI export(s)" % (abi, ", ".join(f[0]), len(f[1]))
                     for abi, f in sorted(facts.items()))
    for platform_id, rec in sorted(android_platforms().items()):
        abi = ANDROID_MODERN_PLATFORM_ABI.get(rec.get("arch"))
        if abi is None or android_api_of(rec) < ANDROID_MODERN_MIN_API:
            continue
        yield {
            "platform_id": platform_id,
            "ui_variant": "modern",
            "cpp_std": "c++11",
            "compiler": "clang-%s-ndk-r30" % clang_version,
            "target": "%s (API %d, NDK r30, libc++ linked statically)" % (abi, ANDROID_MODERN_MIN_API),
            "output": "native",
            "ui_language": "kotlin",
            "ui_toolkit": "Jetpack Compose (Material 3), Compose BOM %s, activity-compose %s; "
                          "minSdk %d, targetSdk 37, compileSdk 37; JNI glue bridge/jni/loomora_jni.cpp"
                          % (versions["composeBom"], versions["activityCompose"],
                             ANDROID_MODERN_MIN_API),
            "ui_compiler": "Kotlin %s (Compose compiler plugin), Android Gradle Plugin %s, Gradle %s, "
                           "R8 minify and resource shrinking, JDK %s, apksigner v2/v3 with a "
                           "throwaway key" % (versions["kotlin"], versions["agp"], versions["gradle"],
                                              Path(java_home).name),
            "artifact": {"name": apk.name, "size": apk.stat().st_size},
            "disabled_features": [],
            "fallbacks": fallbacks_of(platform_id),
            "notes": "gradlew assembleRelease (ui/modern/android-compose), externalNativeBuild with "
                     "CMake 3.22.1 and NDK %s (Clang %s, ANDROID_STL c++_static) for the C++11 "
                     "backend (static libloomora.a, target property CXX_STANDARD 11) and "
                     "libloomora_jni.so per ABI (%s); ONE universal APK holding the four ABIs "
                     "armeabi-v7a, arm64-v8a, x86, x86_64; the ABI of this platform: %s; artifact "
                     "sha256 %s; artifact copied to out/android-modern/."
                     % (ANDROID_MODERN_NDK, clang_version, libs, abi, sha),
        }


# ---- windows modern (T041, T046) ------------------------------------------------------------

WINUI_DIR = common.ROOT / "ui" / "modern" / "windows-winui3"
WINUI_PROJECT = WINUI_DIR / "Loomora.WinUI.csproj"
# short name -> target framework: the same sources, two .NET targets (net8 only for Windows 10 1809).
WINUI_TARGETS = (("net8", "net8.0-windows10.0.19041.0"), ("net10", "net10.0-windows10.0.19041.0"))
# architecture -> (runtime identifier, CMake preset of loomora.dll, MSVC host/target folder)
WINUI_ARCHS = (("x64", "win-x64", "host-msvc", "Hostx64/x64"),
               ("arm64", "win-arm64", "host-msvc-arm64", "Hostx64/arm64"))
WINUI_ARCHS_PRESET = dict((arch, preset) for arch, _rid, preset, _host in WINUI_ARCHS)
# Where each Windows platform takes its deliverable: Windows 11 runs the recent target, a
# Windows 10 platform (none is recorded yet) would run the .NET 8 fallback.
WINUI_PLATFORM_TARGET = {"Windows 11": "net10", "Windows 10": "net8"}
WINUI_ARCH_OF_PLATFORM = {"x86_64": "x64", "arm64": "arm64"}

WINUI_INSTALL_HELP = """\
windows modern needs, on the Windows host (no WSL):
  Visual Studio 2026 (Community is enough) with the C++ desktop workload and, for ARM64, the
  component "MSVC Build Tools for ARM64/ARM64EC (Latest)"; cmake on PATH (generator
  "Visual Studio 18 2026", CMake 4.2 or later)
  the .NET SDK named by ui/modern/windows-winui3/global.json (10.0.404 or a later 10.0 patch);
  the packs of the two target frameworks and the Windows App SDK package come from NuGet
  (network access, or a filled NuGet cache)
  to RUN the result: the .NET Desktop Runtime of the target (8 or 10, x64 or ARM64) and the
  Windows App Runtime 2.6 (Microsoft.WindowsAppRuntime.2, same architecture); see
  docs/implementation/validation/procedures/windows.md"""


def vswhere_install_path():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        sys.exit("windows modern: vswhere.exe not found (Visual Studio 2026 is not installed?)\n\n%s"
                 % WINUI_INSTALL_HELP)
    path = capture([vswhere, "-latest", "-products", "*", "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                    "installationPath"])
    if not path:
        sys.exit("windows modern: no Visual Studio with the MSVC x64 tools\n\n%s" % WINUI_INSTALL_HELP)
    return Path(path)


def latest_msvc_dir(vs_path):
    versions = sorted((vs_path / "VC" / "Tools" / "MSVC").iterdir(),
                      key=lambda p: [int(x) for x in p.name.split(".")])
    if not versions:
        sys.exit("windows modern: no MSVC toolset under %s" % vs_path)
    return versions[-1]


def cl_version(cl):
    """Version of cl.exe, from its banner (written on standard error)."""
    res = subprocess.run([str(cl)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, errors="replace")
    m = re.search(r"(\d+\.\d+\.\d+(?:\.\d+)?)", res.stdout.splitlines()[0] if res.stdout else "")
    return m.group(1) if m else "unknown"


def header_functions():
    text = (common.CORE / "include" / "loomora.h").read_text(encoding="utf-8")
    return sorted(set(re.findall(r"LOOMORA_API[^;(]*?\b(loomora_\w+)\s*\(", text)))


def build_loomora_dll(preset, msvc):
    """Configure and build the preset of CMakePresets.json, then check the DLL: it exports exactly
    the loomora_* functions of the header and imports KERNEL32 only (the C runtime is static, /MT)."""
    for step in (["--preset", preset], ["--build", "--preset", preset]):
        res = subprocess.run(["cmake"] + step, cwd=str(common.ROOT))
        if res.returncode != 0:
            sys.exit("cmake %s failed (%d)" % (" ".join(step), res.returncode))
    dll = common.ROOT / "build" / preset / "core" / "Release" / "loomora.dll"
    if not dll.is_file():
        sys.exit("cmake finished but %s is missing" % dll)
    exports, by_ordinal = check_imports.read_exports(dll)
    expected = set(header_functions())
    if exports != expected or by_ordinal:
        sys.exit("%s exports %s, expected exactly %s" % (dll, sorted(exports), sorted(expected)))
    dumpbin = msvc / "bin" / "Hostx64" / "x64" / "dumpbin.exe"
    text = capture([dumpbin, "/headers", "/dependents", dll])
    machine = re.search(r"machine \(([^)]+)\)|\(([^)]+)\) machine", text)
    needed = sorted(set(re.findall(r"^\s+(\S+\.dll)\s*$", text, re.M | re.I)), key=str.lower)
    if needed != ["KERNEL32.dll"]:
        sys.exit("%s imports %s, expected KERNEL32.dll only (static C runtime)" % (dll, needed))
    machine = (machine.group(1) or machine.group(2)) if machine else "unknown machine"
    print("%s: %s, exports exactly the %d loomora_* functions, imports %s, %d bytes"
          % (dll.relative_to(common.ROOT).as_posix(), machine, len(exports), ", ".join(needed),
             dll.stat().st_size))
    return dll, machine


def folder_facts(folder):
    files = [p for p in folder.rglob("*") if p.is_file()]
    return {"files": len(files), "bytes": sum(p.stat().st_size for p in files)}


def zip_folder(folder, archive):
    """Zip the published folder (files at the root of the archive, a fixed time stamp so that
    identical folders give identical archives)."""
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(str(archive), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(p for p in folder.rglob("*") if p.is_file()):
            info = zipfile.ZipInfo(path.relative_to(folder).as_posix(), (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, path.read_bytes())


def winui_package_versions(assets):
    """({package id: version} of the packages the last restore resolved,
    {target framework: {pack id: version}} of the packs it downloaded: runtime packs, Windows SDK
    projection)."""
    data = common.read_json(assets)
    libraries = {}
    for key in data.get("libraries", {}):
        name, _, version = key.partition("/")
        libraries[name] = version
    packs = {}
    for tf, info in data.get("project", {}).get("frameworks", {}).items():
        packs[tf] = dict((d["name"], d["version"].strip("[]").split(",")[0].strip())
                         for d in info.get("downloadDependencies", []))
    return libraries, packs


def publish_winui(tf_short, tf, arch, rid, dll, build_root):
    out = build_root / "windows-modern" / ("%s-%s" % (tf_short, arch))
    if out.exists():
        shutil.rmtree(str(out))
    cmd = ["dotnet", "publish", str(WINUI_PROJECT), "-c", "Release", "-f", tf, "-r", rid,
           "-p:Platform=" + arch, "-p:LoomoraDll=" + str(dll), "-o", str(out), "--nologo"]
    res = subprocess.run(cmd, cwd=str(WINUI_DIR))
    if res.returncode != 0:
        sys.exit("dotnet publish failed (%d): %s" % (res.returncode, " ".join(cmd)))
    for needed in ("Loomora.WinUI.exe", "Loomora.WinUI.dll", "loomora.dll",
                   "Loomora.WinUI.runtimeconfig.json", "Microsoft.WindowsAppRuntime.Bootstrap.dll"):
        if not (out / needed).is_file():
            sys.exit("%s is missing from the published folder %s" % (needed, out))
    machine = {"x64": 0x8664, "arm64": 0xAA64}[arch]
    for native in ("Loomora.WinUI.exe", "loomora.dll", "Microsoft.WindowsAppRuntime.Bootstrap.dll"):
        header = (out / native).read_bytes()[:4096]
        found = struct.unpack_from("<H", header, struct.unpack_from("<I", header, 0x3C)[0] + 4)[0]
        if found != machine:
            sys.exit("%s of %s has machine type 0x%04X, expected 0x%04X (%s)"
                     % (native, out, found, machine, arch))
    facts = folder_facts(out)
    archive = common.ROOT / "out" / "windows-modern" / ("loomora-winui-%s-%s.zip" % (tf_short, arch))
    zip_folder(out, archive)
    facts.update(folder=out, archive=archive, zip_bytes=archive.stat().st_size,
                 zip_sha256=common.sha256(archive), tf=tf, arch=arch)
    print("%s %s: published folder %d files, %d bytes; %s %d bytes (sha256 %s)"
          % (tf_short, arch, facts["files"], facts["bytes"], archive.name, facts["zip_bytes"],
             facts["zip_sha256"]))
    return facts


def run_windows_modern(args):
    if sys.platform != "win32":
        sys.exit("windows modern builds on a Windows host (PowerShell or Python), not in WSL\n\n%s"
                 % WINUI_INSTALL_HELP)
    require_tools(["cmake", "dotnet"])
    vs_path = vswhere_install_path()
    msvc = latest_msvc_dir(vs_path)
    msvc_version = cl_version(msvc / "bin" / "Hostx64" / "x64" / "cl.exe")
    dotnet_version = capture(["dotnet", "--version"], cwd=WINUI_DIR)
    print("Visual Studio: %s; MSVC toolset %s (cl.exe %s); .NET SDK %s (ui/modern/windows-winui3/"
          "global.json)" % (vs_path, msvc.name, msvc_version, dotnet_version))
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (common.ROOT / "build"))
    built = {}  # architecture -> {"dll", "machine", "targets": {short name: facts}}
    for arch, rid, preset, host in WINUI_ARCHS:
        if not (msvc / "bin" / host.replace("/", os.sep) / "cl.exe").is_file():
            print("%s: SKIPPED, the MSVC tools for %s are not installed (%s/cl.exe is missing); "
                  "install the component \"MSVC Build Tools for ARM64/ARM64EC (Latest)\" of "
                  "Visual Studio" % (arch, arch, host))
            continue
        dll, machine = build_loomora_dll(preset, msvc)
        entry = {"dll": dll, "machine": machine, "targets": {}}
        for tf_short, tf in WINUI_TARGETS:
            entry["targets"][tf_short] = publish_winui(tf_short, tf, arch, rid, dll, build_root)
        built[arch] = entry
    if not built:
        sys.exit("windows modern: nothing was built")
    assets = WINUI_DIR / "obj" / "project.assets.json"
    packages = winui_package_versions(assets) if assets.is_file() else ({}, {})
    for record, artifact in windows_modern_records(built, windows_platforms(), msvc.name,
                                                   msvc_version, dotnet_version, packages):
        write_deliverable(record, artifact)


def windows_modern_records(built, platforms, msvc_toolset, msvc_version, dotnet_version, packages):
    """[(record, artifact)]: one Deliverable per Windows platform record whose architecture was
    built (Windows 11: the net10 zip, Windows 10: the net8 zip), and a generic record
    windows-<arch>-modern for an architecture without any platform record (the validator reports
    it as a deliverable without platform until the platform record exists)."""
    libraries, packs = packages
    wasdk = libraries.get("Microsoft.WindowsAppSDK.WinUI", "unknown")

    def make(platform_id, arch, tf_short):
        entry = built[arch]
        facts = entry["targets"][tf_short]
        other = ["%s: %s, %d bytes, sha256 %s, published folder %d files / %d bytes"
                 % (s, f["archive"].name, f["zip_bytes"], f["zip_sha256"], f["files"], f["bytes"])
                 for s, f in sorted(entry["targets"].items()) if s != tf_short]
        record = {
            "platform_id": platform_id,
            "ui_variant": "modern",
            "cpp_std": "c++11",
            "compiler": "msvc-%s" % msvc_version,
            "target": "%s (loomora.dll, MSVC toolset %s, /MT static C runtime, cl.exe default "
                      "/std:c++14 for the C++11 sources, the lowest it accepts; %s)"
                      % (arch, msvc_toolset, entry["machine"]),
            "output": "native",
            "ui_language": "c#",
            "ui_toolkit": "WinUI 3, Windows App SDK %s (packages Microsoft.WindowsAppSDK.WinUI and "
                          ".Runtime, Base, Foundation and InteractiveExperiences with them, not the "
                          "Microsoft.WindowsAppSDK metapackage), unpackaged and framework-dependent "
                          "(WindowsPackageType None, WindowsAppSDKSelfContained false), %s, "
                          "TargetPlatformMinVersion 10.0.17763.0; loomora.dll called through "
                          "LibraryImport (no C++/CLI, no C++/WinRT)" % (wasdk, facts["tf"]),
            "ui_compiler": ".NET SDK %s (pinned by ui/modern/windows-winui3/global.json), Roslyn, "
                           "LibraryImport source generator, Microsoft.Windows.SDK.NET.Ref %s, "
                           "Microsoft.Windows.SDK.BuildTools %s, Microsoft.Windows.SDK.BuildTools.MSIX "
                           "%s, Microsoft.Web.WebView2 %s, runtime pack Microsoft.NETCore.App %s"
                           % (dotnet_version,
                              packs.get(facts["tf"], {}).get("Microsoft.Windows.SDK.NET.Ref", "unknown"),
                              libraries.get("Microsoft.Windows.SDK.BuildTools", "unknown"),
                              libraries.get("Microsoft.Windows.SDK.BuildTools.MSIX", "unknown"),
                              libraries.get("Microsoft.Web.WebView2", "unknown"),
                              packs.get(facts["tf"], {}).get("Microsoft.NETCore.App.Runtime.win-" + arch, "unknown")),
            "artifact": {"name": facts["archive"].name, "size": facts["zip_bytes"]},
            "disabled_features": [],
            # selftest-no-iostream is a MinGW-w64 (Windows Legacy) fallback: platform ids carry no variant
            "fallbacks": [f for f in fallbacks_of(platform_id) if f != "selftest-no-iostream"],
            "notes": "python tools/build.py windows modern: cmake --preset %s (Visual Studio 18 "
                     "2026 generator; loomora.dll exports exactly the loomora_* functions of "
                     "loomora.h and imports KERNEL32 only) then dotnet publish of "
                     "ui/modern/windows-winui3 for %s; the zip holds the published folder "
                     "(%d files, %d bytes unzipped) with loomora.dll next to Loomora.WinUI.exe; "
                     "artifact sha256 %s; needs the .NET Desktop Runtime (%s) and the Windows App "
                     "Runtime %s installed on the machine (validation/procedures/windows.md). "
                     "Same build, other target: %s. Artifacts copied to out/windows-modern/."
                     % (WINUI_ARCHS_PRESET[arch], facts["tf"], facts["files"], facts["bytes"],
                        facts["zip_sha256"], facts["tf"].split("-")[0].replace("net", ".NET "),
                        ".".join(wasdk.split(".")[:2]), "; ".join(other)),
        }
        return record, facts["archive"]

    out = []
    covered = set()
    for platform_id, rec in sorted(platforms.items()):
        arch = WINUI_ARCH_OF_PLATFORM.get(rec.get("arch"))
        short = next((s for name, s in WINUI_PLATFORM_TARGET.items()
                      if rec.get("os_name", "").startswith(name)), None)
        if arch not in built or short is None:
            continue
        covered.add(arch)
        out.append(make(platform_id, arch, short))
    for arch in sorted(built):
        if arch not in covered:
            out.append(make("windows-%s" % arch, arch, "net10"))
    return out


# ---- web legacy (T085, T086) ---------------------------------------------------------------

WEB_LEGACY_UI = Path("ui") / "legacy" / "web"
WEB_LEGACY_PAGE_FILES = ("index.html", "app.js")
ES_LEVEL = Path("tools") / "trials" / "web-node" / "es-level.js"
# Web platform records (a browser on an operating system) and the path the Legacy page takes there:
# a browser with no WebAssembly runs the JavaScript build (loomora.js), the others the wasm one.
WEB_PLATFORM_PATH = {"web-firefox-10-windows-2000-x86": "js"}


def web_emsdk_dirs():
    home = Path.home()
    return (Path(os.environ.get("LOOMORA_EMSDK") or (home / "emsdk")),
            Path(os.environ.get("LOOMORA_EMSDK_FASTCOMP") or (home / "emsdk-fastcomp")))


def web_run(cmd, env_dir, cwd, fastcomp, log):
    """Run `cmd` with Emscripten activated; the output goes to `log` (a Path); stops on failure.

    The current emsdk is activated by sourcing its emsdk_env.sh. The fastcomp one (1.39.20) is
    activated by what its emsdk_env.sh does not set: EM_CONFIG (its .emscripten file) and the PATH
    of emcc and of its node 14."""
    env = dict(os.environ)
    argv = [str(c) for c in cmd]
    if fastcomp:
        nodes = sorted(env_dir.glob("node/*/bin"))
        env["EM_CONFIG"] = str(env_dir / ".emscripten")
        env["PATH"] = os.pathsep.join(
            [str(env_dir / "fastcomp" / "emscripten")] + [str(n) for n in nodes] + [env["PATH"]])
        full = argv
    else:
        full = ["bash", "-c", 'source "$1/emsdk_env.sh" >/dev/null 2>&1 || exit 97; shift; exec "$@"',
                "bash", str(env_dir)] + argv
    res = subprocess.run(full, cwd=str(cwd), env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, universal_newlines=True)
    with open(str(log), "a", encoding="utf-8", newline="\n") as f:
        f.write("$ %s\n%s\n" % (" ".join(argv), res.stdout))
    if res.returncode != 0:
        sys.exit("web build: command failed (%d), see %s:\n%s\n%s"
                 % (res.returncode, log, " ".join(argv), res.stdout[-2000:]))
    return res.stdout


def web_build_module(kind, toolchain, env_dir, fastcomp, root):
    """Configure, build and test one Web module; returns its build directory."""
    build = root / kind
    log = root / ("%s.log" % kind)
    if log.exists():
        log.unlink()
    web_run(["cmake", "-S", common.ROOT, "-B", build, "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=%s" % (common.ROOT / "cmake" / "toolchains" / toolchain),
             "-DCMAKE_BUILD_TYPE=Release", "-DLOOMORA_BUILD_CATCH2_TESTS=OFF"],
            env_dir, root, fastcomp, log)
    web_run(["cmake", "--build", build], env_dir, root, fastcomp, log)
    out = web_run(["ctest", "--output-on-failure"], env_dir, build, fastcomp, log)
    tail = [l for l in out.splitlines() if "tests passed" in l]
    print("%s: built, self-test: %s (log %s)" % (kind, tail[-1].strip() if tail else "?", log))
    return build


def web_es_level(acorn_dir, js_file):
    """Lowest ECMAScript edition that parses js_file, as es-level.js prints it."""
    res = subprocess.run(["node", str(common.ROOT / ES_LEVEL), str(acorn_dir), str(js_file)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    for line in res.stdout.splitlines():
        if line.startswith("lowest edition that parses:"):
            return line.split(":", 1)[1].strip()
    return "does not parse (%s)" % res.stdout.strip()[-200:]


def run_web_legacy(args):
    if sys.platform == "win32":
        rerun_in_wsl(args, "web", "legacy")
        return
    if not in_wsl_or_linux():
        sys.exit("web legacy cannot be built on this host (%s): use Linux or Windows with WSL"
                 % sys.platform)
    require_tools(["cmake", "ninja", "node", "bash"])
    emsdk, fastcomp = web_emsdk_dirs()
    if not (emsdk / "emsdk_env.sh").is_file():
        sys.exit("web legacy: the current emsdk is not at %s (set LOOMORA_EMSDK; it builds the "
                 "WebAssembly path)" % emsdk)
    if not (fastcomp / ".emscripten").is_file():
        sys.exit("web legacy: the Emscripten 1.39.20 fastcomp emsdk is not at %s (set "
                 "LOOMORA_EMSDK_FASTCOMP; it builds the JavaScript path, spike S5)" % fastcomp)
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    root = build_root / "web-legacy"
    root.mkdir(parents=True, exist_ok=True)

    wasm_dir = web_build_module("wasm", "emscripten-wasm.cmake", emsdk, False, root)
    js_dir = web_build_module("js", "emscripten-js-fastcomp.cmake", fastcomp, True, root)

    dist = root / "dist"
    if dist.exists():
        shutil.rmtree(str(dist))
    dist.mkdir()
    ui = common.ROOT / WEB_LEGACY_UI
    for name in WEB_LEGACY_PAGE_FILES:
        shutil.copyfile(str(ui / name), str(dist / name))
    if (js_dir / "bridge" / "wasm" / "loomora.js.mem").exists():
        sys.exit("web legacy: the JavaScript build wrote loomora.js.mem (--memory-init-file 0 lost?)")
    shutil.copyfile(str(js_dir / "bridge" / "wasm" / "loomora.js"), str(dist / "loomora.js"))
    shutil.copyfile(str(wasm_dir / "bridge" / "wasm" / "loomora.js"), str(dist / "loomora-wasm.js"))
    shutil.copyfile(str(wasm_dir / "bridge" / "wasm" / "loomora.wasm"), str(dist / "loomora.wasm"))

    acorn = fastcomp / "fastcomp" / "emscripten" / "node_modules" / "acorn"
    problems = []
    # loomora-wasm.js is not checked: it is modern JavaScript on purpose (only browsers that have
    # WebAssembly load it) and the acorn of the fastcomp emsdk does not finish on it.
    for name in ("app.js", "loomora.js"):
        level = web_es_level(acorn, dist / name)
        print("%-16s lowest ECMAScript edition that parses: %s" % (name, level))
        if level != "ecmaVersion 3":
            problems.append("%s parses only as %s (ES3 required)" % (name, level))
    for p in sorted(dist.iterdir()):
        print("%8d  %s" % (p.stat().st_size, p.name))
    if problems:
        sys.exit("web legacy: " + "; ".join(problems))
    print("dist: %s" % dist)
    for platform_id, path in sorted(WEB_PLATFORM_PATH.items()):
        if not common.docs_present() or not (common.VALIDATION / "platforms" / (platform_id + ".json")).is_file():
            print("no platform record for %s: no deliverable record written for it" % platform_id)
            continue
        names = ("index.html", "app.js", "loomora.js") if path == "js" else (
            "index.html", "app.js", "loomora-wasm.js", "loomora.wasm")
        compiler = ("emscripten-1.39.20-fastcomp (clang 6.0.1)" if path == "js"
                    else "emscripten-7.0.0")
        record = {
            "platform_id": platform_id, "ui_variant": "legacy", "cpp_std": "c++11",
            "compiler": compiler,
            "target": "asmjs-unknown-emscripten" if path == "js" else "wasm32-unknown-emscripten",
            "output": path,
            "ui_language": "javascript (ECMAScript 3 syntax), html",
            "ui_toolkit": "one HTML page (a one-row text area and a div) and one script, DOM only, no framework; "
                          "the backend is called through ccall / cwrap",
            "ui_compiler": "none: ui/legacy/web/app.js is served as written (tools/build.py checks "
                           "that it parses as ECMAScript 3)",
            "artifact": {"name": "dist/ (%s)" % ", ".join(names),
                         "size": sum((dist / n).stat().st_size for n in names)},
            "disabled_features": [],
            "fallbacks": fallbacks_of(platform_id),
            "notes": ("Deliverable of platform %s (validation/platforms/%s.json), the %s path of the "
                      "page. Built by python tools/build.py web legacy; the files are in "
                      "$HOME/loomora-build/web-legacy/dist/ (never in the repository tree); sha256 %s. "
                      "The same dist/ folder also holds the files of the other path; the page picks "
                      "one at run time (typeof WebAssembly)."
                      % (platform_id, platform_id, path,
                         ", ".join("%s %s" % (n, common.sha256(dist / n)) for n in names)))}
        write_deliverable(record, dist / names[-1])


# ---- web modern (T038-T040) -----------------------------------------------------------------

WEB_MODERN_UI = Path("ui") / "modern" / "web-ts"
WEB_MODERN_PAGE_FILES = ("index.html",)
WEB_MODERN_TS_INPUTS = ("package.json", "package-lock.json", "tsconfig.json")
WEB_MODERN_TS_OUTPUTS = ("main.js", "native.js")
# Web platform records (a current browser) the Modern page is delivered for: the page only has the wasm path.
WEB_MODERN_PLATFORMS = {"web-chrome-157-windows-11-x64": "wasm"}


def web_plain_run(cmd, cwd, log):
    """Run `cmd` as it is (no Emscripten environment: npm and tsc use the system node); the output
    goes to `log`; stops on failure."""
    argv = [str(c) for c in cmd]
    res = subprocess.run(argv, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True)
    with open(str(log), "a", encoding="utf-8", newline="\n") as f:
        f.write("$ %s\n%s\n" % (" ".join(argv), res.stdout))
    if res.returncode != 0:
        sys.exit("web build: command failed (%d), see %s:\n%s\n%s"
                 % (res.returncode, log, " ".join(argv), res.stdout[-2000:]))
    return res.stdout


def run_web_modern(args):
    if sys.platform == "win32":
        rerun_in_wsl(args, "web", "modern")
        return
    if not in_wsl_or_linux():
        sys.exit("web modern cannot be built on this host (%s): use Linux or Windows with WSL"
                 % sys.platform)
    require_tools(["cmake", "ninja", "node", "npm", "bash"])
    emsdk, _ = web_emsdk_dirs()
    if not (emsdk / "emsdk_env.sh").is_file():
        sys.exit("web modern: the current emsdk is not at %s (set LOOMORA_EMSDK)" % emsdk)
    ui = common.ROOT / WEB_MODERN_UI
    for name in WEB_MODERN_TS_INPUTS:
        if not (ui / name).is_file():
            sys.exit("web modern: %s is missing (a lock file is made by `npm install` in "
                     "a copy of the folder, see tools/build.py)" % (WEB_MODERN_UI / name))
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    root = build_root / "web-modern"
    root.mkdir(parents=True, exist_ok=True)

    wasm_dir = web_build_module("wasm", "emscripten-wasm.cmake", emsdk, False, root)

    # TypeScript: built in a copy, so that node_modules is not made inside the repository tree
    ts = root / "ts"
    log = root / "ts.log"
    if ts.exists():
        shutil.rmtree(str(ts))
    if log.exists():
        log.unlink()
    ts.mkdir()
    for name in WEB_MODERN_TS_INPUTS:
        shutil.copyfile(str(ui / name), str(ts / name))
    shutil.copytree(str(ui / "src"), str(ts / "src"))
    web_plain_run(["npm", "ci", "--no-audit", "--no-fund"], ts, log)
    version = web_plain_run([str(ts / "node_modules" / ".bin" / "tsc"), "--version"], ts, log).strip()
    web_plain_run([str(ts / "node_modules" / ".bin" / "tsc"), "-p", "tsconfig.json"], ts, log)
    print("typescript: %s (log %s)" % (version, log))

    dist = root / "dist"
    if dist.exists():
        shutil.rmtree(str(dist))
    dist.mkdir()
    for name in WEB_MODERN_PAGE_FILES:
        shutil.copyfile(str(ui / name), str(dist / name))
    for name in WEB_MODERN_TS_OUTPUTS:
        shutil.copyfile(str(ts / "dist" / name), str(dist / name))
    shutil.copyfile(str(wasm_dir / "bridge" / "wasm" / "loomora.js"), str(dist / "loomora.js"))
    shutil.copyfile(str(wasm_dir / "bridge" / "wasm" / "loomora.wasm"), str(dist / "loomora.wasm"))
    extra = sorted(p.name for p in (ts / "dist").iterdir() if p.name not in WEB_MODERN_TS_OUTPUTS)
    if extra:
        sys.exit("web modern: tsc wrote files that are not served: %s" % ", ".join(extra))
    for p in sorted(dist.iterdir()):
        print("%8d  %s" % (p.stat().st_size, p.name))
    print("dist: %s" % dist)
    emscripten = (emsdk / "upstream" / "emscripten" / "emscripten-version.txt").read_text().strip().strip('"')
    names = ("index.html",) + WEB_MODERN_TS_OUTPUTS + ("loomora.js", "loomora.wasm")
    for platform_id in sorted(WEB_MODERN_PLATFORMS):
        if not common.docs_present() or not (common.VALIDATION / "platforms" / (platform_id + ".json")).is_file():
            print("no platform record for %s: no deliverable record written for it" % platform_id)
            continue
        record = {
            "platform_id": platform_id, "ui_variant": "modern", "cpp_std": "c++11",
            "compiler": "emscripten-" + emscripten,
            "target": "wasm32-unknown-emscripten",
            "output": "wasm",
            "ui_language": "typescript (ES modules), html, css",
            "ui_toolkit": "one HTML page (a textarea and an output element, CSS with prefers-color-scheme) and two "
                          "ES modules, DOM only, no framework and no runtime dependency; the backend is called "
                          "through ccall / cwrap of the Emscripten module and WebAssembly.instantiateStreaming",
            "ui_compiler": "TypeScript %s (tsc -p tsconfig.json, npm ci from package-lock.json), node %s"
                           % (version.replace("Version ", ""), web_plain_run(["node", "--version"], ts, log).strip()),
            "artifact": {"name": "dist/ (%s)" % ", ".join(names),
                         "size": sum((dist / n).stat().st_size for n in names)},
            "disabled_features": [],
            "fallbacks": fallbacks_of(platform_id),
            "notes": ("Deliverable of platform %s (validation/platforms/%s.json), the wasm path of the "
                      "page (it has no other). Built by python tools/build.py web modern; the files are in "
                      "$HOME/loomora-build/web-modern/dist/ (never in the repository tree); sha256 %s."
                      % (platform_id, platform_id,
                         ", ".join("%s %s" % (n, common.sha256(dist / n)) for n in names)))}
        write_deliverable(record, dist / "loomora.wasm")


TARGETS = {
    ("android", "legacy"): run_android_legacy,
    ("android", "modern"): run_android_modern,
    ("linux", "modern"): run_linux_modern,
    ("linux", "legacy"): run_linux_legacy,
    ("windows", "legacy"): run_windows_legacy,
    ("windows", "modern"): run_windows_modern,
    ("web", "legacy"): run_web_legacy,
    ("web", "modern"): run_web_modern,
}


def main(argv=None):
    common.setup_utf8_output()
    parser = argparse.ArgumentParser(description="Build one Loomora deliverable.")
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("variant", choices=VARIANTS)
    parser.add_argument("--wsl-distro", help="WSL distribution used on a Windows host "
                        "(default $LOOMORA_WSL_DISTRO or %s)" % DEFAULT_WSL_DISTRO)
    parser.add_argument("--min-sdk", type=int, help="android modern only: build with this minSdk "
                        "instead of 24 (spike S8: 21), the APK is then NOT the deliverable")
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
