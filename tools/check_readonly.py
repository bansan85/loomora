#!/usr/bin/env python
"""Use mode is read-only (spec 002, User Story 2, SC-012, T042).

Runs core/tests/use_demo.cpp (`loomora_use_demo`, a client written against the public C API) on the
`basic` reference file written by `loomora_refgen`, and checks two things:

1. Read-only file. The file is marked read-only (and, on POSIX, so is its directory). `list`,
   `result` and `thumb` succeed and print what the archive holds (compared with Python's own zipfile
   reading); afterwards the bytes, the modification time (ns), the permissions and the directory
   listing are exactly what they were: nothing was written, no temporary or lock file was created.
2. Atomic replace under a reader. A first process (`hold`) opens the file and fingerprints every
   entry; a second process (`write-basic`, the real writer, loomora_editor_save_path) replaces the
   file while the first still holds it. The first keeps reading exactly the old archive (same
   fingerprint, same title), a process started afterwards reads exactly the new one, and once the
   reader is closed nothing is left beside the file (no temporary, no moved-aside file). On Windows 11
   MoveFileExW cannot replace a file a reader holds open (spike S-F3): the writer falls back to a
   rename with POSIX semantics or to a move-aside, and this scenario proves it.

Usage: python tools/check_readonly.py [--build-dir DIR] [--keep]
  --build-dir DIR   the CMake build directory holding loomora_refgen and loomora_use_demo
                    (default: the one of tools/verify.py, `build/`)
Exit status: 0 all good, 1 a check failed, 2 a binary is missing (build first) or a usage error.
"""
import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import threading
import zipfile
from pathlib import Path

try:
    import common
    common.setup_utf8_output()
    ROOT = common.ROOT
except ImportError:  # pragma: no cover
    ROOT = Path(__file__).resolve().parent.parent

IS_WINDOWS = os.name == "nt"
EXE = ".exe" if IS_WINDOWS else ""
TIMEOUT = 60

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL: %s" % what)
    return ok


def find_exe(build_dir, name):
    for path in sorted(Path(build_dir).rglob(name + EXE)):
        if path.is_file() and "CMakeFiles" not in path.parts:
            return path
    return None


def run(cmd, **kw):
    kw.setdefault("timeout", TIMEOUT)
    return subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, **kw)


def snapshot(path):
    st = os.stat(str(path))
    return {"bytes": Path(path).read_bytes(), "mtime_ns": st.st_mtime_ns, "mode": stat.S_IMODE(st.st_mode),
            "dir": sorted(os.listdir(str(Path(path).parent)))}


def fingerprint(path):
    """FNV-1a 64 over the entries in name order (name, 0, bytes): the value use_demo `hold` prints."""
    with zipfile.ZipFile(str(path)) as z:
        manifest = z.read("manifest.json")
        names = set(json.loads(manifest.decode("utf-8"))["entries"]) | {"manifest.json", "mimetype"}
        h = 14695981039346656037
        for name in sorted(names):
            for b in name.encode("utf-8") + b"\0" + z.read(name):
                h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return "%016x" % h, json.loads(manifest.decode("utf-8"))["title"]


def writable(path, on):
    if IS_WINDOWS:
        os.chmod(str(path), stat.S_IWRITE | stat.S_IREAD if on else stat.S_IREAD)
    elif Path(path).is_dir():
        os.chmod(str(path), 0o755 if on else 0o555)
    else:
        os.chmod(str(path), 0o644 if on else 0o444)


def scenario_read_only(demo, ref, work):
    d = work / "ro"
    out = work / "out"
    d.mkdir()
    out.mkdir()
    f = d / "ref.loomora"
    shutil.copyfile(str(ref), str(f))
    writable(f, False)
    if not IS_WINDOWS:
        writable(d, False)  # any attempt to create a file beside the project fails
    try:
        check(not os.access(str(f), os.W_OK) or (hasattr(os, "geteuid") and os.geteuid() == 0), "the file is read-only")
        before = snapshot(f)
        with zipfile.ZipFile(str(ref)) as z:
            thumb = z.read("results/1/thumb.png")
            manifest = json.loads(z.read("manifest.json").decode("utf-8"))

        r = run([demo, "list", f])
        check(r.returncode == 0, "list succeeds on a read-only file (%s)" % r.stderr.strip())
        check(("title: %s" % manifest["title"]) in r.stdout and "results: 1" in r.stdout and "spools: 3" in r.stdout,
              "list prints the title and the counts")
        r = run([demo, "result", f, "1", "0", "3"])
        check(r.returncode == 0 and "result 1: 5000 steps" in r.stdout and r.stdout.count("\nstep ") == 3,
              "result prints the step count and the first steps")
        r = run([demo, "thumb", f, "result", "1", out / "t.png"])
        check(r.returncode == 0 and (out / "t.png").read_bytes() == thumb, "thumb writes the exact thumbnail outside the project directory")
        r = run([demo, "result", f, "99"])
        check(r.returncode == 1, "an unknown result is an error, not a crash")

        after = snapshot(f)
        check(after["bytes"] == before["bytes"], "the file bytes are unchanged")
        check(after["mtime_ns"] == before["mtime_ns"], "the modification time is unchanged (ns)")
        check(after["mode"] == before["mode"], "the permissions are unchanged")
        check(after["dir"] == before["dir"] == ["ref.loomora"], "no new file in the directory: %s" % after["dir"])
    finally:
        if not IS_WINDOWS:
            writable(d, True)
        writable(f, True)


