#!/usr/bin/env python
"""Feed every valid reference input of core/tests/vectors.json to a built Loomora UI.

Usage:
  python tools/run_vectors.py <platform-id> <variant> [--vectors FILE] [--jobs N] [--timeout S]
                              [--evidence-dir DIR] [--wsl-distro NAME] [--arch x64|x86]
                              [--exe PATH] [--ui-abi ABI] [--udid UDID] [--dry-run]

  platform-id  for example linux-ubuntu-24.04-x86_64, windows-11-x64 or windows-2000-x86 (the id
               the UI itself puts in its probe line)
  variant      legacy | modern
  --arch       Windows only: which executable of the Windows Legacy UI runs, x64 (default) or x86
               (windows-2000-x86 runs the x86 executable in a VirtualBox VM and accepts x86 only)
  --ui-abi     Android only: the ABI the UI process really runs as, when it is not the one of the
               platform id: the Legacy APK has no x86_64 or arm64-v8a library, so on a 64-bit
               device it runs as a 32-bit process (x86 on android-7.0-x86_64) and its probe line
               says android-7.0-x86. Only the 32-bit companion of the device's ABI is accepted
               (see android_probe_platform_id); the device must list it in ro.product.cpu.abilist
               (ro.product.cpu.abi and abi2 before API 21)

For each valid vector the UI is launched in probe mode with the input override of
contracts/ui-contract.md (`--probe-input <utf8>` on Windows, macOS and Linux; the intent extra
`probe_input` on Android, the launch argument `-probe_input` on iOS and the URL parameter
`?probe&input=` on the Web are the other launchers' business). The probe line it prints is
checked with tools/check_probe.py (schema, message == "Bonjour " + input, greeting_matches)
and against the vector's own expected output; the record's platform_id and ui_variant must be
those given on the command line. Exit status: 0 when every vector passed, 1 on any mismatch,
launch problem or when no vector could run, 2 on a usage error (SC-011, FR-027).

Which vectors are "valid", and what a UI is expected to show for them (decision, UI limits):
  * A vector is valid when it has an expected `output` (not an `error`), its input is a
    non-NULL, valid UTF-8 text without NUL, and it is not an API-only case (an explicit
    `capacity`, a property of the buffer of loomora_greet that no UI controls). Error vectors
    (too long, invalid UTF-8, NULL pointer, small capacity) cannot be typed into a UI field and
    are covered by the backend tests, so they are reported as skipped, never as passed.
  * ui-contract.md limits every UI's field to 1,000 characters. A valid vector of at most 1,000
    characters is therefore expected verbatim: `input` is the vector's input and `message` its
    `output`. A valid vector of more than 1,000 characters (the 4,096-byte limit vectors, which
    the backend accepts) cannot be fully entered; the UI is expected to keep exactly the FIRST
    1,000 characters and to greet that text: `input` = the kept text, `message` = "Bonjour " +
    that text. They are reported as "truncated" and only count as passed when the truncation is
    exactly that; a UI that lets more through, drops the input or fails is a failure, nothing is
    relaxed. What a "character" is depends on the UI (Launcher.length_unit): code points for the
    GTK UI (GtkEntry max length), UTF-16 code units for the Windows UI (EDIT control limit),
    where a character outside the BMP counts as 2 and is never cut in two (so the kept text may
    be 999 units long when the 1,000th unit would split a surrogate pair). No current vector
    mixes the two cases (the vectors longer than 1,000 characters are all BMP): the rule is
    there for future vectors.

Platform launchers (registry LAUNCHERS, keyed by family and variant; select_launcher picks the
VirtualBox launcher for the guest platforms of VBOX_PLATFORMS). Implemented: linux modern, linux
legacy (the Xlib UI, same way; executable from $HOME/loomora-build/linux-legacy-x86_64, then out/,
or --exe), windows legacy on the local Windows host and windows legacy in a VirtualBox guest (windows-2000-x86).
Windows modern (WinUI 3, platform id windows-11-x64): see WindowsModernLauncher; the published folder of
`python tools/build.py windows modern` (build/windows-modern/net10-x64, or the folder of --exe, for example
the net8 one), run serially from a scratch copy, the line read from standard output and checked against
loomora-probe.json.
Android legacy and modern (platform id android-<release>-<abi>, for example android-4.1.2-x86 or
android-7.0-x86_64): see AndroidLegacyLauncher and AndroidModernLauncher (out/android-modern/). The APK out/android-legacy/loomora.apk (or --exe) is installed on the one
emulator or device adb sees (--serial / $ANDROID_SERIAL to choose); the launcher never starts or
stops the emulator. Each run starts the activity with the intent extras of the UI contract and
reads the probe line from logcat (tag LOOMORA_PROBE; a long line is logged in numbered parts
"#i/n <text>" and joined here). Runs are serial.
Web legacy under node (platform ids web-node-js and web-node-wasm, see WebNodeLauncher): runs the
page script ui/legacy/web/app.js and the module of `python tools/build.py web legacy`
($HOME/loomora-build/web-legacy/dist, or the folder of --exe) in tools/trials/web-node/page-harness.js,
in a minimal DOM stub and without any browser, with the URL `?probe=1&input=<percent-encoded>&platform=<id>`
(ASCII, passed on standard input). The page decides by itself between the JavaScript build and the
WebAssembly one; the id says which one the harness makes possible.
Web modern under node (platform id web-node-wasm only, same WebNodeLauncher): the same harness runs
the Modern page (index.html + the ES modules of `python tools/build.py web modern`,
$HOME/loomora-build/web-modern/dist, or the folder of --exe) with `node --experimental-vm-modules`, in
the same DOM stub, with the same query; the record must say ui_variant modern and core cpp-wasm.
Web legacy in a real browser in a VirtualBox guest (platform id web-firefox-10-windows-2000-x86, see
WebFirefoxVmLauncher): the dist folder is served over HTTP by this process, Firefox 10 in the VM
"Windows 2000" opens `http://10.0.2.2:<port>/index.html?probe=1&input=<text>&platform=<id>&post=1&run=<n>`
(a long text through a redirect), the page POSTs its probe record back (post=1) and the window is
closed with Alt+F4; the leftover firefox.exe processes are counted. Same VM rules as the Windows guest.
Web modern in Chrome on the Windows 11 host (platform id web-chrome-<major>-windows-11-x64, see
WebChromeHostLauncher): the dist folder of `python tools/build.py web modern` is served over HTTP on
127.0.0.1, headless Chrome (`--headless=new --dump-dom`, a throw-away --user-data-dir per run) opens
`http://127.0.0.1:<port>/index.html?probe=1&input=<text>&platform=<id>`, and the probe record is read from
<pre id="probe"> of the dumped DOM (the title must be LOOMORA_PROBE_DONE); leftover chrome.exe are counted.
macOS (Modern and Legacy) and iOS Modern (platform ids macos-<major>-<arch>, for example macos-26-arm64, and
ios-<major.minor>-arm64-simulator, for example ios-26.5-arm64-simulator), on a macOS host only (the `macos-26`
runner, T126): see MacOSModernLauncher and IOSSimulatorLauncher. The app of `python tools/build.py macos
modern|legacy` or `ios modern --ios-sdk iphonesimulator` is started with the probe arguments of
ui-contract.md; the simulator one through `xcrun simctl launch --console`, on the simulator --udid.
Every other family exits with status 1 and "launcher not implemented yet" until the tasks that
add the UIs (T060-T093) add theirs: they only have to implement Launcher.run().
Linux modern: the executable built by `python tools/build.py linux modern`
($HOME/loomora-build/linux-modern/loomora-gtk3, or out/linux-modern/ as fallback) runs on this
machine on Linux or inside WSL on a Windows host (`wsl -d <distro>`, default Ubuntu-24.04 or
$LOOMORA_WSL_DISTRO). To avoid every quoting problem (quotes, backslashes, newlines, emoji, empty
input) the Windows side never puts the text on a command line: it passes the UTF-8 bytes as hex
digits to this same script run inside WSL (`--inner-run`), which starts the UI with an argv list
(no shell) and returns the exit status, standard output and standard error as one JSON line.
Linux in a Docker container (--docker IMAGE [--docker-volume NAME]): for a system that is a container
image, such as the manylinux2010 one (CentOS 6, glibc 2.12; see LinuxDockerLauncher). The UI is built
in that same image (`docker run ... python3.8 tools/build.py linux legacy`, build directory = the
volume mounted as /root/loomora-build); this script starts one container (worktree mounted at /src,
private Xvfb on :99) and runs each vector through `docker exec` of its own --inner-run.
Windows legacy: the executable out/windows-legacy/<arch>/loomora.exe built by
`python tools/build.py windows legacy` (built through WSL when missing) runs on a Windows host
only (no Wine). Each run copies it into its own scratch directory (so the probe files of parallel
runs cannot collide) and starts the copy with CreateProcess through Python's subprocess and an
argument list: Python builds the UTF-16 command line with the rules of the Microsoft C runtime,
which probe.cpp applies in reverse, so quotes, backslashes, tabs, newlines, emoji and the empty
string (passed as "") need no shell quoting. It waits for the exit (the process is killed on
timeout), reads loomora-probe.json next to the copy (the GUI executable has no console: its
stdout is empty) and hands that line to the same checks as the other launchers. Not
representable: a NUL character (a Windows command line is a NUL-terminated string), which
plan_vectors already skips, and an unpaired surrogate, which cannot come from valid UTF-8. The
platform id the exe reports is windows-<system>-<architecture of the operating system>, so the
x86 exe under WOW64 reports the same id as the x64 exe (windows-11-x64 on 64-bit Windows 11):
--arch only selects the file and is recorded in the log and the evidence file names.
Windows legacy in a VirtualBox guest (windows-2000-x86): see WindowsVirtualBoxLauncher. The VM
must already be running with a logged-on desktop (the launcher never starts, stops, snapshots or
restores it and changes no setting); credentials and names come from LOOMORA_VM_NAME,
LOOMORA_VM_USER (default "name"), LOOMORA_VM_PASSWORD (default empty), LOOMORA_VM_DIR and
LOOMORA_VBOXMANAGE. The runs are serial (one VM). A NUL is the only character a Windows command
line cannot carry; tabs, newlines, control characters, quotes, backslashes, %VARIABLES%, every
BMP and non-BMP character and the empty string reach the executable unchanged (the helper
tools/guest/runw.c passes the exact UTF-16 command line to CreateProcessW).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import common

UI_MAX_CHARS = 1000  # ui-contract.md: the field is limited to 1,000 characters
DEFAULT_WSL_DISTRO = "Ubuntu-24.04"
# Linux in a Docker container (--docker): the Python of the manylinux2010 image that runs this script
# (the system Python of CentOS 6 is 2.6), the private X display, the build volume mounted as the
# $HOME/loomora-build of the container (root).
DOCKER_PYTHON = "/opt/python/cp38-cp38/bin/python3"
DOCKER_DISPLAY = ":99"
DOCKER_DEFAULT_VOLUME = "loomora-build"
WINDOWS_ARCHS = ("x64", "x86")
FAMILY_PREFIXES = {
    "win": "windows", "windows": "windows", "macos": "macos", "mac": "macos", "ios": "ios",
    "android": "android", "linux": "linux", "web": "web",
}


# ---- vectors -----------------------------------------------------------------------------------

def enc(value):
    """Decode an 'enc' of vectors.json into bytes (same format as tools/gen_vectors.py)."""
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, list):
        return b"".join(enc(v) for v in value)
    if isinstance(value, dict):
        if "repeat" in value:
            return enc(value["repeat"]) * int(value["count"])
        if "hex" in value:
            return bytes.fromhex(value["hex"])
    raise ValueError("unsupported text encoding: %r" % (value,))


class Vector(object):
    def __init__(self, vid, text, expected_input, expected_message, kind):
        self.id = vid
        self.text = text                        # text given to the UI
        self.expected_input = expected_input    # `input` the probe line must hold
        self.expected_message = expected_message
        self.kind = kind                        # "exact" or "truncated"

    def describe(self):
        return "%s (%s, %d chars)" % (self.id, self.kind, len(self.text))


def length_in(text, unit):
    """Length of a text in the unit a UI counts its 1,000-character limit in."""
    if unit == "utf16":
        return len(text.encode("utf-16-le")) // 2
    return len(text)  # code points


def truncate_to(text, unit):
    """The first UI_MAX_CHARS characters of a text, a surrogate pair never cut in two."""
    if unit != "utf16":
        return text[:UI_MAX_CHARS]
    kept, units = [], 0
    for ch in text:
        width = 2 if ord(ch) > 0xFFFF else 1
        if units + width > UI_MAX_CHARS:
            break
        kept.append(ch)
        units += width
    return "".join(kept)


def plan_vectors(data, unit="codepoints"):
    """Split the greet vectors into (vectors to run, [(id, reason)] skipped).

    unit is how the UI counts its 1,000-character limit: "codepoints" or "utf16"."""
    greet = data["greet"]
    max_bytes = greet["max_input_bytes"]
    prefix = greet["prefix"]
    runs, skipped = [], []
    for case in greet["cases"]:
        vid = case["id"]
        if "error" in case:
            skipped.append((vid, "expects error %d (not enterable in a UI field)" % case["error"]))
            continue
        if "capacity" in case:
            skipped.append((vid, "API-only case (explicit buffer capacity)"))
            continue
        if case.get("input") is None:
            skipped.append((vid, "NULL input"))
            continue
        raw = enc(case["input"])
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            skipped.append((vid, "invalid UTF-8"))
            continue
        if len(raw) > max_bytes:
            skipped.append((vid, "over %d bytes" % max_bytes))
            continue
        if u"\x00" in text:
            skipped.append((vid, "contains NUL (cannot be passed as a command-line argument)"))
            continue
        expected_output = enc(case["output"]).decode("utf-8")
        if length_in(text, unit) <= UI_MAX_CHARS:
            runs.append(Vector(vid, text, text, expected_output, "exact"))
        else:
            kept = truncate_to(text, unit)
            runs.append(Vector(vid, text, kept, prefix + kept, "truncated"))
    return runs, skipped


# ---- launchers ---------------------------------------------------------------------------------

class RunResult(object):
    def __init__(self, returncode, stdout, stderr, elapsed, error=None):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.elapsed = elapsed
        self.error = error  # the launcher could not even start the UI
        self.leftover = None  # UI processes still alive after the run (None: not measured)
        self.guest_wall_ms = None  # process lifetime measured inside the guest (None: not measured)
        self.page_ms = None  # Web in a browser: start of the browser to the posted record (None: not measured)
        self.relaunched = False  # Web in a browser: firefox.exe did not start the first time and was started once more


class Launcher(object):
    """Starts the built UI of one platform family and variant with one input text."""

    length_unit = "codepoints"  # what the field limit counts: "codepoints" or "utf16"
    tag = ""                    # evidence file name suffix when one id has several runs
    max_jobs = None             # runs in parallel the target allows (None: no limit)

    def __init__(self, args):
        self.args = args

    def prepare(self):
        """Check that the UI can be launched; return a one-line description of what is run."""
        raise NotImplementedError

    def run(self, text, timeout):
        """Launch the UI in probe mode with `text` (str) as input override; return RunResult."""
        raise NotImplementedError

    def probe_platform_id(self, platform_id):
        """The platform_id the UI's probe line must carry when it is run for `platform_id`
        (the id of the platform record). Only a launcher whose UI can run as another ABI than the
        platform's overrides it."""
        return platform_id

    def extra_evidence(self):
        """Other text files for --evidence-dir, as {file name suffix: text} (after the runs)."""
        return {}

    def close(self):
        """Release what prepare() acquired."""


