#!/usr/bin/env python
"""Feed every valid reference input of core/tests/vectors.json to a built Loomora UI.

Usage:
  python tools/run_vectors.py <platform-id> <variant> [--vectors FILE] [--jobs N] [--timeout S]
                              [--evidence-dir DIR] [--wsl-distro NAME] [--arch x64|x86]
                              [--exe PATH] [--dry-run]

  platform-id  for example linux-ubuntu-24.04-x86_64, windows-11-x64 or windows-2000-x86 (the id
               the UI itself puts in its probe line)
  variant      legacy | modern
  --arch       Windows only: which executable of the Windows Legacy UI runs, x64 (default) or x86
               (windows-2000-x86 runs the x86 executable in a VirtualBox VM and accepts x86 only)

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
Android legacy (platform id android-<release>-<abi>, for example android-4.1.2-x86): see
AndroidLegacyLauncher. The APK out/android-legacy/loomora.apk (or --exe) is installed on the one
emulator or device adb sees (--serial / $ANDROID_SERIAL to choose); the launcher never starts or
stops the emulator. Each run starts the activity with the intent extras of the UI contract and
reads the probe line from logcat (tag LOOMORA_PROBE; a long line is logged in numbered parts
"#i/n <text>" and joined here). Runs are serial.
Every other family exits with status 1 and "launcher not implemented yet" until the tasks that
add the UIs (T060-T093) add theirs: they only have to implement Launcher.run().
Linux modern: the executable built by `python tools/build.py linux modern`
($HOME/loomora-build/linux-modern/loomora-gtk3, or out/linux-modern/ as fallback) runs on this
machine on Linux or inside WSL on a Windows host (`wsl -d <distro>`, default Ubuntu-24.04 or
$LOOMORA_WSL_DISTRO). To avoid every quoting problem (quotes, backslashes, newlines, emoji, empty
input) the Windows side never puts the text on a command line: it passes the UTF-8 bytes as hex
digits to this same script run inside WSL (`--inner-run`), which starts the UI with an argv list
(no shell) and returns the exit status, standard output and standard error as one JSON line.
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
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import common

UI_MAX_CHARS = 1000  # ui-contract.md: the field is limited to 1,000 characters
DEFAULT_WSL_DISTRO = "Ubuntu-24.04"
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

ANDROID_PACKAGE = "fr.loomora.probe"
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


class AndroidLegacyLauncher(Launcher):
    """The Android Legacy UI (an APK) on ONE running emulator or device, driven with adb.

    The APK (out/android-legacy/loomora.apk from `python tools/build.py android legacy`, or --exe)
    is installed in prepare(). A run force-stops the application, clears logcat, starts
    fr.loomora.probe/.MainActivity with `am start -W --ez probe true --es probe_input '<text>'`
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

    def __init__(self, args):
        Launcher.__init__(self, args)
        self.apk = Path(args.exe) if args.exe else common.ROOT / "out" / "android-legacy" / "loomora.apk"
        self.serial = args.serial or os.environ.get("ANDROID_SERIAL")
        self.adb = adb_executable(args)

    def _adb(self, *cmd, **kwargs):
        full = [self.adb] + (["-s", self.serial] if self.serial else []) + [str(c) for c in cmd]
        timeout = kwargs.pop("timeout", 60)
        res = subprocess.run(full, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return res.returncode, res.stdout.decode("utf-8", "replace"), res.stderr.decode("utf-8", "replace")

    def _prop(self, name):
        return self._adb("shell", "getprop " + name)[1].strip()

    def prepare(self):
        if not self.apk.is_file():
            raise RuntimeError("APK not found: %s (run 'python tools/build.py android legacy')"
                               % self.apk)
        code, out, err = self._adb("get-state")
        if code != 0 or out.strip() != "device":
            raise RuntimeError("no Android device or emulator ready (adb get-state: %r %s); start "
                               "one first, this tool never does" % (out.strip(), err.strip()))
        if self._prop("sys.boot_completed") != "1":
            raise RuntimeError("the device has not finished booting (sys.boot_completed)")
        code, out, err = self._adb("install", "-r", self.apk, timeout=300)
        if "INCONSISTENT_CERTIFICATES" in out:  # an APK signed with another debug key is installed
            self._adb("uninstall", ANDROID_PACKAGE)
            code, out, err = self._adb("install", self.apk, timeout=300)
        if code != 0 or "Success" not in out:
            raise RuntimeError("adb install failed: %s %s" % (out.strip(), err.strip()))
        return "Android Legacy %s (release %s, API %s, abi %s): %s (%d bytes, sha256 %s)" % (
            self._prop("ro.product.model") or "device", self._prop("ro.build.version.release"),
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
        self._adb("shell", "am force-stop " + ANDROID_PACKAGE)
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
        cmd = "am start -W -n %s --ez probe true %s" % (ANDROID_ACTIVITY, extra)
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
            resumed = [l for l in top.splitlines() if "esumed" in l and ANDROID_PACKAGE in l]
            if not resumed:
                break
            if time.time() > deadline:
                status, stderr = 1, "the activity is still resumed 6 s after the probe line"
                break
            time.sleep(0.5)
        return RunResult(status, line + "\n", stderr, time.time() - start)


LAUNCHERS = {("linux", "modern"): LinuxModernLauncher,
             ("linux", "legacy"): LinuxLegacyLauncher,
             ("windows", "legacy"): WindowsLegacyLauncher,
             ("android", "legacy"): AndroidLegacyLauncher}


def select_launcher(family, variant, platform_id):
    """The launcher class (or factory) for a platform: a VirtualBox guest platform of the Windows
    Legacy UI gets the VM launcher, everything else the launcher of its family and variant."""
    if (family, variant) == ("windows", "legacy") and platform_id in VBOX_PLATFORMS:
        return WindowsVirtualBoxLauncher
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
        problems.append("%d loomora.exe process(es) still alive after the run" % result.leftover)
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


def _clip(value, limit=80):
    text = value if isinstance(value, str) else repr(value)
    return text if len(text) <= limit else text[:limit] + "...(%d chars)" % len(text)


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
    ap.add_argument("--dry-run", action="store_true",
                    help="only list the vectors that would run and those skipped")
    ap.add_argument("--exe", help="the executable to run instead of the one that tools/build.py "
                    "left (Linux: a path on the Linux side, in $HOME/loomora-build or out/; "
                    "Windows legacy: a loomora.exe, default out/windows-legacy/<arch>/)")
    ap.add_argument("--adb", help="Android only: the adb executable (default $LOOMORA_ADB, the "
                    "platform-tools of $LOOMORA_ANDROID_SDK / $ANDROID_HOME, then PATH)")
    ap.add_argument("--serial", help="Android only: adb serial of the device (default "
                    "$ANDROID_SERIAL, or the only device)")
    ap.add_argument("--inner-variant", choices=("legacy", "modern"), help=argparse.SUPPRESS)
    ap.add_argument("--inner-run", help=argparse.SUPPRESS)    # Linux side, see the docstring
    ap.add_argument("--inner-where", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

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

    data = common.read_json(args.vectors)
    launcher_cls = select_launcher(family, args.variant, args.platform_id)
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
        problems, line = check_run(vector, result, args.platform_id, args.variant)
        return vector, result, problems, line

    try:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            outcomes = list(pool.map(one, runs))  # keeps the order of vectors.json
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
            say("ok   %s  %.1fs" % (vector.describe(), result.elapsed))
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
        print("evidence written to %s" % out)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
