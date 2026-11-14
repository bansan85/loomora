#!/usr/bin/env python
"""Prove that the greeting prefix is changed in ONE place and every UI follows (SC-006, FR-009, T105).

Usage:
  python tools/prefix_check.py [--prefix "hello "] [--work DIR] [--targets ID[,ID]] [--list]
                               [--wsl-distro NAME] [--jobs N] [--selftest]

What it does, without ever editing the repository working tree:

  1. copies the sources (CMakeLists.txt, cmake/, core/, ui/, bridge/, tools/) into WORK/src, where
     WORK is $HOME/loomora-build/prefix-check (--work), and changes the prefix "Bonjour " to
     "hello " in the COPY of core/src/greet.cpp: the single line that holds it.
  2. proves the change is that one line: the files of the copy that differ from the repository are
     listed (expected: core/src/greet.cpp only; none under ui/ or bridge/) with the diff, before and
     after the builds, and the repository's own core/, ui/, bridge/, cmake/ are hashed before and
     after the run to show nothing was touched.
  3. builds every platform and variant this machine can build, from the copy, with the project's own
     tools (tools/build.py of the copy; the Windows host build with clang + Ninja as verify.py does;
     WSL builds go under $HOME/loomora-build/prefix-check/build):
         windows-host        clang + Ninja Win32 UI           (Windows host only)
         windows-x64         MinGW-w64 x86_64 Win32 UI        (built in WSL, run on the Windows host)
         windows-x86         MinGW-w64 i686 Win32 UI          (built in WSL, run on the Windows host, WOW64)
         windows-2000-x86    the i686 exe in the VirtualBox VM (only with --vm: the VM must already
                             run with a logged-on desktop, this tool never starts, stops or restores it)
         linux-legacy-x86_64 / linux-legacy-i686   Xlib UI    (native, in WSL on a Windows host)
         linux-modern        gtkmm 3 UI                       (native, in WSL on a Windows host)
  4. runs each built UI in probe mode (--probe --probe-input, with the launchers of
     tools/run_vectors.py) on every valid reference vector of core/tests/vectors.json plus "Coucou",
     and checks with tools/check_probe.py (schema, greeting_matches) that the displayed greeting is
     exactly "hello " + input. A UI that still shows "Bonjour " (a prefix of its own) fails.
  5. lists the platforms and variants that were NOT tested and why (no UI yet, no VM, not buildable
     here): never faked.

Exit status: 0 every tested UI shows the new prefix and nothing but core/src/greet.cpp changed, 1 a
mismatch, a build that failed or a repository file that changed, 2 a usage error (or nothing
tested). --selftest checks the checks of this tool on a temporary tree and exits.
Python 3.8+, standard library plus what run_vectors.py needs (jsonschema).
"""
import argparse
import difflib
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import build as build_tool
import check_probe
import common
import measure_sharing
import run_vectors

IS_WINDOWS = sys.platform == "win32"
OLD_PREFIX = u"Bonjour "
GREET_SOURCE = "core/src/greet.cpp"
COPIED = ("CMakeLists.txt", "CMakePresets.json", "cmake", "core", "ui", "bridge", "tools")
NOT_COPIED = ("__pycache__", "build", "out", ".git", "trials", "*.pyc")
# Trees whose files are hashed in the repository (they must not change) and compared with the copy.
WATCHED = ("CMakeLists.txt", "CMakePresets.json", "cmake", "core", "ui", "bridge")
PROBE_LOCK = threading.Lock()  # check_probe.PREFIX is a module global: one check at a time
EXTRA_VECTOR = u"Coucou"  # the default text of every UI field: "hello Coucou" (SC-006)


# ---- copy, change, compare ------------------------------------------------------------------------

def files_under(root, names):
    """{relative posix path: sha256} of the files of the given top-level names under root."""
    found = {}
    root = Path(root)
    for name in names:
        top = root / name
        if top.is_file():
            found[name] = common.sha256(top)
        elif top.is_dir():
            for dirpath, dirnames, filenames in os.walk(str(top)):
                dirnames[:] = sorted(d for d in dirnames if d not in ("__pycache__", "build", "out", ".git", "trials"))
                for fn in sorted(filenames):
                    if fn.endswith(".pyc"):
                        continue
                    p = Path(dirpath) / fn
                    found[p.relative_to(root).as_posix()] = common.sha256(p)
    return found


