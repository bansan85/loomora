#!/usr/bin/env python
"""Validate the data records of docs/implementation/validation/ (git submodule ../loomora-docs)
against specs/001-loomora-cross-platform/data-model.md.

Usage:
  python tools/validate_records.py [--require-complete] [--validation-dir DIR] [--root DIR]
  python tools/validate_records.py --self-test

Files checked (everything else is ignored or reported as INFO/WARN):
  platforms/<id>.json                    Platform
  verdicts/<platform-id>-<variant>.json  Verdict   (web: <platform-id>-<variant>[-<path>].json,
                                                    one verdict per path, see "Web: two paths")
  deliverables/<platform-id>-<variant>.json   Deliverable
  fallbacks.json (list), sources.json (list), spikes.json (list), hypotheses.json (list, if present)

Exit status: 0 = no error, 1 = at least one ERROR, 2 = the docs submodule is not checked out
(docs/implementation/validation is missing: nothing is validated and a message says how to
fetch it; the product build never needs the submodule).  --self-test needs no submodule.

Output: one line per problem, "<LEVEL> <file>: <message>", LEVEL = ERROR (exit status 1),
WARN or INFO (status 0).

Rules enforced as ERROR, each quoted from data-model.md:
  * verdict.status in {ok, ok_restricted, ko, not_tested}
  * web_path: "mandatory for the web family, forbidden otherwise" (values wasm | js)
  * restrictions[]: "mandatory if ok_restricted"
  * cause, attempts[]: "mandatory if ko"; reason: "if not_tested"
  * evidence[] (path + sha256): "mandatory unless not_tested"; the file must exist and its
    SHA-256 must match. Evidence paths are relative to the docs root (--root, default docs/implementation), written
    with "/", for example validation/evidence/<change>/<id>/probe.json; the trial drivers
    (tools/trials/...) are tooling of the main repository: their path is relative to its root. An evidence item of kind "probe"
    (optional field) is also checked as a ProbeResult line (tools/check_probe.py).
  * Verdict -> Platform "(in_scope = true only)": a verdict must reference an existing platform
    whose in_scope is true; a platform with in_scope = true is expected to have a verdict
    (see below).
  * Deliverable -> Fallback: every id of fallbacks[] exists in fallbacks.json; Fallback
    platform_ids[] and Hypothesis platform_ids[] reference existing platforms; ids are unique;
    Spike ids are S1-S9 except S4 (removed on 2026-11-14), status in {open, confirmed, refuted}, "result" non-empty unless open,
    "A refuted spike must name the chosen fallback" (field "fallback" or "adr"); an "adr" path
    must exist; "sources" lists (any record) reference ids of sources.json.

"Incomplete" findings, WARN by default and ERROR with --require-complete (SC-004: "no platform
is left without a verdict"), because trials are not done yet:
  * an in_scope platform without a verdict for a variant (legacy, modern);
  * a deliverable whose platform has no record in platforms/ (build.py writes the deliverable
    before T059/T087 write the platform records);
  * a verdict with no "reproduction" path (SC-004 wants a procedure) unless not_tested.

A variant that was not attempted is justified by a verdict of status "not_tested" with a "reason": it
needs no evidence and no reproduction, and it counts as covered (no "no <variant> verdict" finding);
tools/report.py shows it as a justified absent variant (SC-010).

Other WARN: ok / ok_restricted verdict without a deliverable of that platform and variant; ko
verdict with fewer than two attempts (SC-003); in-scope floor inconsistencies; spikes S1-S9 (S4
excepted) not all present. INFO: optional fields that data-model.md does not name (for example the `verified`
and `note` of sources.json) are allowed and listed, never silently ignored.
"""
import argparse
import datetime
import json
import re
import shutil
import sys
import tempfile
from collections import Counter, OrderedDict
from pathlib import Path

import common

EXIT_NO_DOCS = 2  # the docs submodule (docs/implementation) is not checked out
VARIANTS = ("legacy", "modern")
STATUSES = ("ok", "ok_restricted", "ko", "not_tested")
DECISIONS = ("keep", "restrict", "drop")
FAMILIES = ("windows", "macos", "linux", "android", "ios", "web")
ARCHS = ("x86", "x86_64", "arm", "arm64", "wasm", "js")
ENVIRONMENTS = ("physical", "vm", "emulator", "browser-archive")
OUTPUTS = ("native", "wasm", "js")
WEB_PATHS = ("wasm", "js")
SPIKE_STATUSES = ("open", "confirmed", "refuted")
# S4 (manylinux2010 build, glibc 2.12) was removed on 2026-11-14: Linux binaries are built on the
# machine that runs them (docs/implementation/adr/0007).
SPIKE_IDS = tuple("S%d" % i for i in range(1, 10) if i != 4)
HYPOTHESIS_IDS = tuple("V%d" % i for i in range(1, 21))
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")  # same pattern as probe-result.schema.json
SHA_RE = re.compile(r"^[0-9a-f]{64}$")

PLATFORM_FIELDS = ("id", "family", "os_name", "os_version", "arch", "release_date", "age_years",
                   "is_floor", "in_scope", "environment", "environment_detail", "eol")
VERDICT_FIELDS = ("platform_id", "ui_variant", "web_path", "status", "restrictions",
                  "disabled_features", "cause", "attempts", "reason", "evidence", "reproduction",
                  "decision", "sources")
DELIVERABLE_REQUIRED = ("platform_id", "ui_variant", "cpp_std", "compiler", "target", "output",
                        "ui_language", "ui_toolkit", "ui_compiler", "artifact",
                        "disabled_features", "fallbacks")
# shared_loc, glue_loc, ui_loc and sharing_ratio are measured later (measure_sharing.py), so
# they are optional here but typed when present.
DELIVERABLE_FIELDS = DELIVERABLE_REQUIRED + ("shared_loc", "glue_loc", "ui_loc", "sharing_ratio",
                                             "notes", "sources")
FALLBACK_FIELDS = ("id", "description", "macro", "min_compiler", "platform_ids", "reason",
                   "sources")
