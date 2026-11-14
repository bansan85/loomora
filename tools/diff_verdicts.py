#!/usr/bin/env python
"""Compare two sets of verdicts and print every difference per platform and variant (FR-018).

Usage:
  python tools/diff_verdicts.py OLD [NEW] [--repo DIR] [--prefix PATH] [--full] [--strict]
  python tools/diff_verdicts.py --self-test

OLD and NEW each name one set of validation/verdicts/ (with the validation/deliverables/ next to
it). A set is, in the order tried:
  * an existing folder: the verdicts folder itself, a validation folder (it holds verdicts/) or
    the docs root (it holds validation/verdicts/); "dir:PATH" forces this reading;
  * otherwise a git revision (a commit, a branch, a tag, HEAD~1, ...) read with git from the
    repository DIR (default docs/implementation, the docs submodule) under the folder PREFIX
    (default "validation"); "git:REV" forces this reading.
NEW defaults to the verdicts of the working tree (docs/implementation/validation). OLD is the
baseline (what was expected), NEW what was observed.

Output, one block per platform and variant (the verdict file name without ".json": platform id,
variant, and the web path for the Web family), sorted:
  * STATUS CHANGE old -> new, flagged first, with "(better)" / "(worse)" for a move between ok,
    ok_restricted and ko ("not_tested" to a result is "(first result)", the other way "(result lost)");
  * every other field of the verdict that differs: scalar fields as "field: old -> new"; list
    fields (restrictions, disabled_features, attempts, evidence, ...) as the items added (+) and
    removed (-), an evidence item being identified by its path and reported when its sha256 or its
    kind changed; dictionary fields (measurements) as the changed leaves; very long texts are cut
    at 110 characters unless --full is given;
  * the same for the deliverable record of that platform and variant (fallbacks, disabled
    features, compiler, target, UI toolkit, artifact; its free-text "notes" is left out because it
    changes with every build);
  * verdicts and deliverables present on one side only.
A last "summary:" line counts them. Exit status: 0 (also when there are differences, the report is
the product), 1 with --strict when a status changed or a verdict disappeared, 2 on a usage error
or an unreadable set. Python 3.8+, standard library only; the Windows console is switched to UTF-8.

Importable: load_set(spec, repo, prefix) -> VerdictSet and compare_sets(old, new, full) ->
Comparison (.lines, .status_changes, .added, .removed, ...), meant for tools/report.py (T098), which
does not exist yet: the section of the report is still to be written.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import common

VERDICT_FIELDS_FIRST = ("status", "decision", "web_path", "restrictions", "disabled_features",
                        "cause", "attempts", "reason", "evidence", "reproduction")
SKIP_DELIVERABLE_FIELDS = ("notes",)  # free text, rewritten (with a hash) by every build
STATUS_RANK = {"ko": 1, "ok_restricted": 2, "ok": 3}
CUT = 110
MAX_LEAVES = 12


class SetError(Exception):
    """A set of verdicts cannot be read: the message says why."""


class VerdictSet(object):
    def __init__(self, label):
        self.label = label
        self.verdicts = {}      # key -> record
        self.deliverables = {}  # key -> record


# ---- loading -----------------------------------------------------------------------------------

def _json(text, where):
    try:
        return json.loads(text)
    except ValueError as e:
        raise SetError("%s is not valid JSON: %s" % (where, e))


def _folder_set(path, label):
    path = Path(path)
    verdict_dir = None
    for candidate in (path / "validation" / "verdicts", path / "verdicts", path):
        if candidate.is_dir() and any(candidate.glob("*.json")):
            verdict_dir = candidate
            break
    if verdict_dir is None:
        raise SetError("no verdict (*.json) found in %s, %s or %s" % (
            path / "validation" / "verdicts", path / "verdicts", path))
    result = VerdictSet(label)
    for item in sorted(verdict_dir.glob("*.json")):
        result.verdicts[item.stem] = _json(item.read_text(encoding="utf-8"), str(item))
    deliverable_dir = verdict_dir.parent / "deliverables"
    if deliverable_dir.is_dir():
        for item in sorted(deliverable_dir.glob("*.json")):
            result.deliverables[item.stem] = _json(item.read_text(encoding="utf-8"), str(item))
    return result


def _git(repo, args):
    try:
        res = subprocess.run(["git", "-C", str(repo)] + list(args), stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    except OSError as e:
        raise SetError("cannot run git: %s" % e)
    if res.returncode != 0:
        raise SetError("git %s failed in %s: %s" % (" ".join(args), repo,
                                                    res.stderr.decode("utf-8", "replace").strip()))
    return res.stdout


def _git_set(rev, repo, prefix, label):
    if not Path(repo).is_dir():
        raise SetError("repository %s does not exist (is the docs submodule checked out? %s)"
                       % (repo, common.docs_missing_message("nothing to compare")))
    prefix = prefix.strip("/")
    names = _git(repo, ["ls-tree", "-r", "--name-only", rev, "--", prefix + "/verdicts",
                        prefix + "/deliverables"]).decode("utf-8").split("\n")
    result = VerdictSet(label)
    for name in names:
        name = name.strip()
        if not name.endswith(".json"):
            continue
        folder, _, leaf = name.rpartition("/")
        record = _json(_git(repo, ["show", "%s:%s" % (rev, name)]).decode("utf-8"),
                       "%s:%s" % (rev, name))
        if folder == prefix + "/verdicts":
            result.verdicts[leaf[:-5]] = record
        elif folder == prefix + "/deliverables":
            result.deliverables[leaf[:-5]] = record
    if not result.verdicts:
        raise SetError("revision %s of %s has no verdict under %s/verdicts" % (rev, repo, prefix))
    return result


def load_set(spec, repo=None, prefix="validation"):
    """The VerdictSet named by `spec` (see the module docstring)."""
    repo = repo or common.DOCS
    if spec.startswith("dir:"):
        return _folder_set(spec[4:], spec)
    if spec.startswith("git:"):
        return _git_set(spec[4:], repo, prefix, spec)
    if os.path.isdir(spec):
        return _folder_set(spec, spec)
    return _git_set(spec, repo, prefix, spec)


# ---- comparison --------------------------------------------------------------------------------

def _cut(value, full):
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, sort_keys=True)
    text = " ".join(text.split())
    if not full and len(text) > CUT:
        text = text[:CUT - 3] + "..."
    return text


def _item_text(item):
    return item if isinstance(item, str) else json.dumps(item, ensure_ascii=False, sort_keys=True)


def _leaves(value, prefix=""):
    """{dotted path: scalar or list} of a dictionary field."""
    if isinstance(value, dict):
        out = {}
        for key in value:
            out.update(_leaves(value[key], "%s.%s" % (prefix, key) if prefix else str(key)))
        return out
    return {prefix: value}


def _evidence_lines(old, new, full):
    lines = []
    old_by = dict((e.get("path"), e) for e in old if isinstance(e, dict))
    new_by = dict((e.get("path"), e) for e in new if isinstance(e, dict))
    for path in sorted(set(new_by) - set(old_by)):
        lines.append("+ evidence %s (%s)" % (path, new_by[path].get("kind", "?")))
    for path in sorted(set(old_by) - set(new_by)):
        lines.append("- evidence %s (%s)" % (path, old_by[path].get("kind", "?")))
    for path in sorted(set(old_by) & set(new_by)):
        a, b = old_by[path], new_by[path]
        if a.get("sha256") != b.get("sha256"):
            lines.append("~ evidence %s: sha256 %s -> %s" % (
                path, str(a.get("sha256"))[:12], str(b.get("sha256"))[:12]))
        if a.get("kind") != b.get("kind"):
            lines.append("~ evidence %s: kind %s -> %s" % (path, a.get("kind"), b.get("kind")))
    return lines


def field_changes(field, old, new, full=False):
    """Lines (without indentation) describing how one field changed; [] when it did not."""
    if old == new:
        return []
    if old is None:
        return ["%s: (absent) -> %s" % (field, _cut(new, full))]
    if new is None:
        return ["%s: %s -> (absent)" % (field, _cut(old, full))]
    if isinstance(old, list) and isinstance(new, list):
        if field == "evidence":
            body = _evidence_lines(old, new, full)
        else:
            old_items = [_item_text(i) for i in old]
            new_items = [_item_text(i) for i in new]
            body = ["+ " + _cut(i, full) for i in new_items if i not in old_items]
            body += ["- " + _cut(i, full) for i in old_items if i not in new_items]
            if not body:
                body = ["(same items, other order)"]
        return ["%s: %d -> %d item(s)" % (field, len(old), len(new))] + ["  " + b for b in body]
    if isinstance(old, dict) and isinstance(new, dict):
        a, b = _leaves(old), _leaves(new)
        body = []
        for key in sorted(set(a) | set(b)):
            if key not in a:
                body.append("+ %s = %s" % (key, _cut(b[key], full)))
            elif key not in b:
                body.append("- %s = %s" % (key, _cut(a[key], full)))
            elif a[key] != b[key]:
                body.append("~ %s: %s -> %s" % (key, _cut(a[key], full), _cut(b[key], full)))
        if not full and len(body) > MAX_LEAVES:
            body = body[:MAX_LEAVES] + ["... %d more" % (len(body) - MAX_LEAVES)]
        return ["%s: %d value(s) differ" % (field, len(body))] + ["  " + b for b in body]
    if isinstance(old, str) and isinstance(new, str) and (len(old) > CUT or len(new) > CUT) \
            and not full:
        return ["%s: text changed (%d -> %d characters; --full shows both)" % (
            field, len(old), len(new))]
    return ["%s: %s -> %s" % (field, _cut(old, full), _cut(new, full))]


def _ordered_fields(old, new, skip=()):
    names = [f for f in VERDICT_FIELDS_FIRST if f in old or f in new]
    names += sorted(f for f in (set(old) | set(new)) if f not in names)
    return [n for n in names if n not in skip]


def status_hint(old, new):
    if old == "not_tested":
        return "(first result)"
    if new == "not_tested":
        return "(result lost)"
    if old in STATUS_RANK and new in STATUS_RANK:
        return "(better)" if STATUS_RANK[new] > STATUS_RANK[old] else "(worse)"
    return ""


class Comparison(object):
    def __init__(self, old, new):
        self.old, self.new = old, new
        self.lines = []
        self.compared = 0
        self.changed = 0
        self.status_changes = []   # (key, old status, new status)
        self.added = []            # keys of verdicts only in NEW
        self.removed = []          # keys of verdicts only in OLD
        self.deliverable_changed = []
        self.deliverable_added = []
        self.deliverable_removed = []

    def summary(self):
        return ("summary: %d verdict(s) in %s, %d in %s; %d compared, %d with differences, "
                "%d status change(s), %d added, %d removed; %d deliverable(s) with differences, "
                "%d added, %d removed"
                % (len(self.old.verdicts), self.old.label, len(self.new.verdicts), self.new.label,
                   self.compared, self.changed, len(self.status_changes), len(self.added),
                   len(self.removed), len(self.deliverable_changed),
                   len(self.deliverable_added), len(self.deliverable_removed)))


def _title(key, record):
    if isinstance(record, dict) and record.get("platform_id") and record.get("ui_variant"):
        title = "%s / %s" % (record["platform_id"], record["ui_variant"])
        if record.get("web_path"):
            title += " / " + record["web_path"]
        return title
    return key


def compare_sets(old, new, full=False):
    """Compare two VerdictSets; returns a Comparison whose .lines is the report."""
    result = Comparison(old, new)
    keys = sorted(set(old.verdicts) | set(old.deliverables) | set(new.verdicts) | set(new.deliverables))
    for key in keys:
        block = []
        a, b = old.verdicts.get(key), new.verdicts.get(key)
        title = _title(key, b if b is not None else a if a is not None
                       else new.deliverables.get(key) or old.deliverables.get(key))
        if a is not None and b is None:
            result.removed.append(key)
            block.append("verdict REMOVED (status was %s, decision %s)" % (a.get("status"), a.get("decision")))
        elif a is None and b is not None:
            result.added.append(key)
            block.append("verdict ADDED (status %s, decision %s)" % (b.get("status"), b.get("decision")))
        elif a is not None and b is not None:
            result.compared += 1
            if a.get("status") != b.get("status"):
                result.status_changes.append((key, a.get("status"), b.get("status")))
                block.append("STATUS CHANGE %s -> %s %s" % (
                    a.get("status"), b.get("status"), status_hint(a.get("status"), b.get("status"))))
            for field in _ordered_fields(a, b, skip=("status",)):
                block += field_changes(field, a.get(field), b.get(field), full)
            if block:
                result.changed += 1
        da, db = old.deliverables.get(key), new.deliverables.get(key)
        if da is not None and db is None:
            result.deliverable_removed.append(key)
            block.append("deliverable REMOVED")
        elif da is None and db is not None:
            result.deliverable_added.append(key)
            block.append("deliverable ADDED (fallbacks %s, disabled_features %s)" % (
                json.dumps(db.get("fallbacks", [])), json.dumps(db.get("disabled_features", []))))
        elif da is not None and db is not None:
            lines = []
            for field in _ordered_fields(da, db, skip=SKIP_DELIVERABLE_FIELDS):
                lines += field_changes(field, da.get(field), db.get(field), full)
            if lines:
                result.deliverable_changed.append(key)
                block.append("deliverable:")
                block += ["  " + l for l in lines]
        if block:
            result.lines.append("== %s  (%s)" % (title, key))
            result.lines += ["   " + l for l in block]
    result.lines.append(result.summary())
    return result


# ---- self-test ---------------------------------------------------------------------------------

def _verdict(platform, variant, status, **extra):
    record = {"platform_id": platform, "ui_variant": variant, "status": status, "decision": "keep",
              "restrictions": ["one machine"], "disabled_features": ["dark-mode (no setting)"],
              "evidence": [{"kind": "probe", "path": "validation/evidence/x/p.json", "sha256": "a" * 64}],
              "reproduction": "validation/procedures/p.md"}
    record.update(extra)
    return record


def _write_set(root, verdicts, deliverables):
    for sub, records in (("verdicts", verdicts), ("deliverables", deliverables)):
        folder = Path(root) / "validation" / sub
        folder.mkdir(parents=True, exist_ok=True)
        for key in records:
            (folder / (key + ".json")).write_text(json.dumps(records[key], indent=2) + "\n",
                                                  encoding="utf-8")


def self_test():
    failures = []

    def check(name, condition, detail=""):
        print("%s %s%s" % ("ok  " if condition else "FAIL", name, "" if condition else ": " + detail))
        if not condition:
            failures.append(name)

    base_v = {"win-1-legacy": _verdict("win-1", "legacy", "ok"),
              "lin-1-modern": _verdict("lin-1", "modern", "ok_restricted",
                                       measurements={"display_ms": {"max": 3}}),
              "lin-1-legacy": _verdict("lin-1", "legacy", "ko", cause="c", attempts=["a", "b"]),
              "old-1-legacy": _verdict("old-1", "legacy", "ok")}
    base_d = {"win-1-legacy": {"fallbacks": ["f1"], "disabled_features": [], "compiler": "gcc-13",
                               "artifact": {"name": "x.exe", "size": 10}, "notes": "sha aaa"}}
    new_v = json.loads(json.dumps(base_v))
    new_d = json.loads(json.dumps(base_d))
    new_v["win-1-legacy"]["status"] = "ok_restricted"                      # worse
    new_v["lin-1-legacy"]["status"] = "ok"                                 # better
    new_v["lin-1-modern"]["disabled_features"].append("glyph-substitution")
    new_v["lin-1-modern"]["evidence"][0]["sha256"] = "b" * 64
    new_v["lin-1-modern"]["measurements"] = {"display_ms": {"max": 5}, "rss": {"max": 9}}
    new_v["lin-1-modern"]["notes"] = "n" * 300
    del new_v["old-1-legacy"]
    new_v["new-1-modern"] = _verdict("new-1", "modern", "not_tested", reason="no image")
    new_d["win-1-legacy"]["fallbacks"].append("dark-mode")
    new_d["win-1-legacy"]["notes"] = "sha bbb"                              # ignored
    new_d["new-1-modern"] = {"fallbacks": [], "disabled_features": []}

    tmp = tempfile.mkdtemp(prefix="loomora-diff-")
    try:
        old_dir, new_dir, same_dir = (os.path.join(tmp, n) for n in ("old", "new", "same"))
        _write_set(old_dir, base_v, base_d)
        _write_set(new_dir, new_v, new_d)
        _write_set(same_dir, base_v, base_d)

        same = compare_sets(load_set(old_dir), load_set(same_dir))
        check("identical sets: no difference", same.changed == 0 and not same.status_changes
              and len(same.lines) == 1 and not same.added and not same.removed
              and not same.deliverable_changed, "\n".join(same.lines))

        cmp_ = compare_sets(load_set(old_dir), load_set(new_dir))
        text = "\n".join(cmp_.lines)
        check("status changes found", sorted(cmp_.status_changes) == [
            ("lin-1-legacy", "ko", "ok"), ("win-1-legacy", "ok", "ok_restricted")], text)
        check("status change flagged with its direction",
              "STATUS CHANGE ok -> ok_restricted (worse)" in text
              and "STATUS CHANGE ko -> ok (better)" in text, text)
        check("verdict added and removed", cmp_.added == ["new-1-modern"]
              and cmp_.removed == ["old-1-legacy"], text)
        check("disabled feature added", "+ glyph-substitution" in text, text)
        check("evidence sha256 change", "evidence validation/evidence/x/p.json: sha256 aaaaaaaaaaaa -> bbbbbbbbbbbb" in text, text)
        check("dictionary field leaves", "~ display_ms.max: 3 -> 5" in text
              and "+ rss.max = 9" in text, text)
        check("long text cut", "notes: (absent) -> " + "n" * 107 + "..." in text, text)
        check("deliverable fallback change", "+ dark-mode" in text and cmp_.deliverable_changed == ["win-1-legacy"], text)
        check("deliverable notes ignored", "sha bbb" not in text, text)
        check("platform and variant in the heading", "== lin-1 / modern  (lin-1-modern)" in text, text)
        check("summary line", text.endswith("2 status change(s), 1 added, 1 removed; 1 deliverable(s) "
                                            "with differences, 1 added, 0 removed"), text)
        full = compare_sets(load_set(old_dir), load_set(new_dir), full=True)
        check("--full keeps the whole text", "n" * 300 in "\n".join(full.lines))

        # A verdicts folder given directly, and the web path in the title.
        flat = os.path.join(tmp, "flat")
        os.makedirs(flat)
        (Path(flat) / "web-1-modern-js.json").write_text(
            json.dumps(_verdict("web-1", "modern", "ok", web_path="js")), encoding="utf-8")
        loaded = load_set(flat)
        check("folder of verdicts read directly", list(loaded.verdicts) == ["web-1-modern-js"])
        check("web path in the title", compare_sets(loaded, VerdictSet("empty")).lines[0].startswith(
            "== web-1 / modern / js"), "\n".join(compare_sets(loaded, VerdictSet("empty")).lines))

        try:
            load_set(os.path.join(tmp, "nothing"), repo=os.path.join(tmp, "norepo"))
            check("unknown set refused", False, "no error")
        except SetError:
            check("unknown set refused", True)

        # Two git revisions of a repository.
        if shutil.which("git") is None:
            print("skip git revisions: git not found")
        else:
            repo = os.path.join(tmp, "repo")
            os.makedirs(repo)
            git = ["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                   "-c", "commit.gpgsign=false"]
            ok = subprocess.call(git + ["init", "-q"]) == 0
            _write_set(repo, base_v, base_d)
            ok = ok and subprocess.call(git + ["add", "-A"]) == 0
            ok = ok and subprocess.call(git + ["commit", "-q", "-m", "one"]) == 0
            _write_set(repo, new_v, new_d)
            os.remove(os.path.join(repo, "validation", "verdicts", "old-1-legacy.json"))
            ok = ok and subprocess.call(git + ["add", "-A"]) == 0
            ok = ok and subprocess.call(git + ["commit", "-q", "-m", "two"]) == 0
            if not ok:
                check("git revisions: temporary repository", False, "git could not commit")
            else:
                by_rev = compare_sets(load_set("HEAD~1", repo), load_set("HEAD", repo))
                check("git revisions give the same report as the folders",
                      by_rev.lines[:-1] == cmp_.lines[:-1] and by_rev.status_changes == cmp_.status_changes,
                      "\n".join(by_rev.lines))
                try:
                    load_set("no-such-revision", repo)
                    check("unknown revision refused", False, "no error")
                except SetError:
                    check("unknown revision refused", True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


# ---- main --------------------------------------------------------------------------------------

def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(
        description="Print the differences between two sets of validation verdicts.",
        epilog="OLD and NEW: a folder or a git revision, see the docstring of this file.")
    ap.add_argument("old", nargs="?", help="baseline set (folder or git revision)")
    ap.add_argument("new", nargs="?", help="set to compare (default: the verdicts of the working tree)")
    ap.add_argument("--repo", help="git repository for revisions (default docs/implementation)")
    ap.add_argument("--prefix", default="validation",
                    help="folder of verdicts/ and deliverables/ in that repository (default validation)")
    ap.add_argument("--full", action="store_true", help="do not cut long texts")
    ap.add_argument("--strict", action="store_true",
                    help="exit with status 1 when a status changed or a verdict disappeared")
    ap.add_argument("--self-test", action="store_true", help="run the built-in comparison tests")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.old:
        ap.error("OLD is required (or --self-test)")
    try:
        old = load_set(args.old, args.repo, args.prefix)
        if args.new:
            new = load_set(args.new, args.repo, args.prefix)
        else:
            new = _folder_set(common.DOCS, "working tree")
    except SetError as e:
        print("diff_verdicts: %s" % e, file=sys.stderr)
        return 2
    result = compare_sets(old, new, args.full)
    print("diff_verdicts: OLD = %s, NEW = %s" % (old.label, new.label))
    for line in result.lines:
        print(line)
    if args.strict and (result.status_changes or result.removed):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
