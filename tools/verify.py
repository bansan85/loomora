#!/usr/bin/env python
"""Cheap, standard verification of a Loomora branch: one line per check, logs on disk.

Usage:
  python tools/verify.py [quick|full] [--only NAME[,NAME]] [--variants] [--list]
                         [--build-dir DIR] [--jobs N] [--wsl-distro NAME]

quick (default)  builds what exists for this host, then runs ctest, tools/run_vectors.py for
                 the UIs built here, tools/check_std.py, tools/check_ui_logic.py (self-test, then the search of
                 ui/ for a greeting built outside the backend), tools/validate_records.py and the self-tests
                 of tools/diff_verdicts.py and tools/run_vectors.py.
                 Linux / WSL: core + the Linux UIs the top-level CMake offers, default variant
                 only (the UIs run on a private Xvfb display when Xvfb is installed, else on the
                 inherited display). Windows host: the clang + Ninja build of core and the Win32
                 UI (as CMakePresets.json), run_vectors on that executable. No screenshot, no GUI
                 driving, no sanitizer, no variant build.
full             quick, plus the graphical checks (Linux, under a private Xvfb display that is
                 never WSLg's :0): each UI shows a mixed Unicode text and a 1,000-character text,
                 ONE screenshot per UI and scenario, and the Xlib harness under ASan and UBSan.
                 On a Windows host it also relays `full` to WSL. Missing Xvfb: prints the apt line
                 and exits 2.

WHEN TO USE full: ONLY when a change touches the graphics: a new graphical component, a new
graphics platform or toolkit, new font / Unicode / emoji / non-ASCII rendering support, or a
change of how text is drawn. Never to "check that it still works": that is quick (the probe
line, the vectors, ctest and validate_records are the functional proof).

Screenshots are written under the build directory and are CANDIDATES, not committed: never copy
them into the repository; asking the owner first is required before any image goes into
docs/implementation (see CLAUDE.md).

--only NAME,...  limit the checks; a NAME is a kind (build, ctest, vectors, std, ui-logic, records, diff,
                 screenshots, sanitizer, variants, wsl) or a UI (legacy, modern) or a variant id
                 (gtk-floor, gtk4, i686, xlib-corefont); the checks matching any NAME run.
--variants       also build / run the non-default variants (Linux: GTK 3.10 floor, GTK 4, i686
                 Xlib, Xlib core-font run), in quick or full. Only on request or when the files
                 of that variant change.
--list           print the checks of this host and mode, run nothing.
--build-dir DIR  default $HOME/loomora-build/verify-<hash of the repository path>: one per
                 worktree, reused between runs (incremental). Logs are in DIR/logs.
--jobs N         parallelism of the build and of the vectors (default: CPU count).

Exit status: 0 all checks passed (skipped ones do not count), 1 a check failed, 2 a tool that
full needs is missing or a usage error. Python 3.8+, standard library only. Docker, the
VirtualBox VM and Windows runs through it are separate explicit commands, never started here.
"""
import argparse
import atexit
import ctypes
import ctypes.util
import hashlib
import os
import platform
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import time
import traceback
import zlib
from pathlib import Path

import build as build_tool
import common

ROOT = common.ROOT
IS_WINDOWS = sys.platform == "win32"
UIS = ("legacy", "modern")
KINDS = ("build", "ctest", "vectors", "std", "ui-logic", "records", "diff", "screenshots", "sanitizer", "variants", "wsl")
UI_EXE = {"legacy": "loomora-xlib", "modern": "loomora-gtk3"}  # product names of the Linux UIs
# Texts of the graphical scenarios: mixed Unicode, and a long text that is cut at 1,000 characters.
SCENARIOS = (
    ("unicode", u"Hélène Ça 日本語 مرحبا "
                u"שלום \U0001F600 \U0001F468‍\U0001F469‍\U0001F467"),
    ("long", (u"Zoé 日本語 " * 143)[:1000]),
)
TAIL_LINES = 15


# ---- state and reporting -----------------------------------------------------------------------