SOURCE_FIELDS = ("id", "url", "title", "accessed", "claim")
SPIKE_FIELDS = ("id", "question", "status", "result", "adr", "fallback", "sources")
HYPOTHESIS_FIELDS = ("id", "title", "criterion", "fallback", "covered_by_probe", "platform_ids",
                     "status", "sources", "old_targets", "records", "note")
EVIDENCE_FIELDS = ("path", "sha256", "kind", "description")


class Report(object):
    def __init__(self, root, require_complete):
        self.root = root
        self.require_complete = require_complete
        self.items = []  # (level, path, message)

    def rel(self, path):
        try:
            return Path(path).resolve().relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return str(path)

    def add(self, level, path, message):
        self.items.append((level, self.rel(path), message))

    def error(self, path, message):
        self.add("ERROR", path, message)

    def warn(self, path, message):
        self.add("WARN", path, message)

    def info(self, path, message):
        self.add("INFO", path, message)

    def incomplete(self, path, message):
        """Missing data that trials will provide: WARN, or ERROR with --require-complete."""
        self.add("ERROR" if self.require_complete else "WARN", path, message)

    def count(self, level):
        return sum(1 for item in self.items if item[0] == level)


# ---- small type helpers ----------------------------------------------------------------------

def is_text(value):
    return isinstance(value, str) and value.strip() != ""


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def is_date(value):
    if not isinstance(value, str) or not re.match(r"^\d{4}-\d{2}-\d{2}$", value):
        return False
    try:
        datetime.datetime.strptime(value, "%Y-%m-%d")
    except ValueError:
        return False
    return True


def is_text_list(value):
    return isinstance(value, list) and all(is_text(v) for v in value)


def load(path, report):
    try:
        with open(str(path), encoding="utf-8") as f:
            return json.load(f, object_pairs_hook=OrderedDict)
    except ValueError as e:
        report.error(path, "not valid JSON: %s" % e)
    except (OSError, UnicodeDecodeError) as e:
        report.error(path, "cannot be read: %s" % e)
    return None


def need_object(data, path, report):
    if not isinstance(data, dict):
        report.error(path, "must hold one JSON object")
        return False
    return True


def need_list(data, path, report):
    if not isinstance(data, list):
        report.error(path, "must hold a JSON array of records")
        return False
    return True


def require_text(obj, key, path, report, label):
    if not is_text(obj.get(key)):
        report.error(path, "%s: '%s' is mandatory and must be a non-empty string" % (label, key))
        return False
    return True


def require_enum(obj, key, values, path, report, label):
    if obj.get(key) not in values or not isinstance(obj.get(key), str):
        report.error(path, "%s: '%s' must be one of %s (got %r)"
                     % (label, key, ", ".join(values), obj.get(key)))
        return False
    return True


def require_bool(obj, key, path, report, label):
    if not isinstance(obj.get(key), bool):
        report.error(path, "%s: '%s' must be a boolean (got %r)" % (label, key, obj.get(key)))


def note_unknown(obj, known, counter):
    for key in obj:
        if key not in known:
            counter[key] += 1


def report_unknown(counter, path, report, what):
    if counter:
        fields = ", ".join("%s (%d)" % (k, counter[k]) for k in sorted(counter))
        report.info(path, "%s: optional field(s) not named in data-model.md, accepted: %s"
                    % (what, fields))


def check_source_refs(obj, known_sources, path, report, label):
    refs = obj.get("sources")
    if refs is None:
        return
    if not is_text_list(refs):
        report.error(path, "%s: 'sources' must be a list of source ids" % label)
        return
    for ref in refs:
        if ref not in known_sources:
            report.error(path, "%s: source %r is not in validation/sources.json" % (label, ref))


def json_files(directory, report):
    """The *.json files of a directory (sorted); other regular files are reported."""
    if not directory.is_dir():
        return []
    files = []
    for entry in sorted(directory.iterdir()):
        if entry.is_dir() or entry.name.startswith("."):
            continue
        if entry.suffix == ".json":
            files.append(entry)
        else:
            report.warn(entry, "unexpected file (only *.json records live here)")
    return files


# ---- Source, Fallback, Spike, Hypothesis -------------------------------------------------------

def check_sources(path, report):
    ids = set()
    data = load(path, report)
    if data is None or not need_list(data, path, report):
        return ids
    unknown = Counter()
    for i, rec in enumerate(data):
        label = "record %d" % i
        if not need_object(rec, path, report):
            continue
        label = "source %r" % rec.get("id", i)
        note_unknown(rec, SOURCE_FIELDS, unknown)
        if require_text(rec, "id", path, report, label):
            if rec["id"] in ids:
                report.error(path, "%s: duplicate id" % label)
            ids.add(rec["id"])
        for key in ("title", "claim"):
            require_text(rec, key, path, report, label)
        if not (is_text(rec.get("url")) and re.match(r"^https?://\S+$", rec["url"])):
            report.error(path, "%s: 'url' must be an http(s) URL" % label)
        if not is_date(rec.get("accessed")):
            report.error(path, "%s: 'accessed' must be a date YYYY-MM-DD" % label)
    report_unknown(unknown, path, report, "sources")
    return ids


def check_fallbacks(path, source_ids, report):
    ids = OrderedDict()
    data = load(path, report)
    if data is None or not need_list(data, path, report):
        return ids
    unknown = Counter()
    for i, rec in enumerate(data):
        if not need_object(rec, path, report):
            continue
        label = "fallback %r" % rec.get("id", i)
        note_unknown(rec, FALLBACK_FIELDS, unknown)
        if require_text(rec, "id", path, report, label):
            if not ID_RE.match(rec["id"]):
                report.error(path, "%s: id must match %s" % (label, ID_RE.pattern))
            if rec["id"] in ids:
                report.error(path, "%s: duplicate id" % label)
            ids[rec["id"]] = rec
        for key in ("description", "min_compiler", "reason"):
            require_text(rec, key, path, report, label)
        if not (is_text(rec.get("macro")) and re.match(r"^[A-Z][A-Z0-9_]*$", rec["macro"])):
            report.error(path, "%s: 'macro' must be a feature macro name such as "
                         "LOOMORA_HAS_CXX17_NODISCARD" % label)
        if not is_text_list(rec.get("platform_ids")):
            report.error(path, "%s: 'platform_ids' must be a list of platform ids (may be empty)"
                         % label)
        check_source_refs(rec, source_ids, path, report, label)
    report_unknown(unknown, path, report, "fallbacks")
    return ids