# Which Linux UI the local (Linux side) functions run: set from the command line by main(), also
# for the --inner-* calls that the Windows launcher makes inside WSL.
LINUX_UI = {"variant": "modern", "exe": None}


def local_executable():
    """The Linux UI executable: --exe when given, else the build tree under $HOME, then out/."""
    if LINUX_UI["exe"]:
        path = Path(LINUX_UI["exe"])
        return path if path.is_file() else None
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
    if LINUX_UI["variant"] == "legacy":
        candidates = [build_root / "linux-legacy-x86_64" / "loomora-xlib",
                      common.ROOT / "out" / "linux-legacy" / "x86_64" / "loomora-xlib"]
    else:
        candidates = [build_root / "linux-modern" / "loomora-gtk3",
                      common.ROOT / "out" / "linux-modern" / "loomora-gtk3"]
    for path in candidates:
        if path.is_file():
            return path
    return None


def run_ui(data, timeout):
    """Start the Linux Modern UI with `data` (bytes) as input override; return a reply dict."""
    exe = local_executable()
    if exe is None:
        return {"error": "executable not found: build it with 'python tools/build.py linux %s'"
                         % LINUX_UI["variant"]}
    start = time.time()
    try:
        res = subprocess.run([str(exe), "--probe", "--probe-input", data],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        out = {"returncode": res.returncode,
               "stdout": res.stdout.decode("utf-8", "replace"),
               "stderr": res.stderr.decode("utf-8", "replace")}
    except subprocess.TimeoutExpired:
        out = {"error": "timeout after %s s (the UI should exit by itself after 2 s)" % timeout}
    except OSError as e:
        out = {"error": "cannot start %s: %s" % (exe, e)}
    out["elapsed"] = time.time() - start
    return out


def inner_run(hex_text, timeout):
    """Linux side of the Windows launcher: decode the text, run the UI, print one JSON line."""
    data = bytes.fromhex(hex_text[1:])  # the leading "x" keeps an empty input a non-empty argument
    print(json.dumps(run_ui(data, timeout)))  # ASCII: safe through any console encoding
    return 0


class LinuxModernLauncher(Launcher):
    """Runs the Linux UI of LINUX_UI["variant"]: the GTK one (modern) or the Xlib one (legacy, see
    LinuxLegacyLauncher). The text is passed to the UI process exactly as on the Windows side."""

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
        self.on_windows = sys.platform == "win32"

    def _wsl_script(self, *script_args):
        inner = ["--inner-variant", LINUX_UI["variant"]]
        if LINUX_UI["exe"]:
            inner += ["--exe", LINUX_UI["exe"]]
        return ["wsl", "-d", self.distro, "--cd", str(common.ROOT), "--", "python3",
                "tools/run_vectors.py"] + inner + list(script_args)

    def prepare(self):
        if self.on_windows:
            res = subprocess.run(self._wsl_script("--inner-where"), stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=120)
            exe = res.stdout.decode("utf-8", "replace").strip().splitlines()
            if res.returncode != 0 or not exe or not exe[-1].startswith("/"):
                raise RuntimeError(
                    "cannot locate the Linux %s executable in WSL (%s): run "
                    "'python tools/build.py linux %s'\n%s"
                    % (LINUX_UI["variant"], self.distro, LINUX_UI["variant"],
                       res.stderr.decode("utf-8", "replace").strip()))
            return "WSL %s: %s" % (self.distro, exe[-1])
        exe = local_executable()
        if exe is None:
            raise RuntimeError("executable not found: run 'python tools/build.py linux %s'"
                               % LINUX_UI["variant"])
        return str(exe)

    def run(self, text, timeout):
        start = time.time()
        if self.on_windows:
            cmd = self._wsl_script("--inner-run", "x" + text.encode("utf-8").hex(),
                                   "--timeout", str(timeout))
            try:
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     timeout=timeout + 40)
            except subprocess.TimeoutExpired:
                return RunResult(None, "", "", time.time() - start, "WSL call timed out")
            lines = [l for l in res.stdout.decode("utf-8", "replace").splitlines()
                     if l.startswith("{")]
            if res.returncode != 0 or not lines:
                return RunResult(None, "", res.stderr.decode("utf-8", "replace"),
                                 time.time() - start,
                                 "WSL helper failed (status %d)" % res.returncode)
            reply = json.loads(lines[-1])
        else:
            reply = run_ui(text.encode("utf-8"), timeout)
        if "error" in reply:
            return RunResult(None, "", "", time.time() - start, reply["error"])
        return RunResult(reply["returncode"], reply["stdout"], reply["stderr"], time.time() - start)


class LinuxLegacyLauncher(LinuxModernLauncher):
    """The Linux Legacy (Xlib) UI: the same launch and checks, another executable."""


def docker_start_cmd(image, root, volume):
    """`docker run` command of the long-lived container: the worktree is /src, the build volume is
    the $HOME/loomora-build of its root user, and a private Xvfb serves DOCKER_DISPLAY (never the
    display of the host)."""
    return ["docker", "run", "-d", "--rm", "-v", "%s:/src" % root,
            "-v", "%s:/root/loomora-build" % volume, "-w", "/src",
            "-e", "DISPLAY=" + DOCKER_DISPLAY, "-e", "LANG=C.UTF-8", image,
            "bash", "-c", "Xvfb %s -screen 0 1280x1024x24 -nolisten tcp & exec sleep 86400"
            % DOCKER_DISPLAY]


def docker_inner_cmd(container, python, variant, exe, *script_args):
    """`docker exec` of this script's Linux side (--inner-*) in the container."""
    cmd = ["docker", "exec", container, python, "tools/run_vectors.py", "--inner-variant", variant]
    if exe:
        cmd += ["--exe", exe]
    return cmd + list(script_args)


class LinuxDockerLauncher(LinuxModernLauncher):
    """The Linux UIs inside a Docker container (--docker IMAGE), for a system that is not a
    machine, such as the manylinux2010 image (CentOS 6, glibc 2.12). prepare() starts one container
    with the worktree mounted at /src, the build volume (where tools/build.py built the UI inside
    that same image, see --docker-volume) and a private Xvfb; every run is a `docker exec` of this
    script's --inner-run (the text travels as hex, as through WSL); close() removes the container."""

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.image = args.docker
        self.volume = args.docker_volume
        self.container = None

    def _inner(self, *script_args):
        return docker_inner_cmd(self.container, DOCKER_PYTHON, LINUX_UI["variant"],
                                LINUX_UI["exe"], *script_args)

    def _exec(self, command, timeout=120):
        return subprocess.run(["docker", "exec", self.container, "bash", "-c", command],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)

    def prepare(self):
        res = subprocess.run(docker_start_cmd(self.image, common.ROOT, self.volume),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
        if res.returncode != 0:
            raise RuntimeError("cannot start the container of image %s: %s"
                               % (self.image, res.stderr.decode("utf-8", "replace").strip()))
        self.container = res.stdout.decode("utf-8", "replace").strip()
        for _ in range(50):  # the X server of the container takes a moment
            if self._exec("test -S /tmp/.X11-unix/X%s" % DOCKER_DISPLAY[1:], 20).returncode == 0:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("the Xvfb of the container did not start")
        res = subprocess.run(self._inner("--inner-where"), stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=120)
        exe = res.stdout.decode("utf-8", "replace").strip().splitlines()
        if res.returncode != 0 or not exe or not exe[-1].startswith("/"):
            raise RuntimeError(
                "cannot locate the Linux %s executable in the container (volume %s): build it there "
                "with 'python tools/build.py linux %s'\n%s"
                % (LINUX_UI["variant"], self.volume, LINUX_UI["variant"],
                   res.stderr.decode("utf-8", "replace").strip()))
        system = self._exec("head -n 1 /etc/redhat-release 2>/dev/null || head -n 1 /etc/issue; "
                            "ldd --version | head -n 1").stdout.decode("utf-8", "replace")
        return "docker %s (%s): %s" % (self.image, "; ".join(system.strip().splitlines()), exe[-1])

    def run(self, text, timeout):
        start = time.time()
        cmd = self._inner("--inner-run", "x" + text.encode("utf-8").hex(), "--timeout", str(timeout))
        try:
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 timeout=timeout + 40)
        except subprocess.TimeoutExpired:
            return RunResult(None, "", "", time.time() - start, "docker exec timed out")
        lines = [l for l in res.stdout.decode("utf-8", "replace").splitlines() if l.startswith("{")]
        if res.returncode != 0 or not lines:
            return RunResult(None, "", res.stderr.decode("utf-8", "replace"), time.time() - start,
                             "docker exec failed (status %d)" % res.returncode)
        reply = json.loads(lines[-1])
        if "error" in reply:
            return RunResult(None, "", "", time.time() - start, reply["error"])
        return RunResult(reply["returncode"], reply["stdout"], reply["stderr"], time.time() - start)

    def close(self):
        if self.container:
            subprocess.run(["docker", "rm", "-f", self.container], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=120)
            self.container = None


WINDOWS_PROBE_FILE = "loomora-probe.json"


def windows_executable(arch):
    return common.ROOT / "out" / "windows-legacy" / arch / "loomora.exe"