def make_copy(repo, dest):
    """Fresh copy of the sources of `repo` in `dest`."""
    repo, dest = Path(repo), Path(dest)
    if dest.exists():
        shutil.rmtree(str(dest))
    dest.mkdir(parents=True)
    for name in COPIED:
        src = repo / name
        if src.is_dir():
            shutil.copytree(str(src), str(dest / name), ignore=shutil.ignore_patterns(*NOT_COPIED))
        elif src.is_file():
            shutil.copyfile(str(src), str(dest / name))


def change_prefix(copy, new_prefix):
    """Replace the literal "Bonjour " by new_prefix in the copy's greet.cpp, which must hold it exactly
    once; returns the unified diff lines. Raises ValueError otherwise."""
    path = Path(copy) / GREET_SOURCE
    text = path.read_bytes().decode("utf-8")
    literal = u'"%s"' % OLD_PREFIX
    if text.count(literal) != 1:
        raise ValueError("%s holds %s %d time(s), expected exactly once" % (GREET_SOURCE, literal, text.count(literal)))
    new = text.replace(literal, u'"%s"' % new_prefix)
    path.write_bytes(new.encode("utf-8"))
    return list(difflib.unified_diff(text.splitlines(), new.splitlines(), "a/" + GREET_SOURCE,
                                     "b/" + GREET_SOURCE, lineterm="", n=0))


def differing_files(repo, copy):
    """Sorted relative paths of the watched files that differ between the repository and the copy
    (changed, added or missing)."""
    a, b = files_under(repo, WATCHED), files_under(copy, WATCHED)
    return sorted(p for p in set(a) | set(b) if a.get(p) != b.get(p))


def tools_differ(repo, copy):
    a, b = files_under(repo, ("tools",)), files_under(copy, ("tools",))
    return sorted(p for p in set(a) | set(b) if a.get(p) != b.get(p))


def check_change_scope(differing):
    """Problems with the list of differing files: only GREET_SOURCE may differ."""
    problems = []
    if differing != [GREET_SOURCE]:
        problems.append("files that differ between the repository and the copy: %s (expected only %s)"
                        % (", ".join(differing) or "none", GREET_SOURCE))
    bad = [p for p in differing if p.startswith(("ui/", "bridge/"))]
    if bad:
        problems.append("%d file(s) under ui/ or bridge/ differ: %s" % (len(bad), ", ".join(bad)))
    return problems


# ---- the probe check ---------------------------------------------------------------------------------

def check_greeting(prefix, expected_input, result):
    """Problems of one launch: the probe line must say message == prefix + input and agree with the
    backend (greeting_matches); tools/check_probe.py does the schema and the consistency checks."""
    if result.error:
        return ["launch: " + result.error]
    problems = []
    if result.returncode != 0:
        problems.append("exit status %s, stderr: %s" % (result.returncode, result.stderr.strip()[:200]))
    line, problem = check_probe.find_probe_line(result.stdout)
    if problem:
        return problems + ["stdout: " + problem]
    with PROBE_LOCK:
        saved = check_probe.PREFIX
        check_probe.PREFIX = prefix  # the tool's own constant is the old prefix; the repository file is untouched
        try:
            problems += check_probe.check_probe_line(line, expected_input)
        finally:
            check_probe.PREFIX = saved
    try:
        record = json.loads(line)
    except ValueError:
        return problems
    if isinstance(record, dict) and record.get("message") != prefix + expected_input:
        problems.append("message: %r, expected %r" % (_clip(record.get("message")), _clip(prefix + expected_input)))
    return problems


def _clip(value, limit=60):
    text = value if isinstance(value, str) else repr(value)
    return text if len(text) <= limit else text[:limit] + "...(%d chars)" % len(text)


def vectors_for(launcher_cls):
    """The vectors of core/tests/vectors.json valid for this launcher, plus "Coucou"."""
    unit = launcher_cls.length_unit
    runs, _ = run_vectors.plan_vectors(common.read_json(common.VECTORS), unit)
    runs.append(run_vectors.Vector("default-coucou", EXTRA_VECTOR, EXTRA_VECTOR, None, "exact"))
    return runs