def check_spikes(path, root, source_ids, report):
    data = load(path, report)
    if data is None or not need_list(data, path, report):
        return
    unknown = Counter()
    seen = set()
    for i, rec in enumerate(data):
        if not need_object(rec, path, report):
            continue
        label = "spike %r" % rec.get("id", i)
        note_unknown(rec, SPIKE_FIELDS, unknown)
        if require_enum(rec, "id", SPIKE_IDS, path, report, label):
            if rec["id"] in seen:
                report.error(path, "%s: duplicate id" % label)
            seen.add(rec["id"])
        require_text(rec, "question", path, report, label)
        status = rec.get("status")
        if require_enum(rec, "status", SPIKE_STATUSES, path, report, label):
            if status != "open" and not is_text(rec.get("result")):
                report.error(path, "%s: 'result' must be non-empty unless the status is open "
                             "(status %s)" % (label, status))
        adr = rec.get("adr", "")
        if not isinstance(adr, str):
            report.error(path, "%s: 'adr' must be a path string (empty if none)" % label)
        elif adr.strip() and not (root / adr).is_file():
            report.error(path, "%s: adr %r does not exist" % (label, adr))
        fallback = rec.get("fallback", "")
        if not isinstance(fallback, str):
            report.error(path, "%s: 'fallback' must be a string" % label)
        if status == "refuted" and not (is_text(adr) or is_text(fallback)):
            report.error(path, "%s: a refuted spike must name the chosen fallback "
                         "(field 'fallback' or 'adr')" % label)
        check_source_refs(rec, source_ids, path, report, label)
    missing = [s for s in SPIKE_IDS if s not in seen]
    if missing:
        report.warn(path, "spikes not recorded: %s (research.md defines S1-S9, S4 removed)" % ", ".join(missing))
    report_unknown(unknown, path, report, "spikes")


def check_hypotheses(path, platform_ids, source_ids, report, root=None):
    if not path.is_file():
        report.info(path, "not present yet (validation/hypotheses.json is created later)")
        return
    data = load(path, report)
    if data is None or not need_list(data, path, report):
        return
    unknown = Counter()
    seen = set()
    for i, rec in enumerate(data):
        if not need_object(rec, path, report):
            continue
        label = "hypothesis %r" % rec.get("id", i)
        note_unknown(rec, HYPOTHESIS_FIELDS, unknown)
        if require_enum(rec, "id", HYPOTHESIS_IDS, path, report, label):
            if rec["id"] in seen:
                report.error(path, "%s: duplicate id" % label)
            seen.add(rec["id"])
        for key in ("title", "criterion"):
            require_text(rec, key, path, report, label)
        if not isinstance(rec.get("fallback"), str):
            report.error(path, "%s: 'fallback' must be a string" % label)
        require_bool(rec, "covered_by_probe", path, report, label)
        require_enum(rec, "status", STATUSES + ("not_covered",), path, report, label)
        pids = rec.get("platform_ids")
        if not is_text_list(pids):
            report.error(path, "%s: 'platform_ids' must be a list of platform ids" % label)
        else:
            for pid in pids:
                if pid not in platform_ids:
                    report.error(path, "%s: platform %r has no record in platforms/" % (label, pid))
        check_source_refs(rec, source_ids, path, report, label)
        # "records" (optional): the records the status rests on, paths relative to the docs root.
        records = rec.get("records")
        if records is not None:
            if not is_text_list(records):
                report.error(path, "%s: 'records' must be a list of paths" % label)
            elif root is not None:
                for rel in records:
                    if not (Path(root) / rel).exists():
                        report.error(path, "%s: record %r does not exist (path relative to the "
                                     "docs root)" % (label, rel))
        if rec.get("status") not in (None, "not_covered") and rec.get("covered_by_probe") is False:
            report.error(path, "%s: status %r needs covered_by_probe = true (only 'not_covered' "
                         "is allowed otherwise)" % (label, rec.get("status")))
    report_unknown(unknown, path, report, "hypotheses")


# ---- Platform --------------------------------------------------------------------------------

def check_platforms(directory, report):
    platforms = OrderedDict()
    unknown = Counter()
    for path in json_files(directory, report):
        rec = load(path, report)
        if rec is None or not need_object(rec, path, report):
            continue
        label = "platform %r" % rec.get("id", path.stem)
        note_unknown(rec, PLATFORM_FIELDS, unknown)
        if require_text(rec, "id", path, report, label):
            if not ID_RE.match(rec["id"]):
                report.error(path, "%s: id must match %s" % (label, ID_RE.pattern))
            if rec["id"] != path.stem:
                report.error(path, "%s: file name must be <id>.json" % label)
            if rec["id"] in platforms:
                report.error(path, "%s: duplicate id (unique)" % label)
            platforms[rec["id"]] = rec
        require_enum(rec, "family", FAMILIES, path, report, label)
        require_enum(rec, "arch", ARCHS, path, report, label)
        require_enum(rec, "environment", ENVIRONMENTS, path, report, label)
        for key in ("os_name", "os_version", "environment_detail"):
            require_text(rec, key, path, report, label)
        if not is_date(rec.get("release_date")):
            report.error(path, "%s: 'release_date' must be a date YYYY-MM-DD" % label)
        if not is_number(rec.get("age_years")) or rec["age_years"] < 0:
            report.error(path, "%s: 'age_years' must be a number >= 0" % label)
        for key in ("is_floor", "in_scope", "eol"):
            require_bool(rec, key, path, report, label)
        if rec.get("is_floor") is True and rec.get("in_scope") is False:
            report.warn(path, "%s: is_floor is true but in_scope is false (in_scope = false means "
                        "older than the floor)" % label)
    report_unknown(unknown, directory, report, "platforms")
    return platforms


# ---- Deliverable -----------------------------------------------------------------------------

