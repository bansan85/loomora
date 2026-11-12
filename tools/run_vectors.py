#!/usr/bin/env python
"""Feed every valid reference input of core/tests/vectors.json to a built Loomora UI.

Usage:
  python tools/run_vectors.py <platform-id> <variant> [--vectors FILE] [--jobs N] [--timeout S]
                              [--evidence-dir DIR] [--wsl-distro NAME] [--dry-run]

  platform-id  for example linux-ubuntu-24.04-x86_64 (the id the UI itself puts in its probe line)
  variant      legacy | modern

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
  * ui-contract.md limits every UI's field to 1,000 characters (the GTK entry's max length).
    A valid vector of at most 1,000 characters is therefore expected verbatim: `input` is the
    vector's input and `message` its `output`. A valid vector of more than 1,000 characters
    (the 4,096-byte limit vectors, which the backend accepts) cannot be fully entered; the UI is
    expected to keep exactly the FIRST 1,000 characters (code points) and to greet that text:
    `input` = first 1,000 characters, `message` = "Bonjour " + that text. They are reported as
    "truncated" and only count as passed when the truncation is exactly that; a UI that
    lets more through, drops the input or fails is a failure, nothing is relaxed.

Platform launchers (registry LAUNCHERS, keyed by family and variant). Implemented: linux
modern. Every other family exits with status 1 and "launcher not implemented yet" until the
tasks that add the UIs (T060-T093) add theirs: they only have to implement Launcher.run().
Linux modern: the executable built by `python tools/build.py linux modern`
($HOME/loomora-build/linux-modern/loomora-gtk3, or out/linux-modern/ as fallback) runs on this
machine on Linux or inside WSL on a Windows host (`wsl -d <distro>`, default Ubuntu-24.04 or
$LOOMORA_WSL_DISTRO). To avoid every quoting problem (quotes, backslashes, newlines, emoji, empty
input) the Windows side never puts the text on a command line: it passes the UTF-8 bytes as hex
digits to this same script run inside WSL (`--inner-run`), which starts the UI with an argv list
(no shell) and returns the exit status, standard output and standard error as one JSON line.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import common

UI_MAX_CHARS = 1000  # ui-contract.md: the field is limited to 1,000 characters
DEFAULT_WSL_DISTRO = "Ubuntu-24.04"
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


def plan_vectors(data):
    """Split the greet vectors into (vectors to run, [(id, reason)] skipped)."""
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
        if len(text) <= UI_MAX_CHARS:
            runs.append(Vector(vid, text, text, expected_output, "exact"))
        else:
            kept = text[:UI_MAX_CHARS]
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


class Launcher(object):
    """Starts the built UI of one platform family and variant with one input text."""

    def __init__(self, args):
        self.args = args

    def prepare(self):
        """Check that the UI can be launched; return a one-line description of what is run."""
        raise NotImplementedError

    def run(self, text, timeout):
        """Launch the UI in probe mode with `text` (str) as input override; return RunResult."""
        raise NotImplementedError


def local_executable():
    """The Linux Modern executable: the build tree under $HOME first, then out/."""
    build_root = Path(os.environ.get("LOOMORA_BUILD_DIR") or (Path.home() / "loomora-build"))
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
        return {"error": "executable not found: build it with 'python tools/build.py linux modern'"}
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
    def __init__(self, args):
        Launcher.__init__(self, args)
        self.distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
        self.on_windows = sys.platform == "win32"

    def _wsl_script(self, *script_args):
        return ["wsl", "-d", self.distro, "--cd", str(common.ROOT), "--", "python3",
                "tools/run_vectors.py"] + list(script_args)

    def prepare(self):
        if self.on_windows:
            res = subprocess.run(self._wsl_script("--inner-where"), stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=120)
            exe = res.stdout.decode("utf-8", "replace").strip().splitlines()
            if res.returncode != 0 or not exe or not exe[-1].startswith("/"):
                raise RuntimeError(
                    "cannot locate the Linux Modern executable in WSL (%s): run "
                    "'python tools/build.py linux modern'\n%s"
                    % (self.distro, res.stderr.decode("utf-8", "replace").strip()))
            return "WSL %s: %s" % (self.distro, exe[-1])
        exe = local_executable()
        if exe is None:
            raise RuntimeError("executable not found: run 'python tools/build.py linux modern'")
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


LAUNCHERS = {("linux", "modern"): LinuxModernLauncher}


def family_of(platform_id):
    return FAMILY_PREFIXES.get(platform_id.split("-", 1)[0])


# ---- checking ----------------------------------------------------------------------------------

def check_run(vector, result, platform_id, variant):
    """Return (problems, probe_line) for one launch."""
    import check_probe
    if result.error:
        return ["launch: " + result.error], None
    problems = []
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
    ap.add_argument("--dry-run", action="store_true",
                    help="only list the vectors that would run and those skipped")
    ap.add_argument("--inner-run", help=argparse.SUPPRESS)    # Linux side, see the docstring
    ap.add_argument("--inner-where", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

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
    runs, skipped = plan_vectors(data)
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

    launcher_cls = LAUNCHERS.get((family, args.variant))
    if launcher_cls is None:
        print("run_vectors: launcher not implemented yet for %s-%s (see tasks T060-T093); "
              "implemented: %s" % (family, args.variant,
                                   ", ".join("%s-%s" % k for k in sorted(LAUNCHERS))),
              file=sys.stderr)
        return 1
    launcher = launcher_cls(args)
    try:
        say("launcher: " + launcher.prepare())
    except (RuntimeError, OSError, subprocess.SubprocessError) as e:
        print("run_vectors: %s" % e, file=sys.stderr)
        return 1

    started = time.time()

    def one(vector):
        result = launcher.run(vector.text, args.timeout)
        problems, line = check_run(vector, result, args.platform_id, args.variant)
        return vector, result, problems, line

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        outcomes = list(pool.map(one, runs))  # keeps the order of vectors.json

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
    say("summary: %d vector(s) run, %d passed, %d failed, %d skipped; %d truncated to %d chars; "
        "%.1f s with %d worker(s)"
        % (len(runs), passed, failed, len(skipped),
           sum(1 for v in runs if v.kind == "truncated"), UI_MAX_CHARS, total, args.jobs))
    say("result: %s" % ("PASSED" if failed == 0 else "FAILED"))

    if args.evidence_dir:
        out = Path(args.evidence_dir)
        out.mkdir(parents=True, exist_ok=True)
        stem = "run-vectors-%s-%s" % (args.platform_id, args.variant)
        with open(str(out / (stem + "-probe-lines.tsv")), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(l + "\n" for l in lines))
        with open(str(out / (stem + "-log.txt")), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(l + "\n" for l in log))
        print("evidence written to %s" % out)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
