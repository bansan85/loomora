#!/usr/bin/env python
"""Build one Loomora deliverable and record it.

Usage:
  python tools/build.py <platform> <variant> [--wsl-distro NAME] [--arch x86_64|i686|all]
                        [--toolchain native|manylinux2010|all]

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

  linux legacy   make on ui/legacy/linux-xlib (T084): plain C and Xlib, the C++11 backend as a
                 static libloomora.a, libX11 the only library linked besides libc (libXft and
                 fontconfig, which draw the text, are opened with dlopen at run time). Two
                 executables per toolchain, x86_64 and i686 (--arch x86_64|i686|all, default
                 all), and two toolchains (--toolchain, default all):
                   native         the host gcc (inside WSL when started on Windows), the C++
                                  runtime of the distribution linked dynamically (STATIC_CXX=0).
                                  Records <host platform id>-legacy.json (x86_64) and
                                  linux-i686-legacy.json. It records the highest GLIBC symbol
                                  version and says that the glibc 2.12 baseline is NOT verified;
                                  the i686 one needs the i386 libX11 (the tool prints the apt
                                  packages when it is missing).
                   manylinux2010  the images quay.io/pypa/manylinux2010_x86_64 and _i686 run
                                  with Docker (docker.exe of Windows when started on Windows),
                                  the sources mounted read-only, the build in the container's own
                                  file system with libstdc++ and libgcc linked statically
                                  (STATIC_CXX=1, fallback static-cxx-runtime-xlib); the CentOS 6
                                  repositories moved to vault.centos.org, which the container
                                  script switches to when libX11-devel does not install as is.
                                  The tool fails if the executable needs a GLIBC symbol version
                                  above 2.12, any GLIBCXX / CXXABI / GCC one or a shared library
                                  other than libX11 and the C library. Records
                                  linux-manylinux2010-x86_64-legacy.json and
                                  linux-manylinux2010-i686-legacy.json with the image digests;
                                  artifacts in out/linux-legacy-manylinux2010/<arch>/.

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
"linux-ubuntu-24.04-x86_64", the same rule as ui/modern/linux-gtk3/probe.c and
ui/legacy/linux-xlib/probe.c use for the probe record's platform_id (keep them in step). The
Linux Legacy native x86_64 record uses the id of the build machine; the native i686 executable
has no platform record yet (T087) and is recorded under the generic id "linux-i686" until it exists;
the manylinux2010 container builds are recorded under "linux-manylinux2010-x86_64" and
"linux-manylinux2010-i686" (what they are, not a platform they run on).
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
        "ui_toolkit": "gtk+-3.0 %s (3.10 floor, newer features by macro)" % gtk,
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
LINUX_LEGACY_TOOLCHAINS = ("native", "manylinux2010", "all")

# Shared libraries the Xlib executable may name: libX11 and the C library of the system (with its
# loader), plus the C++ runtime of the distribution in a native build (the default of the
# Makefile, STATIC_CXX=0). libXft and fontconfig are never named: they are opened with dlopen.
LINUX_LEGACY_ALLOWED_NEEDED = re.compile(
    r"^(libX11\.so\.6|libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|"
    r"ld-linux[-a-z0-9_.]*\.so\.[0-9]+)$")
LINUX_LEGACY_CXX_RUNTIME_NEEDED = re.compile(r"^(libstdc\+\+\.so\.6|libgcc_s\.so\.1)$")

# The manylinux2010 baseline: the oldest glibc the Linux Legacy UI must run on (CentOS 6 / Debian 6).
MANYLINUX2010_GLIBC_MAX = (2, 12)
MANYLINUX2010_IMAGES = {
    "x86_64": "quay.io/pypa/manylinux2010_x86_64",
    "i686": "quay.io/pypa/manylinux2010_i686",
}
# Platform ids of the container builds: no platform record exists for them, the id says what they
# are (the glibc baseline and the architecture), as the generic "linux-i686" does for the native one.
MANYLINUX2010_PLATFORM_IDS = {
    "x86_64": "linux-manylinux2010-x86_64",
    "i686": "linux-manylinux2010-i686",
}

# Fallbacks of the Linux Legacy deliverables whose id is not a platform record (so fallbacks.json
# cannot name them in platform_ids): the container builds take the static C++ runtime
# (static-cxx-runtime-xlib); an i686 executable takes the core-font text path at run time on a
# machine without the 32-bit libXft, which is the case of the Ubuntu 24.04 build machine
# (xlib-core-font-glyphs). The x86_64 executable on that machine finds libXft: no such fallback.
LINUX_LEGACY_EXTRA_FALLBACKS = {
    "linux-i686": ("xlib-core-font-glyphs",),
    "linux-manylinux2010-x86_64": ("static-cxx-runtime-xlib",),
    "linux-manylinux2010-i686": ("static-cxx-runtime-xlib", "xlib-core-font-glyphs"),
}

I386_X11_HELP = """\
linux legacy i686: the 32-bit libX11 is not installed, so the i686 executable cannot be linked
(`gcc -m32` itself works). On Ubuntu / Debian install it with:
  sudo dpkg --add-architecture i386
  sudo apt update
  sudo apt install libx11-dev:i386