class WindowsLegacyLauncher(Launcher):
    length_unit = "utf16"  # EM_LIMITTEXT counts UTF-16 code units

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.arch = args.arch or "x64"
        self.tag = self.arch
        self.exe = Path(args.exe) if args.exe else windows_executable(self.arch)

    def _build(self):
        distro = self.args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
        res = subprocess.run(["wsl", "-d", distro, "--cd", str(common.ROOT), "--", "python3",
                              "tools/build.py", "windows", "legacy"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
        if res.returncode != 0:
            raise RuntimeError("the build of the Windows Legacy UI failed in WSL (%s):\n%s"
                               % (distro, res.stdout.decode("utf-8", "replace")[-2000:]))

    def prepare(self):
        if sys.platform != "win32":
            raise RuntimeError("the Windows Legacy executables run on a Windows host only "
                               "(this host is %s; no Wine)" % sys.platform)
        if not self.exe.is_file():
            self._build()
        if not self.exe.is_file():
            raise RuntimeError("executable not found: %s (run 'python tools/build.py windows "
                               "legacy' in WSL)" % self.exe)
        try:
            shown = self.exe.relative_to(common.ROOT).as_posix()
        except ValueError:  # --exe outside the repository
            shown = str(self.exe)
        return "Windows legacy %s: %s (%d bytes, sha256 %s)" % (
            self.arch, shown, self.exe.stat().st_size, common.sha256(self.exe))

    def run(self, text, timeout):
        start = time.time()
        scratch = tempfile.mkdtemp(prefix="loomora-run-")
        try:
            exe = os.path.join(scratch, "loomora.exe")
            shutil.copyfile(str(self.exe), exe)
            try:
                res = subprocess.run([exe, "--probe", "--probe-input", text], cwd=scratch,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     timeout=timeout)
            except subprocess.TimeoutExpired:
                return RunResult(None, "", "", time.time() - start,
                                 "timeout after %s s (the UI should exit by itself after 2 s)"
                                 % timeout)
            except OSError as e:
                return RunResult(None, "", "", time.time() - start,
                                 "cannot start %s: %s" % (self.exe, e))
            try:
                with open(os.path.join(scratch, WINDOWS_PROBE_FILE), "rb") as f:
                    line = f.read().decode("utf-8", "replace")
            except OSError:
                line = ""  # check_run reports the missing line next to the exit status
            return RunResult(res.returncode, line, res.stderr.decode("utf-8", "replace"),
                             time.time() - start)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)


# ---- Windows Modern UI (WinUI 3) on the local Windows host ---------------------------------------

WINUI_EXE = "Loomora.WinUI.exe"


def winui_folder(arch, target="net10"):
    """The published folder that `python tools/build.py windows modern` leaves."""
    return common.ROOT / "build" / "windows-modern" / ("%s-%s" % (target, arch))


class WindowsModernLauncher(Launcher):
    """The Windows Modern UI (WinUI 3, C#, loomora.dll through LibraryImport): the published
    folder of tools/build.py windows modern (default build/windows-modern/net10-x64, or the folder
    of --exe). The folder is copied once into a scratch directory (the probe file lands next to the
    executable, never in the build folder) and the copy is started once per vector, serially (one
    WinUI process at a time), with `--probe --probe-input <text>` through CreateProcess and an
    argument list, like the Legacy launcher. The probe line is read from standard output (the
    contract requires it, here a pipe) and compared with loomora-probe.json next to the
    executable; a difference is a failure. The field limit (TextBox MaxLength) counts UTF-16
    units."""

    length_unit = "utf16"
    max_jobs = 1

    def __init__(self, args):
        Launcher.__init__(self, args)
        arch = args.arch or "x64"
        if arch != "x64":
            raise RuntimeError("the Windows Modern launcher runs x64 only on this host (--arch %s)" % arch)
        self.exe = Path(args.exe) if args.exe else winui_folder(arch) / WINUI_EXE
        self.tag = self.exe.parent.name  # for example net10-x64
        self.scratch = None
        self.copy = None

    def prepare(self):
        if sys.platform != "win32":
            raise RuntimeError("the Windows Modern UI runs on a Windows host only (this host is %s)"
                               % sys.platform)
        if not self.exe.is_file():
            raise RuntimeError("executable not found: %s (run 'python tools/build.py windows "
                               "modern' on the Windows host)" % self.exe)
        self.scratch = tempfile.mkdtemp(prefix="loomora-winui-run-")
        folder = os.path.join(self.scratch, "app")
        shutil.copytree(str(self.exe.parent), folder)
        self.copy = os.path.join(folder, self.exe.name)
        try:
            shown = self.exe.relative_to(common.ROOT).as_posix()
        except ValueError:  # --exe outside the repository
            shown = str(self.exe)
        return "Windows modern %s: %s (%d bytes, sha256 %s; %d files, %d bytes in the folder)" % (
            self.tag, shown, self.exe.stat().st_size, common.sha256(self.exe),
            sum(1 for p in self.exe.parent.rglob("*") if p.is_file()),
            sum(p.stat().st_size for p in self.exe.parent.rglob("*") if p.is_file()))

    @staticmethod
    def _alive():
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + WINUI_EXE, "/NH"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
        return out.decode("oem", "replace").count(WINUI_EXE)

    def run(self, text, timeout):
        start = time.time()
        probe_file = os.path.join(os.path.dirname(self.copy), WINDOWS_PROBE_FILE)
        if os.path.exists(probe_file):
            os.remove(probe_file)
        try:
            res = subprocess.run([self.copy, "--probe", "--probe-input", text],
                                 cwd=os.path.dirname(self.copy), stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/F", "/IM", WINUI_EXE], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            return RunResult(None, "", "", time.time() - start,
                             "timeout after %s s (the UI should exit by itself after 2 s)" % timeout)
        except OSError as e:
            return RunResult(None, "", "", time.time() - start, "cannot start %s: %s" % (self.copy, e))
        stdout = res.stdout.decode("utf-8", "replace")
        try:
            with open(probe_file, "rb") as f:
                from_file = f.read().decode("utf-8", "replace")
        except OSError:
            from_file = ""
        result = RunResult(res.returncode, stdout, res.stderr.decode("utf-8", "replace"),
                           time.time() - start)
        if from_file != stdout:
            result.error = ("the probe line on standard output differs from loomora-probe.json "
                            "(stdout %r, file %r)" % (_clip(stdout), _clip(from_file)))
        result.leftover = self._alive()
        return result

    def close(self):
        if self.scratch:
            shutil.rmtree(self.scratch, ignore_errors=True)


# ---- Windows Legacy UI in a VirtualBox guest (Windows 2000) -------------------------------------

# Guest platforms run through VirtualBox: platform id -> (default VM name, executable architecture).
# The VM name can be overridden with LOOMORA_VM_NAME.
VBOX_PLATFORMS = {"windows-2000-x86": ("Windows 2000", "x86")}

GUEST_EXE = "loomora.exe"
GUEST_HELPER = "runw.exe"
GUEST_CMDLINE = "cmdline.txt"
GUEST_RUN_BAT = "run.bat"
GUEST_KILL_BAT = "kill.bat"
GUEST_LAUNCH_BAT = "launch.bat"
HELPER_SOURCE = "tools/guest/runw.c"
HELPER_BUILD = "tools/guest/build_runw.sh"
TAG_EXIT = "LOOMORA-EXIT "
TAG_WALL = "LOOMORA-WALLMS "
TAG_STARTED = "LOOMORA-STARTED "
TAG_PROBE = "LOOMORA-PROBE "
TAG_LEFTOVER = "LOOMORA-LEFTOVER "
TAG_ERROR = "LOOMORA-RUNERROR "


def quote_windows_arg(arg):
    """One argument quoted for the Microsoft C runtime command-line rules (the rules probe.cpp
    applies in reverse): always in double quotes, a quote preceded by a backslash, the
    backslashes in front of a quote or of the closing quote doubled. "" for the empty string."""
    out = ['"']
    slashes = 0
    for ch in arg:
        if ch == "\\":
            slashes += 1
            continue
        if ch == '"':
            out.append("\\" * (2 * slashes + 1) + '"')
        else:
            out.append("\\" * slashes + ch)
        slashes = 0
    out.append("\\" * (2 * slashes) + '"')
    return "".join(out)


def build_command_line(exe_path, args):
    """The command line of the guest process: the quoted program path and the quoted arguments."""
    return " ".join([quote_windows_arg(exe_path)] + [quote_windows_arg(a) for a in args])


def encode_command_line(command_line):
    """cmdline.txt as runw.exe reads it: UTF-16LE with a byte order mark, no line ending."""
    return b"\xff\xfe" + command_line.encode("utf-16-le")


def sha256_retry(path, attempts=20):
    """sha256 of a file another process has just written: a virus scanner may hold it for a moment."""
    for attempt in range(attempts):
        try:
            return common.sha256(path)
        except PermissionError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.5)


class VBoxError(RuntimeError):
    """The VM cannot be driven: not running, credentials rejected, VBoxManage missing, ..."""