def check_deliverables(directory, platforms, fallback_ids, source_ids, report):
    found = OrderedDict()  # (platform_id, variant) -> record
    unknown = Counter()
    for path in json_files(directory, report):
        rec = load(path, report)
        if rec is None or not need_object(rec, path, report):
            continue
        label = "deliverable %s" % path.stem
        note_unknown(rec, DELIVERABLE_FIELDS, unknown)
        ok_id = require_text(rec, "platform_id", path, report, label)
        ok_var = require_enum(rec, "ui_variant", VARIANTS, path, report, label)
        if ok_id and ok_var:
            if path.stem != "%s-%s" % (rec["platform_id"], rec["ui_variant"]):
                report.error(path, "%s: file name must be <platform_id>-<ui_variant>.json" % label)
            if (rec["platform_id"], rec["ui_variant"]) in found:
                report.error(path, "%s: duplicate platform_id + ui_variant" % label)
            found[(rec["platform_id"], rec["ui_variant"])] = rec
            if rec["platform_id"] not in platforms:
                report.incomplete(path, "%s: platform %r has no record in platforms/ (T059, T087)"
                                  % (label, rec["platform_id"]))
        if rec.get("cpp_std") != "c++11":
            report.error(path, "%s: 'cpp_std' must be \"c++11\" (always, for the backend)" % label)
        for key in ("compiler", "target", "ui_language", "ui_toolkit", "ui_compiler"):
            require_text(rec, key, path, report, label)
        require_enum(rec, "output", OUTPUTS, path, report, label)
        art = rec.get("artifact")
        if not (isinstance(art, dict) and is_text(art.get("name")) and is_int(art.get("size"))
                and art["size"] >= 0):
            report.error(path, "%s: 'artifact' must be {name, size in bytes >= 0}" % label)
        for key in ("shared_loc", "glue_loc", "ui_loc"):
            if key in rec and not (is_int(rec[key]) and rec[key] >= 0):
                report.error(path, "%s: '%s' must be an integer >= 0" % (label, key))
        if "sharing_ratio" in rec and not (is_number(rec["sharing_ratio"])
                                           and 0 <= rec["sharing_ratio"] <= 1):
            report.error(path, "%s: 'sharing_ratio' must be a number between 0 and 1" % label)
        if not is_text_list(rec.get("disabled_features")):
            report.error(path, "%s: 'disabled_features' must be a list of strings (may be empty)"
                         % label)
        fbs = rec.get("fallbacks")
        if not is_text_list(fbs):
            report.error(path, "%s: 'fallbacks' must be a list of Fallback ids (may be empty)"
                         % label)
        else:
            for fid in fbs:
                if fid not in fallback_ids:
                    report.error(path, "%s: fallback %r is not in validation/fallbacks.json"
                                 % (label, fid))
        check_source_refs(rec, source_ids, path, report, label)
    report_unknown(unknown, directory, report, "deliverables")
    return found


# ---- Verdict ---------------------------------------------------------------------------------

def evidence_base(root, rel):
    """Directory an evidence path is relative to: the docs root, except the trial drivers, which are
    tooling kept in the main repository (tools/trials/<platform-id>/...)."""
    if rel.replace("\\", "/").startswith("tools/trials/"):
        return common.ROOT
    return root


def check_evidence(rec, vpath, root, label, report):
    evidence = rec.get("evidence")
    if evidence is None or evidence == []:
        if rec.get("status") != "not_tested":
            report.error(vpath, "%s: 'evidence' (path + sha256) is mandatory unless not_tested"
                         % label)
        return
    if not isinstance(evidence, list):
        report.error(vpath, "%s: 'evidence' must be a list of {path, sha256}" % label)
        return
    unknown = Counter()
    for i, item in enumerate(evidence):
        where = "%s: evidence[%d]" % (label, i)
        if not isinstance(item, dict):
            report.error(vpath, "%s must be an object {path, sha256}" % where)
            continue
        note_unknown(item, EVIDENCE_FIELDS, unknown)
        if not is_text(item.get("path")) or not is_text(item.get("sha256")):
            report.error(vpath, "%s needs a 'path' and a 'sha256'" % where)
            continue
        base = evidence_base(root, item["path"])
        target = (base / item["path"]).resolve()
        try:
            target.relative_to(base.resolve())
        except ValueError:
            report.error(vpath, "%s: path %r leaves the repository" % (where, item["path"]))
            continue
        if Path(item["path"]).is_absolute():
            report.error(vpath, "%s: path %r must be relative to the repository root"
                         % (where, item["path"]))
            continue
        if not target.is_file():
            report.error(vpath, "%s: file %r does not exist (paths are relative to the "
                         "repository root)" % (where, item["path"]))
            continue
        if not SHA_RE.match(item["sha256"]):
            report.error(vpath, "%s: sha256 must be 64 lowercase hex digits" % where)
        elif common.sha256(target) != item["sha256"]:
            report.error(vpath, "%s: sha256 of %r does not match the file (file has %s)"
                         % (where, item["path"], common.sha256(target)))
        if item.get("kind") == "probe":
            check_probe_evidence(target, item["path"], rec, vpath, label, report)
    report_unknown(unknown, vpath, report, "%s evidence" % label)


def check_probe_evidence(target, shown, rec, vpath, label, report):
    """An evidence item of kind "probe" holds one ProbeResult line (maybe among other lines)."""
    try:
        import check_probe
        text = target.read_text(encoding="utf-8", errors="replace")
        line, problem = check_probe.find_probe_line(text)
        if problem:
            report.error(vpath, "%s: probe evidence %r: %s" % (label, shown, problem))
            return
        for err in check_probe.check_probe_line(line):
            report.error(vpath, "%s: probe evidence %r: %s" % (label, shown, err))
        record = json.loads(line)
        for key, want in (("platform_id", rec.get("platform_id")),
                          ("ui_variant", rec.get("ui_variant"))):
            if isinstance(record, dict) and key in record and record[key] != want:
                report.error(vpath, "%s: probe evidence %r has %s %r, the verdict has %r"
                             % (label, shown, key, record[key], want))
    except ImportError:
        report.warn(vpath, "%s: probe evidence %r not checked: the 'jsonschema' package is "
                    "missing" % (label, shown))