(gcc-multilib and g++-multilib, for -m32, are needed too; they are already there when `gcc -m32`
links a program.) Then run the tool again. libXft and fontconfig are NOT needed to build: the
program opens them with dlopen at run time (a 32-bit system without libXft.so.2 draws with the
core font, see the fallback xlib-core-font-glyphs)."""

DOCKER_HELP = """\
linux legacy manylinux2010: `docker` is not available on this machine (or its daemon is not
running). The container build runs the images quay.io/pypa/manylinux2010_x86_64 and
quay.io/pypa/manylinux2010_i686 with Docker (Docker Desktop on a Windows host: run this tool from
Windows, not from inside WSL, unless the WSL integration of Docker Desktop is enabled). Use
--toolchain native to build with the host compiler only, which does NOT verify the glibc 2.12
baseline."""


def can_link_i386_x11(cc):
    """True when a 32-bit program using libX11 links (the i386 libX11 development files exist)."""
    src = "#include <X11/Xlib.h>\nint main(void) { return XOpenDisplay(0) != 0; }\n"
    res = subprocess.run([cc, "-m32", "-x", "c", "-", "-o", os.devnull, "-lX11"],
                         input=src, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    return res.returncode == 0


def parse_elf_report(dynamic_headers, dynamic_symbols):
    """(NEEDED libraries, {symbol family: highest version string}) from the text of
    `objdump -p` (NEEDED lines) and `objdump -T` (version nodes GLIBC_2.34, GLIBCXX_3.4.21, ...)."""
    needed = re.findall(r"^\s*NEEDED\s+(\S+)", dynamic_headers, re.MULTILINE)
    highest = {}
    for m in re.finditer(r"\b(GLIBC|GLIBCXX|CXXABI|GCC)_([0-9][0-9.]*)", dynamic_symbols):
        key = tuple(int(x) for x in m.group(2).split("."))
        if m.group(1) not in highest or key > highest[m.group(1)][0]:
            highest[m.group(1)] = (key, m.group(2))
    return needed, dict((family, value[1]) for family, value in highest.items())


def elf_report(exe):
    """parse_elf_report of an ELF executable on this machine."""
    return parse_elf_report(capture(["objdump", "-p", exe]), capture(["objdump", "-T", exe]))


def check_linux_legacy_elf(exe, needed, highest, static_cxx):
    """Fails when the executable needs a library or a C++ runtime symbol it must not: only libX11
    and the C library, plus the C++ runtime when it is linked dynamically (native build)."""
    allowed = [LINUX_LEGACY_ALLOWED_NEEDED]
    if not static_cxx:
        allowed.append(LINUX_LEGACY_CXX_RUNTIME_NEEDED)
    bad = [lib for lib in needed if not any(rx.match(lib) for rx in allowed)]
    if bad:
        sys.exit("%s needs shared libraries the Linux Legacy UI must not need: %s (allowed: "
                 "libX11, the C library%s; libXft and fontconfig are opened with dlopen)"
                 % (exe, ", ".join(bad), "" if static_cxx else ", the C++ runtime"))
    if static_cxx and ("GLIBCXX" in highest or "CXXABI" in highest):
        sys.exit("%s needs libstdc++ symbols (%s): the C++ runtime must be linked statically here"
                 % (exe, ", ".join(sorted(highest))))


def version_tuple(text):
    return tuple(int(x) for x in text.split("."))


def build_linux_legacy_arch(arch, build_root):
    """Native build of ui/legacy/linux-xlib for one architecture with make (the C++ runtime of the
    distribution, dynamically); returns (record, artifact)."""
    src = common.ROOT / "ui" / "legacy" / "linux-xlib"
    build_dir = build_root / ("linux-legacy-" + arch)
    common.run(["make", "-C", src, "ARCH=" + arch, "BUILD=" + str(build_dir)])
    exe = build_dir / "loomora-xlib"
    if not exe.is_file():
        sys.exit("build finished but %s is missing" % exe)

    needed, highest = elf_report(exe)
    check_linux_legacy_elf(exe, needed, highest, static_cxx=False)

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
    glibc_max = highest.get("GLIBC")
    record = {
        "platform_id": platform_id,
        "ui_variant": "legacy",
        "cpp_std": "c++11",
        "compiler": "gcc-%s" % gxx_version,
        "target": target,
        "output": "native",
        "ui_language": "c",
        "ui_toolkit": LINUX_LEGACY_TOOLKIT,
        "ui_compiler": "gcc-%s" % gcc_version,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": fallbacks_of(platform_id, LINUX_LEGACY_EXTRA_FALLBACKS.get(platform_id, ())),
        "notes": "make (ui/legacy/linux-xlib/Makefile, ARCH=%s, STATIC_CXX=0) on %s (%s); static "
                 "libloomora.a built from core/src with -std=c++11; the C++ runtime of the "
                 "distribution (libstdc++, libgcc_s) linked dynamically; libXft and fontconfig "
                 "opened at run time with dlopen, not linked; shared libraries needed: %s; "
                 "highest symbol versions (objdump -T): %s. BUILT NATIVELY, NOT IN THE "
                 "manylinux2010 CONTAINER: the executable needs %s or newer, so the glibc 2.12 "
                 "baseline is NOT verified by this record (the manylinux2010 deliverables "
                 "linux-manylinux2010-x86_64 and linux-manylinux2010-i686 are the ones built "
                 "for it, with the static C++ runtime). Artifact copied to out/linux-legacy/%s/, "
                 "sha256 %s."
                 % (arch, host_id, glibc, ", ".join(needed),
                    ", ".join("%s_%s" % item for item in sorted(highest.items())) or "none",
                    "GLIBC_" + glibc_max if glibc_max else "no GLIBC symbol version", arch,
                    common.sha256(artifact)),
    }
    return record, artifact


LINUX_LEGACY_TOOLKIT = ("Xlib (libX11, no toolkit); text drawn with libXft and fontconfig opened "
                        "at run time with dlopen (core X11 font fixed when libXft cannot be "
                        "loaded); C89 (gnu89) with POSIX")

# What runs inside the container (fed to `bash -s`): the manylinux2010 images already hold
# libX11-devel (no network is needed then: yum would fetch repository metadata even to say so, and
# vault.centos.org answers 403 at times); when it is missing the CentOS 6 repositories, which have
# moved to vault.centos.org, are pointed there and yum installs it; then the sources are copied from the read-only mount and built with the container's GCC
# (devtoolset) and the static C++ runtime. Markers on stdout let the tool find each part.
MANYLINUX_SCRIPT = r"""
set -e
echo "@@yum-begin"
if rpm -q libX11-devel >/dev/null 2>&1; then
  echo "libX11-devel already installed"