class Ctx(object):
    def __init__(self, args):
        self.mode = args.mode
        self.only = set(args.only.split(",")) if args.only else None
        self.variants = args.variants
        self.jobs = args.jobs
        self.distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or "Ubuntu-24.04"
        self.build_dir = Path(args.build_dir) if args.build_dir else (
            Path.home() / "loomora-build" / ("verify-" + hashlib.sha1(str(ROOT).encode("utf-8")).hexdigest()[:8]))
        self.passed = self.failed = self.skipped = 0
        self.build_ok = None  # None: not built in this run
        self.exes = {}        # ui -> executable of the default build
        self.shots = []       # screenshot candidates
        self.step_start = 0.0

    def log(self, name):
        path = self.build_dir / "logs" / (re.sub(r"[^A-Za-z0-9_.-]+", "-", name) + ".log")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(u"", encoding="utf-8")
        return path

    def ok(self, name, detail=""):
        self.passed += 1
        print("PASS %s%s (%.1fs)" % (name, " " + detail if detail else "", time.time() - self.step_start))

    def fail(self, name, detail, log=None):
        self.failed += 1
        print("FAIL %s: %s%s" % (name, detail, " (log: %s)" % log if log else ""))
        if log and Path(log).is_file():
            lines = Path(log).read_text(encoding="utf-8", errors="replace").splitlines()
            for line in lines[-TAIL_LINES:]:
                print("    " + line)

    def skip(self, name, why):
        self.skipped += 1
        print("SKIP %s: %s" % (name, why))

    def selected(self, names):
        return self.only is None or bool(self.only & set(names))