def check_verdicts(directory, root, platforms, deliverables, source_ids, report):
    found = OrderedDict()  # (platform_id, variant, web_path) -> path
    unknown = Counter()
    for path in json_files(directory, report):
        rec = load(path, report)
        if rec is None or not need_object(rec, path, report):
            continue
        label = "verdict %s" % path.stem
        note_unknown(rec, VERDICT_FIELDS, unknown)
        ok_id = require_text(rec, "platform_id", path, report, label)
        ok_var = require_enum(rec, "ui_variant", VARIANTS, path, report, label)
        status = rec.get("status")
        ok_status = require_enum(rec, "status", STATUSES, path, report, label)
        web_path = rec.get("web_path")
        platform = platforms.get(rec.get("platform_id")) if ok_id else None

        if ok_id and ok_var:
            stems = ["%s-%s" % (rec["platform_id"], rec["ui_variant"])]
            if platform is not None and platform.get("family") == "web" and web_path in WEB_PATHS:
                stems.append("%s-%s" % (stems[0], web_path))
            if path.stem not in stems:
                report.error(path, "%s: file name must be %s.json" % (label, " or ".join(stems)))
            key = (rec["platform_id"], rec["ui_variant"], web_path)
            if key in found:
                report.error(path, "%s: duplicate verdict for platform, variant and web_path"
                             % label)
            found[key] = path
            if platform is None:
                report.error(path, "%s: platform %r has no record in platforms/ (a verdict "
                             "references a Platform)" % (label, rec["platform_id"]))
            elif platform.get("in_scope") is not True:
                report.error(path, "%s: platform %r is not in_scope (a verdict is only allowed "
                             "for in_scope = true)" % (label, rec["platform_id"]))
            elif ok_status and status in ("ok", "ok_restricted") and \
                    (rec["platform_id"], rec["ui_variant"]) not in deliverables:
                report.warn(path, "%s: status %s but no deliverable record %s-%s.json"
                            % (label, status, rec["platform_id"], rec["ui_variant"]))

        # web_path: mandatory for the web family, forbidden otherwise
        if platform is not None:
            if platform.get("family") == "web":
                if web_path not in WEB_PATHS:
                    report.error(path, "%s: 'web_path' (wasm | js) is mandatory for the web "
                                 "family" % label)
            elif "web_path" in rec:
                report.error(path, "%s: 'web_path' is forbidden for the %s family"
                             % (label, platform.get("family")))
        elif "web_path" in rec and web_path not in WEB_PATHS:
            report.error(path, "%s: 'web_path' must be wasm or js" % label)

        if status == "ok_restricted" and not (is_text_list(rec.get("restrictions"))
                                              and rec.get("restrictions")):
            report.error(path, "%s: 'restrictions[]' is mandatory (non-empty) if ok_restricted"
                         % label)
        if status == "ko":
            if not is_text(rec.get("cause")):
                report.error(path, "%s: 'cause' is mandatory if ko" % label)
            attempts = rec.get("attempts")
            if not is_text_list(attempts) or not attempts:
                report.error(path, "%s: 'attempts[]' is mandatory (non-empty) if ko" % label)
            elif len(attempts) < 2:
                report.warn(path, "%s: only %d attempt recorded; SC-003 expects at least two "
                            "solutions tried for a KO platform" % (label, len(attempts)))
        if status == "not_tested" and not is_text(rec.get("reason")):
            report.error(path, "%s: 'reason' is mandatory if not_tested" % label)
        if ok_status:
            check_evidence(rec, path, root, label, report)

        feats = rec.get("disabled_features")
        if feats is not None:
            if not isinstance(feats, list):
                report.error(path, "%s: 'disabled_features' must be a list" % label)
            else:
                for feat in feats:
                    if not (is_text(feat) or (isinstance(feat, dict) and is_text(feat.get("reason")))):
                        report.error(path, "%s: a disabled feature is a string or an object with "
                                     "a 'reason' (FR-025)" % label)
        if "decision" in rec and rec["decision"] not in DECISIONS:
            report.error(path, "%s: 'decision' must be one of %s" % (label, ", ".join(DECISIONS)))
        repro = rec.get("reproduction")
        if is_text(repro):
            if not (root / repro).exists():
                report.error(path, "%s: reproduction %r does not exist (path relative to the "
                             "repository root)" % (label, repro))
        elif repro is not None:
            report.error(path, "%s: 'reproduction' must be a path string" % label)
        elif status in ("ok", "ok_restricted", "ko"):
            report.incomplete(path, "%s: no 'reproduction' procedure (SC-004)" % label)
        check_source_refs(rec, source_ids, path, report, label)
    report_unknown(unknown, directory, report, "verdicts")
    return found


# ---- whole tree ------------------------------------------------------------------------------

KNOWN_TOP = ("sources.json", "fallbacks.json", "spikes.json", "hypotheses.json")
KNOWN_DIRS = ("platforms", "verdicts", "deliverables", "evidence", "procedures")


def validate(validation_dir, root, require_complete=False):
    validation_dir = Path(validation_dir)
    root = Path(root)
    report = Report(root, require_complete)
    if not validation_dir.is_dir():
        report.error(validation_dir, "directory not found")
        return report

    for entry in sorted(validation_dir.iterdir()):
        if entry.is_dir() and entry.name not in KNOWN_DIRS:
            report.info(entry, "directory not described in data-model.md, not checked")
        elif entry.is_file() and entry.name not in KNOWN_TOP and not entry.name.startswith(".") \
                and entry.suffix != ".md":
            report.info(entry, "file not described in data-model.md, not checked")

    source_ids = set()
    if (validation_dir / "sources.json").is_file():
        source_ids = check_sources(validation_dir / "sources.json", report)
    else:
        report.error(validation_dir / "sources.json", "file not found")
    fallback_ids = OrderedDict()
    if (validation_dir / "fallbacks.json").is_file():
        fallback_ids = check_fallbacks(validation_dir / "fallbacks.json", source_ids, report)
    else:
        report.error(validation_dir / "fallbacks.json", "file not found")
    if (validation_dir / "spikes.json").is_file():
        check_spikes(validation_dir / "spikes.json", root, source_ids, report)
    else:
        report.error(validation_dir / "spikes.json", "file not found")

    platforms = check_platforms(validation_dir / "platforms", report)
    deliverables = check_deliverables(validation_dir / "deliverables", platforms, fallback_ids,
                                      source_ids, report)
    verdicts = check_verdicts(validation_dir / "verdicts", root, platforms, deliverables,
                              source_ids, report)
    check_hypotheses(validation_dir / "hypotheses.json", platforms, source_ids, report, root)

    for fid, fb in fallback_ids.items():
        for pid in fb.get("platform_ids") or []:
            if isinstance(pid, str) and pid not in platforms:
                report.error(validation_dir / "fallbacks.json",
                             "fallback %r: platform %r has no record in platforms/" % (fid, pid))

    # SC-004: no platform in scope left without a verdict, for each variant.
    covered = set((pid, variant) for (pid, variant, _) in verdicts)
    for pid, platform in platforms.items():
        if platform.get("in_scope") is not True:
            continue
        for variant in VARIANTS:
            if (pid, variant) not in covered:
                report.incomplete(validation_dir / "platforms" / (pid + ".json"),
                                  "platform %r is in_scope but has no %s verdict (SC-004)"
                                  % (pid, variant))
    return report