def run_target(launcher, vectors, prefix, jobs, say):
    """Run every vector through the launcher; returns (passed, failed, example)."""
    def one(v):
        result = launcher.run(v.text, 30)
        return v, check_greeting(prefix, v.expected_input, result), result

    workers = max(1, min(jobs, launcher.max_jobs or jobs))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        outcomes = list(pool.map(one, vectors))
    passed = failed = 0
    example = None
    for v, problems, result in outcomes:
        if problems:
            failed += 1
            say("    FAIL %s" % v.describe())
            for p in problems:
                say("         " + p)
        else:
            passed += 1
            if v.id == "default-coucou":
                line, _ = check_probe.find_probe_line(result.stdout)
                example = json.loads(line)["message"]
    return passed, failed, example


# ---- machine: where things run -----------------------------------------------------------------------

class Machine(object):
    def __init__(self, distro):
        self.distro = distro
        self.logs = None

    def wsl_home(self):
        res = subprocess.run(["wsl", "-d", self.distro, "--", "printenv", "HOME"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, universal_newlines=True)
        home = res.stdout.strip()
        if res.returncode != 0 or not home.startswith("/"):
            raise RuntimeError("cannot read $HOME in WSL (%s): %s" % (self.distro, res.stderr.strip()))
        return home

    def linux_build_root(self):
        """Build root of the Linux and MinGW builds, as a Linux path: always under $HOME."""
        base = self.wsl_home() if IS_WINDOWS else str(Path.home())
        return base + "/loomora-build/prefix-check/build"

    def run_linux(self, cmd, cwd, env, log):
        """Run a command on the Linux side (WSL on a Windows host) with extra environment; log the output."""
        full_env = dict(os.environ)
        if IS_WINDOWS:
            names = list(env)
            full_env.update(env)
            full_env["WSLENV"] = ":".join([full_env.get("WSLENV", "")] + names).strip(":")
            cmd = ["wsl", "-d", self.distro, "--cd", str(cwd), "--"] + cmd
        else:
            full_env.update(env)
        with open(str(log), "ab") as f:
            f.write(("$ %s\n" % " ".join(cmd)).encode("utf-8"))
            f.flush()
            rc = subprocess.call(cmd, cwd=None if IS_WINDOWS else str(cwd), env=full_env, stdout=f, stderr=subprocess.STDOUT)
        return rc


def log_tail(log, lines=6):
    try:
        text = Path(log).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return " | ".join(l.strip() for l in text[-lines:] if l.strip())


# ---- targets --------------------------------------------------------------------------------------------

class NotBuilt(Exception):
    pass


class Target(object):
    """One platform and variant: build(ctx) returns the executable (a Path, or a Linux path string),
    launcher(ctx, exe) a started-able launcher object; `runs_on_windows` marks the targets that need
    a Windows host to run."""

    def __init__(self, tid, platform_id, variant, label, needs_windows, build, launcher):
        self.id, self.platform_id, self.variant, self.label = tid, platform_id, variant, label
        self.needs_windows, self.build, self.launcher = needs_windows, build, launcher


class Ctx(object):
    def __init__(self, args, repo, work, machine):
        self.args, self.repo, self.work, self.machine = args, repo, work, machine
        self.copy = work / "src"
        self.log_dir = work / "logs"
        self.linux_root = None
        self.built_windows = False
        self.built_linux = set()

    def log(self, name):
        self.log_dir.mkdir(parents=True, exist_ok=True)
        path = self.log_dir / (name + ".log")
        path.write_text(u"", encoding="utf-8")
        return path

    def linux_env(self):
        if self.linux_root is None:
            self.linux_root = self.machine.linux_build_root()
        return {"LOOMORA_BUILD_DIR": self.linux_root}


def build_windows_host(ctx):
    if not IS_WINDOWS:
        raise NotBuilt("not a Windows host")
    for tool in ("cmake", "ninja", "clang", "clang++"):
        if shutil.which(tool) is None:
            raise NotBuilt("%s not found" % tool)
    bdir = ctx.work / "build-windows-host"
    log = ctx.log("build-windows-host")
    cfg = ["cmake", "-S", ctx.copy, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DLOOMORA_BUILD_TESTS=OFF"]
    for cmd in (cfg, ["cmake", "--build", bdir, "--target", "loomora_win32"]):
        with open(str(log), "ab") as f:
            rc = subprocess.call([str(c) for c in cmd], stdout=f, stderr=subprocess.STDOUT)
        if rc != 0:
            raise NotBuilt("build failed (log %s): %s" % (log, log_tail(log)))
    exe = bdir / "ui" / "legacy" / "windows-win32" / "loomora.exe"
    if not exe.is_file():
        raise NotBuilt("%s is missing after the build" % exe)
    return exe


def ensure_mingw(ctx):
    """tools/build.py windows legacy in the copy, run on the Linux side (both architectures), once."""
    if ctx.built_windows is not False:
        if ctx.built_windows is not True:
            raise NotBuilt(ctx.built_windows)
        return
    log = ctx.log("build-windows-legacy")
    rc = ctx.machine.run_linux(["python3", "tools/build.py", "windows", "legacy"], ctx.copy, ctx.linux_env(), log)
    if rc != 0:
        ctx.built_windows = "tools/build.py windows legacy failed (log %s): %s" % (log, log_tail(log))
        raise NotBuilt(ctx.built_windows)
    ctx.built_windows = True


def build_windows_arch(arch):
    def build(ctx):
        ensure_mingw(ctx)
        exe = ctx.copy / "out" / "windows-legacy" / arch / "loomora.exe"
        if not exe.is_file():
            raise NotBuilt("%s is missing after the build" % exe)
        return exe
    return build


def build_linux(variant, arch):
    """Linux Legacy (Xlib, per architecture) or Linux Modern (gtkmm) built from the copy."""
    def build(ctx):
        key = variant + "-" + arch
        log = ctx.log("build-linux-" + key)
        cmd = ["python3", "tools/build.py", "linux", variant] + (["--arch", arch] if variant == "legacy" else [])
        if ctx.machine.run_linux(cmd, ctx.copy, ctx.linux_env(), log) != 0:
            raise NotBuilt("tools/build.py linux %s failed (log %s): %s" % (variant, log, log_tail(log)))
        if variant == "legacy":
            return "%s/linux-legacy-%s/loomora-xlib" % (ctx.linux_root, arch)
        return "%s/linux-modern/loomora-gtk3" % ctx.linux_root
    return build


def args_for(ctx, platform_id, variant, exe, arch=None):
    import argparse as ap
    return ap.Namespace(platform_id=platform_id, variant=variant, exe=str(exe), arch=arch,
                        wsl_distro=ctx.args.wsl_distro, jobs=ctx.args.jobs, timeout=30)


def windows_launcher(arch, platform_id):
    def make(ctx, exe):
        return run_vectors.WindowsLegacyLauncher(args_for(ctx, platform_id, "legacy", exe, arch))
    return make


def linux_launcher(variant, platform_id):
    def make(ctx, exe):
        run_vectors.LINUX_UI["variant"], run_vectors.LINUX_UI["exe"] = variant, str(exe)
        cls = run_vectors.LinuxLegacyLauncher if variant == "legacy" else run_vectors.LinuxModernLauncher
        return cls(args_for(ctx, platform_id, variant, exe))
    return make


def vm_launcher(ctx, exe):
    launcher = run_vectors.WindowsVirtualBoxLauncher(args_for(ctx, "windows-2000-x86", "legacy", exe, "x86"))
    launcher.exe = Path(exe)  # the copy's i686 executable instead of out/windows-legacy/x86/ of the repository
    return launcher


def host_windows_id():
    import verify
    return verify.host_platform_id()


def make_targets(ctx):
    win_id = host_windows_id() if IS_WINDOWS else "windows-11-x64"
    return [
        Target("windows-host", win_id, "legacy", "Windows Legacy UI, host build (clang + Ninja)", True,
               build_windows_host, windows_launcher("x64", win_id)),
        Target("windows-x64", win_id, "legacy", "Windows Legacy UI, MinGW-w64 x86_64 (XP x64 -> 11)", True,
               build_windows_arch("x64"), windows_launcher("x64", win_id)),
        Target("windows-x86", win_id, "legacy", "Windows Legacy UI, MinGW-w64 i686 (2000 -> 11) under WOW64", True,
               build_windows_arch("x86"), windows_launcher("x86", win_id)),
        Target("windows-2000-x86", "windows-2000-x86", "legacy", "Windows Legacy UI, i686 exe in the Windows 2000 VM", True,
               build_windows_arch("x86"), vm_launcher),
        Target("linux-legacy-x86_64", None, "legacy", "Linux Legacy UI (Xlib), x86_64", False,
               build_linux("legacy", "x86_64"), None),
        Target("linux-legacy-i686", None, "legacy", "Linux Legacy UI (Xlib), i686 (gcc -m32)", False,
               build_linux("legacy", "i686"), None),
        Target("linux-modern", None, "modern", "Linux Modern UI (gtkmm 3)", False,
               build_linux("modern", "x86_64"), None),
    ]


def linux_platform_id_for(ctx):
    """Platform id the Linux UIs report: the id of the Linux side (WSL), through the project's own rule."""
    if not IS_WINDOWS:
        return build_tool.linux_platform_id()
    res = subprocess.run(["wsl", "-d", ctx.args.wsl_distro, "--cd", str(ctx.repo), "--", "python3", "-c",
                          "import sys; sys.path.insert(0, 'tools'); import build; print(build.linux_platform_id())"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    return res.stdout.strip().splitlines()[-1] if res.returncode == 0 and res.stdout.strip() else "linux"


def vm_running():
    try:
        vbm = run_vectors.VBoxGuest.find_vboxmanage()
        name = os.environ.get("LOOMORA_VM_NAME") or run_vectors.VBOX_PLATFORMS["windows-2000-x86"][0]
        res = subprocess.run([vbm, "showvminfo", name, "--machinereadable"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, universal_newlines=True, timeout=60)
        return 'VMState="running"' in res.stdout
    except Exception:  # no VirtualBox: the VM is simply not available
        return False


def not_tested_platforms(tested_ids):
    """[(platform/variant, reason)] for every UI directory that has no code yet."""
    out = []
    for (family, variant), directory in sorted(measure_sharing.UI_DIR.items()):
        if (family, variant) in (("windows", "legacy"), ("linux", "legacy"), ("linux", "modern")):
            continue
        task = build_tool.PENDING.get((family, variant), "no task")
        out.append(("%s %s (ui/%s/%s)" % (family, variant, variant, directory), "no UI built yet (%s)" % task))
    return out


# ---- main -----------------------------------------------------------------------------------------------

def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Change the greeting prefix in a copy and check every UI follows.")
    ap.add_argument("--prefix", default=u"hello ", help='the new prefix (default "hello ")')
    ap.add_argument("--work", help="work directory (default $HOME/loomora-build/prefix-check)")
    ap.add_argument("--targets", help="comma-separated target ids (see --list); default: every target available here")
    ap.add_argument("--vm", action="store_true", help="also run the Windows 2000 VM target (the VM must already run)")
    ap.add_argument("--list", action="store_true", help="list the targets and exit")
    ap.add_argument("--wsl-distro", default=os.environ.get("LOOMORA_WSL_DISTRO") or "Ubuntu-24.04")
    ap.add_argument("--jobs", type=int, default=4, help="UI instances run in parallel (default 4)")
    ap.add_argument("--evidence-dir", help="also write the text output of each platform into DIR/<platform-id>/prefix-check.txt")
    ap.add_argument("--selftest", action="store_true", help="check the checks of this tool and exit")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    repo = common.ROOT
    work = Path(args.work) if args.work else Path.home() / "loomora-build" / "prefix-check"
    ctx = Ctx(args, repo, work, Machine(args.wsl_distro))
    targets = make_targets(ctx)
    ids = [t.id for t in targets]
    if args.list:
        for t in targets:
            print("%-22s %s" % (t.id, t.label))
        return 0
    wanted = set(args.targets.split(",")) if args.targets else None
    if wanted and not wanted <= set(ids):
        print("prefix_check: unknown target(s) %s; known: %s" % (", ".join(sorted(wanted - set(ids))), ", ".join(ids)), file=sys.stderr)
        return 2

    rec = Recorder()
    say = rec.say
    say("prefix_check: prefix %r -> %r in a COPY of the sources (the repository is never edited)" % (OLD_PREFIX, args.prefix))
    say("host: %s %s, repository %s" % (platform.system(), platform.release(), repo))
    before = files_under(repo, WATCHED)
    make_copy(repo, ctx.copy)
    diff = change_prefix(ctx.copy, args.prefix)
    say("copy: %s" % ctx.copy)
    say("change in the copy:")
    for line in diff:
        say("  " + line)
    differing = differing_files(repo, ctx.copy)
    problems = check_change_scope(differing)
    say("files of core/ ui/ bridge/ cmake/ CMakeLists.txt that differ from the repository: %d (%s); under ui/ or bridge/: %d"
        % (len(differing), ", ".join(differing) or "none", len([p for p in differing if p.startswith(("ui/", "bridge/"))])))
    say("")

    results = []  # (target, status, detail)
    for t in targets:
        if wanted is not None and t.id not in wanted:
            continue
        if t.id == "windows-2000-x86" and not args.vm:
            results.append((t, "NOT TESTED", "VM target: pass --vm with the VM running and logged on (this tool never starts or restores it)"))
            continue
        if t.needs_windows and not IS_WINDOWS:
            results.append((t, "NOT TESTED", "needs a Windows host to run (no Wine)"))
            continue
        if t.id == "windows-2000-x86" and not vm_running():
            results.append((t, "NOT TESTED", "the VM is not running"))
            continue
        rec.section = t.id
        say("== %s: %s" % (t.id, t.label))
        started = time.time()
        try:
            exe = t.build(ctx)
        except NotBuilt as e:
            results.append((t, "NOT BUILT", str(e)))
            say("   NOT BUILT: %s" % e)
            continue
        except RuntimeError as e:
            results.append((t, "NOT BUILT", str(e)))
            say("   NOT BUILT: %s" % e)
            continue
        make = t.launcher
        if make is None:
            make = linux_launcher(t.variant, linux_platform_id_for(ctx))
        launcher = None
        try:
            launcher = make(ctx, exe)
            what = launcher.prepare()
            say("   built in %.0f s; launcher: %s" % (time.time() - started, what))
            vectors = vectors_for(type(launcher))
            passed, failed, example = run_target(launcher, vectors, args.prefix, args.jobs, say)
        except (RuntimeError, OSError, subprocess.SubprocessError) as e:
            results.append((t, "FAIL", "could not run: %s" % e))
            say("   FAIL: could not run: %s" % e)
            continue
        finally:
            if launcher is not None:
                launcher.close()
        sha = common.sha256(exe) if not isinstance(exe, str) else "(Linux executable, built in WSL)"
        status = "PASS" if failed == 0 else "FAIL"
        results.append((t, status, "%d of %d vector(s) show %r + input; default text -> %r" % (passed, passed + failed, args.prefix, example)))
        say("   %s: %d vector(s) run, %d passed, %d failed; the default text shows %r; %s"
            % (status, passed + failed, passed, failed, example, sha))

    rec.section = "footer"
    after_diff = differing_files(repo, ctx.copy)
    problems += check_change_scope(after_diff)
    after = files_under(repo, WATCHED)
    changed_repo = sorted(p for p in set(before) | set(after) if before.get(p) != after.get(p))
    if changed_repo:
        problems.append("the repository changed during the run: %s" % ", ".join(changed_repo))
    say("")
    say("after the builds: files that differ between the repository and the copy: %s; repository files changed by this run: %d"
        % (", ".join(after_diff) or "none", len(changed_repo)))
    say("")
    say("%-22s %-11s %s" % ("target", "result", "detail"))
    for t, status, detail in results:
        rec.section = t.id
        say("%-22s %-11s %s" % (t.id, status, detail))
    rec.section = "footer"
    for label, reason in not_tested_platforms(None):
        say("%-22s %-11s %s" % (label, "NOT TESTED", reason))
    for p in problems:
        say("PROBLEM: " + p)
    tested = [r for r in results if r[1] == "PASS"]
    bad = [r for r in results if r[1] in ("FAIL", "NOT BUILT")]
    rec.section = "global"
    say("")
    say("prefix_check: %d platform/variant(s) tested and show %r + input, %d failed or not built, %d not tested; %s"
        % (len(tested), args.prefix, len(bad), len([r for r in results if r[1] == "NOT TESTED"]) + len(not_tested_platforms(None)),
           "only %s changed, nothing under ui/ or bridge/" % GREET_SOURCE if not problems else "%d problem(s)" % len(problems)))
    if args.evidence_dir:
        write_evidence(Path(args.evidence_dir), rec, results, ctx, args.prefix, problems)
    if not tested and not bad:
        return 2
    return 1 if (bad or problems) else 0


class Recorder(object):
    """Prints and remembers every line with the section (target id, header, footer, global) it belongs to."""

    def __init__(self):
        self.lines = []
        self.section = "header"

    def say(self, text=""):
        print(text)
        self.lines.append((self.section, text))


def evidence_id(target, ctx):
    if target.platform_id:
        return target.platform_id
    return linux_platform_id_for(ctx)


def write_evidence(directory, rec, results, ctx, prefix, problems):
    """DIR/<platform-id>/prefix-check.txt: the header, the sections of the targets of that platform,
    the integrity lines, the rows of its targets and a one-line summary."""
    groups = {}
    for t, status, detail in results:
        groups.setdefault(evidence_id(t, ctx), []).append((t, status, detail))
    for pid, rows in sorted(groups.items()):
        mine = set(r[0].id for r in rows)
        out = [text for section, text in rec.lines
               if section in ("header", "footer") or section in mine]
        ok = [r for r in rows if r[1] == "PASS"]
        out.append("")
        out.append("platform %s: %d of %d target(s) tested show %r + input; %s"
                   % (pid, len(ok), len(rows), prefix, "no problem with the scope of the change" if not problems else "%d problem(s)" % len(problems)))
        path = Path(directory) / pid / "prefix-check.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(str(path), "w", encoding="utf-8", newline=chr(10)) as f:
            f.write(chr(10).join(out) + chr(10))
        print("evidence written: %s" % path)


# ---- self-test ---------------------------------------------------------------------------------------------

def selftest():
    import tempfile
    failures = []

    def expect(name, got, want):
        ok = got == want
        print("%s selftest %-62s got %r, want %r" % ("OK  " if ok else "FAIL", name, got, want))
        if not ok:
            failures.append(name)

    base = Path(tempfile.mkdtemp(prefix="loomora-prefix-selftest-"))
    try:
        repo = base / "repo"
        for rel, text in (("core/src/greet.cpp", u'return "Bonjour " + input;\n'), ("ui/legacy/x/main.cpp", u"int n;\n"),
                          ("bridge/jni/j.cpp", u"int j;\n"), ("CMakeLists.txt", u"project(x)\n"), ("tools/t.py", u"x\n")):
            p = repo / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        copy = base / "copy"
        make_copy(repo, copy)
        expect("fresh copy equals the repository", differing_files(repo, copy), [])
        diff = change_prefix(copy, u"hello ")
        expect("one line replaced in the copy", [l for l in diff if l.startswith(("-", "+")) and not l.startswith(("---", "+++"))],
               ['-return "Bonjour " + input;', '+return "hello " + input;'])
        expect("only greet.cpp differs", differing_files(repo, copy), [GREET_SOURCE])
        expect("scope ok", check_change_scope(differing_files(repo, copy)), [])
        expect("repository untouched", (repo / GREET_SOURCE).read_text(encoding="utf-8"), u'return "Bonjour " + input;\n')
        (copy / "ui/legacy/x/main.cpp").write_text(u"int n = 1;\n", encoding="utf-8")
        scope = check_change_scope(differing_files(repo, copy))
        expect("a changed UI file is reported", len(scope), 2)
        (copy / "ui/legacy/x/main.cpp").write_text(u"int n;\n", encoding="utf-8")
        for bad in (u"nothing\n", u'"Bonjour " "Bonjour "\n'):
            (copy / GREET_SOURCE).write_text(bad, encoding="utf-8")
            try:
                change_prefix(copy, u"hello ")
                raised = False
            except ValueError:
                raised = True
            expect("prefix literal not exactly once is an error", raised, True)

        def result(line, rc=0):
            return run_vectors.RunResult(rc, line + "\n", "", 0.1)
        good = json.loads(check_probe.GOOD_LINE)
        good.update(message=u"hello Hélène")
        line = json.dumps(good)  # ASCII escapes, as the UIs write it
        expect("a hello probe line passes", check_greeting(u"hello ", u"Hélène", result(line)), [])
        expect("the old prefix is rejected", len(check_greeting(u"hello ", u"Hélène", result(check_probe.GOOD_LINE))) > 0, True)
        expect("a launch error is reported", len(check_greeting(u"hello ", u"x", run_vectors.RunResult(None, "", "", 0, "boom"))), 1)
        expect("a non-zero exit is reported", len(check_greeting(u"hello ", u"Hélène", result(line, 3))), 1)
        with ThreadPoolExecutor(max_workers=8) as pool:  # the launchers run vectors in parallel
            parallel = list(pool.map(lambda _: check_greeting(u"hello ", u"Hélène", result(line)), range(200)))
        expect("200 parallel checks all pass", sum(1 for r in parallel if r), 0)
        expect("check_probe constant restored", check_probe.PREFIX, u"Bonjour ")
    finally:
        shutil.rmtree(str(base), ignore_errors=True)
    print("selftest: %d failed" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