def run_logged(log, cmd, cwd=None, env=None, timeout=1800):
    """Run a command with its whole output appended to `log`; returns (status, seconds)."""
    start = time.time()
    with open(str(log), "ab") as f:
        f.write(("$ %s\n" % " ".join(str(c) for c in cmd)).encode("utf-8"))
        f.flush()
        try:
            rc = subprocess.call([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env,
                                 stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            f.write(b"\ntimeout\n")
            rc = 124
        except OSError as e:
            f.write(("\ncannot start: %s\n" % e).encode("utf-8"))
            rc = 127
    return rc, time.time() - start


def log_text(log):
    return Path(log).read_text(encoding="utf-8", errors="replace")


# ---- private X display -------------------------------------------------------------------------

class Xvfb(object):
    """A private Xvfb on a free display number (-displayfd): never WSLg's :0."""

    def __init__(self):
        self.proc = None

    def start(self, log):
        r, w = os.pipe()
        logf = open(str(log), "wb")
        self.proc = subprocess.Popen(
            ["Xvfb", "-displayfd", str(w), "-screen", "0", "1024x768x24", "-nolisten", "tcp", "-noreset"],
            pass_fds=(w,), stdout=logf, stderr=logf)
        os.close(w)
        atexit.register(self.stop)
        ready = select.select([r], [], [], 20)[0]
        number = os.read(r, 32).strip() if ready else b""
        os.close(r)
        if not number.isdigit():
            self.stop()
            raise RuntimeError("Xvfb did not report a display number (log: %s)" % log)
        os.environ["DISPLAY"] = ":" + number.decode("ascii")
        os.environ.pop("WAYLAND_DISPLAY", None)
        os.environ["GDK_BACKEND"] = "x11"
        os.environ["NO_AT_BRIDGE"] = "1"

    def stop(self):
        proc, self.proc = self.proc, None
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def grab_screen(path):
    """Screenshot of the whole X display, cropped to what is not black, as PNG (libX11 through
    ctypes, no extra package). Returns False while the screen is still blank."""
    x11 = ctypes.CDLL(ctypes.util.find_library("X11"))

    class XImage(ctypes.Structure):
        _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int),
                    ("format", ctypes.c_int), ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                    ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
                    ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                    ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int)]

    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XDisplayWidth.argtypes = x11.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XGetImage.restype = ctypes.POINTER(XImage)
    x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                              ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
    x11.XFree.argtypes = [ctypes.c_void_p]
    dpy = x11.XOpenDisplay(None)
    if not dpy:
        return False
    try:
        w, h = x11.XDisplayWidth(dpy, 0), x11.XDisplayHeight(dpy, 0)
        img = x11.XGetImage(dpy, x11.XDefaultRootWindow(dpy), 0, 0, w, h, 0xFFFFFFFF, 2)  # ZPixmap
        if not img or img.contents.bits_per_pixel != 32:
            return False
        bpl = img.contents.bytes_per_line
        raw = ctypes.string_at(img.contents.data, bpl * h)
        x11.XFree(img.contents.data)  # what XDestroyImage does, which is a macro
        x11.XFree(ctypes.cast(img, ctypes.c_void_p))
    finally:
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay(dpy)
    rows = [raw[y * bpl:y * bpl + w * 4] for y in range(h)]
    used = [y for y, r in enumerate(rows) if r.strip(b"\0")]
    if not used:
        return False
    y0, y1 = used[0], used[-1] + 1
    x0 = min((len(r) - len(r.lstrip(b"\0"))) // 4 for r in rows[y0:y1] if r.strip(b"\0"))
    x1 = max((len(r.rstrip(b"\0")) + 3) // 4 for r in rows[y0:y1])
    pixels = bytearray()
    for r in rows[y0:y1]:
        b = r[x0 * 4:x1 * 4]
        row = bytearray(len(b) // 4 * 3)  # BGRX -> RGB
        row[0::3], row[1::3], row[2::3] = b[2::4], b[1::4], b[0::4]
        pixels += b"\0" + row
    write_png(path, x1 - x0, y1 - y0, bytes(pixels))
    return True


def write_png(path, w, h, filtered_rows):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    with open(str(path), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(filtered_rows, 6)) + chunk(b"IEND", b""))


# ---- steps -------------------------------------------------------------------------------------

def ui_options():
    """The LOOMORA_BUILD_<UI> options of the top-level CMakeLists.txt (found, not hard-coded):
    [(name, ui)]. Tests and the shared library are not UIs."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    names = re.findall(r"^\s*option\(\s*(LOOMORA_BUILD_[A-Z0-9_]+)", text, re.M | re.I)
    return [(n, "modern" if "MODERN" in n else "legacy") for n in names
            if not n.endswith(("_TESTS", "_SHARED"))]


def configure_and_build(ctx, name, bdir, uis, extra=()):
    """cmake configure + build into bdir; returns (ok, log). A UI option that cannot be
    configured (development files missing) fails the check, naming the option."""
    log = ctx.log("build-" + name)
    base = ["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    if IS_WINDOWS:  # the host-clang preset of CMakePresets.json
        base += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
    opts = [] if IS_WINDOWS else ui_options()  # the Win32 UI needs no option; Linux ones are off on Windows

    def flags(on):
        return ["-D%s=%s" % (n, "ON" if (u in on) else "OFF") for n, u in opts]

    rc, _ = run_logged(log, base + flags(uis) + list(extra))
    if rc != 0 and opts:  # a UI the host cannot build: say which, still check the rest
        rc2, _ = run_logged(log, base + flags(()) + list(extra))
        if rc2 == 0:
            ctx.fail("build " + name, "configure fails with %s" % ", ".join(n for n, u in opts if u in uis), log)
            rc = 0
    if rc != 0:
        ctx.fail("build " + name, "configure failed", log)
        return False, log
    rc, _ = run_logged(log, ["cmake", "--build", bdir, "-j", ctx.jobs])
    if rc != 0:
        ctx.fail("build " + name, "build failed", log)
        return False, log
    return True, log


def find_exe(bdir, name):
    for path in [Path(bdir) / name] + sorted(Path(bdir).rglob(name)):
        if path.is_file() and "CMakeFiles" not in path.parts:
            return path
    return None


def wanted_uis(ctx):
    chosen = set(UIS) & (ctx.only or set())
    return chosen or set(UIS)


def step_build(ctx):
    if shutil.which("ninja") is None or shutil.which("cmake") is None:
        return ctx.fail("build", "cmake and ninja are needed (Debian/Ubuntu: sudo apt install cmake ninja-build)")
    bdir = ctx.build_dir / "default"
    good, log = configure_and_build(ctx, "default", bdir, wanted_uis(ctx))
    ctx.build_ok = good
    if not good:
        return
    if IS_WINDOWS:
        exe = find_exe(bdir, "loomora.exe")
        if exe:
            ctx.exes["legacy"] = exe
    else:
        for ui, name in UI_EXE.items():
            exe = find_exe(bdir, name)
            if exe:
                ctx.exes[ui] = exe
    ctx.ok("build", "core + " + (", ".join(sorted(p.name for p in ctx.exes.values())) or "no UI"))


def step_ctest(ctx):
    log = ctx.log("ctest")
    rc, _ = run_logged(log, ["ctest", "--output-on-failure", "-j", ctx.jobs], cwd=ctx.build_dir / "default")
    text = log_text(log)
    total = re.search(r"tests passed.*? out of (\d+)", text)
    failed = re.search(r"(\d+) tests failed", text)
    if rc != 0 or not total:
        ctx.fail("ctest", "%s of %s failed" % (failed.group(1), total.group(1)) if total and failed else "no result", log)
    else:
        ctx.ok("ctest", "%d/%s" % (int(total.group(1)) - int(failed.group(1) if failed else 0), total.group(1)))


def step_std(ctx):
    if IS_WINDOWS and shutil.which("g++") is None:
        return ctx.skip("std", "no g++ on this host (clang with the MSVC STL cannot do strict C++11); run quick in WSL")
    log = ctx.log("std")
    rc, _ = run_logged(log, [sys.executable, ROOT / "tools" / "check_std.py", "--build-dir", ctx.build_dir / "check_std"])
    ctx.ok("std") if rc == 0 else ctx.fail("std", "check_std status %d" % rc, log)


def step_ui_logic(ctx):
    """T108: the greeting prefix is never in a UI (FR-010, FR-029): self-test of the checker, then the check."""
    log = ctx.log("ui-logic")
    for args in (["--selftest"], []):
        rc, _ = run_logged(log, [sys.executable, ROOT / "tools" / "check_ui_logic.py"] + args)
        if rc != 0:
            return ctx.fail("ui-logic", "check_ui_logic %s status %d" % (args[0] if args else "check", rc), log)
    m = re.findall(r"^OK   ((?:legacy|modern)/\S+)", log_text(log), re.M)
    ctx.ok("ui-logic", "%d UI(s), no greeting built in a UI" % len(m))


def step_records(ctx):
    log = ctx.log("records")
    if not common.docs_present():  # the docs submodule is local data: fetch it, the product never needs it
        run_logged(log, ["git", "-c", "protocol.file.allow=always", "submodule", "update", "--init", "docs/implementation"], cwd=ROOT)
    if not common.docs_present():
        return ctx.skip("records", common.docs_missing_message("nothing validated"))
    rc, _ = run_logged(log, [sys.executable, ROOT / "tools" / "validate_records.py"])
    ctx.ok("records") if rc == 0 else ctx.fail("records", "validate_records status %d" % rc, log)


def step_diff(ctx):
    log = ctx.log("diff_verdicts")
    rc, _ = run_logged(log, [sys.executable, ROOT / "tools" / "diff_verdicts.py", "--self-test"])
    ctx.ok("diff_verdicts") if rc == 0 else ctx.fail("diff_verdicts", "self-test status %d" % rc, log)


def step_vectors_selftest(ctx):
    log = ctx.log("run_vectors_selftest")
    rc, _ = run_logged(log, [sys.executable, ROOT / "tools" / "run_vectors.py", "--self-test"])
    ctx.ok("run_vectors_selftest") if rc == 0 else ctx.fail("run_vectors_selftest", "self-test status %d" % rc, log)


def host_platform_id():
    if IS_WINDOWS:
        rel = "11" if sys.getwindowsversion().build >= 22000 else platform.release()
        arch = {"AMD64": "x64", "x86": "x86", "ARM64": "arm64"}.get(platform.machine(), platform.machine().lower())
        return "windows-%s-%s" % (rel, arch)
    return build_tool.linux_platform_id()


def run_vectors(ctx, ui, exe, label, env=None):
    name = "run_vectors %s %s" % ("windows" if IS_WINDOWS else "linux", ui) + (" " + label if label else "")
    if exe is None:
        return ctx.skip(name, "not built on this host (missing development files? see the build log)")
    log = ctx.log(name)
    environ = dict(os.environ, **(env or {}))
    rc, _ = run_logged(log, [sys.executable, ROOT / "tools" / "run_vectors.py", host_platform_id(), ui,
                             "--exe", exe, "--jobs", ctx.jobs], env=environ)
    m = re.search(r"summary: (\d+) vector\(s\) run, (\d+) passed, (\d+) failed", log_text(log))
    if rc == 0 and m:
        ctx.ok(name, "%s/%s" % (m.group(2), m.group(1)))
    elif m:
        ctx.fail(name, "%s of %s" % (m.group(3), m.group(1)), log)
    else:
        ctx.fail(name, "did not run", log)


def step_vectors(ctx, ui):
    run_vectors(ctx, ui, ctx.exes.get(ui), "")


def step_screenshots(ctx, ui):
    exe = ctx.exes.get(ui)
    for scenario, text in SCENARIOS:
        ctx.step_start = time.time()
        name = "screenshot %s %s" % (ui, scenario)
        if exe is None:
            ctx.skip(name, "UI not built on this host")
            continue
        png = ctx.build_dir / "shots" / ("%s-%s.png" % (ui, scenario))
        png.parent.mkdir(parents=True, exist_ok=True)
        if png.exists():
            png.unlink()
        proc = subprocess.Popen([str(exe), "--probe", "--probe-input", text],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shot, last = False, time.time() + 1.6  # the probe run exits by itself after 2 s
        try:
            previous = None
            while time.time() < last and not shot:
                time.sleep(0.25)
                if grab_screen(png):  # drawn: take it once the picture has settled (two equal grabs)
                    data = png.read_bytes()
                    shot, previous = (previous == data), data
        finally:
            proc.wait()
        if shot:
            ctx.shots.append(png)
            ctx.ok(name, str(png))
        else:
            ctx.fail(name, "no stable window picture within the 2 s life of the UI")


def step_sanitizer(ctx):
    harness = ROOT / "tools" / "trials" / "linux-ubuntu-24.04-x86_64" / "xlib-ui-harness-cpp.cpp"
    d = ctx.build_dir / "sanitizer"
    d.mkdir(parents=True, exist_ok=True)
    log = ctx.log("sanitizer")
    copy = d / "xlib-ui-harness-san.cpp"  # a copy: the stored harness is never edited
    copy.write_text(re.sub(r'#include "(\.\./)+ui/legacy/linux-xlib/main\.cpp"',
                           '#include "%s"' % (ROOT / "ui" / "legacy" / "linux-xlib" / "main.cpp").as_posix(),
                           harness.read_text(encoding="utf-8")), encoding="utf-8")
    ui_dir = ROOT / "ui" / "legacy" / "linux-xlib"
    sources = [p for p in sorted(ui_dir.glob("*.cpp")) if p.name != "main.cpp"] + sorted((ROOT / "core" / "src").glob("*.cpp"))
    exe = d / "harness"
    inputs = [copy] + sources + sorted(ui_dir.glob("*.hpp")) + sorted(ui_dir.glob("main.cpp"))
    if not exe.exists() or exe.stat().st_mtime < max(p.stat().st_mtime for p in inputs):
        rc, _ = run_logged(log, ["g++", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                 "-fno-omit-frame-pointer", "-std=c++11", "-Dmain=loomora_ui_main",
                                 "-I", ROOT / "core" / "include", "-I", ui_dir, copy] + sources +
                            ["-o", exe, "-lX11", "-ldl"])
        if rc != 0:
            return ctx.fail("sanitizer xlib harness", "compile failed", log)
    supp = d / "lsan.supp"  # fontconfig keeps a few allocations for the life of the process
    supp.write_text(u"leak:libfontconfig\nleak:libfreetype\n", encoding="utf-8")
    env = dict(os.environ, LSAN_OPTIONS="suppressions=%s" % supp)
    for label, extra in (("xft", {}), ("core-font", {"LOOMORA_XLIB_NO_XFT": "1"})):
        rc, _ = run_logged(log, [exe], env=dict(env, **extra), timeout=300)
        if rc != 0:
            return ctx.fail("sanitizer xlib harness", "%s path: status %d" % (label, rc), log)
    ctx.ok("sanitizer xlib harness", "ASan + UBSan, Xft and core-font paths")


def step_wsl(ctx):
    """Windows host: the Linux graphical checks run in WSL (this same script, `full`)."""
    cmd = ["wsl", "-d", ctx.distro, "--cd", str(ROOT), "--", "python3", "tools/verify.py", "full", "--jobs", str(ctx.jobs)]
    if ctx.variants:
        cmd.append("--variants")
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True,
                         encoding="utf-8", errors="replace")
    for line in res.stdout.splitlines():
        m = re.match(r"full: (\d+) passed, (\d+) failed(?:, (\d+) skipped)?", line)
        if m:
            ctx.passed += int(m.group(1))
            ctx.failed += int(m.group(2))
            ctx.skipped += int(m.group(3) or 0)
        else:
            print(line)
    if res.returncode == 2:  # a tool of the WSL side is missing: its message is printed above
        sys.exit(2)


# Variants (Linux, --variants): [(id, ui, cmake arguments, hint when the UI cannot be built)]
VARIANTS = (
    ("gtk-floor", "modern", ["-DLOOMORA_GTK_FLOOR=ON"], ""),
    ("gtk4", "modern", ["-DLOOMORA_GTK_API=4"], "libgtkmm-4.0-dev"),
    ("i686", "legacy", ["-DCMAKE_TOOLCHAIN_FILE=%s" % (ROOT / "cmake" / "toolchains" / "linux-gcc-i686.cmake")],
     "sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install gcc-multilib g++-multilib libx11-dev:i386"),
)


def step_variant(ctx, vid, ui, extra, hint):
    name = "build " + vid
    bdir = ctx.build_dir / vid
    good, _ = configure_and_build(ctx, vid, bdir, {ui}, extra)
    if not good:
        return
    exe = find_exe(bdir, UI_EXE[ui])
    if exe is None:
        return ctx.skip(name, "the UI was not built here" + (" (%s)" % hint if hint else ""))
    ctx.ok(name)
    ctx.step_start = time.time()
    run_vectors(ctx, ui, exe, vid)


def step_corefont(ctx):
    run_vectors(ctx, "legacy", ctx.exes.get("legacy"), "xlib-corefont", {"LOOMORA_XLIB_NO_XFT": "1"})


# ---- plan and main -----------------------------------------------------------------------------

def make_plan(ctx):
    """[(label, names, description, function)] for this host and mode, filtered by --only."""
    S = []
    S.append(("build", {"build", "legacy", "modern"}, "configure and build core + the UIs of this host (default variant)", step_build, True))
    S.append(("ctest", {"ctest"}, "ctest of the default build", step_ctest, True))
    if IS_WINDOWS:
        S.append(("run_vectors windows legacy", {"vectors", "legacy"}, "tools/run_vectors.py on the Win32 executable", lambda c: step_vectors(c, "legacy"), True))
    else:
        for ui in UIS:
            S.append(("run_vectors linux " + ui, {"vectors", ui}, "tools/run_vectors.py on the Linux %s UI" % ui, (lambda u: lambda c: step_vectors(c, u))(ui), True))
    S.append(("std", {"std"}, "tools/check_std.py (strict C++11 backend, clean C API)", step_std, False))
    S.append(("ui-logic", {"ui-logic"}, "tools/check_ui_logic.py (no UI builds the greeting; self-test first)", step_ui_logic, False))
    S.append(("records", {"records"}, "tools/validate_records.py (fetches the docs submodule when missing)", step_records, False))
    S.append(("diff_verdicts", {"diff"}, "tools/diff_verdicts.py --self-test", step_diff, False))
    S.append(("run_vectors_selftest", {"vectors-selftest"}, "tools/run_vectors.py --self-test (platform id rules)", step_vectors_selftest, False))
    if ctx.variants or (ctx.only and ctx.only & ({"variants", "xlib-corefont"} | set(v[0] for v in VARIANTS))):
        if not IS_WINDOWS:
            for vid, ui, extra, hint in VARIANTS:
                S.append(("build " + vid, {"variants", vid, ui}, "variant build + vectors: " + vid,
                          (lambda a: lambda c: step_variant(c, *a))((vid, ui, extra, hint)), False))
            S.append(("run_vectors xlib-corefont", {"variants", "xlib-corefont", "legacy"}, "Xlib UI with LOOMORA_XLIB_NO_XFT=1", step_corefont, True))
    if ctx.mode == "full":
        if IS_WINDOWS:
            S.append(("wsl full", {"wsl", "screenshots", "sanitizer"}, "relays `full` (screenshots, sanitizer) to WSL", step_wsl, False))
        else:
            for ui in UIS:
                S.append(("screenshot " + ui, {"screenshots", ui}, "one screenshot per scenario (unicode, long) of the %s UI" % ui, (lambda u: lambda c: step_screenshots(c, u))(ui), True))
            S.append(("sanitizer", {"sanitizer", "legacy"}, "Xlib harness under ASan + UBSan (copy under the build dir)", step_sanitizer, False))
    return S


def preflight_full():
    """Tools that the graphical checks need; exit status 2 with the exact install line."""
    missing = []
    if shutil.which("Xvfb") is None:
        missing.append("xvfb")
    if not ctypes.util.find_library("X11"):
        missing.append("libx11-6")
    if shutil.which("g++") is None:
        missing.append("g++")
    if missing:
        print("verify full: missing %s. Install with: sudo apt install %s" % (", ".join(missing), " ".join(missing)))
        sys.exit(2)


def reexec_with_private_x11_dir(argv):
    """Under WSLg /tmp/.X11-unix is a read-only mount, so Xvfb cannot create its socket there.
    Re-run this script in a private mount namespace (unshare -rm) with a writable tmpfs on that
    directory: the real WSLg sockets are not touched and the children of this run see the private one.
    If the namespace cannot be created, carry on: Xvfb then fails and the inherited display is used."""
    sockdir = "/tmp/.X11-unix"
    if (IS_WINDOWS or os.environ.get("LOOMORA_VERIFY_NS") or not shutil.which("Xvfb") or not shutil.which("unshare")
            or not os.path.isdir(sockdir) or os.access(sockdir, os.W_OK)):
        return
    mount = "mount -t tmpfs -o mode=1777 tmpfs " + sockdir
    if subprocess.call(["unshare", "-rm", "sh", "-c", mount], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        return
    os.environ["LOOMORA_VERIFY_NS"] = "1"
    args = list(sys.argv[1:] if argv is None else argv)
    os.execvp("unshare", ["unshare", "-rm", "sh", "-c", mount + ' && exec "$0" "$@"', sys.executable,
                          os.path.abspath(sys.argv[0])] + args)


def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                 usage=argparse.SUPPRESS)
    ap.add_argument("mode", nargs="?", choices=("quick", "full"), default="quick")
    ap.add_argument("--only", help="comma-separated checks, UIs or variants to limit to")
    ap.add_argument("--variants", action="store_true", help="also build the non-default variants")
    ap.add_argument("--list", action="store_true", help="list the checks and exit")
    ap.add_argument("--build-dir", help="default $HOME/loomora-build/verify-<hash of the repository path>")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="build and vectors parallelism")
    ap.add_argument("--wsl-distro", help="Windows host: the WSL distribution of full (default Ubuntu-24.04)")
    args = ap.parse_args(argv)

    ctx = Ctx(args)
    known = set(KINDS) | set(UIS) | set(v[0] for v in VARIANTS) | {"xlib-corefont"}
    if ctx.only and not ctx.only <= known:
        print("verify: unknown name(s) %s; known: %s" % (", ".join(sorted(ctx.only - known)), ", ".join(sorted(known))), file=sys.stderr)
        return 2
    plan = [s for s in make_plan(ctx) if ctx.selected(s[1])]
    if args.list:
        print("%s checks on this host (%s), build dir %s" % (ctx.mode, "Windows" if IS_WINDOWS else "Linux", ctx.build_dir))
        for label, names, desc, _, _ in plan:
            print("  %-28s %s  [%s]" % (label, desc, ",".join(sorted(names))))
        return 0
    if not plan:
        print("verify: --only selects no check on this host (see --list)", file=sys.stderr)
        return 2
    if ctx.mode == "full" and not IS_WINDOWS:
        preflight_full()
    reexec_with_private_x11_dir(argv)
    if any(s[4] for s in plan) and not any(s[0] == "build" for s in plan):
        plan.insert(0, make_plan(ctx)[0])  # the checks that run a binary need the build
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143)) if hasattr(signal, "SIGTERM") else None

    ctx.build_dir.mkdir(parents=True, exist_ok=True)
    xvfb = Xvfb()
    if not IS_WINDOWS and shutil.which("Xvfb"):
        try:
            xvfb.start(ctx.log("xvfb"))
        except (RuntimeError, OSError) as e:
            if ctx.mode == "full":
                print("verify full: %s" % e)
                return 2
    try:
        for label, names, desc, fn, uses_build in plan:
            ctx.step_start = time.time()
            if uses_build and ctx.build_ok is False:
                ctx.skip(label, "the build failed")
                continue
            try:
                fn(ctx)
            except Exception:  # one broken step must not hide the others
                ctx.fail(label, "internal error: " + traceback.format_exc().strip().splitlines()[-1])
    finally:
        xvfb.stop()
    if ctx.shots:
        print("screenshots (candidates, not committed; do not copy them into the repository):")
        for p in ctx.shots:
            print("  " + str(p))
    print("%s: %d passed, %d failed%s" % (ctx.mode, ctx.passed, ctx.failed, ", %d skipped" % ctx.skipped if ctx.skipped else ""))
    return 1 if ctx.failed else 0


if __name__ == "__main__":
    sys.exit(main())