class Holder(object):
    """A `hold` process: reads one line on stdout at open, one more after it is told to continue."""

    def __init__(self, demo, path):
        self.p = subprocess.Popen([str(demo), "hold", str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, universal_newlines=True)
        self.timer = threading.Timer(TIMEOUT, self.p.kill)
        self.timer.start()
        self.opened = self.p.stdout.readline().strip()

    def finish(self):
        try:
            self.p.stdin.write("go\n")
            self.p.stdin.flush()
        except (OSError, ValueError):
            pass
        out, err = self.p.communicate()
        self.timer.cancel()
        return self.p.returncode, out.strip(), err.strip()


def scenario_replace(demo, ref, work):
    d = work / "replace"
    d.mkdir()
    f = d / "p.loomora"
    shutil.copyfile(str(ref), str(f))
    old_fp, old_title = fingerprint(f)
    holder = Holder(demo, f)
    check(holder.opened == "opened title=%s fp=%s" % (old_title, old_fp), "the reader sees the old archive: %r" % holder.opened)

    r = run([demo, "write-basic", f, "Replaced"])  # the writer replaces the file the reader holds open
    check(r.returncode == 0, "the writer replaces a file held open by a reader (%s)" % r.stderr.strip())
    new_fp, new_title = fingerprint(f)
    check(new_title == "Replaced" and new_fp != old_fp, "the file on disk is the new archive")
    names = sorted(os.listdir(str(d)))
    extra = [n for n in names if n != "p.loomora"]
    # Windows: a moved-aside old file may stay listed (delete pending) while a reader holds it open.
    check(all(IS_WINDOWS and n.startswith("p.loomora.old-") for n in extra), "nothing but the file beside it while held: %s" % names)

    fresh = Holder(demo, f)  # a reader opened after the replace
    check(fresh.opened == "opened title=%s fp=%s" % (new_title, new_fp), "a reader opened after the replace sees only the new archive: %r" % fresh.opened)
    rc, out, err = fresh.finish()
    check(rc == 0 and out.endswith("title=%s fp=%s" % (new_title, new_fp)), "the new reader is stable (%s)" % err)

    rc, out, err = holder.finish()  # the old reader, after the replacement
    check(rc == 0 and out == "after title=%s fp=%s" % (old_title, old_fp), "the old reader still reads only the old archive: %r %s" % (out, err))
    check(sorted(os.listdir(str(d))) == ["p.loomora"], "no temporary or moved-aside file once the readers are closed: %s" % sorted(os.listdir(str(d))))
    check(fingerprint(f) == (new_fp, new_title), "the file is still the new archive")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Use mode is read-only (spec 002, T042).")
    ap.add_argument("--build-dir", default=str(ROOT / "build"), help="CMake build directory")
    ap.add_argument("--keep", action="store_true", help="keep the working directory")
    args = ap.parse_args(argv)
    build = Path(args.build_dir)
    refgen, demo = find_exe(build, "loomora_refgen"), find_exe(build, "loomora_use_demo")
    if refgen is None or demo is None:
        print("check_readonly: loomora_refgen or loomora_use_demo not found under %s (build first)" % build, file=sys.stderr)
        return 2
    work = build / "check_readonly"
    if work.exists():
        for p in work.rglob("*"):
            try:
                os.chmod(str(p), 0o755 if p.is_dir() else stat.S_IWRITE | stat.S_IREAD)
            except OSError:
                pass
        shutil.rmtree(str(work))
    work.mkdir(parents=True)
    gen = work / "gen"
    gen.mkdir()
    r = run([refgen, gen])
    if r.returncode != 0 or not (gen / "basic.loomora").is_file():
        print("check_readonly: loomora_refgen failed: %s" % r.stderr.strip(), file=sys.stderr)
        return 2
    ref = gen / "basic.loomora"
    scenario_read_only(demo, ref, work)
    scenario_replace(demo, ref, work)
    if not args.keep:
        shutil.rmtree(str(work), ignore_errors=True)
    if failures:
        print("check_readonly: %d check(s) failed" % len(failures))
        return 1
    print("check_readonly: ok (read-only open changes nothing; replace under a reader keeps old and new apart)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