class VBoxGuest(object):
    """Drives one running VirtualBox guest with VBoxManage guestcontrol, as a given guest user.
    Never starts, stops, snapshots, restores or reconfigures the VM."""

    def __init__(self, vboxmanage, vm, user, password):
        self.vboxmanage = vboxmanage
        self.vm = vm
        self.user = user
        self.password = password
        self._password_file = None

    @staticmethod
    def find_vboxmanage():
        candidates = [os.environ.get("LOOMORA_VBOXMANAGE"), shutil.which("VBoxManage"),
                      r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"]
        for c in candidates:
            if c and os.path.isfile(c):
                return c
        raise VBoxError("VBoxManage not found: install VirtualBox or set LOOMORA_VBOXMANAGE")

    def close(self):
        if self._password_file:
            try:
                os.remove(self._password_file)
            except OSError:
                pass
            self._password_file = None

    def _credentials(self):
        if not self.password:
            return ["--username", self.user, "--password", ""]
        if self._password_file is None:  # not on the command line, where any process could read it
            fd, self._password_file = tempfile.mkstemp(prefix="loomora-vm-")
            with os.fdopen(fd, "w") as f:
                f.write(self.password)
        return ["--username", self.user, "--passwordfile", self._password_file]

    def vbm(self, args, timeout):
        """Runs VBoxManage; returns (returncode, stdout text, stderr text)."""
        try:
            res = subprocess.run([self.vboxmanage] + list(args), stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise VBoxError("VBoxManage %s timed out after %s s" % (args[0], timeout))
        except OSError as e:
            raise VBoxError("cannot start %s: %s" % (self.vboxmanage, e))
        return (res.returncode, res.stdout.decode("utf-8", "replace"),
                res.stderr.decode("utf-8", "replace"))

    def guestcontrol(self, sub, args, timeout=60):
        rc, out, err = self.vbm(["guestcontrol", self.vm, sub] + self._credentials() + list(args),
                                timeout)
        if "not able to logon" in err or "VERR_AUTHENTICATION_FAILURE" in err:
            raise VBoxError("the guest rejected the credentials of user %r on VM %r (set "
                            "LOOMORA_VM_USER / LOOMORA_VM_PASSWORD)" % (self.user, self.vm))
        return rc, out, err

    def state(self):
        rc, out, err = self.vbm(["showvminfo", self.vm, "--machinereadable"], 60)
        if rc != 0:
            raise VBoxError("VM %r not found by VBoxManage (LOOMORA_VM_NAME): %s"
                            % (self.vm, err.strip()[:300]))
        info = {}
        for line in out.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                info[key] = value.strip().strip('"')
        return info

    def guest_property(self, name):
        rc, out, _err = self.vbm(["guestproperty", "get", self.vm, name], 60)
        return out.strip()[len("Value: "):] if rc == 0 and out.startswith("Value: ") else ""

    def mkdir(self, guest_dir):
        rc, _out, err = self.guestcontrol("mkdir", ["--parents", guest_dir])
        if rc != 0:
            raise VBoxError("cannot create %s in the guest: %s" % (guest_dir, err.strip()[:300]))

    def copyto(self, host_path, guest_path):
        rc, _out, err = self.guestcontrol("copyto", [str(host_path), guest_path])
        if rc != 0:
            raise VBoxError("cannot copy %s to %s in the guest: %s"
                            % (host_path, guest_path, err.strip()[:300]))

    def copyfrom(self, guest_path, host_path):
        rc, _out, _err = self.guestcontrol("copyfrom", [guest_path, str(host_path)])
        return rc == 0

    def run_program(self, guest_exe, timeout):
        """Runs a guest program and waits for it (the old Guest Additions pass no arguments, so
        this is a .bat or an .exe that takes none); returns (VBoxManage status, stdout, stderr)."""
        return self.guestcontrol("run", ["--exe", guest_exe, "--wait-stdout", "--wait-stderr",
                                         "--timeout", str(int(timeout * 1000))], timeout + 60)


class WindowsVirtualBoxLauncher(Launcher):
    """The Windows Legacy UI (x86 executable) in a VirtualBox guest, for example Windows 2000.

    Two limits of the guest shape the design (measured on Windows 2000 build 2195, Guest Additions
    6.1.18): VBoxManage guestcontrol run does not pass command-line arguments to the guest
    program, and Windows Script Host 5.1 (WshShell.Run) converts the command line to the ANSI code
    page (every other character becomes '?') and expands %VARIABLES%, so a script cannot carry
    the vectors. Each run therefore copies cmdline.txt (the whole command line, UTF-16LE) into
    the guest folder and runs run.bat, which starts tools/guest/runw.c (built with MinGW-w64 on
    first use). runw.exe reads cmdline.txt, starts loomora.exe with that exact Unicode command line
    through CreateProcessW, waits for it, and prints the exit status, the first line of
    loomora-probe.json (written next to the executable) and the number of loomora.exe processes
    still alive (terminated), which come back on the standard output of the guest run. Quoting
    follows the C runtime rules probe.cpp applies in reverse (quote_windows_arg).

    Settings (environment; nothing secret is in the repository): LOOMORA_VM_NAME, LOOMORA_VM_USER
    (default "name"), LOOMORA_VM_PASSWORD (default empty), LOOMORA_VM_DIR (default
    "C:\\Documents and Settings\\<user>\\loomora-vectors"), LOOMORA_VBOXMANAGE. The VM must already
    be running with the user logged on; the launcher never starts, stops or restores it."""

    length_unit = "utf16"  # EM_LIMITTEXT counts UTF-16 code units
    max_jobs = 1           # one VM, one guest session at a time

    def __init__(self, args):
        Launcher.__init__(self, args)
        default_vm, self.arch = VBOX_PLATFORMS[args.platform_id]
        if args.arch and args.arch != self.arch:
            raise VBoxError("platform %s runs the %s executable only (--arch %s given)"
                            % (args.platform_id, self.arch, args.arch))
        self.vm = os.environ.get("LOOMORA_VM_NAME") or default_vm
        self.user = os.environ.get("LOOMORA_VM_USER") or "name"
        password = os.environ.get("LOOMORA_VM_PASSWORD") or ""
        self.guest_dir = (os.environ.get("LOOMORA_VM_DIR")
                          or "C:\\Documents and Settings\\%s\\loomora-vectors" % self.user)
        self.exe = windows_executable(self.arch)
        self.helper = common.ROOT / "out" / "windows-legacy" / "guest" / GUEST_HELPER
        self.guest = VBoxGuest(VBoxGuest.find_vboxmanage(), self.vm, self.user, password)
        # Not tempfile.mkdtemp: its owner-only ACL (Python 3.13 on Windows) makes the files that
        # VBoxManage copyfrom creates in it unreadable to us.
        self.scratch = os.path.join(tempfile.gettempdir(), "loomora-vbox-%d" % os.getpid())
        os.makedirs(self.scratch)
        self._serial = threading.Lock()  # copy of cmdline.txt and its run are one step

    def close(self):
        self.guest.close()
        shutil.rmtree(self.scratch, ignore_errors=True)

    def guest_path(self, name):
        return self.guest_dir + "\\" + name

    def _host_file(self, name, data):
        path = os.path.join(self.scratch, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def _build_helper(self):
        distro = self.args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
        res = subprocess.run(["wsl", "-d", distro, "--cd", str(common.ROOT), "--", "sh",
                              HELPER_BUILD], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=600)
        if res.returncode != 0 or not self.helper.is_file():
            raise VBoxError("cannot build the guest helper %s in WSL (%s):\n%s"
                            % (HELPER_SOURCE, distro, res.stdout.decode("utf-8", "replace")[-1500:]))

    def _sync(self, host_file, name):
        """Makes the guest copy of a file identical to the host file; True when it was reused."""
        want = sha256_retry(str(host_file))
        have = os.path.join(self.scratch, "guest-" + name)
        if self.guest.copyfrom(self.guest_path(name), have) and sha256_retry(have) == want:
            return True
        self.guest.copyto(host_file, self.guest_path(name))
        if not (self.guest.copyfrom(self.guest_path(name), have) and sha256_retry(have) == want):
            raise VBoxError("the copy of %s in the guest differs from the host file" % name)
        return False

    def prepare(self):
        info = self.guest.state()
        if info.get("VMState") != "running":
            raise VBoxError("VM %r is not running (state: %s): start it and log on first, this "
                            "launcher never starts, stops or restores a VM"
                            % (self.vm, info.get("VMState", "unknown")))
        if not self.exe.is_file():
            raise VBoxError("executable not found: %s (run 'python tools/build.py windows legacy' "
                            "in WSL)" % self.exe)
        source = common.ROOT / HELPER_SOURCE
        if not self.helper.is_file() or self.helper.stat().st_mtime < source.stat().st_mtime:
            self._build_helper()
        self.guest.mkdir(self.guest_dir)  # also proves the credentials
        reused_exe = self._sync(self.exe, GUEST_EXE)
        self._sync(self.helper, GUEST_HELPER)
        for name, option in ((GUEST_RUN_BAT, ""), (GUEST_KILL_BAT, " --kill"),
                             (GUEST_LAUNCH_BAT, " --nowait")):
            bat = "@echo off\r\ncd /d \"%s\"\r\n\"%s\"%s\r\n" % (
                self.guest_dir, self.guest_path(GUEST_HELPER), option)
            self.guest.copyto(self._host_file(name, bat.encode("ascii")), self.guest_path(name))
        product = self.guest.guest_property("/VirtualBox/GuestInfo/OS/Product")
        release = self.guest.guest_property("/VirtualBox/GuestInfo/OS/Release")
        service_pack = self.guest.guest_property("/VirtualBox/GuestInfo/OS/ServicePack")
        return ("VirtualBox VM %r (%s MB RAM), guest %s %s service pack %r, guest user %r, "
                "folder %s, %s %s (%d bytes, sha256 %s)"
                % (self.vm, info.get("memory", "?"), product or "?", release or "?",
                   service_pack or "none reported", self.user, self.guest_dir,
                   "reused" if reused_exe else "copied",
                   self.shown_exe(), self.exe.stat().st_size,
                   common.sha256(self.exe)))

    def shown_exe(self):
        """The executable as shown in the launcher line: relative to the repository, or as given
        when it lies outside it (tools/prefix_check.py runs the exe of its copy)."""
        try:
            return self.exe.relative_to(common.ROOT).as_posix()
        except ValueError:
            return str(self.exe)

    def kill_leftovers(self):
        """Terminates loomora.exe and runw.exe in the guest (after a timeout); returns the count
        reported by the guest, or None."""
        try:
            _rc, out, _err = self.guest.run_program(self.guest_path(GUEST_KILL_BAT), 60)
        except VBoxError:
            return None
        for line in out.splitlines():
            if line.startswith("LOOMORA-KILLED "):
                return int(line.split()[-1])
        return None

    def run_exe(self, exe_args, timeout):
        """Runs loomora.exe in the guest with `exe_args` (list of str, any Unicode text) and
        waits; returns a RunResult whose `leftover` is the number of loomora.exe processes alive
        after the run (None: unknown) and whose stdout is the first line of loomora-probe.json."""
        start = time.time()
        command = build_command_line(self.guest_path(GUEST_EXE), exe_args)
        with self._serial:
            try:
                self.guest.copyto(self._host_file(GUEST_CMDLINE, encode_command_line(command)),
                                  self.guest_path(GUEST_CMDLINE))
                rc, out, err = self.guest.run_program(self.guest_path(GUEST_RUN_BAT), timeout)
            except VBoxError as e:
                if "credentials" in str(e):
                    raise
                killed = self.kill_leftovers()
                return RunResult(None, "", "", time.time() - start,
                                 "%s (guest processes terminated: %s)" % (e, killed))
            result = RunResult(None, "", err, 0)
            probe = []
            for line in out.splitlines():
                if line.startswith(TAG_EXIT):
                    result.returncode = int(line[len(TAG_EXIT):])
                elif line.startswith(TAG_WALL):
                    result.guest_wall_ms = int(line[len(TAG_WALL):])
                elif line.startswith(TAG_PROBE):
                    probe.append(line[len(TAG_PROBE):])
                elif line.startswith(TAG_LEFTOVER):
                    result.leftover = int(line[len(TAG_LEFTOVER):])
                elif line.startswith(TAG_ERROR):
                    result.error = "guest helper: " + line[len(TAG_ERROR):]
            result.stdout = "".join(l + "\n" for l in probe)
            if result.returncode is None and not result.error:
                killed = self.kill_leftovers()
                result.error = ("no exit status came back from the guest (VBoxManage status %d, "
                                "timeout %s s, guest processes terminated: %s): %s"
                                % (rc, timeout, killed, (out + err).strip()[:300]))
        result.elapsed = time.time() - start
        return result

    def start_exe(self, exe_args, timeout=60, exe_name=GUEST_EXE):
        """Starts a program of the guest folder (loomora.exe) without waiting (a window left open
        on the desktop); returns its process id. Raises VBoxError when it does not start."""
        command = build_command_line(self.guest_path(exe_name), exe_args)
        with self._serial:
            self.guest.copyto(self._host_file(GUEST_CMDLINE, encode_command_line(command)),
                              self.guest_path(GUEST_CMDLINE))
            rc, out, err = self.guest.run_program(self.guest_path(GUEST_LAUNCH_BAT), timeout)
        for line in out.splitlines():
            if line.startswith(TAG_STARTED):
                return int(line[len(TAG_STARTED):])
        raise VBoxError("loomora.exe did not start in the guest: %s" % (out + err).strip()[:300])

    def run(self, text, timeout):
        return self.run_exe(["--probe", "--probe-input", text], timeout)


# ---- Android Legacy UI on an emulator or device through adb -----------------------------------

ANDROID_PACKAGE = "fr.bansan.loomora.probe"  # Legacy UI
ANDROID_MODERN_PACKAGE = ANDROID_PACKAGE + ".modern"
ANDROID_ACTIVITY = ANDROID_PACKAGE + "/.MainActivity"
ANDROID_TAG = "LOOMORA_PROBE"
ANDROID_INPUT_FILE = "/data/local/tmp/loomora-probe-input.txt"
ANDROID_MAX_EXTRA_BYTES = 1500  # longest text given as the plain probe_input extra


def adb_executable(args):
    """adb: --adb, $LOOMORA_ADB, platform-tools of the SDK ($LOOMORA_ANDROID_SDK / $ANDROID_HOME /
    $ANDROID_SDK_ROOT), then PATH."""
    if args.adb:
        return args.adb
    if os.environ.get("LOOMORA_ADB"):
        return os.environ["LOOMORA_ADB"]
    for name in ("LOOMORA_ANDROID_SDK", "ANDROID_HOME", "ANDROID_SDK_ROOT"):
        sdk = os.environ.get(name)
        if sdk:
            for exe in ("adb.exe", "adb"):
                path = Path(sdk) / "platform-tools" / exe
                if path.is_file():
                    return str(path)
    found = shutil.which("adb")
    if found:
        return found
    raise RuntimeError("adb not found: set LOOMORA_ANDROID_SDK (or ANDROID_HOME), LOOMORA_ADB or "
                       "use --adb")


def shell_quote(text):
    """The text as one POSIX shell word (single quotes; every character, newlines included, kept)."""
    return "'" + text.replace("'", "'\\''") + "'"


# 32-bit ABIs that a 64-bit Android device also runs: a UI whose APK has no library of the 64-bit
# ABI runs as a 32-bit process, and the probe line then names that ABI (Build.CPU_ABI).
ANDROID_COMPANION_ABIS = {"x86_64": ("x86",), "arm64-v8a": ("armeabi-v7a", "armeabi")}


def android_probe_platform_id(platform_id, ui_abi=None):
    """The platform_id the probe line of an Android UI must carry on the platform `platform_id`
    (android-<release>-<device ABI>). Without ui_abi it is `platform_id` itself, so nothing is
    relaxed. With ui_abi (the ABI the UI process runs as) only the ABI part changes, and only to
    a 32-bit companion of the device's ABI (x86 on x86_64, armeabi-v7a or armeabi on arm64-v8a):
    the release must still match and any other ABI is refused with a ValueError."""
    if not ui_abi:
        return platform_id
    m = re.match(r"^(android-[0-9][0-9.]*)-(.+)$", platform_id)
    if not m:
        raise ValueError("cannot read the release and the ABI in the platform id %r "
                         "(expected android-<release>-<abi>)" % platform_id)
    release, device_abi = m.groups()
    if ui_abi == device_abi:
        return platform_id
    if ui_abi not in ANDROID_COMPANION_ABIS.get(device_abi, ()):
        raise ValueError("--ui-abi %s: a UI of this ABI does not run on a %s device "
                         "(accepted there: %s)" % (ui_abi, device_abi,
                         ", ".join(ANDROID_COMPANION_ABIS.get(device_abi, ())) or "none"))
    return "%s-%s" % (release, ui_abi)


class AndroidLegacyLauncher(Launcher):
    """The Android Legacy UI (an APK) on ONE running emulator or device, driven with adb.

    The APK (out/android-legacy/loomora.apk from `python tools/build.py android legacy`, or --exe)
    is installed in prepare(). A run force-stops the application, clears logcat, starts
    fr.bansan.loomora.probe/.MainActivity with `am start -W --ez probe true --es probe_input '<text>'`
    (the text is one single-quoted shell word, so quotes, newlines, backslashes and the empty
    string reach the extra unchanged; a text with a character outside the BMP or of more than
    ANDROID_MAX_EXTRA_BYTES bytes is pushed to a file and given as `probe_input_file`, because the
    `am` of Android 4.1 crashes on the first and adb refuses a 4,096-byte command for the second) and polls `logcat -d -v raw -s LOOMORA_PROBE:I` for the
    probe line. The UI logs a long line as numbered parts "#i/n <text>" (a logcat entry holds
    about 4,000 bytes): they are joined here. The run is a failure when the line does not come, the
    process crashed (a FATAL EXCEPTION in logcat) or the activity is still resumed 6 s after the
    line (the UI finishes by itself). Runs are serial (one device). The emulator is started and
    stopped by the person running this tool, never here."""

    length_unit = "utf16"  # the field limit counts Java chars (UTF-16 units)
    max_jobs = 1
    variant = "legacy"
    package = ANDROID_PACKAGE
    activity = ".MainActivity"
    build_command = "python tools/build.py android legacy"

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.apk = Path(args.exe) if args.exe else common.ROOT / "out" / ("android-" + self.variant) / "loomora.apk"
        self.serial = args.serial or os.environ.get("ANDROID_SERIAL")
        self.adb = adb_executable(args)
        self.ui_abi = getattr(args, "ui_abi", None)
        if self.ui_abi:
            self.tag = "ui-" + self.ui_abi  # evidence files of this run are named after it

    def probe_platform_id(self, platform_id):
        try:
            return android_probe_platform_id(platform_id, self.ui_abi)
        except ValueError as e:
            raise RuntimeError(str(e))

    def _adb(self, *cmd, **kwargs):
        full = [self.adb] + (["-s", self.serial] if self.serial else []) + [str(c) for c in cmd]
        timeout = kwargs.pop("timeout", 60)
        res = subprocess.run(full, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return res.returncode, res.stdout.decode("utf-8", "replace"), res.stderr.decode("utf-8", "replace")

    def _prop(self, name):
        return self._adb("shell", "getprop " + name)[1].strip()

    def prepare(self):
        if not self.apk.is_file():
            raise RuntimeError("APK not found: %s (run '%s')"
                               % (self.apk, self.build_command))
        code, out, err = self._adb("get-state")
        if code != 0 or out.strip() != "device":
            raise RuntimeError("no Android device or emulator ready (adb get-state: %r %s); start "
                               "one first, this tool never does" % (out.strip(), err.strip()))
        if self._prop("sys.boot_completed") != "1":
            raise RuntimeError("the device has not finished booting (sys.boot_completed)")
        if self.ui_abi:
            abilist = self._prop("ro.product.cpu.abilist").split(",")
            if abilist == [""]:
                # before API 21 there is no abilist: the ABIs are in ro.product.cpu.abi and abi2
                abilist = [self._prop("ro.product.cpu.abi"), self._prop("ro.product.cpu.abi2")]
                abilist = [abi for abi in abilist if abi]
            if self.ui_abi not in abilist:
                raise RuntimeError("--ui-abi %s: the device lists %s in ro.product.cpu.abilist"
                                   % (self.ui_abi, ",".join(abilist)))
        code, out, err = self._adb("install", "-r", self.apk, timeout=300)
        if "INCONSISTENT_CERTIFICATES" in out + err or "INSTALL_FAILED_UPDATE_INCOMPATIBLE" in out + err:
            # an APK signed with another debug key is installed (the name of the error depends on the API level)
            self._adb("uninstall", self.package)
            code, out, err = self._adb("install", self.apk, timeout=300)
        if code != 0 or "Success" not in out:
            raise RuntimeError("adb install failed: %s %s" % (out.strip(), err.strip()))
        return "Android %s UI %s (release %s, API %s, abi %s): %s (%d bytes, sha256 %s)" % (
            self.variant.capitalize(), self._prop("ro.product.model") or "device",
            self._prop("ro.build.version.release"),
            self._prop("ro.build.version.sdk"), self._prop("ro.product.cpu.abi"),
            self.apk.name, self.apk.stat().st_size, common.sha256(self.apk))

    def _probe_line(self):
        """The probe line currently in logcat ("" when none), parts joined."""
        out = self._adb("logcat", "-d", "-v", "raw", "-s", ANDROID_TAG + ":I", timeout=30)[1]
        entries = [l.strip("\r") for l in out.split("\n") if l.strip("\r") and not l.startswith("-----")]
        parts = {}
        total = None
        whole = []
        for entry in entries:
            m = re.match(r"^#(\d+)/(\d+) (.*)$", entry)
            if m:
                parts[int(m.group(1))] = m.group(3)
                total = int(m.group(2))
            else:
                whole.append(entry)
        if whole:
            return whole[-1]
        if total and len(parts) == total:
            return "".join(parts[i] for i in range(1, total + 1))
        return ""

    def run(self, text, timeout):
        start = time.time()
        self._adb("shell", "am force-stop " + self.package)
        self._adb("logcat", "-c")
        data = text.encode("utf-8")
        if len(data) <= ANDROID_MAX_EXTRA_BYTES and all(ord(c) < 0x10000 for c in text):
            extra = "--es probe_input " + shell_quote(text)
        else:  # the `am` of Android 4.1 crashes on a non-BMP character; adb limits the command
            self._adb("shell", "rm -f " + ANDROID_INPUT_FILE)
            with tempfile.TemporaryDirectory() as scratch:
                local = os.path.join(scratch, "input.txt")
                with open(local, "wb") as f:
                    f.write(data)
                self._adb("push", local, ANDROID_INPUT_FILE)
            self._adb("shell", "chmod 644 " + ANDROID_INPUT_FILE)
            extra = "--es probe_input_file " + ANDROID_INPUT_FILE
        cmd = "am start -W -n %s/%s --ez probe true %s" % (self.package, self.activity, extra)
        code, out, err = self._adb("shell", cmd, timeout=60)
        if code != 0 or "Error" in out:
            return RunResult(None, "", err, time.time() - start,
                             "am start failed: %s %s" % (out.strip()[:300], err.strip()[:300]))
        line = ""
        while time.time() - start < timeout:
            line = self._probe_line()
            if line:
                break
            time.sleep(0.5)
        crashed = "FATAL EXCEPTION" in self._adb("logcat", "-d", "-s", "AndroidRuntime:E")[1]
        if not line:
            return RunResult(1 if crashed else None, "", "", time.time() - start,
                             "no probe line in logcat after %s s%s"
                             % (timeout, " (the application crashed)" if crashed else ""))
        status = 1 if crashed else 0
        stderr = "FATAL EXCEPTION in logcat" if crashed else ""
        # the UI finishes by itself: the activity must not stay resumed
        deadline = time.time() + 6
        while status == 0:
            top = self._adb("shell", "dumpsys activity activities")[1]
            resumed = [l for l in top.splitlines() if "esumed" in l and self.package in l]
            if not resumed:
                break
            if time.time() > deadline:
                status, stderr = 1, "the activity is still resumed 6 s after the probe line"
                break
            time.sleep(0.5)
        return RunResult(status, line + "\n", stderr, time.time() - start)


class AndroidModernLauncher(AndroidLegacyLauncher):
    """The Android Modern UI (Kotlin and Jetpack Compose, applicationId fr.bansan.loomora.probe.modern,
    out/android-modern/loomora.apk from `python tools/build.py android modern`): the same adb
    protocol as the Legacy UI (same intent extras, logcat tag, chunked lines). The field limit
    counts code points (a character outside the BMP counts as one)."""

    length_unit = "codepoints"
    variant = "modern"
    package = ANDROID_MODERN_PACKAGE
    # the class lives in the namespace of the code (fr.bansan.loomora.probe), not in the applicationId
    activity = ANDROID_PACKAGE + ".MainActivity"
    build_command = "python tools/build.py android modern"


# ---- web legacy under node (T085) --------------------------------------------------------------

WEB_NODE_PLATFORMS = {"web-node-js": "js", "web-node-wasm": "wasm"}  # platform id -> page path
WEB_HARNESS = "tools/trials/web-node/page-harness.js"


def web_probe_query(text, platform_id):
    """The URL query of the Web page's probe mode (ui-contract.md): ASCII, the text UTF-8
    percent-encoded (a '+' is encoded too, the page reads a plain '+' as a plus sign)."""
    return "?probe=1&input=%s&platform=%s" % (urllib.parse.quote(text, safe=""), platform_id)


class WebNodeLauncher(Launcher):
    """The Web page (Legacy: index.html + app.js; Modern: index.html + ES modules) and its built module
    run by page-harness.js under node, in a DOM stub: no browser. The query goes through standard
    input, so that no command line (the one of wsl.exe included) can change it. The field limit
    counts UTF-16 units (JavaScript). The Modern page only has the wasm path (web-node-wasm)."""

    length_unit = "utf16"

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.path = WEB_NODE_PLATFORMS.get(args.platform_id)
        self.prefix = []
        if sys.platform == "win32":
            distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
            self.prefix = ["wsl", "-d", distro, "--cd", str(common.ROOT), "--"]

    def command(self):
        modern = self.args.variant == "modern"
        # the Modern page is an ES module run through vm.SourceTextModule (experimental in node)
        node = ["node", "--experimental-vm-modules", "--disable-warning=ExperimentalWarning"] if modern else ["node"]
        cmd = self.prefix + node + [WEB_HARNESS, "--variant", self.args.variant, "--path", self.path, "--query", "-"]
        if self.args.exe:
            cmd += ["--dist", self.args.exe]
        return cmd

    def prepare(self):
        if self.path is None:
            raise RuntimeError("%s is not a Web platform run under node (%s)"
                               % (self.args.platform_id, ", ".join(sorted(WEB_NODE_PLATFORMS))))
        probe = subprocess.run(self.command(),
                               input=web_probe_query("Coucou", self.args.platform_id).encode("ascii"),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if probe.returncode != 0:
            raise RuntimeError("the page does not run (build it: python tools/build.py web %s; "
                               "folder of --exe or $LOOMORA_WEB_DIST): %s"
                               % (self.args.variant, probe.stderr.decode("utf-8", "replace").strip()[:300]))
        return ("node page-harness.js, %s path (the page and the module of tools/build.py web %s)"
                % (self.path, self.args.variant))

    def run(self, text, timeout):
        start = time.time()
        try:
            res = subprocess.run(self.command(),
                                 input=web_probe_query(text, self.args.platform_id).encode("ascii"),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            return RunResult(None, "", "", time.time() - start, "timeout after %s s" % timeout)
        except OSError as e:
            return RunResult(None, "", "", time.time() - start, "cannot start node: %s" % e)
        return RunResult(res.returncode, res.stdout.decode("utf-8", "replace"),
                         res.stderr.decode("utf-8", "replace"), time.time() - start)


# ---- Web legacy page in Firefox 10 in a VirtualBox guest (T093, spike S5) -------------------------

# Browser platforms run in a VirtualBox guest: platform id -> (default VM name, firefox.exe in the guest).
WEB_FIREFOX_VM_PLATFORMS = {
    "web-firefox-10-windows-2000-x86": ("Windows 2000", "C:\\Program Files\\Mozilla Firefox\\firefox.exe"),
}
WEB_HOST_ADDRESS = "10.0.2.2"      # the host as seen from a VirtualBox NAT guest
WEB_START_WAIT = 30                # seconds without any request before firefox.exe is started once more
WEB_DIRECT_URL_MAX = 1500          # longer URLs go through a redirect: cmd.exe cuts a line at 8,191
WEB_CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "application/javascript",
                     ".wasm": "application/wasm"}
FIREFOX_PROFILE_PREFS = (
    # Only what removes first-run pages, update and default-browser questions and the restore-session
    # page after a close: nothing here touches JavaScript, the cache or the network.
    ('browser.shell.checkDefaultBrowser', 'false'),
    ('browser.startup.homepage', '"about:blank"'),
    ('browser.startup.page', '0'),
    ('startup.homepage_welcome_url', '""'),
    ('browser.startup.homepage_override.mstone', '"ignore"'),
    ('browser.rights.3.shown', 'true'),
    ('browser.sessionstore.resume_from_crash', 'false'),
    ('browser.tabs.warnOnClose', 'false'),
    ('browser.warnOnQuit', 'false'),
    ('toolkit.telemetry.prompted', '2'),
    ('toolkit.telemetry.rejected', 'true'),
    ('app.update.enabled', 'false'),
    ('app.update.auto', 'false'),
    ('extensions.update.enabled', 'false'),
    ('extensions.blocklist.enabled', 'false'),
    ('browser.search.update', 'false'),
)
GUEST_FF_PROCS = "ffprocs.exe"
FF_HELPER_SOURCE = "tools/guest/ffprocs.c"
FF_HELPER_BUILD = "tools/guest/build_ffprocs.sh"
GUEST_FF_COUNT_BAT = "ffcount.bat"
GUEST_FF_KILL_BAT = "ffkill.bat"
GUEST_FF_LAUNCH_BAT = "fflaunch.bat"
TAG_FF_COUNT = "FIREFOX-PROCESSES "
TAG_FF_KILLED = "FIREFOX-KILLED "


class WebProbeServer(object):
    """The HTTP server of the Firefox runs: serves the dist folder of `python tools/build.py web
    legacy` on every address of the host (the guest reaches it at 10.0.2.2), logs every request,
    and stores what the page sends with post=1: POST /probe (the probe record) and POST
    /probe-error (a failure message). `redirects` maps /v/<run> to a long URL; `posts` maps the
    `run` query field of a POST to (path, body, time). Nothing is cached (Cache-Control: no-store),
    so every run fetches every file again."""

    def __init__(self, dist_dir, port=0, host="0.0.0.0"):
        import http.server
        self.dist = Path(dist_dir)
        self.requests = []       # (utc time text, client, method, path, status, bytes, user agent)
        self.posts = {}
        self.events = {}
        self.redirects = {}
        self.lock = threading.Lock()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"
            server_version = "loomora-probe-server"

            def log_message(self, fmt, *a):  # the log is ours (record)
                pass

            def _done(self, status, size):
                owner.record(self.client_address[0], self.command, self.path, status, size,
                             self.headers.get("User-Agent", ""))

            def _send(self, status, body, ctype="text/plain; charset=us-ascii", extra=()):
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                for k, v in extra:
                    self.send_header(k, v)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)
                self._done(status, len(body))

            def do_HEAD(self):
                self.do_GET()

            def do_GET(self):
                parsed = urllib.parse.urlsplit(self.path)
                name = parsed.path
                if name.startswith("/v/") and name[3:].isdigit() and int(name[3:]) in owner.redirects:
                    self._send(302, b"", extra=(("Location", owner.redirects[int(name[3:])]),))
                    return
                if name == "/":
                    name = "/index.html"
                target = owner.dist / name.lstrip("/")
                if "/" in name.lstrip("/") or not target.is_file():
                    self._send(404, b"not found")
                    return
                self._send(200, target.read_bytes(), WEB_CONTENT_TYPES.get(target.suffix, "application/octet-stream"))

            def do_POST(self):
                parsed = urllib.parse.urlsplit(self.path)
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                if parsed.path not in ("/probe", "/probe-error"):
                    self._send(404, b"not found")
                    return
                query = urllib.parse.parse_qs(parsed.query)
                run = int((query.get("run") or ["0"])[0] or 0)
                with owner.lock:
                    owner.posts[run] = (parsed.path, body, time.time())
                    event = owner.events.get(run)
                self._send(204, b"")
                if event is not None:
                    event.set()

        import socketserver

        class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        self.httpd = Server((host, port), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def record(self, client, method, path, status, size, agent):
        stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        with self.lock:
            self.requests.append((stamp, client, method, path, status, size, agent))

    def expect(self, run):
        event = threading.Event()
        with self.lock:
            self.events[run] = event
            self.posts.pop(run, None)
        return event

    def requested(self, run):
        """True when the page of run number `run` (or its redirect) has been requested."""
        mine = re.compile(r"(?:[?&]run=%d(?:&|$)|^/v/%d$)" % (run, run))
        with self.lock:
            return any(method == "GET" and mine.search(path) for _s, _c, method, path, _st, _z, _a in self.requests)

    def log_text(self):
        lines = []
        for stamp, client, method, path, status, size, agent in self.requests:
            shown = path if len(path) <= 160 else path[:160] + "...(%d chars)" % len(path)
            lines.append("%s %s %s %s -> %d (%d bytes) UA=%s" % (stamp, client, method, shown, status, size, agent))
        return "".join(l + "\n" for l in lines)


class WebFirefoxVmLauncher(Launcher):
    """The Web Legacy page in Firefox 10 in a VirtualBox guest (Windows 2000), JavaScript path.

    The dist folder of `python tools/build.py web legacy` (--exe DIR, $LOOMORA_WEB_DIST, or the
    build folder of WSL) is served over HTTP by this process (WebProbeServer, any free port). Each
    run starts firefox.exe in the guest through a .bat file (the old Guest Additions pass no
    argument to a program) with `-no-remote -profile <guest folder>\\ff-profile` and the URL
    http://10.0.2.2:<port>/index.html?probe=1&input=<text>&platform=<id>&post=1&run=<n>; the page
    posts its probe record back (post=1), which is what is checked. Then the window is closed with
    Alt+F4 (VBoxManage controlvm keyboardputscancode) and the firefox.exe processes still alive
    are counted (tools/guest/ffprocs.c, tool help library), terminated if any. A long text (a URL of
    more than WEB_DIRECT_URL_MAX characters) is reached through a redirect of the server.
    The profile is new, in the guest folder, with only the preferences of FIREFOX_PROFILE_PREFS.
    Settings: LOOMORA_VM_NAME, LOOMORA_VM_USER (default "name"), LOOMORA_VM_PASSWORD (default
    empty), LOOMORA_VM_DIR (default "C:\\Documents and Settings\\<user>\\loomora-web"),
    LOOMORA_VBOXMANAGE, LOOMORA_WEB_DIST. The VM must be running with the user logged on; this
    never starts, stops, snapshots or restores it. Serial runs."""

    length_unit = "utf16"  # the field limit counts UTF-16 units (JavaScript)
    max_jobs = 1

    def __init__(self, args):
        Launcher.__init__(self, args)
        default_vm, self.firefox = WEB_FIREFOX_VM_PLATFORMS[args.platform_id]
        self.vm = os.environ.get("LOOMORA_VM_NAME") or default_vm
        self.user = os.environ.get("LOOMORA_VM_USER") or "name"
        password = os.environ.get("LOOMORA_VM_PASSWORD") or ""
        self.guest_dir = (os.environ.get("LOOMORA_VM_DIR")
                          or "C:\\Documents and Settings\\%s\\loomora-web" % self.user)
        self.profile = self.guest_dir + "\\ff-profile"
        self.guest = VBoxGuest(VBoxGuest.find_vboxmanage(), self.vm, self.user, password)
        self.scratch = os.path.join(tempfile.gettempdir(), "loomora-webvm-%d" % os.getpid())
        os.makedirs(self.scratch)
        self.server = None
        self.run_number = 0
        self.dist = None

    def close(self):
        if self.server is not None:
            self.server.stop()
            self.server = None
        self.guest.close()
        shutil.rmtree(self.scratch, ignore_errors=True)

    def guest_path(self, name):
        return self.guest_dir + "\\" + name

    def _host_file(self, name, data):
        path = os.path.join(self.scratch, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def _put(self, name, data):
        self.guest.copyto(self._host_file(name, data), self.guest_path(name))

    def dist_dir(self):
        if self.args.exe:
            return Path(self.args.exe)
        if os.environ.get("LOOMORA_WEB_DIST"):
            return Path(os.environ["LOOMORA_WEB_DIST"])
        if sys.platform == "win32":  # the build folder of WSL, as a UNC path
            distro = self.args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
            res = subprocess.run(["wsl", "-d", distro, "--", "sh", "-c",
                                  'wslpath -w "${LOOMORA_BUILD_DIR:-$HOME/loomora-build}/web-legacy/dist"'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if res.returncode == 0:
                return Path(res.stdout.decode("utf-8", "replace").strip())
        return Path.home() / "loomora-build" / "web-legacy" / "dist"

    def ff_count(self, kill=False):
        """The number of firefox.exe processes alive in the guest (None: unknown); kill=True
        terminates them first-counted (the count is the one before the termination)."""
        bat = GUEST_FF_KILL_BAT if kill else GUEST_FF_COUNT_BAT
        try:
            _rc, out, _err = self.guest.run_program(self.guest_path(bat), 60)
        except VBoxError:
            return None
        for line in out.splitlines():
            if line.startswith(TAG_FF_COUNT):
                try:
                    return int(line[len(TAG_FF_COUNT):])
                except ValueError:
                    return None
        return None

    def prepare(self):
        info = self.guest.state()
        if info.get("VMState") != "running":
            raise VBoxError("VM %r is not running (state: %s): start it and log on first, this "
                            "launcher never starts, stops or restores a VM"
                            % (self.vm, info.get("VMState", "unknown")))
        self.dist = self.dist_dir()
        for name in ("index.html", "app.js", "loomora.js"):
            if not (self.dist / name).is_file():
                raise VBoxError("%s not found in %s (python tools/build.py web legacy, then "
                                "--exe DIR or LOOMORA_WEB_DIST)" % (name, self.dist))
        rc, _out, err = self.guest.guestcontrol("stat", [self.firefox])
        if rc != 0:
            raise VBoxError("%s not found in the guest: %s" % (self.firefox, err.strip()[:200]))
        self.guest.mkdir(self.guest_dir)
        self.guest.mkdir(self.profile)
        prefs = "".join('user_pref("%s", %s);\n' % kv for kv in FIREFOX_PROFILE_PREFS)
        self.guest.copyto(self._host_file("user.js", prefs.encode("ascii")), self.profile + "\\user.js")
        helper = common.ROOT / "out" / "windows-legacy" / "guest" / GUEST_FF_PROCS
        source = common.ROOT / FF_HELPER_SOURCE
        if not helper.is_file() or helper.stat().st_mtime < source.stat().st_mtime:
            distro = self.args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
            res = subprocess.run(["wsl", "-d", distro, "--cd", str(common.ROOT), "--", "sh",
                                  FF_HELPER_BUILD], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=600)
            if res.returncode != 0 or not helper.is_file():
                raise VBoxError("cannot build the guest helper %s in WSL (%s):\n%s"
                                % (FF_HELPER_SOURCE, distro, res.stdout.decode("utf-8", "replace")[-1500:]))
        self.guest.copyto(helper, self.guest_path(GUEST_FF_PROCS))
        for name, option in ((GUEST_FF_COUNT_BAT, ""), (GUEST_FF_KILL_BAT, " --kill")):
            bat = "@echo off\r\ncd /d \"%s\"\r\n\"%s\"%s\r\n" % (
                self.guest_dir, self.guest_path(GUEST_FF_PROCS), option)
            self._put(name, bat.encode("ascii"))
        before = self.ff_count(kill=True)
        if before is None:
            raise VBoxError("cannot count the firefox.exe processes of the guest (WMI)")
        self.server = WebProbeServer(self.dist)
        self.server.start()
        version = self.firefox_version()
        digests = ", ".join("%s sha256 %s" % (n, common.sha256(self.dist / n)[:16])
                            for n in ("index.html", "app.js", "loomora.js"))
        return ("Firefox %s in VirtualBox VM %r (%s MB RAM), guest user %r, folder %s, server http://%s:%d/ "
                "serving %s (%s); %d firefox.exe process(es) terminated before the series"
                % (version, self.vm, info.get("memory", "?"), self.user, self.guest_dir,
                   WEB_HOST_ADDRESS, self.server.port, self.dist, digests, before))

    def firefox_version(self):
        ini = os.path.join(self.scratch, "application.ini")
        folder = self.firefox.rsplit("\\", 1)[0]
        if self.guest.copyfrom(folder + "\\application.ini", ini):
            try:
                with open(ini, "r", encoding="utf-8", errors="replace") as f:
                    m = re.search(r"^Version=(\S+)", f.read(), re.M)
                if m:
                    return m.group(1)
            except OSError:
                pass
        return "?"

    def page_url(self, query):
        return "http://%s:%d/index.html%s" % (WEB_HOST_ADDRESS, self.server.port, query)

    def open_url(self, url):
        """Starts firefox.exe on `url` without waiting (a window on the guest desktop)."""
        safe = url.replace("%", "%%")
        bat = ("@echo off\r\ncd /d \"%s\"\r\nstart \"\" \"%s\" -no-remote -profile \"%s\" \"%s\"\r\n"
               % (self.firefox.rsplit("\\", 1)[0], self.firefox, self.profile, safe))
        self._put(GUEST_FF_LAUNCH_BAT, bat.encode("ascii"))
        # `start`, not `run`: firefox.exe inherits the output of the .bat, so a `run` would wait
        # for the browser to exit
        rc, _out, err = self.guest.guestcontrol("start", ["--exe", self.guest_path(GUEST_FF_LAUNCH_BAT)])
        if rc != 0:
            raise VBoxError("cannot start %s in the guest: %s" % (GUEST_FF_LAUNCH_BAT, err.strip()[:300]))

    def send_scancodes(self, codes):
        rc, _out, err = self.guest.vbm(["controlvm", self.vm, "keyboardputscancode"]
                                       + ["%02x" % c for c in codes], 60)
        if rc != 0:
            raise VBoxError("keyboardputscancode failed: %s" % err.strip()[:200])

    def close_window(self, wait=20):
        """Alt+F4 on the Firefox window, then waits for the firefox.exe processes to end; returns
        the number still alive after `wait` seconds (they are then terminated), None if unknown."""
        self.send_scancodes([0x38, 0x3E, 0x3E | 0x80, 0x38 | 0x80])
        deadline = time.time() + wait
        count = self.ff_count()
        while count and time.time() < deadline:
            time.sleep(1.5)
            count = self.ff_count()
        if count:
            self.ff_count(kill=True)
        return count

    def extra_evidence(self):
        return {"server-requests.txt": self.server.log_text()}

    def run(self, text, timeout):
        start = time.time()
        self.run_number += 1
        n = self.run_number
        query = web_probe_query(text, self.args.platform_id) + "&post=1&run=%d" % n
        url = self.page_url(query)
        launch_url = url
        if len(url) > WEB_DIRECT_URL_MAX:
            self.server.redirects[n] = url
            launch_url = "http://%s:%d/v/%d" % (WEB_HOST_ADDRESS, self.server.port, n)
        event = self.server.expect(n)
        try:
            self.open_url(launch_url)
        except VBoxError as e:
            self.ff_count(kill=True)
            return RunResult(None, "", "", time.time() - start, str(e))
        launched = time.time()
        relaunched = False
        # About one start in fifteen of firefox.exe is lost in this 168 MB guest (no request ever
        # reaches the server): when the page has not even been requested after WEB_START_WAIT
        # seconds, the processes are ended and the browser is started once more (reported).
        if not event.wait(min(timeout, WEB_START_WAIT)) and not self.server.requested(n):
            relaunched = True
            self.ff_count(kill=True)
            try:
                self.open_url(launch_url)
            except VBoxError as e:
                self.ff_count(kill=True)
                return RunResult(None, "", "", time.time() - start, str(e))
            launched = time.time()
        if not event.wait(timeout):
            self.ff_count(kill=True)
            return RunResult(None, "", "", time.time() - start,
                             "no probe record posted in %s s (firefox.exe processes terminated)" % timeout)
        path, body, posted = self.server.posts[n]
        leftover = self.close_window()
        result = RunResult(0 if path == "/probe" else 1, body.decode("ascii", "replace") + "\n",
                           "" if path == "/probe" else body.decode("ascii", "replace"),
                           time.time() - start)
        result.leftover = leftover
        result.page_ms = int((posted - launched) * 1000)  # start of firefox.exe to the posted record
        result.relaunched = relaunched
        return result


# ---- Web modern page in Chrome on the Windows host (T063) ------------------------------------------

WEB_CHROME_HOST_RE = re.compile(r"^web-chrome-(\d+)-windows-11-x64$")   # a browser run on this host
WEB_CHROME_FLAGS = ("--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                    "--disable-extensions", "--disable-component-update", "--disable-background-networking",
                    "--disable-sync", "--metrics-recording-only")
WEB_CHROME_VIRTUAL_MS = 15000       # --virtual-time-budget: the page needs well under 1 s
WEB_LOOPBACK = "127.0.0.1"


def chrome_executable():
    """chrome.exe of the host: $LOOMORA_CHROME, else the usual install folders."""
    candidates = [os.environ.get("LOOMORA_CHROME")]
    for var in ("ProgramFiles", "ProgramFiles(x86)", "LocalAppData"):
        if os.environ.get(var):
            candidates.append(os.path.join(os.environ[var], "Google", "Chrome", "Application", "chrome.exe"))
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


def chrome_version(exe):
    """The version of an installed Chrome: the name of the version folder next to chrome.exe
    (`chrome.exe --version` prints nothing on Windows); None when there is none."""
    for name in sorted(os.listdir(os.path.dirname(exe)), reverse=True):
        if re.match(r"^\d+(\.\d+){3}$", name):
            return name
    return None


def parse_dumped_dom(dom):
    """(title, probe text) of a DOM dumped by `chrome --dump-dom` (the probe text is None when
    <pre id="probe"> is absent): what the Modern page shows at the end of a probe run."""
    import html
    title = re.search(r"<title>(.*?)</title>", dom, re.S)
    probe = re.search(r'<pre\b[^>]*\bid="probe"[^>]*>(.*?)</pre>', dom, re.S)
    return (html.unescape(title.group(1)) if title else None,
            html.unescape(probe.group(1)) if probe else None)


class WebChromeHostLauncher(Launcher):
    """The Web Modern page in Chrome on the Windows host (platform id web-chrome-<major>-windows-11-x64,
    WebAssembly path). The dist folder of `python tools/build.py web modern` ($LOOMORA_WEB_DIST, --exe
    DIR, or the build folder of WSL) is served over HTTP on 127.0.0.1 (a free port) by this process
    (WebProbeServer). Each run starts chrome.exe `--headless=new --dump-dom` with a new throw-away
    --user-data-dir (a folder of the system temp folder, deleted by close(); the profile of the user
    is never touched) on http://127.0.0.1:<port>/index.html?probe=1&input=<text>&platform=<id>; the
    DOM it dumps must have the title LOOMORA_PROBE_DONE and the probe record in <pre id="probe">
    (the page writes it there). Chrome processes that use the run's profile folder and are still
    alive after the exit are counted (and terminated by PID). Settings: $LOOMORA_CHROME, $LOOMORA_WEB_DIST.
    Serial runs."""

    length_unit = "utf16"  # the field limit counts UTF-16 units (JavaScript)
    max_jobs = 1

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.major = int(WEB_CHROME_HOST_RE.match(args.platform_id).group(1))
        self.exe = None
        self.server = None
        self.profiles = None
        self.run_number = 0

    def dist_dir(self):
        if self.args.exe:
            return Path(self.args.exe)
        if os.environ.get("LOOMORA_WEB_DIST"):
            return Path(os.environ["LOOMORA_WEB_DIST"])
        if sys.platform == "win32":  # the build folder of WSL, as a UNC path
            distro = self.args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
            res = subprocess.run(["wsl", "-d", distro, "--", "sh", "-c",
                                  'wslpath -w "${LOOMORA_BUILD_DIR:-$HOME/loomora-build}/web-modern/dist"'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if res.returncode == 0:
                return Path(res.stdout.decode("utf-8", "replace").strip())
        return Path.home() / "loomora-build" / "web-modern" / "dist"

    def prepare(self):
        if sys.platform != "win32":
            raise RuntimeError("%s runs Chrome on a Windows 11 host" % self.args.platform_id)
        self.exe = chrome_executable()
        if self.exe is None:
            raise RuntimeError("chrome.exe not found (install Chrome or set LOOMORA_CHROME)")
        version = chrome_version(self.exe)
        if version is None or int(version.split(".")[0]) != self.major:
            raise RuntimeError("%s is Chrome %s, the platform id says Chrome %d"
                               % (self.exe, version, self.major))
        dist = self.dist_dir()
        for name in ("index.html", "main.js", "native.js", "loomora.js", "loomora.wasm"):
            if not (dist / name).is_file():
                raise RuntimeError("%s not found in %s (python tools/build.py web modern, then "
                                   "--exe DIR or LOOMORA_WEB_DIST)" % (name, dist))
        self.profiles = tempfile.mkdtemp(prefix="loomora-chrome-")
        self.server = WebProbeServer(dist, host=WEB_LOOPBACK)
        self.server.start()
        digests = ", ".join("%s sha256 %s" % (n, common.sha256(dist / n)[:16])
                            for n in ("index.html", "main.js", "native.js", "loomora.js", "loomora.wasm"))
        return ("Chrome %s (%s), headless=new, new --user-data-dir per run under %s, server http://%s:%d/ "
                "serving %s (%s)" % (version, self.exe, self.profiles, WEB_LOOPBACK, self.server.port, dist, digests))

    def close(self):
        if self.server is not None:
            self.server.stop()
            self.server = None
        if self.profiles:
            shutil.rmtree(self.profiles, ignore_errors=True)
            self.profiles = None

    def chrome_pids(self, token):
        """Process ids of the chrome.exe whose command line contains `token` (a profile folder)."""
        script = ("Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
                  "Where-Object { $_.CommandLine -like '*%s*' } | ForEach-Object { $_.ProcessId }"
                  % token.replace("'", "''"))
        res = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return [int(w) for w in res.stdout.decode("ascii", "replace").split() if w.isdigit()]

    def kill(self, pids):
        for pid in pids:
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def extra_evidence(self):
        return {"server-requests.txt": self.server.log_text()}

    def run(self, text, timeout):
        start = time.time()
        self.run_number += 1
        profile = os.path.join(self.profiles, "run%d" % self.run_number)
        url = "http://%s:%d/index.html%s" % (WEB_LOOPBACK, self.server.port,
                                             web_probe_query(text, self.args.platform_id))
        cmd = [self.exe] + list(WEB_CHROME_FLAGS) + ["--dump-dom", "--virtual-time-budget=%d" % WEB_CHROME_VIRTUAL_MS,
                                                     "--user-data-dir=" + profile, url]
        try:
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill(self.chrome_pids(profile))
            return RunResult(None, "", "", time.time() - start, "timeout after %s s (chrome.exe terminated)" % timeout)
        except OSError as e:
            return RunResult(None, "", "", time.time() - start, "cannot start chrome.exe: %s" % e)
        elapsed = time.time() - start
        title, probe = parse_dumped_dom(res.stdout.decode("utf-8", "replace"))
        left = self.chrome_pids(profile)
        self.kill(left)
        if title == "LOOMORA_PROBE_DONE" and probe:
            out, err, code = probe + "\n", "", res.returncode
        else:
            out, err, code = "", "title %r, probe record %s" % (title, "absent" if not probe else "present"), 1
        result = RunResult(code, out, err, elapsed)
        result.leftover = len(left)
        result.page_ms = int(elapsed * 1000)  # start of chrome.exe to its exit after the DOM dump
        return result


# ---- macOS and the iOS simulator on a macOS host (T126, the `macos-26` runner) ------------------

APPLE_PROCESS = "Loomora"  # CFBundleExecutable of the four Apple UIs
SIMCTL_INSTALL_TIMEOUT = 180  # seconds `simctl install` may take (once, in prepare)


def apple_probe_line(text):
    """The probe record in a text that may hold other output (the log prefix of NSLog, simctl's
    own lines): the last line that holds `{"schema"`, from that `{` on; "" when there is none."""
    found = ""
    for row in text.splitlines():
        at = row.find('{"schema"')
        if at >= 0:
            found = row[at:].strip()
    return found


class MacOSModernLauncher(Launcher):
    """The macOS UI (Modern: SwiftUI, out/macos-modern/Loomora.app of `python tools/build.py macos
    modern`; Legacy: AppKit, out/macos-legacy/Loomora.app, an x86_64 binary that needs Rosetta 2 on
    Apple silicon) run on the macOS host itself: the executable inside the bundle is started with
    `--probe --probe-input <text>` and an argument list (no shell), the probe line is read from
    standard output (ui-contract.md). Runs are serial (one window at a time); the UI exits by itself
    after 2 seconds, a process of the same name still alive after the run is a failure. The field
    limit counts Unicode scalars (code points). Needs a logged-in GUI session (the hosted macOS
    runner has one)."""

    max_jobs = 1
    variant = "modern"
    build_command = "python tools/build.py macos modern"

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.exe = Path(args.exe) if args.exe else (
            common.ROOT / "out" / ("macos-" + self.variant) / "Loomora.app" / "Contents" / "MacOS" / APPLE_PROCESS)

    def prepare(self):
        if sys.platform != "darwin":
            raise RuntimeError("the macOS UI runs on a macOS host only (this host is %s)" % sys.platform)
        if not self.exe.is_file():
            raise RuntimeError("executable not found: %s (run '%s')" % (self.exe, self.build_command))
        version = subprocess.run(["sw_vers", "-productVersion"], stdout=subprocess.PIPE).stdout.decode().strip()
        return "macOS %s UI on macOS %s: %s (%d bytes, sha256 %s)" % (
            self.variant, version, self.exe, self.exe.stat().st_size, common.sha256(self.exe))

    @staticmethod
    def _alive():
        res = subprocess.run(["pgrep", "-x", APPLE_PROCESS], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return len(res.stdout.split())

    def run(self, text, timeout):
        start = time.time()
        try:
            res = subprocess.run([str(self.exe), "--probe", "--probe-input", text],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            subprocess.run(["pkill", "-x", APPLE_PROCESS], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            return RunResult(None, "", "", time.time() - start,
                             "timeout after %s s (the UI should exit by itself after 2 s); %s"
                             % (timeout, _partial_output(exc)))
        except OSError as e:
            return RunResult(None, "", "", time.time() - start, "cannot start %s: %s" % (self.exe, e))
        result = RunResult(res.returncode, res.stdout.decode("utf-8", "replace"),
                           res.stderr.decode("utf-8", "replace"), time.time() - start)
        result.leftover = self._alive()
        return result


class MacOSLegacyLauncher(MacOSModernLauncher):
    """The macOS Legacy UI: the same protocol, out/macos-legacy/Loomora.app. Run on macOS 26 under
    Rosetta 2 it proves nothing about macOS 10.6 (that stays `not_tested`)."""

    variant = "legacy"
    build_command = "python tools/build.py macos legacy"


class IOSSimulatorLauncher(Launcher):
    """The iOS Modern UI in a booted iOS simulator, driven with `xcrun simctl`: the app
    out/ios-modern/iphonesimulator/Loomora.app (or --exe) is installed in prepare() on the simulator
    --udid (or $LOOMORA_SIM_UDID, or the only booted one); this tool never boots or erases it. A run
    launches the bundle with `simctl launch --console --terminate-running-process <udid> <bundle id>
    -probe -probe_input <text>` (an argument list, no shell): the UI exits by itself after 2
    seconds and its NSLog line comes back on the console (stderr), prefixed by NSLog's date and
    process; the record is cut out of it. When the console holds none, `simctl spawn <udid> log
    show` of the last minute is read instead. The raw output of every run goes to the evidence
    (suffix console.txt). Runs are serial. The field limit counts Unicode scalars."""

    max_jobs = 1
    variant = "modern"
    build_command = "python tools/build.py ios modern --ios-sdk iphonesimulator"

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.app = Path(args.exe) if args.exe else (
            common.ROOT / "out" / ("ios-" + self.variant) / "iphonesimulator" / "Loomora.app")
        self.udid = getattr(args, "udid", None) or os.environ.get("LOOMORA_SIM_UDID")
        self.bundle_id = None
        self.console = []

    def _simctl(self, *cmd, **kwargs):
        timeout = kwargs.pop("timeout", 120)
        res = subprocess.run(["xcrun", "simctl"] + [str(c) for c in cmd], stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return res.returncode, res.stdout.decode("utf-8", "replace"), res.stderr.decode("utf-8", "replace")

    def prepare(self):
        import plistlib
        if sys.platform != "darwin":
            raise RuntimeError("the iOS simulator runs on a macOS host only (this host is %s)" % sys.platform)
        plist = self.app / "Info.plist"
        if not plist.is_file():
            raise RuntimeError("app not found: %s (run '%s')" % (self.app, self.build_command))
        with open(str(plist), "rb") as f:
            self.bundle_id = plistlib.load(f).get("CFBundleIdentifier")
        if not self.bundle_id:
            raise RuntimeError("%s has no CFBundleIdentifier" % plist)
        code, out, err = self._simctl("list", "devices", "booted", "-j")
        if code != 0:
            raise RuntimeError("simctl list failed: %s %s" % (out.strip(), err.strip()))
        booted = [(rt, d) for rt, devs in json.loads(out)["devices"].items() for d in devs
                  if d.get("state") == "Booted"]
        if self.udid:
            booted = [(rt, d) for rt, d in booted if d["udid"] == self.udid]
        if len(booted) != 1:
            raise RuntimeError("expected exactly one booted simulator%s, found %d (boot one first, "
                               "this tool never does: xcrun simctl boot <udid>)"
                               % (" with udid " + self.udid if self.udid else "", len(booted)))
        runtime, device = booted[0]
        self.udid = device["udid"]
        code, out, err = self._simctl("install", self.udid, self.app, timeout=SIMCTL_INSTALL_TIMEOUT)
        if code != 0:
            raise RuntimeError("simctl install failed: %s %s" % (out.strip(), err.strip()))
        return "iOS Modern UI in the simulator %s (%s, udid %s): %s, bundle id %s" % (
            device.get("name"), runtime.rsplit(".", 1)[-1], self.udid, self.app, self.bundle_id)

    def run(self, text, timeout):
        start = time.time()
        cmd = ["launch", "--console", "--terminate-running-process", self.udid, self.bundle_id,
               "-probe", "-probe_input", text]
        try:
            code, out, err = self._simctl(*cmd, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            self._simctl("terminate", self.udid, self.bundle_id)
            return RunResult(None, "", "", time.time() - start,
                             "simctl launch timed out after %s s (the UI should exit by itself after 2 s); %s"
                             % (timeout, _partial_output(exc)))
        raw = out + ("\n" if out and not out.endswith("\n") else "") + err
        line = apple_probe_line(raw)
        source = "console"
        if not line:  # the unified log of the simulator: the same NSLog line, perhaps cut when very long
            try:
                logged = self._simctl("spawn", self.udid, "log", "show", "--last", "1m", "--style", "compact",
                                      "--predicate", 'process == "%s"' % APPLE_PROCESS, timeout=90)[1]
            except subprocess.TimeoutExpired:
                logged = ""
            line = apple_probe_line(logged)
            source = "log show"
            raw += "\n--- log show:\n" + logged
        self.console.append("=== %s (simctl exit %s, line from %s)\n%s\n" % (_clip(text, 40), code, source, raw))
        self._simctl("terminate", self.udid, self.bundle_id)
        if not line:
            return RunResult(code, "", err.strip()[-300:], time.time() - start,
                             "no probe line on the console nor in the log (simctl exit %s): %s"
                             % (code, _clip(raw.strip().replace("\n", " | "), 300)))
        return RunResult(code, line + "\n", err.strip()[-300:] if code else "", time.time() - start)

    def extra_evidence(self):
        return {"console.txt": "\n".join(self.console)}


LAUNCHERS = {("linux", "modern"): LinuxModernLauncher,
             ("linux", "legacy"): LinuxLegacyLauncher,
             ("windows", "legacy"): WindowsLegacyLauncher,
             ("windows", "modern"): WindowsModernLauncher,
             ("android", "legacy"): AndroidLegacyLauncher,
             ("android", "modern"): AndroidModernLauncher,
             ("macos", "modern"): MacOSModernLauncher,
             ("macos", "legacy"): MacOSLegacyLauncher,
             ("ios", "modern"): IOSSimulatorLauncher}


def select_launcher(family, variant, platform_id):
    """The launcher class (or factory) for a platform: a VirtualBox guest platform of the Windows
    Legacy UI gets the VM launcher, everything else the launcher of its family and variant."""
    if (family, variant) == ("windows", "legacy") and platform_id in VBOX_PLATFORMS:
        return WindowsVirtualBoxLauncher
    if (family, variant) == ("web", "legacy") and platform_id in WEB_NODE_PLATFORMS:
        return WebNodeLauncher
    if (family, variant) == ("web", "modern") and WEB_NODE_PLATFORMS.get(platform_id) == "wasm":
        return WebNodeLauncher   # the Modern page has no JavaScript path
    if (family, variant) == ("web", "legacy") and platform_id in WEB_FIREFOX_VM_PLATFORMS:
        return WebFirefoxVmLauncher
    if (family, variant) == ("web", "modern") and WEB_CHROME_HOST_RE.match(platform_id):
        return WebChromeHostLauncher
    return LAUNCHERS.get((family, variant))


def family_of(platform_id):
    return FAMILY_PREFIXES.get(platform_id.split("-", 1)[0])


# ---- checking ----------------------------------------------------------------------------------

def check_run(vector, result, platform_id, variant):
    """Return (problems, probe_line) for one launch."""
    import check_probe
    if result.error:
        return ["launch: " + result.error], None
    problems = []
    if result.leftover:
        problems.append("%d UI process(es) still alive after the run" % result.leftover)
    if result.returncode != 0:
        problems.append("exit status %s, stderr: %s" % (result.returncode, result.stderr.strip()[:300]))
    line, problem = check_probe.find_probe_line(result.stdout)
    if problem:
        problems.append("stdout: " + problem)
        return problems, None
    problems.extend(check_probe.check_probe_line(line, vector.expected_input))
    try:
        record = json.loads(line)
    except ValueError:
        return problems, line
    if isinstance(record, dict):
        if record.get("message") != vector.expected_message:
            problems.append("message: %r, the vector expects %r"
                            % (_clip(record.get("message")), _clip(vector.expected_message)))
        if record.get("platform_id") != platform_id:
            problems.append("platform_id: %r, expected %r" % (record.get("platform_id"), platform_id))
        if record.get("ui_variant") != variant:
            problems.append("ui_variant: %r, expected %r" % (record.get("ui_variant"), variant))
    return problems, line


def _partial_output(exc):
    """The start of the output a timed-out process wrote before it was killed (diagnostic only)."""
    parts = []
    for name, data in (("stdout", exc.stdout), ("stderr", exc.stderr)):
        if data:
            text = data.decode("utf-8", "replace") if isinstance(data, bytes) else str(data)
            parts.append("%s %r" % (name, text[:400]))
    return "; ".join(parts) or "no output"


def _clip(value, limit=80):
    text = value if isinstance(value, str) else repr(value)
    return text if len(text) <= limit else text[:limit] + "...(%d chars)" % len(text)


# ---- self-test ---------------------------------------------------------------------------------

def self_test():
    """The platform id rules of the Android launchers and the query of the Web one; no device, no build needed."""
    failures = 0

    def expect(label, got, want):
        nonlocal failures
        if got == want:
            print("ok   " + label)
        else:
            failures += 1
            print("FAIL %s: got %r, expected %r" % (label, got, want))

    def refused(label, *args):
        nonlocal failures
        try:
            got = android_probe_platform_id(*args)
        except ValueError as e:
            print("ok   refused (%s): %s" % (label, e))
        else:
            failures += 1
            print("FAIL %s: accepted, gave %r" % (label, got))

    expect("no --ui-abi: the platform id as is", android_probe_platform_id("android-7.0-x86_64"),
           "android-7.0-x86_64")
    expect("same ABI is a no-op", android_probe_platform_id("android-7.0-x86_64", "x86_64"),
           "android-7.0-x86_64")
    expect("x86 UI on an x86_64 device", android_probe_platform_id("android-7.0-x86_64", "x86"),
           "android-7.0-x86")
    expect("armeabi-v7a UI on an arm64-v8a device",
           android_probe_platform_id("android-5.0-arm64-v8a", "armeabi-v7a"),
           "android-5.0-armeabi-v7a")
    refused("x86_64 UI on an x86 device", "android-4.1.2-x86", "x86_64")
    refused("arm UI on an x86_64 device", "android-7.0-x86_64", "armeabi-v7a")
    refused("x86 UI on an arm64-v8a device", "android-5.0-arm64-v8a", "x86")
    refused("an ABI of nothing on an x86 device", "android-4.1.2-x86", "mips")
    refused("not an Android id", "linux-ubuntu-24.04-x86_64", "x86")

    # check_run: the id the probe line carries must be the one expected, whatever the cause
    base = json.loads(open_good_line())
    vec = type("V", (), {"expected_input": base["input"], "expected_message": base["message"]})()
    line = json.dumps(dict(base, platform_id="android-7.0-x86", ui_variant="legacy"))
    res = RunResult(0, line + chr(10), "", 0.0)
    expect("probe says android-7.0-x86 and the platform id is expected: refused",
           any(p.startswith("platform_id") for p in check_run(vec, res, "android-7.0-x86_64", "legacy")[0]), True)
    expect("probe says android-7.0-x86 and that id is expected: platform_id accepted",
           any(p.startswith("platform_id") for p in check_run(vec, res, "android-7.0-x86", "legacy")[0]), False)

    # the docker launcher: the commands it builds (no Docker needed)
    start = docker_start_cmd("img", "/work/tree", "vol")
    expect("docker run: image, mounts, display, private Xvfb",
           (start[:4], start[4:8], start[8:10], start[10:14], start[14], start[-1]),
           (["docker", "run", "-d", "--rm"],
            ["-v", "/work/tree:/src", "-v", "vol:/root/loomora-build"], ["-w", "/src"],
            ["-e", "DISPLAY=" + DOCKER_DISPLAY, "-e", "LANG=C.UTF-8"], "img",
            "Xvfb %s -screen 0 1280x1024x24 -nolisten tcp & exec sleep 86400" % DOCKER_DISPLAY))
    expect("docker exec: the Linux side of this script, variant and text",
           docker_inner_cmd("c1", "/py3", "legacy", None, "--inner-run", "x41", "--timeout", "30"),
           ["docker", "exec", "c1", "/py3", "tools/run_vectors.py", "--inner-variant", "legacy",
            "--inner-run", "x41", "--timeout", "30"])
    expect("docker exec: --exe is passed on",
           docker_inner_cmd("c1", "/py3", "legacy", "/x/loomora-xlib", "--inner-where"),
           ["docker", "exec", "c1", "/py3", "tools/run_vectors.py", "--inner-variant", "legacy",
            "--exe", "/x/loomora-xlib", "--inner-where"])
    # Web: the query of the probe mode, and the launcher choice by platform id
    for label, text in (("ASCII", "Coucou"), ("empty", ""), ("reserved characters", "a&b=c%d+e f#g?h"),
                        ("accents", u"\u00e9t\u00e9"), ("emoji", u"\U0001F600 x"), ("newline", "a\nb")):
        query = web_probe_query(text, "web-node-js")
        fields = dict(part.split("=", 1) for part in query[1:].split("&"))
        expect("web query, %s: ASCII, three fields, text round-trips" % label,
               (query.isascii() and sorted(fields) == ["input", "platform", "probe"]
                and urllib.parse.unquote(fields["input"]) == text and fields["platform"] == "web-node-js"),
               True)
    expect("web launcher for web-node-js", select_launcher("web", "legacy", "web-node-js"), WebNodeLauncher)
    expect("web launcher for web-node-wasm", select_launcher("web", "legacy", "web-node-wasm"), WebNodeLauncher)
    expect("web modern launcher for web-node-wasm", select_launcher("web", "modern", "web-node-wasm"), WebNodeLauncher)
    expect("no web modern launcher for web-node-js (no JavaScript path)", select_launcher("web", "modern", "web-node-js"), None)
    expect("no web modern launcher for the Firefox VM platform", select_launcher("web", "modern", "web-firefox-10-windows-2000-x86"), None)
    for variant, flag in (("legacy", False), ("modern", True)):
        args = type("A", (), {"variant": variant, "platform_id": "web-node-wasm", "exe": None, "wsl_distro": None})()
        launcher = WebNodeLauncher(args)
        cmd = launcher.command()
        expect("web node command, %s: --variant and the path are passed on" % variant,
               cmd[cmd.index(WEB_HARNESS) + 1:], ["--variant", variant, "--path", "wasm", "--query", "-"])
        expect("web node command, %s: vm modules flag only for modern" % variant,
               "--experimental-vm-modules" in cmd, flag)
    expect("no launcher for a browser platform that was not run", select_launcher("web", "legacy", "web-firefox-12-windows-2000-x86"), None)
    expect("web launcher for Firefox 10 in the Windows 2000 VM",
           select_launcher("web", "legacy", "web-firefox-10-windows-2000-x86"), WebFirefoxVmLauncher)
    expect("web modern launcher for Chrome on the Windows 11 host",
           select_launcher("web", "modern", "web-chrome-157-windows-11-x64"), WebChromeHostLauncher)
    expect("no web legacy launcher for Chrome (the Legacy page is validated on Firefox 10)",
           select_launcher("web", "legacy", "web-chrome-157-windows-11-x64"), None)
    expect("no web modern launcher for another system of Chrome",
           select_launcher("web", "modern", "web-chrome-157-windows-10-x64"), None)
    dom = ('<html><head><title>LOOMORA_PROBE_DONE</title></head><body><pre id="probe" aria-label="Probe record">'
           '{"message":"Bonjour a&amp;b"}</pre></body></html>')
    expect("Chrome DOM dump: title and probe record", parse_dumped_dom(dom),
           ("LOOMORA_PROBE_DONE", '{"message":"Bonjour a&b"}'))
    expect("Chrome DOM dump with an empty probe record",
           parse_dumped_dom('<title>Loomora</title><pre id="probe" hidden=""></pre>'), ("Loomora", ""))
    expect("Chrome launcher counts UTF-16 units", WebChromeHostLauncher.length_unit, "utf16")
    expect("Firefox VM launcher counts UTF-16 units", WebFirefoxVmLauncher.length_unit, "utf16")
    expect("web launcher counts UTF-16 units", WebNodeLauncher.length_unit, "utf16")
    # Apple: the record is cut out of an NSLog console line; the launcher choice
    nslog = ('2026-11-16 10:00:02.123 Loomora[4242:99] {"schema":1,"message":"Bonjour {a}"}\n'
             'com.example.loomora: 4242\n')
    expect("NSLog console line: the record starts at the first {\"schema\"",
           apple_probe_line(nslog), '{"schema":1,"message":"Bonjour {a}"}')
    expect("no record in the console", apple_probe_line("com.example: 1\nnothing\n"), "")
    expect("macOS modern launcher", select_launcher("macos", "modern", "macos-26-arm64"), MacOSModernLauncher)
    expect("macOS legacy launcher", select_launcher("macos", "legacy", "macos-26-arm64"), MacOSLegacyLauncher)
    expect("iOS simulator launcher", select_launcher("ios", "modern", "ios-26.5-arm64-simulator"),
           IOSSimulatorLauncher)
    expect("no iOS legacy launcher (never run)", select_launcher("ios", "legacy", "ios-12.0-arm64"), None)
    print("self-test %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


def open_good_line():
    import check_probe
    return check_probe.GOOD_LINE


# ---- main --------------------------------------------------------------------------------------

def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Run the reference vectors through a built UI.")
    ap.add_argument("platform_id", nargs="?", help="for example linux-ubuntu-24.04-x86_64")
    ap.add_argument("variant", nargs="?", choices=("legacy", "modern"))
    ap.add_argument("--vectors", default=str(common.VECTORS),
                    help="vectors file (default core/tests/vectors.json)")
    ap.add_argument("--jobs", type=int, default=4, help="UI instances run in parallel (default 4)")
    ap.add_argument("--timeout", type=int, default=30,
                    help="seconds allowed to one UI run (it exits by itself after 2 s)")
    ap.add_argument("--evidence-dir", help="write the probe lines and the log into this directory")
    ap.add_argument("--wsl-distro", help="WSL distribution on a Windows host "
                    "(default $LOOMORA_WSL_DISTRO or %s)" % DEFAULT_WSL_DISTRO)
    ap.add_argument("--arch", choices=WINDOWS_ARCHS,
                    help="Windows only: executable to run, x64 (default) or x86 (under WOW64 it "
                    "reports the same platform id as the x64 one)")
    ap.add_argument("--docker", metavar="IMAGE", help="Linux only: run the UI inside a container "
                    "of this image (for example loomora-manylinux2010, built from "
                    "tools/trials/linux-manylinux2010-x86_64/Dockerfile) with a private Xvfb; the "
                    "UI must have been built there, into --docker-volume")
    ap.add_argument("--docker-volume", default=DOCKER_DEFAULT_VOLUME, help="Docker volume mounted "
                    "as $HOME/loomora-build of the container (default %s)" % DOCKER_DEFAULT_VOLUME)
    ap.add_argument("--dry-run", action="store_true",
                    help="only list the vectors that would run and those skipped")
    ap.add_argument("--exe", help="the executable to run instead of the one that tools/build.py "
                    "left (Linux: a path on the Linux side, in $HOME/loomora-build or out/; "
                    "Windows legacy: a loomora.exe, default out/windows-legacy/<arch>/)")
    ap.add_argument("--ui-abi", help="Android only: the ABI the UI process runs as when it differs "
                    "from the platform id's: the 32-bit companion of a 64-bit device (x86 on "
                    "android-7.0-x86_64); the probe line must then carry that ABI")
    ap.add_argument("--self-test", action="store_true", help="check the platform id rules and exit")
    ap.add_argument("--adb", help="Android only: the adb executable (default $LOOMORA_ADB, the "
                    "platform-tools of $LOOMORA_ANDROID_SDK / $ANDROID_HOME, then PATH)")
    ap.add_argument("--serial", help="Android only: adb serial of the device (default "
                    "$ANDROID_SERIAL, or the only device)")
    ap.add_argument("--udid", help="iOS simulator only: the UDID of the booted simulator (default "
                    "$LOOMORA_SIM_UDID, or the only booted one)")
    ap.add_argument("--inner-variant", choices=("legacy", "modern"), help=argparse.SUPPRESS)
    ap.add_argument("--inner-run", help=argparse.SUPPRESS)    # Linux side, see the docstring
    ap.add_argument("--inner-where", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    LINUX_UI["exe"] = args.exe
    LINUX_UI["variant"] = args.inner_variant or args.variant or "modern"
    if args.inner_where:
        exe = local_executable()
        if exe is None:
            print("executable not found", file=sys.stderr)
            return 3
        print(exe)
        return 0
    if args.inner_run is not None:
        return inner_run(args.inner_run, args.timeout)
    if not args.platform_id or not args.variant:
        ap.error("platform-id and variant are required")

    family = family_of(args.platform_id)
    if family is None:
        print("run_vectors: cannot tell the platform family from %r (expected a prefix among %s)"
              % (args.platform_id, ", ".join(sorted(FAMILY_PREFIXES))), file=sys.stderr)
        return 2

    if args.ui_abi and family != "android":
        ap.error("--ui-abi is for Android platform ids only")
    data = common.read_json(args.vectors)
    if args.docker and family != "linux":
        ap.error("--docker is for Linux platform ids only")
    launcher_cls = select_launcher(family, args.variant, args.platform_id)
    if args.docker:
        launcher_cls = LinuxDockerLauncher
    unit = launcher_cls.length_unit if launcher_cls else Launcher.length_unit
    runs, skipped = plan_vectors(data, unit)
    log = []

    def say(text=""):
        print(text)
        log.append(text)

    say("run_vectors %s %s: %d vector(s) to run, %d skipped (%s)"
        % (args.platform_id, args.variant, len(runs), len(skipped), args.vectors))
    for vid, reason in skipped:
        say("skip %-34s %s" % (vid, reason))
    if args.dry_run:
        for v in runs:
            say("run  %s" % v.describe())
        return 0
    if not runs:
        say("FAILED: no valid vector to run")
        return 1

    if launcher_cls is None:
        print("run_vectors: launcher not implemented yet for %s-%s (see tasks T060-T093); "
              "implemented: %s" % (family, args.variant,
                                   ", ".join("%s-%s" % k for k in sorted(LAUNCHERS))),
              file=sys.stderr)
        return 1
    launcher = None
    try:
        launcher = launcher_cls(args)
        say("launcher: " + launcher.prepare())
        expected_id = launcher.probe_platform_id(args.platform_id)
        if expected_id != args.platform_id:
            say("the probe line must carry platform_id %s (platform record %s: the UI runs as "
                "another ABI)" % (expected_id, args.platform_id))
    except (RuntimeError, OSError, subprocess.SubprocessError) as e:
        print("run_vectors: %s" % e, file=sys.stderr)
        if launcher is not None:
            launcher.close()
        return 1
    jobs = max(1, args.jobs)
    if launcher.max_jobs:
        jobs = min(jobs, launcher.max_jobs)

    started = time.time()

    def one(vector):
        result = launcher.run(vector.text, args.timeout)
        problems, line = check_run(vector, result, expected_id, args.variant)
        return vector, result, problems, line

    extra = {}
    try:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            outcomes = list(pool.map(one, runs))  # keeps the order of vectors.json
        extra = launcher.extra_evidence()
    except VBoxError as e:  # the guest stopped accepting the credentials in the middle of a run
        print("run_vectors: %s" % e, file=sys.stderr)
        return 1
    finally:
        launcher.close()

    passed = failed = 0
    lines = []
    for vector, result, problems, line in outcomes:
        if line is not None:
            lines.append("%s\t%s" % (vector.id, line))
        if problems:
            failed += 1
            say("FAIL %s  %.1fs" % (vector.describe(), result.elapsed))
            for p in problems:
                say("       " + p)
        else:
            passed += 1
            say("ok   %s  %.1fs%s" % (vector.describe(), result.elapsed,
                                       ("" if result.page_ms is None else " (browser start to record %d ms)" % result.page_ms)
                                       + (" (firefox.exe lost at the first start, started once more)" if result.relaunched else "")))
    total = time.time() - started
    say("summary: %d vector(s) run, %d passed, %d failed, %d skipped; %d truncated to %d %s; "
        "%.1f s with %d worker(s)"
        % (len(runs), passed, failed, len(skipped),
           sum(1 for v in runs if v.kind == "truncated"), UI_MAX_CHARS,
           "UTF-16 units" if unit == "utf16" else "chars", total, jobs))
    say("result: %s" % ("PASSED" if failed == 0 else "FAILED"))

    if args.evidence_dir:
        out = Path(args.evidence_dir)
        out.mkdir(parents=True, exist_ok=True)
        stem = "run-vectors-%s-%s" % (args.platform_id, args.variant)
        if launcher.tag:
            stem += "-" + launcher.tag
        with open(str(out / (stem + "-probe-lines.tsv")), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(l + "\n" for l in lines))
        with open(str(out / (stem + "-log.txt")), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(l + "\n" for l in log))
        for suffix, text in sorted(extra.items()):
            with open(str(out / (stem + "-" + suffix)), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        print("evidence written to %s" % out)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