def print_report(report):
    order = {"ERROR": 0, "WARN": 1, "INFO": 2}
    for level, path, message in sorted(report.items, key=lambda it: (order[it[0]], it[1])):
        print("%-5s %s: %s" % (level, path, message))
    print("validate_records: %d error(s), %d warning(s), %d info"
          % (report.count("ERROR"), report.count("WARN"), report.count("INFO")))


# ---- self-test -------------------------------------------------------------------------------

def _good_tree():
    """A small, valid validation/ tree in memory: relative path -> JSON value or bytes."""
    evidence_bytes = b'{"note": "stand-in evidence file"}\n'
    sha = __import__("hashlib").sha256(evidence_bytes).hexdigest()

    def platform(pid, family, arch, **extra):
        rec = OrderedDict([
            ("id", pid), ("family", family), ("os_name", "Test OS"), ("os_version", "1.0"),
            ("arch", arch), ("release_date", "2020-01-31"), ("age_years", 6.5),
            ("is_floor", False), ("in_scope", True), ("environment", "vm"),
            ("environment_detail", "test hypervisor"), ("eol", False)])
        rec.update(extra)
        return rec

    def evidence():
        return [OrderedDict([("path", "validation/evidence/e.json"), ("sha256", sha)])]

    tree = OrderedDict()
    tree["validation/evidence/e.json"] = evidence_bytes
    tree["validation/procedures/p.md"] = b"procedure\n"
    tree["adr/0002-x.md"] = b"adr\n"
    tree["validation/sources.json"] = [
        OrderedDict([("id", "src-a"), ("url", "https://example.org/a"), ("title", "A"),
                     ("accessed", "2026-11-12"), ("claim", "c"), ("verified", "yes"),
                     ("note", "extra field")])]
    tree["validation/fallbacks.json"] = [
        OrderedDict([("id", "fb-a"), ("description", "d"), ("macro", "LOOMORA_HAS_A"),
                     ("min_compiler", "gcc 7"), ("platform_ids", ["lin-1"]), ("reason", "r")])]
    tree["validation/spikes.json"] = [
        OrderedDict([("id", "S%d" % i), ("question", "q"), ("status", "open"), ("result", ""),
                     ("adr", "")]) for i in range(1, 10) if i != 4]
    tree["validation/spikes.json"][0].update(status="confirmed", result="done")
    tree["validation/spikes.json"][1].update(status="refuted", result="no", adr="adr/0002-x.md")
    tree["validation/hypotheses.json"] = [
        OrderedDict([("id", "V1"), ("title", "t"), ("criterion", "c"), ("fallback", "f"),
                     ("covered_by_probe", True), ("platform_ids", ["lin-1"]),
                     ("status", "ok_restricted"), ("records", ["validation/procedures/p.md"]),
                     ("note", "n")]),
        OrderedDict([("id", "V2"), ("title", "t"), ("criterion", "c"), ("fallback", "f"),
                     ("covered_by_probe", False), ("platform_ids", []),
                     ("status", "not_covered"), ("records", [])])]
    tree["validation/platforms/lin-1.json"] = platform("lin-1", "linux", "x86_64")
    tree["validation/platforms/web-1.json"] = platform("web-1", "web", "wasm")
    tree["validation/platforms/old-1.json"] = platform("old-1", "windows", "x86", in_scope=False)
    import check_probe
    probe = json.loads(check_probe.GOOD_LINE)
    probe["platform_id"] = "lin-1"
    probe_bytes = (json.dumps(probe) + "\n").encode("ascii")
    tree["validation/evidence/probe.json"] = probe_bytes
    for pid in ("lin-1",):
        for variant in VARIANTS:
            tree["validation/deliverables/%s-%s.json" % (pid, variant)] = OrderedDict([
                ("platform_id", pid), ("ui_variant", variant), ("cpp_std", "c++11"),
                ("compiler", "gcc-13"), ("target", "x86_64-linux-gnu"), ("output", "native"),
                ("ui_language", "c"), ("ui_toolkit", "gtk"), ("ui_compiler", "gcc-13"),
                ("artifact", OrderedDict([("name", "a"), ("size", 10)])),
                ("disabled_features", []), ("fallbacks", ["fb-a"]), ("sharing_ratio", 0.5)])
            tree["validation/verdicts/%s-%s.json" % (pid, variant)] = OrderedDict([
                ("platform_id", pid), ("ui_variant", variant), ("status", "ok"),
                ("evidence", evidence()), ("reproduction", "validation/procedures/p.md"),
                ("decision", "keep"), ("sources", ["src-a"])])
    tree["validation/verdicts/lin-1-modern.json"]["evidence"].append(OrderedDict([
        ("path", "validation/evidence/probe.json"),
        ("sha256", __import__("hashlib").sha256(probe_bytes).hexdigest()), ("kind", "probe")]))
    for variant, wp in (("modern", "wasm"), ("legacy", "js")):
        tree["validation/deliverables/web-1-%s.json" % variant] = OrderedDict([
            ("platform_id", "web-1"), ("ui_variant", variant), ("cpp_std", "c++11"),
            ("compiler", "emscripten-3"), ("target", "wasm32"), ("output", wp),
            ("ui_language", "ts"), ("ui_toolkit", "dom"), ("ui_compiler", "tsc"),
            ("artifact", OrderedDict([("name", "loomora.wasm"), ("size", 10)])),
            ("disabled_features", []), ("fallbacks", [])])
        tree["validation/verdicts/web-1-%s-%s.json" % (variant, wp)] = OrderedDict([
            ("platform_id", "web-1"), ("ui_variant", variant), ("web_path", wp), ("status", "ok"),
            ("evidence", evidence()), ("reproduction", "validation/procedures/p.md")])
    return tree


