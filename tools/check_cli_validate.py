#!/usr/bin/env python
"""Test of the host command `loomora validate` (spec 002, T076, contracts/cli-validate.md).

Usage:
  python tools/check_cli_validate.py --loomora PATH_TO_EXE [--corpus DIR] [--jobs N] [-v]

--corpus DIR is a directory written by `python tools/gen_invalid_corpus.py DIR` (it holds corpus.json);
when DIR has no corpus.json the corpus is generated into it and kept. Without --corpus it is generated
into a temporary directory that is removed at the end.

What it runs (standard library only; the program is started as a child process, nothing of Loomora
is imported):
  valid-*       status 0 and no issue line
  warning-*     status 0 and the `warn.*` code printed as a warning
  large-*       with the --set-limit options of corpus.json: status 0 and the warning; without them: no
                `warn.count` (the thresholds are the defaults)
  invalid-*, hostile-*   status 1 (1 or 2 for a file that is not a ZIP archive at all) and an `error` line
                carrying the expected code (the code of the table, or any code of its family when
                corpus.json says `contract_code` is false: the contract does not name that sub-code)
  truncate-*    status 1 or 2, never 0, never a crash
  a missing path, a directory, no argument, a bad option: status 2
  --json        every line parses; counts, `valid` and the issues agree with the text output
  --quiet       only summary lines; --max-issues N prints at most N issues and the counts stay exact
  several files give the highest status, one summary line per file
  the files are not changed (same bytes) and no file is created in the corpus directory
  a file whose name holds non-ASCII characters keeps its name, in UTF-8, in the output

Exit status: 0 all rows pass, 1 a row failed, 2 usage error or the program or the corpus is missing.
Rows that fail because the semantic validator (references, geometry, warnings) is not merged yet
are listed under their own code family in the failure list, like any other row.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import common  # noqa: E402

TIMEOUT = 120
KIND_NAMES = {1: "frame", 2: "layout", 3: "image", 4: "spool", 5: "result"}


class Run(object):
    """The result of one execution of the program."""

    def __init__(self, status, out, err):
        self.status, self.out, self.err = status, out, err
        self.lines = out.splitlines()

    def issues(self, severity=None):
        """[(severity, code)] of the issue lines (`error ` / `warning ` first word)."""
        found = []
        for line in self.lines:
            words = line.split()
            if len(words) >= 2 and words[0] in ("error", "warning"):
                if severity is None or words[0] == severity:
                    found.append((words[0], words[1]))
        return found

    def summaries(self):
        return [l for l in self.lines if l.startswith(("OK ", "FAIL "))]


def run(exe, args, raw=False):
    """Starts `loomora validate ARGS` (raw: `loomora ARGS`, without the word)."""
    try:
        p = subprocess.run([exe] + ([] if raw else ["validate"]) + [str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return Run(-999, "", "timeout")
    except OSError as e:
        return Run(-998, "", str(e))
    return Run(p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace"))


def crashed(r):
    """A signal, a Windows exception code or a timeout: never an acceptable status."""
    return r.status < 0 or r.status > 100


def code_ok(meta, found_codes):
    expected = meta.get("code")
    if not expected:
        return True
    if expected == "schema.number":  # the library reports a non-finite number as num.not_finite
        return "num.not_finite" in found_codes or "schema.number" in found_codes
    if meta.get("contract_code"):
        return expected in found_codes
    family = meta.get("family") or expected.split(".")[0]
    return any(c == expected or c.startswith(family + ".") for c in found_codes)


def check_row(exe, corpus, name, meta, problems):
    """One corpus file; appends (name, family, text) to problems."""
    path = os.path.join(corpus, name)
    kind = meta["kind"]
    fam = (meta.get("code") or kind).split(".")[0] if kind != "large" else "large"
    limits = []
    for item in meta.get("set_limit") or []:
        limits += ["--set-limit", item]

    def bad(text):
        problems.append((name, meta.get("code") or kind, text))

    r = run(exe, limits + [path])
    if crashed(r):
        return bad("status %d (crash or timeout) %s" % (r.status, r.err.strip()[:120]))
    expected_status = meta.get("exit") or [0]
    if r.status not in expected_status:
        return bad("status %d, expected %s; first lines: %s" % (r.status, expected_status, " | ".join(r.lines[:3])))
    if len(r.summaries()) != 1 and r.status in (0, 1):
        return bad("%d summary lines, expected 1" % len(r.summaries()))
    errors, warnings = [c for _, c in r.issues("error")], [c for _, c in r.issues("warning")]
    summary = r.summaries()[0] if r.summaries() else ""
    if r.status == 0 and not summary.startswith("OK "):
        bad("status 0 but the summary line is %r" % summary)
    if r.status == 1 and not summary.startswith("FAIL "):
        bad("status 1 but the summary line is %r" % summary)

    if kind == "valid":
        if errors or warnings:
            bad("a valid file reports %s" % (errors + warnings))
    elif kind in ("invalid", "hostile"):
        if r.status == 1 and not errors:
            bad("status 1 and no `error` line")
        elif r.status == 1 and not code_ok(meta, errors):
            bad("expected an error %s, got %s" % (meta["code"], errors or "none"))
    elif kind == "truncate":
        if r.status == 1 and not errors:
            bad("status 1 and no `error` line")
    elif kind in ("warning", "large"):
        if errors:
            bad("unexpected error(s) %s" % errors)
        if meta.get("code") not in warnings:
            bad("expected the warning %s, got %s" % (meta.get("code"), warnings or "none"))
    # incidental issues: the validation stops at the container level, so only files that get past it carry them
    for extra in (meta.get("also") or []) if kind != "invalid" and kind != "hostile" and kind != "truncate" else []:
        if extra not in errors + warnings:
            bad("expected the incidental issue %s, got %s" % (extra, errors + warnings))
    if kind == "large":  # the thresholds are the defaults without --set-limit: no count warning
        r2 = run(exe, [path])
        if crashed(r2) or r2.status != 0 or "warn.count" in [c for _, c in r2.issues()]:
            bad("without --set-limit: status %d, issues %s" % (r2.status, r2.issues()))


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class Checker(object):
    def __init__(self):
        self.rows = 0
        self.failures = []  # (row, family, text)

    def row(self, name, cond, text=""):
        self.rows += 1
        if not cond:
            self.failures.append((name, "cli", text))


def option_rows(exe, corpus, files, ck):
    """Options, statuses and formats on a few files picked from the corpus."""
    valid = os.path.join(corpus, "valid-basic.loomora")
    valid2 = os.path.join(corpus, "valid-two-frames.loomora")
    # the invalid file with the most issues in the full output, and the number of them
    best, best_run = None, None
    for name, meta in files.items():
        if meta["kind"] in ("invalid", "hostile") and meta.get("code") and meta["code"].split(".")[0] != "zip":
            r = run(exe, [os.path.join(corpus, name)])
            if r.status == 1 and (best_run is None or len(r.issues()) > len(best_run.issues())):
                best, best_run = os.path.join(corpus, name), r
    if best is None:  # no row passed: the rows above already report it
        best = os.path.join(corpus, "invalid-ref.missing-entry.loomora")
        best_run = run(exe, [best])
    missing = os.path.join(corpus, "does-not-exist.loomora")

    # statuses of the usage and read errors
    r = run(exe, [missing])
    ck.row("missing path", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, [valid, missing])
    ck.row("valid + missing path", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, [missing, best])
    ck.row("missing + invalid", r.status == 2, "status %d, expected 2 (highest)" % r.status)
    r = run(exe, [corpus])
    ck.row("a directory", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, [])
    ck.row("no `validate` argument", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, [], raw=True)
    ck.row("no argument", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, ["--no-such-option", valid])
    ck.row("unknown option", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, ["--max-issues", "x", valid])
    ck.row("--max-issues not a number", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, ["--max-issues"])
    ck.row("--max-issues without value", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, ["--set-limit", "bogus=1", valid])
    ck.row("--set-limit unknown name", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, ["--set-limit", "warn_nails", valid])
    ck.row("--set-limit without =value", r.status == 2, "status %d, expected 2" % r.status)
    r = run(exe, ["--version"], raw=True)
    ck.row("--version", r.status == 0 and r.out.strip() != "", "status %d, output %r" % (r.status, r.out[:60]))
    r = run(exe, [valid], raw=True)  # the word `validate` may be left out
    ck.row("without the validate word", r.status == 0 and len(r.summaries()) == 1, "status %d: %s" % (r.status, r.lines[:2]))
    r = run(exe, ["--progress", valid])
    ck.row("--progress", r.status == 0 and len(r.summaries()) == 1, "status %d, output %s" % (r.status, r.lines[:2]))

    # several files: one summary per file, highest status
    r = run(exe, [valid, valid2])
    ck.row("two valid files", r.status == 0 and len(r.summaries()) == 2, "status %d, %d summaries" % (r.status, len(r.summaries())))
    r = run(exe, [valid, best, valid2])
    ck.row("valid + invalid + valid", r.status == 1 and len(r.summaries()) == 3,
           "status %d, %d summaries, expected 1 and 3" % (r.status, len(r.summaries())))
    s = r.summaries()
    ck.row("summary lines in argument order", len(s) == 3 and s[0].startswith("OK ") and s[1].startswith("FAIL ")
           and s[2].startswith("OK "), "summaries %s" % s)

    # --quiet
    r = run(exe, ["--quiet", best, valid])
    ck.row("--quiet", r.status == 1 and len(r.lines) == 2 and not r.issues() and len(r.summaries()) == 2,
           "status %d, lines %s" % (r.status, r.lines[:4]))
    full = run(exe, [best])
    ck.row("--quiet summary is the full output summary", bool(r.summaries()) and bool(full.summaries())
           and r.summaries()[0] == full.summaries()[-1], "%s vs %s" % (r.summaries()[:1], full.summaries()[-1:]))

    # --max-issues
    n_full = len(full.issues())
    r0 = run(exe, ["--max-issues", "0", best])
    ck.row("--max-issues 0", r0.status == 1 and not r0.issues() and r0.summaries() == full.summaries(),
           "status %d, issues %s, summary %s vs %s" % (r0.status, r0.issues(), r0.summaries(), full.summaries()))
    if n_full >= 2:
        r1 = run(exe, ["--max-issues", "1", best])
        ck.row("--max-issues 1", r1.status == 1 and len(r1.issues()) == 1 and r1.summaries() == full.summaries(),
               "status %d, %d issue lines, summary %s vs %s" % (r1.status, len(r1.issues()), r1.summaries(), full.summaries()))
    rbig = run(exe, ["--max-issues", "1000", best])
    ck.row("--max-issues above the count", rbig.lines == full.lines, "output differs from the unlimited run")

    # --json
    r = run(exe, ["--json", best, valid])
    objs, parse_ok = [], True
    for line in r.lines:
        try:
            objs.append(json.loads(line))
        except ValueError:
            parse_ok = False
    ck.row("--json parses", parse_ok and len(objs) == 2 and r.status == 1,
           "status %d, %d objects, parsed %s: %s" % (r.status, len(objs), parse_ok, r.out[:160]))
    if parse_ok and len(objs) == 2:
        bad_obj, good_obj = objs
        keys = ("file", "valid", "errors", "warnings", "issues")
        ck.row("--json keys", all(k in o for o in objs for k in keys), "keys %s" % [sorted(o) for o in objs])
        ck.row("--json invalid file", bad_obj.get("valid") is False and bad_obj.get("errors", 0) >= 1
               and bad_obj.get("file") == best, "%s" % json.dumps(bad_obj)[:200])
        ck.row("--json valid file", good_obj.get("valid") is True and good_obj.get("errors") == 0
               and good_obj.get("issues") == [] and good_obj.get("file") == valid, "%s" % json.dumps(good_obj)[:200])
        issues = bad_obj.get("issues", [])
        fields = all(isinstance(i, dict) and i.get("severity") in ("error", "warning") and i.get("code")
                     and "entry" in i and "item" in i and "message" in i for i in issues)
        ck.row("--json issue fields", fields and len(issues) == n_full, "%d issues vs %d text lines; %s"
               % (len(issues), n_full, json.dumps(issues[:1])[:200]))
        ck.row("--json counts agree", sum(1 for i in issues if i.get("severity") == "error") == bad_obj.get("errors")
               and sum(1 for i in issues if i.get("severity") == "warning") == bad_obj.get("warnings"),
               json.dumps(bad_obj)[:200])
        ck.row("--json codes match the text output", [(i.get("severity"), i.get("code")) for i in issues]
               == full.issues(), "%s vs %s" % ([(i.get("severity"), i.get("code")) for i in issues], full.issues()))
        items = [i["item"] for i in issues if i.get("item")]
        ck.row("--json item shape", all(isinstance(i, dict) and i.get("kind") in KIND_NAMES.values()
                                        and isinstance(i.get("id"), int) for i in items), "%s" % items[:3])
    rj = run(exe, ["--json", "--max-issues", "0", best])
    try:
        oj = json.loads(rj.lines[0])
        ck.row("--json --max-issues 0", oj.get("issues") == [] and oj.get("errors") == bad_obj.get("errors")
               if parse_ok and len(objs) == 2 else oj.get("issues") == [], json.dumps(oj)[:200])
    except (ValueError, IndexError):
        ck.row("--json --max-issues 0", False, "does not parse: %r" % rj.out[:120])
    rm = run(exe, ["--json", missing])
    try:
        om = json.loads(rm.lines[0])
        ck.row("--json missing path", rm.status == 2 and om.get("valid") is False, "status %d %s" % (rm.status, rm.out[:160]))
    except (ValueError, IndexError):
        ck.row("--json missing path", False, "status %d, does not parse: %r" % (rm.status, rm.out[:120]))

    # a path with non-ASCII characters keeps its name, in UTF-8, in the output
    tmp = tempfile.mkdtemp(prefix="loomora-cli-")
    try:
        name = u"préférence-日本-\U0001F600.loomora"
        copy = os.path.join(tmp, name)
        shutil.copyfile(valid, copy)
        r = run(exe, [copy])
        ck.row("non-ASCII file name", r.status == 0 and name in r.out, "status %d, output %r" % (r.status, r.out[:160]))
        r = run(exe, ["--json", copy])
        try:
            same = json.loads(r.lines[0]).get("file") == copy
        except (ValueError, IndexError):
            same = False
        ck.row("non-ASCII file name (--json)", r.status == 0 and same, "status %d, output %r" % (r.status, r.out[:160]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--loomora", required=True, help="the built `loomora` host command")
    ap.add_argument("--corpus", help="directory written by tools/gen_invalid_corpus.py (generated when missing)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("-v", "--verbose", action="store_true", help="also list the rows that pass")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    exe = os.path.abspath(args.loomora)
    if not os.path.isfile(exe):
        print("check_cli_validate: %s is not a file" % args.loomora, file=sys.stderr)
        return 2

    own_dir = None
    corpus = args.corpus
    if corpus is None or not os.path.isfile(os.path.join(corpus, "corpus.json")):
        if corpus is None:
            own_dir = corpus = tempfile.mkdtemp(prefix="loomora-corpus-")  # removed at the end
        gen = subprocess.run([sys.executable, os.path.join(HERE, "gen_invalid_corpus.py"), corpus],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if gen.returncode != 0:
            print("check_cli_validate: the corpus generator failed:\n" + gen.stdout.decode("utf-8", "replace"), file=sys.stderr)
            if own_dir:
                shutil.rmtree(own_dir, ignore_errors=True)
            return 2
    corpus = os.path.abspath(corpus)
    try:
        index = os.path.join(corpus, "corpus.json")
        if not os.path.isfile(index):
            print("check_cli_validate: no corpus.json in %s" % corpus, file=sys.stderr)
            return 2
        with open(index, encoding="utf-8") as f:
            files = OrderedDict(json.load(f, object_pairs_hook=OrderedDict)["files"])
        before = dict((n, digest(os.path.join(corpus, n))) for n in os.listdir(corpus))
        problems = []

        def one(item):
            local = []
            check_row(exe, corpus, item[0], item[1], local)
            return local

        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for local in pool.map(one, list(files.items())):
                problems += local
        ck = Checker()
        option_rows(exe, corpus, files, ck)
        after = dict((n, digest(os.path.join(corpus, n))) for n in os.listdir(corpus))
        ck.row("files untouched", before == after,
               "changed or created: %s" % sorted(set(before) ^ set(after) | set(n for n in before if before.get(n) != after.get(n)))[:5])
    finally:
        if own_dir:
            shutil.rmtree(own_dir, ignore_errors=True)

    failures = [(n, c, t) for (n, c, t) in problems] + ck.failures
    kinds = {}
    for meta in files.values():
        kinds[meta["kind"]] = kinds.get(meta["kind"], 0) + 1
    failed_files = set(n for n, _, _ in problems)
    for n, c, t in failures:
        print("FAIL %s [%s]: %s" % (n, c, t))
    corpus_rows = len(files)
    print("check_cli_validate: %d corpus file(s) (%s), %d failed; %d option row(s), %d failed"
          % (corpus_rows, ", ".join("%s %d" % kv for kv in sorted(kinds.items())), len(failed_files),
             ck.rows, len(ck.failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