elif ! yum -y -q install libX11-devel; then
  echo "@@yum-vault: the default CentOS 6 repositories do not work, switching to vault.centos.org"
  for f in /etc/yum.repos.d/CentOS-*.repo; do
    sed -i -e 's|^mirrorlist=|#mirrorlist=|' \
           -e 's|^#\?baseurl=http://mirror.centos.org/centos/\$releasever|baseurl=http://vault.centos.org/6.10|' \
           -e 's|^#\?baseurl=http://vault.centos.org/centos/\$releasever|baseurl=http://vault.centos.org/6.10|' "$f"
  done
  yum clean all >/dev/null
  yum -y -q install libX11-devel
fi
rpm -q libX11-devel
echo "@@yum-end"
mkdir -p /build/ui/legacy
cp -r /src/core /build/core
cp -r /src/ui/legacy/linux-xlib /build/ui/legacy/linux-xlib
cd /build/ui/legacy/linux-xlib
echo "@@make-begin"
make ARCH=@ARCH@ STATIC_CXX=1 BUILD=/build/out
echo "@@make-end"
echo "@@gcc"; gcc -dumpfullversion; g++ -dumpfullversion; g++ -dumpmachine
echo "@@glibc"; ldd --version | head -n 1
echo "@@sha256"; sha256sum /build/out/loomora-xlib
echo "@@objdump-p"; objdump -p /build/out/loomora-xlib
echo "@@objdump-T"; objdump -T /build/out/loomora-xlib
echo "@@end"
"""


def docker_command():
    exe = os.environ.get("LOOMORA_DOCKER") or "docker"
    return exe if shutil.which(exe) else None


def section(text, name):
    """The lines between the marker @@name and the next @@ marker of the container output."""
    m = re.search(r"^@@%s[^\n]*\n(.*?)(?=^@@|\Z)" % re.escape(name), text, re.MULTILINE | re.DOTALL)
    return m.group(1) if m else ""


def build_manylinux2010_arch(docker, arch):
    """Builds ui/legacy/linux-xlib for one architecture inside the manylinux2010 image: the sources
    come from a read-only mount, the build happens in the container's own file system (/build),
    only the executable is copied out. Returns (record, artifact)."""
    image = MANYLINUX2010_IMAGES[arch]
    print("manylinux2010 %s: pulling %s" % (arch, image))
    res = subprocess.run([docker, "pull", image])
    if res.returncode != 0:
        sys.exit("linux legacy manylinux2010 %s: cannot pull %s (status %d); nothing was built "
                 "for it" % (arch, image, res.returncode))
    digest = capture([docker, "image", "inspect", "--format", "{{index .RepoDigests 0}}", image])
    image_id = capture([docker, "image", "inspect", "--format", "{{.Id}}", image])
    name = "loomora-xlib-%s-%d" % (arch, os.getpid())
    script = MANYLINUX_SCRIPT.replace("@ARCH@", arch)
    print("manylinux2010 %s: building in the container (%s)" % (arch, digest))
    res = subprocess.run(
        [docker, "run", "-i", "--name", name, "-v", "%s:/src:ro" % common.ROOT, image,
         "bash", "-s"],
        # bytes, not text: a Windows host must not turn the LF of the script into CRLF for bash
        input=script.encode("utf-8"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = res.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    try:
        if res.returncode != 0 or "@@end" not in log:
            sys.exit("linux legacy manylinux2010 %s: the container build failed (status %d); "
                     "last lines:\n%s" % (arch, res.returncode, "\n".join(log.splitlines()[-25:])))
        out_dir = common.ROOT / "out" / "linux-legacy-manylinux2010" / arch
        out_dir.mkdir(parents=True, exist_ok=True)
        artifact = out_dir / "loomora-xlib"
        capture([docker, "cp", "%s:/build/out/loomora-xlib" % name, artifact])
    finally:
        subprocess.run([docker, "rm", "-f", name], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (out_dir / "container-build-log.txt").write_text(log, encoding="utf-8")
    os.chmod(str(artifact), 0o755)

    needed, highest = parse_elf_report(section(log, "objdump-p"), section(log, "objdump-T"))
    check_linux_legacy_elf(artifact, needed, highest, static_cxx=True)
    glibc_max = highest.get("GLIBC")
    if glibc_max is None or version_tuple(glibc_max) > MANYLINUX2010_GLIBC_MAX:
        sys.exit("linux legacy manylinux2010 %s: the executable needs GLIBC_%s, above the "
                 "baseline glibc %d.%d; see %s" % (arch, glibc_max, MANYLINUX2010_GLIBC_MAX[0],
                                                   MANYLINUX2010_GLIBC_MAX[1], out_dir))
    gcc_lines = section(log, "gcc").split()
    gcc_version, gxx_version, triple = gcc_lines[0], gcc_lines[1], gcc_lines[2]
    glibc = "glibc " + section(log, "glibc").split()[-1]
    sha_in_container = section(log, "sha256").split()[0]
    if sha_in_container != common.sha256(artifact):
        sys.exit("linux legacy manylinux2010 %s: the copied executable differs from the one "
                 "built in the container" % arch)
    if "already installed" in log:
        yum_note = "libX11-devel was already installed in the image, no package was installed"
    elif "@@yum-vault" in log:
        yum_note = ("libX11-devel installed with yum from vault.centos.org (the default CentOS 6 "
                    "repositories did not work)")
    else:
        yum_note = "libX11-devel installed with yum from the repositories of the image"
    rpm = [l for l in section(log, "yum-begin").splitlines() if l.startswith("libX11-devel-")]
    platform_id = MANYLINUX2010_PLATFORM_IDS[arch]
    record = {
        "platform_id": platform_id,
        "ui_variant": "legacy",
        "cpp_std": "c++11",
        "compiler": "gcc-%s" % gxx_version,
        "target": triple,
        "output": "native",
        "ui_language": "c",
        "ui_toolkit": LINUX_LEGACY_TOOLKIT,
        "ui_compiler": "gcc-%s" % gcc_version,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": fallbacks_of(platform_id, LINUX_LEGACY_EXTRA_FALLBACKS.get(platform_id, ())),
        "notes": "BUILT IN THE manylinux2010 CONTAINER (spike S4 / T069): image %s, digest %s, "
                 "image id %s; the container's GCC %s with its %s; make (ui/legacy/linux-xlib/"
                 "Makefile, ARCH=%s, STATIC_CXX=1) in the container's own file system, the sources "
                 "mounted read-only; %s (%s), headers only: libXft and fontconfig are opened at "
                 "run time with dlopen and their development files are not installed; libstdc++ "
                 "and libgcc linked statically; shared libraries needed (objdump -p): %s; highest "
                 "symbol versions (objdump -T): %s, so the executable needs GLIBC_%s at most and "
                 "the glibc %d.%d baseline is reached as far as symbol versions show (no "
                 "GLIBCXX / CXXABI / GCC version node: the C++ runtime is static); this is a "
                 "link-level check, not a run on a glibc 2.12 system. Artifact copied to "
                 "out/linux-legacy-manylinux2010/%s/, sha256 %s, container build log next to it."
                 % (image, digest, image_id, gcc_version, glibc, arch, yum_note,
                    rpm[0] if rpm else "rpm version not read", ", ".join(needed),
                    ", ".join("%s_%s" % item for item in sorted(highest.items())) or "none",
                    glibc_max, MANYLINUX2010_GLIBC_MAX[0], MANYLINUX2010_GLIBC_MAX[1], arch,
                    common.sha256(artifact)),
    }
    return record, artifact


def run_linux_legacy_native(args):
    require_tools(["make", "gcc", "g++", "ar", "objdump", "ldd"])
    if platform.machine() != "x86_64":
        sys.exit("linux legacy: this tool builds both architectures on an x86_64 host (gcc -m32 "
                 "for i686); this machine is %s" % platform.machine())
    print("NOTE: linux legacy native build (gcc %s, dynamic C++ runtime): it does NOT verify the "
          "glibc 2.12 baseline, only the manylinux2010 container build does (--toolchain "
          "manylinux2010)." % capture(["gcc", "-dumpfullversion"]))
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
    return failed


def run_linux_legacy_manylinux2010(args):
    docker = docker_command()
    if docker is None:
        print(DOCKER_HELP)
        return list(LINUX_LEGACY_ARCHS if args.arch == "all" else (args.arch,))
    if subprocess.run([docker, "info"], stdout=subprocess.PIPE,
                      stderr=subprocess.PIPE).returncode != 0:
        print(DOCKER_HELP)
        return list(LINUX_LEGACY_ARCHS if args.arch == "all" else (args.arch,))
    failed = []
    for arch in (LINUX_LEGACY_ARCHS if args.arch == "all" else (args.arch,)):
        record, artifact = build_manylinux2010_arch(docker, arch)
        write_deliverable(record, artifact)
    return failed


def run_linux_legacy(args):
    failed = []
    if args.toolchain in ("native", "all"):
        if in_wsl_or_linux():
            failed += run_linux_legacy_native(args)
        elif sys.platform == "win32":
            # the inner run writes the records; the container part below runs on this host
            rerun_in_wsl(args, "linux", "legacy",
                         ["--arch", args.arch, "--toolchain", "native"])
        else:
            sys.exit("linux legacy cannot be built on this host (%s): use Linux or Windows with "
                     "WSL" % sys.platform)
    if args.toolchain in ("manylinux2010", "all"):
        failed += run_linux_legacy_manylinux2010(args)
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
    parser.add_argument("--toolchain", choices=LINUX_LEGACY_TOOLCHAINS, default="all",
                        help="linux legacy only: native (host gcc), manylinux2010 (Docker "
                        "container, glibc 2.12 baseline) or all (default)")
    args = parser.parse_args(argv)

    key = (args.platform, args.variant)
    if key in TARGETS:
        TARGETS[key](args)
        return 0
    task = PENDING.get(key, "no task")
    sys.exit("build target %s-%s: not implemented yet (%s)" % (args.platform, args.variant, task))


if __name__ == "__main__":
    sys.exit(main())