def _mutations():
    """(description, expected message fragment, function(tree)) for each rule."""
    def verdict(tree, name="lin-1-modern", **changes):
        rec = tree["validation/verdicts/%s.json" % name]
        for k, v in changes.items():
            if v is _DEL:
                rec.pop(k, None)
            else:
                rec[k] = v

    def sha_of(tree):
        return tree["validation/verdicts/lin-1-modern.json"]["evidence"][0]

    def path_of(tree, rel, value):
        tree[rel] = value

    web = "web-1-modern-wasm"
    return [
        ("status not in the enum", "'status' must be one of", lambda t: verdict(t, status="fine")),
        ("web family without web_path", "'web_path' (wasm | js) is mandatory",
         lambda t: verdict(t, web, web_path=_DEL)),
        ("web_path on a non-web platform", "'web_path' is forbidden",
         lambda t: verdict(t, web_path="wasm")),
        ("web_path outside wasm|js", "'web_path' (wasm | js) is mandatory",
         lambda t: verdict(t, web, web_path="asmjs")),
        ("ok_restricted without restrictions", "restrictions[]",
         lambda t: verdict(t, status="ok_restricted")),
        ("ko without cause", "'cause' is mandatory if ko",
         lambda t: verdict(t, status="ko", attempts=["a", "b"])),
        ("ko without attempts", "'attempts[]' is mandatory",
         lambda t: verdict(t, status="ko", cause="why")),
        ("not_tested without reason", "'reason' is mandatory if not_tested",
         lambda t: verdict(t, status="not_tested", evidence=_DEL)),
        ("ok without evidence", "'evidence' (path + sha256) is mandatory",
         lambda t: verdict(t, evidence=_DEL)),
        ("evidence file missing", "does not exist",
         lambda t: sha_of(t).update(path="validation/evidence/none.json")),
        ("evidence hash mismatch", "does not match the file",
         lambda t: sha_of(t).update(sha256="0" * 64)),
        ("evidence path outside the repository", "leaves the repository",
         lambda t: sha_of(t).update(path="../outside.json")),
        ("evidence kind probe is not a probe line", "probe evidence",
         lambda t: sha_of(t).update(kind="probe")),
        ("probe evidence of another platform", "has platform_id 'linux-ubuntu-22.04-x86_64'",
         lambda t: t["validation/verdicts/lin-1-modern.json"]["evidence"][1].update(
             path="validation/evidence/foreign.json",
             sha256=__import__("hashlib").sha256(__import__("check_probe").GOOD_LINE.encode(
                 "ascii") + b"\n").hexdigest()) or t.__setitem__(
             "validation/evidence/foreign.json",
             __import__("check_probe").GOOD_LINE.encode("ascii") + b"\n")),
        ("verdict for an unknown platform", "has no record in platforms/",
         lambda t: verdict(t, platform_id="ghost-1")),
        ("verdict for an out-of-scope platform", "not in_scope",
         lambda t: verdict(t, platform_id="old-1")),
        ("verdict file name mismatch", "file name must be",
         lambda t: t.__setitem__("validation/verdicts/other.json",
                                 t["validation/verdicts/lin-1-modern.json"])),
        ("verdict reproduction missing", "reproduction 'validation/none.md' does not exist",
         lambda t: verdict(t, reproduction="validation/none.md")),
        ("verdict decision outside the enum", "'decision' must be one of",
         lambda t: verdict(t, decision="maybe")),
        ("verdict source missing", "source 'nope' is not in validation/sources.json",
         lambda t: verdict(t, sources=["nope"])),
        ("deliverable fallback unknown", "fallback 'fb-zzz' is not in validation/fallbacks.json",
         lambda t: t["validation/deliverables/lin-1-modern.json"].update(fallbacks=["fb-zzz"])),
        ("deliverable cpp_std not c++11", "'cpp_std' must be",
         lambda t: t["validation/deliverables/lin-1-modern.json"].update(cpp_std="c++17")),
        ("deliverable sharing_ratio above 1", "'sharing_ratio' must be",
         lambda t: t["validation/deliverables/lin-1-modern.json"].update(sharing_ratio=1.5)),
        ("deliverable field missing", "'ui_toolkit' is mandatory",
         lambda t: t["validation/deliverables/lin-1-modern.json"].pop("ui_toolkit")),
        ("platform family outside the enum", "'family' must be one of",
         lambda t: t["validation/platforms/lin-1.json"].update(family="plan9")),
        ("platform in_scope not a boolean", "'in_scope' must be a boolean",
         lambda t: t["validation/platforms/lin-1.json"].update(in_scope="yes")),
        ("platform id not matching its file", "file name must be <id>.json",
         lambda t: t["validation/platforms/lin-1.json"].update(id="lin-2")),
        ("platform bad release_date", "'release_date' must be a date",
         lambda t: t["validation/platforms/lin-1.json"].update(release_date="2020-13-45")),
        ("fallback with unknown platform", "platform 'ghost-9' has no record",
         lambda t: t["validation/fallbacks.json"][0].update(platform_ids=["ghost-9"])),
        ("fallback duplicate id", "duplicate id",
         lambda t: t["validation/fallbacks.json"].append(t["validation/fallbacks.json"][0])),
        ("source without url", "'url' must be an http(s) URL",
         lambda t: t["validation/sources.json"][0].update(url="ftp:nope")),
        ("spike id outside S1-S9", "'id' must be one of",
         lambda t: t["validation/spikes.json"][0].update(id="S10")),
        ("spike status outside the enum", "'status' must be one of",
         lambda t: t["validation/spikes.json"][2].update(status="maybe")),
        ("spike confirmed without result", "'result' must be non-empty unless",
         lambda t: t["validation/spikes.json"][0].update(result="")),
        ("spike refuted without fallback or adr", "must name the chosen fallback",
         lambda t: t["validation/spikes.json"][1].update(adr="")),
        ("spike adr path missing", "adr 'adr/9999.md' does not exist",
         lambda t: t["validation/spikes.json"][1].update(adr="adr/9999.md")),
        ("hypothesis record missing", "record 'validation/procedures/none.md' does not exist",
         lambda t: t["validation/hypotheses.json"][0].update(
             records=["validation/procedures/none.md"])),
        ("hypothesis verdict without coverage", "needs covered_by_probe = true",
         lambda t: t["validation/hypotheses.json"][0].update(covered_by_probe=False)),
        ("invalid JSON", "not valid JSON",
         lambda t: t.__setitem__("validation/verdicts/broken.json", b"{not json")),
    ]


_DEL = object()


def _completeness_cases():
    """Incomplete-data cases: WARN by default, ERROR with --require-complete."""
    def drop_verdict(tree):
        del tree["validation/verdicts/lin-1-legacy.json"]

    def drop_platform(tree):
        del tree["validation/platforms/lin-1.json"]
        for name in [n for n in tree if n.startswith("validation/verdicts/lin-1")]:
            del tree[name]
        tree["validation/fallbacks.json"][0]["platform_ids"] = []
        tree["validation/hypotheses.json"][0]["platform_ids"] = []

    def drop_repro(tree):
        tree["validation/verdicts/lin-1-modern.json"].pop("reproduction")

    return [
        ("in-scope platform without a verdict (SC-004)", "has no legacy verdict", drop_verdict),
        ("deliverable without platform record", "has no record in platforms/", drop_platform),
        ("verdict without reproduction (SC-004)", "no 'reproduction' procedure", drop_repro),
    ]


def _materialize(tree, base):
    for rel, content in tree.items():
        path = base / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            common.write_json(path, content)


def _copy_tree(tree):
    import copy
    return OrderedDict((k, v if isinstance(v, bytes) else copy.deepcopy(v)) for k, v in tree.items())


def _run_case(tree, require_complete=False):
    base = Path(tempfile.mkdtemp(prefix="loomora-validate-"))
    try:
        _materialize(tree, base)
        return validate(base / "validation", base, require_complete)
    finally:
        shutil.rmtree(str(base), ignore_errors=True)


def self_test():
    failures = 0
    good = _good_tree()

    report = _run_case(_copy_tree(good))
    if report.count("ERROR") or report.count("WARN"):
        failures += 1
        print("FAIL good tree has problems:")
        print_report(report)
    else:
        print("ok   good tree accepted (%d info line(s): extra optional fields listed)"
              % report.count("INFO"))
    if not any("verified" in m and "note" in m for lv, p, m in report.items if lv == "INFO"):
        failures += 1
        print("FAIL unknown optional fields (verified, note) were not listed as INFO")

    for name, fragment, mutate in _mutations():
        tree = _copy_tree(good)
        mutate(tree)
        report = _run_case(tree)
        hits = [m for lv, p, m in report.items if lv == "ERROR" and fragment in m]
        if hits:
            print("ok   rejected (%s)" % name)
        else:
            failures += 1
            print("FAIL no ERROR containing %r for: %s" % (fragment, name))
            for lv, p, m in report.items:
                print("       %s %s: %s" % (lv, p, m))

    for name, fragment, mutate in _completeness_cases():
        tree = _copy_tree(good)
        mutate(tree)
        soft = _run_case(_copy_tree(tree))
        strict = _run_case(_copy_tree(tree), require_complete=True)
        warned = any(lv == "WARN" and fragment in m for lv, p, m in soft.items)
        no_error = soft.count("ERROR") == 0
        errored = any(lv == "ERROR" and fragment in m for lv, p, m in strict.items)
        if warned and no_error and errored:
            print("ok   incomplete data: WARN by default, ERROR with --require-complete (%s)" % name)
        else:
            failures += 1
            print("FAIL completeness rule (%s): warned=%s no_error=%s errored=%s"
                  % (name, warned, no_error, errored))

    # A justified absent variant: a not_tested verdict with a reason, no evidence, no reproduction.
    tree = _copy_tree(good)
    tree["validation/verdicts/lin-1-legacy.json"] = OrderedDict([
        ("platform_id", "lin-1"), ("ui_variant", "legacy"), ("status", "not_tested"),
        ("reason", "no 32-bit toolkit is installed on the machine")])
    report = _run_case(tree, require_complete=True)
    if report.count("ERROR") or report.count("WARN"):
        failures += 1
        print("FAIL a not_tested verdict with a reason is not accepted as covered:")
        print_report(report)
    else:
        print("ok   not_tested verdict with a reason accepted without evidence or reproduction, "
              "and the variant counts as covered")

    print("self-test %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Validate docs/implementation/validation/ against data-model.md.")
    ap.add_argument("--require-complete", action="store_true",
                    help="treat missing verdicts, platform records and reproduction paths as "
                    "errors (SC-004) instead of warnings")
    ap.add_argument("--validation-dir", help="directory to check (default: <docs root>/validation)")
    ap.add_argument("--root", help="root that evidence and reproduction paths are relative to "
                    "(default: docs/implementation, the docs submodule)")
    ap.add_argument("--self-test", action="store_true",
                    help="build good and bad records in a temporary directory and check each rule")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.root and not args.validation_dir and not common.docs_present():
        # Exit status 2: the docs submodule is not initialised, there is nothing to validate.
        print("validate_records: " + common.docs_missing_message("nothing was validated"), file=sys.stderr)
        return EXIT_NO_DOCS
    root = Path(args.root) if args.root else common.DOCS
    vdir = Path(args.validation_dir) if args.validation_dir else root / "validation"
    report = validate(vdir, root, args.require_complete)
    print_report(report)
    return 1 if report.count("ERROR") else 0


if __name__ == "__main__":
    sys.exit(main())
