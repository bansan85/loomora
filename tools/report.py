#!/usr/bin/env python
"""Generate the validation report docs/implementation/validation/report.md from the records
(git submodule ../loomora-docs): platforms/, verdicts/, deliverables/, fallbacks.json,
spikes.json, hypotheses.json, sources.json (tasks T098 to T103, the shared-code table of T107 and the
verdict comparison of T112).

Usage:
  python tools/report.py                 write validation/report.md
  python tools/report.py --stdout        print the report instead of writing it
  python tools/report.py --baseline REV  also compare the verdicts with the baseline REV (section 14)
  python tools/report.py --check         print the checks (SC-002, SC-003, SC-004, SC-005, SC-007,
                                         SC-009, SC-010) and verify that report.md is up to date with the
                                         records; exit 1 if it is missing or stale
  python tools/report.py --self-test     run the self-test on synthetic records (no submodule)
  options: --validation-dir DIR, --output FILE, --baseline SET, --baseline-repo DIR

--baseline SET names the verdicts to compare with: a git revision of the docs submodule (a commit, a
branch, a tag, HEAD~1) or a folder, as tools/diff_verdicts.py reads it ("dir:PATH" and "git:REV" force
one reading). The section "Verdict changes since a baseline" then holds the output of
diff_verdicts.compare_sets() (every status change, every other difference per platform and variant).
Without it the section says that no baseline was given, so report.md stays the same bytes for the same
records; pass the same --baseline to --check to verify a report generated with one.

Exit status: 0 = done / up to date, 1 = report missing or stale (--check) or a self-test failure,
2 = the docs submodule is not checked out (nothing to report; the product build never needs it) or the
--baseline cannot be read.

Everything the report states about a trial comes from the records. Where no verdict record exists
(macOS, iOS, Android, Web, Windows XP, Windows 11 Modern, ...) the report says "not tested". The
only text of the tool itself is the layout, the requirements document figures it compares with
(floors, tiers, decisions of section 16, in the tables REQ_FLOORS, DEVIATIONS, DECISIONS below,
each with the path of the document that states it), and the reading of the checks. The output is
deterministic (no date, no host data): regenerating from unchanged records gives the same bytes.
"""
import argparse
import json
import re
import sys
import tempfile
import shutil
from collections import OrderedDict
from pathlib import Path

import common
import diff_verdicts
import measure_sharing

EXIT_NO_DOCS = 2
SHARING_MIN = 0.8  # SC-007: at least 80% of the application code is shared
VARIANTS = ("legacy", "modern")
FAMILIES = ("windows", "macos", "linux", "android", "ios", "web")
FAMILY_NAME = {"windows": "Windows", "macos": "macOS", "linux": "Linux", "android": "Android",
               "ios": "iOS / iPadOS", "web": "Web"}
STATUS_LABEL = OrderedDict([("ok", "OK"), ("ok_restricted", "OK with restrictions"),
                            ("ko", "KO"), ("not_tested", "not tested")])
VALIDATED = ("ok", "ok_restricted")  # a platform "validated" for a variant (SC-002, SC-003)
NOT_TESTED = "not tested"
SPEC = "specs/001-loomora-cross-platform/spec.md"
SPEC_ROOT = "specs/001-loomora-cross-platform/"

# The floors of the requirements document (section 2.1), the floor decided for this probe (spec
# Clarifications, second session) and the year of the floor system, used only to tell whether a
# validated system is older than the requirements floor.
REQ_FLOORS = (
    # family, UI, requirements document, decided floor, year of the requirements floor
    ("windows", "legacy", "Windows XP SP3 minimum, MFC UI (sections 2.1 and 2.1.2)",
     "Windows 2000, plain Win32 UI", 2001),
    ("windows", "modern", "Windows 10+, WinUI 3 (section 2.1)",
     "WinUI 3 needs Windows 10 1809 (research R6; source winappsdk-support)", 2015),
    ("macos", "legacy", "macOS 11+ (section 2.1)", "macOS 10.7 Lion", 2020),
    ("linux", "legacy", "Ubuntu 20.04+ / Debian 11+, AppImage and Flatpak (section 2.1)",
     "any distribution with a C++11 compiler, CMake and the libX11 development files; "
     "built on the machine that runs it (ADR 0007)", 2020),
    ("android", "legacy", "minSdk API 21 (Android 5.0) (section 2.1)", "Android 2.3 (API 9)", 2014),
    ("ios", "legacy", "iOS 15.0 (section 2.1)", "iOS 5", 2021),
    ("web", "legacy", "Chrome, Safari and Firefox with WebAssembly (section 2.1)",
     "WebAssembly mandatory, JavaScript build for the browsers of November 2011", 2017),
)

# Deviations from the requirements document: (what the requirements document says, what the
# project does, where it was decided, [paths that must exist, relative to the repository or to
# docs/implementation]).
DEVIATIONS = (
    ("Backend: Rust core, Cargo workspace, `cbindgen` C API (sections 1, 2.2, 14, 16.1)",
     "One backend in strict C++11 (`-std=c++11 -pedantic-errors`) behind a hand-written C API "
     "(`core/include/loomora.h`, valid as C89); no Rust, no `cbindgen`, no UniFFI. Exceptions "
     "never cross the boundary.",
     "Spec Clarifications (second session); ADR 0001, spike S6",
     [SPEC, "adr/0001-backend-cpp11-c-api.md"]),
    ("Windows Legacy UI: MFC with the `v141_xp` toolset (sections 2.1.2 and 16.1, no. 8)",
     "C++ on the plain Win32 API (an EDIT and a STATIC control, no MFC), one source for Windows "
     "2000 to 11, built with MinGW-w64 GCC like the backend: no C++11 MSVC targets Windows 2000 "
     "and recent Visual Studio no longer targets XP (source vs-xp-targets).",
     "Spec Clarifications (third session); research R6",
     [SPEC, SPEC_ROOT + "research.md"]),
    ("Windows floor: Windows XP SP3 (section 2.1)",
     "Windows 2000 (the executable carries a link-time `GetThreadId` shim and the self-test avoids "
     "`<iostream>`, both for systems older than Vista).",
     "Spec Clarifications (second session); ADR 0002, ADR 0004",
     [SPEC, "adr/0002-win2000-getthreadid-shim.md", "adr/0004-fallback-per-case.md"]),
    ("Linux UI: GTK 4 in Rust, alternative Slint (sections 2.2.1 and 16.2, no. 9)",
     "GTK 3 with gtkmm in C++ (C++11 with gtkmm-3.0), built with CMake, GTK 3.10 API as the floor; "
     "GTK 4 with gtkmm-4.0 only through the CMake option `LOOMORA_GTK_API=4`, each newer feature "
     "behind one macro and one fallback record. Slint was not tried.",
     "Spec Clarifications (third session); ADR 0005, ADR 0006",
     [SPEC, "adr/0005-gtk-version-policy.md", "adr/0006-linux-modern-ui-gtkmm-cmake.md"]),
    ("Linux delivery: Ubuntu 20.04+ / Debian 11+, AppImage and Flatpak (section 2.1)",
     "A plain executable built on the machine that runs it (a C++11 compiler, CMake and the libX11 "
     "development files are needed there); no Flatpak, no AppImage, no glibc baseline.",
     "Spec Clarifications; ADR 0007",
     [SPEC, "adr/0007-linux-build-on-target.md"]),
    ("Two UIs only on Windows (section 16.1, no. 8)",
     "Every platform has two UIs on the same backend: a Legacy UI (very old systems, features may "
     "be disabled) and a Modern UI (FR-024). Linux Legacy is a C++ Xlib UI with Xft text.",
     "Spec Clarifications (first session), FR-024",
     [SPEC]),
    ("Build system: Cargo workspace (section 14)",
     "CMake with one toolchain file per cross target; Meson (first Linux Modern UI) dropped; each "
     "native UI keeps its own project where its platform needs one.",
     "Research R9; ADR 0006",
     [SPEC_ROOT + "research.md", "adr/0006-linux-modern-ui-gtkmm-cmake.md"]),
    ("Floors: macOS 11, iOS 15, Android API 21, Windows 10 (section 2.1)",
     "Floors decided without Rust: macOS 10.7, iOS 5, Android 2.3 (API 9), Windows 2000, browsers "
     "of November 2011 through the JavaScript build. What was tried is in section 5.",
     "Spec Clarifications (second session)",
     [SPEC]),
    ("Architectures and tiers by Rust target availability (section 2.1.1)",
     "x86 and x64 on Windows, Linux and macOS 10.7; ARMv6/ARMv7 and x86 on Android 2.3; ARMv7 and "
     "arm64 on iOS; PowerPC, MIPS and Itanium are out of scope. A tier no longer depends on a Rust "
     "toolchain.",
     "Spec Clarifications (second session)",
     [SPEC]),
    ("PoC `Hello Loomora`: nine steps, greedy path, `.loomora`, SQLite, voice, printing (section 15.1.3)",
     "A deliberately minimal probe: a text field pre-filled with `Coucou` and the greeting "
     "`Bonjour ` + text returned by the backend. Hypotheses that need more are `not_covered`.",
     "Spec Assumptions and FR-001, FR-028",
     [SPEC]),
    ("Web: `wasm32-unknown-unknown`, TypeScript UI (section 2.1)",
     "The C++11 backend as WebAssembly, and the same C++ compiled to JavaScript (Emscripten, "
     "WebAssembly off) for the November 2011 browsers (FR-023). Not tried.",
     "Spec Clarifications, FR-023",
     [SPEC]),
)

# Decisions of section 16 of the requirements document and what the probe says about them.
DECISIONS = (
    ("1", "Native UI per platform", "partly tried",
     "Native UIs exist for Windows (Win32) and Linux (Xlib, gtkmm); Kotlin, SwiftUI, WinUI 3 and "
     "the web UI were not tried (spike S8 open)."),
    ("2", "Minimum iOS 15.0", "not tested", "No iOS trial; the decided floor is iOS 5."),
    ("3", "SQLite persistence", "not covered", "The probe stores nothing."),
    ("4", "Speech recognition, system API only", "not covered", "The probe has no voice."),
    ("5", ".loomora format", "not covered", "No file is written by the probe."),
    ("6", "PWA / WebAssembly in scope", "not tested", "No web trial (spike S5 open)."),
    ("7", "Architectures: as many as possible", "revised",
     "Reduced to the architectures of the systems actually used (spec Clarifications)."),
    ("8", "Windows: WinUI 3 and MFC", "revised",
     "MFC replaced by plain Win32 (works on Windows 2000 and 11); WinUI 3 not tried."),
    ("9", "Linux UI: GTK 4 (Rust), alternative Slint", "revised",
     "GTK 3 with gtkmm (C++), GTK 4 as an option (tried to build and run vectors); Slint not tried."),
    ("10-12", "Winding direction, license, pilot platforms", "not covered", "Not part of the probe."),
    ("13", "Compose or Views on Android API 21", "not tested", "No Android trial."),
    ("14", "Rust toolchain for Windows XP", "moot",
     "Rust is dropped (ADR 0001); the question is now whether the Win32 C++11 executable runs on "
     "XP, which no trial answers."),
)

# Where the headline measurements are in a verdict ("measurements" is free-form): a path of keys
# to the section of the current build, per (platform id, variant). Default: the top level, with
# the subtrees of earlier builds (history*, c_ui*) ignored.
MEASURE_BASE = {
    ("linux-ubuntu-24.04-x86_64", "modern"): ["cpp_gtkmm_2026_11_14"],
    ("linux-ubuntu-24.04-x86_64", "legacy"): ["probe_lines_of_the_current_build",
                                              "xft_x86_64_mixed"],
}
MEASURE_SKIP = ("history", "c_ui")


# ---- records ---------------------------------------------------------------------------------

def _read_dir(directory):
    out = []
    directory = Path(directory)
    if directory.is_dir():
        for path in sorted(directory.glob("*.json")):
            try:
                rec = common.read_json(path)
            except ValueError:
                continue
            if isinstance(rec, dict):
                out.append(rec)
    return out


def _read_list(path):
    path = Path(path)
    if not path.is_file():
        return []
    try:
        data = common.read_json(path)
    except ValueError:
        return []
    return [r for r in data if isinstance(r, dict)] if isinstance(data, list) else []


CODE_FILE = re.compile(r"^\s+((?:core|ui/(?:legacy|modern)/[^/\s]+)/\S+)\s+(\d+)\s*$")


def _read_code_sharing(directory):
    """Line counts per file stored with the code-sharing evidence (the --verbose listing of
    tools/measure_sharing.py in evidence/code-sharing/<platform>/measure-sharing.txt):
    {"core": {file: lines}, "ui": {"<variant>/<ui directory>": {file: lines}}}."""
    core, uis = OrderedDict(), OrderedDict()
    directory = Path(directory)
    paths = sorted(directory.glob("*/measure-sharing.txt")) if directory.is_dir() else []
    for path in paths:
        try:
            with open(str(path), encoding="utf-8") as f:
                text = f.read()
        except (OSError, ValueError):
            continue
        for line in text.splitlines():
            m = CODE_FILE.match(line)
            if not m:
                continue
            parts = m.group(1).split("/")
            if parts[0] == "core":
                core[m.group(1)] = int(m.group(2))
            else:
                uis.setdefault("/".join(parts[1:3]), OrderedDict())["/".join(parts[3:])] = int(
                    m.group(2))
    return {"core": core, "ui": uis}


class Records(object):
    """The validation records, in memory."""

    def __init__(self, platforms, verdicts, deliverables, fallbacks, spikes, hypotheses, sources,
                 docs_root=None, code_files=None):
        order = dict((f, i) for i, f in enumerate(FAMILIES))
        plist = sorted(platforms, key=lambda p: (order.get(p.get("family"), 99),
                                                 str(p.get("release_date")), str(p.get("id"))))
        self.platforms = OrderedDict((p["id"], p) for p in plist if "id" in p)
        self.verdicts = sorted(verdicts, key=lambda v: (str(v.get("platform_id")),
                                                        str(v.get("ui_variant")),
                                                        str(v.get("web_path", ""))))
        self.deliverables = OrderedDict(((d.get("platform_id"), d.get("ui_variant")), d)
                                        for d in deliverables)
        self.fallbacks = fallbacks
        self.spikes = sorted(spikes, key=lambda s: int(str(s.get("id", "S0"))[1:] or 0))
        self.hypotheses = sorted(hypotheses, key=lambda h: int(str(h.get("id", "V0"))[1:] or 0))
        self.sources = dict((s.get("id"), s) for s in sources)
        self.docs_root = Path(docs_root) if docs_root else None
        self.code_files = code_files or {"core": OrderedDict(), "ui": OrderedDict()}

    @classmethod
    def load(cls, validation_dir):
        v = Path(validation_dir)
        return cls(_read_dir(v / "platforms"), _read_dir(v / "verdicts"),
                   _read_dir(v / "deliverables"), _read_list(v / "fallbacks.json"),
                   _read_list(v / "spikes.json"), _read_list(v / "hypotheses.json"),
                   _read_list(v / "sources.json"), v.parent,
                   _read_code_sharing(v / "evidence" / "code-sharing"))

    def verdicts_of(self, pid, variant):
        return [v for v in self.verdicts
                if v.get("platform_id") == pid and v.get("ui_variant") == variant]

    def spike(self, sid):
        for s in self.spikes:
            if s.get("id") == sid:
                return s
        return None

    def attempted(self):
        """(platform id, variant) with a result verdict or a deliverable record: the attempted ones.
        A `not_tested` verdict is the recorded justification of a variant that was not attempted."""
        keys = []
        for pid in self.platforms:
            for variant in VARIANTS:
                tried = [v for v in self.verdicts_of(pid, variant) if v.get("status") != "not_tested"]
                if tried or (pid, variant) in self.deliverables:
                    keys.append((pid, variant))
        return keys


# ---- text helpers ----------------------------------------------------------------------------

def cell(text):
    return str(text).replace("\r", " ").replace("\n", " ").replace("|", "\\|").strip()


def clip(text, limit):
    text = " ".join(str(text).split())
    if len(text) <= limit:
        return text
    cut = text[:limit].rsplit(" ", 1)[0].rstrip(" ,;:(")
    return cut + " ..."


def num(n):
    if isinstance(n, float) and n != int(n):
        return ("%.2f" % n).rstrip("0").rstrip(".")
    return "{:,}".format(int(n))


def label(status):
    return STATUS_LABEL.get(status, str(status))


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(cell(c) if not str(c).startswith("<raw>") else str(c)[5:]
                                       for c in row) + " |")
    return lines


def feature_name(item):
    if isinstance(item, dict):
        return str(item.get("name") or item.get("feature") or item.get("id") or item.get("reason"))
    return str(item).split(" (", 1)[0]


def feature_reason(item):
    if isinstance(item, dict):
        return str(item.get("reason", ""))
    text = str(item)
    return text.split(" (", 1)[1].rstrip(")") if " (" in text else ""


def evidence_summary(verdict):
    ev = verdict.get("evidence") or []
    if not ev:
        return "none"
    shots = sum(1 for e in ev if isinstance(e, dict) and e.get("kind") == "screenshot")
    return "%d file(s)%s" % (len(ev), (", %d screenshot(s)" % shots) if shots else "")


# ---- measurements ----------------------------------------------------------------------------

def _is_num(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _numbers(node):
    if _is_num(node):
        return [node]
    if isinstance(node, dict):
        if _is_num(node.get("min")) and _is_num(node.get("max")):
            return [node["min"], node["max"]]
        out = []
        for k, v in node.items():
            if "poll" in k or k == "n":
                continue
            out.extend(_numbers(v))
        return out
    return []


def _find(node, key):
    found = []
    if isinstance(node, dict):
        for k, v in node.items():
            if k == key:
                found.extend(_numbers(v))
                for k2, v2 in (v.items() if isinstance(v, dict) else []):
                    if k2.startswith("all_") and isinstance(v2, dict) and _is_num(v2.get("median")):
                        found.append(("median", v2["median"]))
            elif not k.startswith(MEASURE_SKIP):
                found.extend(_find(v, key))
    return found


def measurement(verdict, key):
    """'min-max (median m)' of a measurement of the verdict, or None when it has none."""
    node = verdict.get("measurements")
    if not isinstance(node, dict):
        return None
    for step in MEASURE_BASE.get((verdict.get("platform_id"), verdict.get("ui_variant")), []):
        node = node.get(step) if isinstance(node, dict) else None
    if node is None:
        return None
    found = _find(node, key)
    values = [x for x in found if _is_num(x)]
    medians = [x[1] for x in found if isinstance(x, tuple)]
    if not values:
        return None
    lo, hi = min(values), max(values)
    text = num(lo) if lo == hi else "%s to %s" % (num(lo), num(hi))
    if medians and lo != hi:
        text += " (median %s)" % num(medians[0])
    return text


# ---- the checks ------------------------------------------------------------------------------

def oldest_validated(rec, family, variant, web_path=None):
    """(platform, verdict) of the oldest platform of the family with a validated verdict."""
    best = None
    for v in rec.verdicts:
        p = rec.platforms.get(v.get("platform_id"))
        if p is None or p.get("family") != family or v.get("ui_variant") != variant:
            continue
        if v.get("status") not in VALIDATED or (web_path and v.get("web_path") != web_path):
            continue
        if best is None or str(p.get("release_date")) < str(best[0].get("release_date")) or (
                str(p.get("release_date")) == str(best[0].get("release_date"))
                and len(v.get("evidence") or []) > len(best[1].get("evidence") or [])):
            best = (p, v)  # on the same release date, the verdict with the most evidence
    return best


def oldest_ids(rec, family, variant, web_path=None):
    """Ids of the validated platforms released the same day as the oldest one (a platform record
    can describe another build on the same system, so the oldest is not always unique), or None."""
    best = oldest_validated(rec, family, variant, web_path)
    if best is None:
        return None
    ids = set()
    for v in rec.verdicts:
        p = rec.platforms.get(v.get("platform_id"))
        if p is None or p.get("family") != family or v.get("ui_variant") != variant:
            continue
        if v.get("status") in VALIDATED and (not web_path or v.get("web_path") == web_path) \
                and str(p.get("release_date")) == str(best[0].get("release_date")):
            ids.add(p["id"])
    return ", ".join(sorted(ids))


def variant_reason(rec, platform, variant):
    """Why a variant was not attempted on a platform, from the records only (or None)."""
    for v in rec.verdicts_of(platform["id"], variant):
        if v.get("status") == "not_tested" and v.get("reason"):
            return v["reason"]
    if platform.get("family") == "windows" and variant == "modern":
        src = rec.sources.get("winappsdk-support")
        if src and str(platform.get("release_date", "9999"))[:4] < "2015":
            return "the Modern UI technology (WinUI 3) is not available on this system: %s " \
                   "(source winappsdk-support)" % src.get("claim", "")
        s8 = rec.spike("S8")
        if s8 and s8.get("status") == "open":
            return "no WinUI 3 Modern UI has been built: spike S8 (WinUI 3 calling the C++11 " \
                   "backend) is open"
    return None


def conclusion(rec, name_pattern):
    """(platforms, verdicts, label) for the platforms whose os_name matches the pattern."""
    pats = [p for p in rec.platforms.values() if re.search(name_pattern, p.get("os_name", ""))]
    vs = [v for p in pats for v in rec.verdicts
          if v.get("platform_id") == p["id"] and v.get("ui_variant") == "legacy"]
    if not vs:
        return pats, vs, "NOT CONCLUDED: no trial"
    best = None
    for v in vs:
        if v.get("status") == "ok":
            best = ("GO: OK", v) if best is None or best[1].get("status") != "ok" else best
        elif v.get("status") == "ok_restricted" and best is None:
            best = ("GO: OK with restrictions", v)
        elif v.get("status") == "ko" and best is None:
            best = ("NO-GO: KO", v)
    return pats, vs, best[0] if best else "NOT CONCLUDED: no trial"


def xp_conclusion(rec):
    """Like conclusion() for Windows XP. PoC scope rule (spec Clarifications, 2026-11-15): a system
    newer than the lowest one tried is taken as passing when the lowest one passes, so without an
    XP trial a GO on Windows 2000 gives a GO by inference for XP (not a measurement)."""
    pats, vs, lab = conclusion(rec, r"Windows XP")
    if vs:
        return pats, vs, lab
    _, _, w2k = conclusion(rec, r"Windows 2000")
    if w2k.startswith("GO"):
        return pats, vs, "GO by inference: " + w2k.split(": ", 1)[1]
    return pats, vs, lab


def sharing_rows(rec):
    """One row per deliverable record: the shared-code numbers written by tools/measure_sharing.py
    (shared_loc, glue_loc, ui_loc, sharing_ratio) and the UI directory the record maps to."""
    rows = []
    for (pid, variant), d in rec.deliverables.items():
        family = (rec.platforms.get(pid) or {}).get("family") or str(pid).split("-")[0]
        numbers = [d.get(k) for k in ("shared_loc", "glue_loc", "ui_loc", "sharing_ratio")]
        measured = all(_is_num(n) for n in numbers)
        rows.append({"pid": pid, "variant": variant, "record": d,
                     "ui": "%s/%s" % (variant, measure_sharing.UI_DIR.get((family, variant), "?")),
                     "shared": d.get("shared_loc"), "glue": d.get("glue_loc"),
                     "uiloc": d.get("ui_loc"), "ratio": d.get("sharing_ratio") if measured else None,
                     "measured": measured})
    return rows


def pct(ratio):
    return "%.1f%%" % (ratio * 100)


class Check(object):
    def __init__(self, ident, result, summary, details=None):
        self.ident = ident
        self.result = result  # "MET", "NOT MET", "INCOMPLETE", "OPEN"
        self.summary = summary
        self.details = details or []


def run_checks(rec):
    checks = []

    # SC-002: oldest working platform per family; 3 families validated on a system >= 10 years
    per_family = []
    ge10, ge15 = [], []
    for fam in FAMILIES:
        found = oldest_validated(rec, fam, "legacy")
        if fam == "web":
            found = oldest_validated(rec, fam, "legacy", "js") or oldest_validated(
                rec, fam, "legacy", "wasm")
        if found is None:
            per_family.append("%s: %s" % (FAMILY_NAME[fam], NOT_TESTED))
            continue
        p, v = found
        age = p.get("age_years")
        per_family.append("%s: %s, %s, %s years old, %s, evidence: %s" % (
            FAMILY_NAME[fam], (oldest_ids(rec, fam, "legacy") if fam != "web" else p["id"]),
            p.get("release_date"), age, label(v["status"]), evidence_summary(v)))
        if _is_num(age) and age >= 10:
            ge10.append(FAMILY_NAME[fam])
        if _is_num(age) and age >= 15:
            ge15.append(FAMILY_NAME[fam])
    identified = sum(1 for line in per_family if NOT_TESTED not in line)
    checks.append(Check(
        "SC-002", "MET" if len(ge10) >= 3 else "NOT MET",
        "%d of 3 required families validated with the Legacy UI on a system 10 years old or "
        "more (%s); the oldest working platform is identified with evidence for %d of 6 families"
        % (len(ge10), ", ".join(ge10) or "none", identified), per_family))

    # SC-003: one family on a system >= 15 years old, or the impossibility demonstrated
    if ge15:
        checks.append(Check("SC-003", "MET", "validated with the Legacy UI on a system 15 years "
                            "old or more: %s (read the restrictions of that verdict: a virtual "
                            "machine, one build)" % ", ".join(ge15)))
    else:
        proven = []
        for fam in FAMILIES:
            ok = any(v.get("status") == "ko" and v.get("cause") and len(v.get("attempts") or []) >= 2
                     for v in rec.verdicts
                     if rec.platforms.get(v.get("platform_id"), {}).get("family") == fam
                     and v.get("ui_variant") == "legacy")
            proven.append(ok)
        checks.append(Check("SC-003", "MET" if all(proven) else "NOT MET",
                            "no family validated on a system 15 years old or more, and the "
                            "impossibility is %s for every family (a cause and two attempts)"
                            % ("demonstrated" if all(proven) else "not demonstrated")))

    # SC-004 / SC-010: every attempted platform and variant has verdict, evidence, reproduction
    problems = []
    unjustified = []
    for pid, variant in rec.attempted():
        vs = rec.verdicts_of(pid, variant)
        where = "%s %s" % (pid, variant)
        if not vs:
            problems.append("%s: no verdict record (built or run, but no verdict)" % where)
        for v in vs:
            st = v.get("status")
            if st == "not_tested":
                if not v.get("reason"):
                    problems.append("%s: not_tested without a reason" % where)
                continue
            if not v.get("evidence"):
                problems.append("%s: no evidence" % where)
            if not v.get("reproduction"):
                problems.append("%s: missing reproduction (no procedure recorded)" % where)
    for pid in sorted(set(p for p, _ in rec.attempted())):
        for variant in VARIANTS:
            if (pid, variant) in rec.attempted():
                continue
            reason = variant_reason(rec, rec.platforms[pid], variant)
            if reason is None:
                unjustified.append("%s %s: not attempted and no reason recorded" % (pid, variant))
    checks.append(Check(
        "SC-004", "MET" if not problems else "INCOMPLETE",
        "%d attempted platform/variant pair(s); %d gap(s) in verdict, evidence or reproduction"
        % (len(rec.attempted()), len(problems)), problems))
    checks.append(Check(
        "SC-010", "MET" if not unjustified else "INCOMPLETE",
        "%d absent variant(s) without a recorded justification" % len(unjustified), unjustified))

    # SC-005: every hypothesis has a verdict or a reason
    ids = set(h.get("id") for h in rec.hypotheses)
    gaps = ["V%d: missing" % i for i in range(1, 21) if "V%d" % i not in ids]
    for h in rec.hypotheses:
        if h.get("status") == "not_covered" and not h.get("note"):
            gaps.append("%s: not_covered without a reason" % h.get("id"))
    covered = sum(1 for h in rec.hypotheses if h.get("status") != "not_covered")
    checks.append(Check("SC-005", "MET" if not gaps else "INCOMPLETE",
                        "%d of 20 hypotheses recorded, %d with a verdict, %d not covered by this "
                        "probe" % (len(ids), covered, len(rec.hypotheses) - covered), gaps))

    # SC-009: go / no-go on Windows 2000 and Windows XP
    lines = []
    open_count = 0
    for name, pat in (("Windows 2000", r"Windows 2000"), ("Windows XP", r"Windows XP")):
        _, _, lab = xp_conclusion(rec) if name == "Windows XP" else conclusion(rec, pat)
        lines.append("%s: %s" % (name, lab))
        open_count += lab.startswith("NOT CONCLUDED")
    checks.append(Check("SC-009", "MET" if not open_count else "INCOMPLETE",
                        "go / no-go concluded for %d of 2 systems" % (2 - open_count), lines))

    # SC-007: at least 80% of the application code shared, the share published, each gap justified
    rows = sharing_rows(rec)
    measured = [r for r in rows if r["measured"]]
    below = [r for r in measured if r["ratio"] < SHARING_MIN]
    details = ["%s %s: %s (%s shared, %s glue, %s platform-specific lines)" % (
        r["pid"], r["variant"], pct(r["ratio"]), num(r["shared"]), num(r["glue"]), num(r["uiloc"]))
        for r in below]
    details += ["%s %s: not measured (the record has no shared_loc, glue_loc, ui_loc and "
                "sharing_ratio: run tools/measure_sharing.py --write)" % (r["pid"], r["variant"])
                for r in rows if not r["measured"]]
    for r in measured:
        whole = r["shared"] + r["glue"] + r["uiloc"]
        if whole and abs(float(r["shared"]) / whole - r["ratio"]) > 0.00006:
            details.append("%s %s: stored sharing_ratio %s differs from shared_loc / (shared_loc + "
                           "glue_loc + ui_loc)" % (r["pid"], r["variant"], r["ratio"]))
    if not rows:
        checks.append(Check("SC-007", "INCOMPLETE", "no deliverable record, so no shared-code share "
                            "is published"))
    else:
        result = "MET" if measured and not below and len(measured) == len(rows) and not details \
            else ("NOT MET" if below else "INCOMPLETE")
        checks.append(Check(
            "SC-007", result,
            "%d of %d measured platform/variant rows reach %d%% shared code; %d below, each one "
            "explained in section 13 from the records (the explanation does not remove the gap)"
            % (len(measured) - len(below), len(measured), round(SHARING_MIN * 100), len(below)),
            details))
    return checks


# ---- the report ------------------------------------------------------------------------------

def sec_checks(rec, checks):
    out = ["## 1. Checks", "",
           "Success criteria evaluated from the records at generation time "
           "(`python tools/report.py --check`). \"Validated\" means a verdict `ok` or "
           "`ok_restricted`; the age is the `age_years` of the platform record.", ""]
    rows = [(c.ident, c.result, c.summary) for c in checks]
    out += table(["Criterion", "Result", "Reading"], rows)
    out.append("")
    for c in checks:
        if c.details:
            out.append("**%s detail**" % c.ident)
            out.append("")
            out += ["- " + d for d in c.details]
            out.append("")
    return out


def sec_conclusion(rec):
    out = ["## 2. Go / no-go: Windows 2000 and Windows XP (SC-009)", ""]
    # Windows 2000
    pats, vs, lab = conclusion(rec, r"Windows 2000")
    out.append("### Windows 2000: %s" % lab)
    out.append("")
    if not vs:
        out.append("No verdict record exists for Windows 2000.")
        out.append("")
    for v in vs:
        p = rec.platforms[v["platform_id"]]
        m = v.get("measurements") or {}
        d = rec.deliverables.get((p["id"], "legacy"), {})
        out.append("- **Verdict**: %s (decision: %s), record `validation/verdicts/%s-legacy.json`." % (
            label(v.get("status")), v.get("decision", "none recorded"), p["id"]))
        out.append("- **System tried**: %s %s; %s; %s. Released %s (%s years old)." % (
            p.get("os_name"), clip(p.get("os_version"), 100), p.get("environment"),
            clip(str(p.get("environment_detail", "")).split(":")[0], 170), p.get("release_date"),
            p.get("age_years")))
        if d:
            out.append("- **Deliverable**: %s, %s bytes, %s; fallbacks taken: %s." % (
                (d.get("artifact") or {}).get("name"), num((d.get("artifact") or {}).get("size", 0)),
                d.get("compiler"), ", ".join(d.get("fallbacks") or []) or "none"))
        exe = m.get("executable") if isinstance(m.get("executable"), dict) else {}
        for key, text in (("imports", exe.get("imports")), ("message_check", m.get("message_check")),
                          ("exit", m.get("exit")), ("vectors", m.get("vectors"))):
            if text:
                out.append("- **%s** (verdict measurements): %s" % (key.replace("_", " "),
                                                                    clip(text, 300)))
        feats = [feature_name(f) for f in v.get("disabled_features") or []]
        out.append("- **Disabled features**: %s." % (", ".join(feats) or "none recorded"))
        restrictions = v.get("restrictions") or []
        out.append("- **Restrictions**: %d recorded, all in section 12. The first one, in full:"
                   % len(restrictions))
        if restrictions:
            out.append("  - " + restrictions[0])
    s1 = rec.spike("S1")
    if s1:
        res = str(s1.get("result", ""))
        out.append("")
        out.append("**Spike S1 (a MinGW-w64 C++11 static library on Windows 2000 SP4): %s.** %s" % (
            s1.get("status"), clip(res.split(" TRIAL (")[0], 1500)))
        if "STILL UNVERIFIED:" in res:
            out.append("")
            out.append("Still unverified, as recorded in S1: %s" % clip(
                res.split("STILL UNVERIFIED:", 1)[1], 900))
    out.append("")
    out.append("**Conclusion (Windows 2000): %s.**" % lab)
    if any(v.get("status") in VALIDATED for v in vs):
        out.append("The verdict is that of the system actually tried (see its record above); "
                   "SP4, if it is not the system tried, was NOT run: the status of S1 then rests "
                   "on an inference accepted by the owner (a service pack adds or fixes exports "
                   "and keeps those the executable imports), not on a measurement. The result "
                   "is for the machines and builds named in the verdict.")
    out.append("")

    # Windows XP
    pats, vs, lab = xp_conclusion(rec)
    out.append("### Windows XP: %s" % lab)
    out.append("")
    if lab.startswith("GO by inference"):
        out.append("No Windows XP system was run. Under the PoC scope rule (the Legacy UI is tried "
                   "on the lowest testable system of a family only; success there is taken as "
                   "success on every newer system and architecture, and a failure elsewhere would "
                   "be a detail to fix later), the Windows 2000 result above stands for XP. This "
                   "is an inference accepted by the owner, not a measurement: XP DLL export tables "
                   "were never read and the XP-specific behaviours (visual styles, end-of-life "
                   "banner) were not observed.")
        out.append("")
        out.append("**Conclusion (Windows XP): %s.**" % lab)
        out.append("")
        return out
    if vs:
        for v in vs:
            out.append("- Verdict `%s` on `%s` (decision: %s); evidence: %s." % (
                label(v.get("status")), v["platform_id"], v.get("decision", "none recorded"),
                evidence_summary(v)))
        out.append("")
        out.append("**Conclusion (Windows XP): %s.**" % lab)
        out.append("")
        return out
    out.append("No platform record and no verdict exist for Windows XP (no service pack, x86 or "
               "x64): it was never started, so no go / no-go can be recorded and the criterion "
               "SC-009 is not met for XP. What the records say, none of it measured on XP:")
    out.append("")
    s9 = rec.spike("S9")
    shim = next((f for f in rec.fallbacks if f.get("id") == "win2000-getthreadid-shim"), None)
    vis = next((f for f in rec.fallbacks if f.get("id") == "visual-styles"), None)
    w2k = [v for p in rec.platforms.values() if "Windows 2000" in p.get("os_name", "")
           for v in rec.verdicts_of(p["id"], "legacy")]
    w11 = [v for p in rec.platforms.values() if "Windows 11" in p.get("os_name", "")
           for v in rec.verdicts_of(p["id"], "legacy")]
    out.append("**Known (from records and documents, not from a trial on XP)**")
    out.append("")
    if w2k:
        out.append("- The Windows Legacy executable is one source for Windows 2000 to 11; on "
                   "Windows 2000 it passes the checks above. XP is a later system of the same "
                   "family, but its DLL export tables were never read, so that every import "
                   "exists on XP is an expectation, not a result.")
    if shim:
        out.append("- Fallback `%s`: %s" % (shim["id"], clip(shim.get("reason", ""), 330)))
    if vis:
        out.append("- Fallback `%s`: the Common Controls 6 manifest is what XP and later honour; "
                   "it was exercised on Windows 11 only (\"Not run on Windows XP to 8.1\")." % vis["id"])
    if w11:
        out.append("- The 32-bit executable also runs under WOW64 on Windows 11 (verdict "
                   "`windows-11-x64-legacy`), which says nothing about a native 32-bit XP.")
    src = rec.sources.get("eol-windows-10")
    if src:
        out.append("- End of life: %s By that rule XP shows the warning banner; it was shown on "
                   "Windows 2000 and never on XP." % clip(src.get("claim", ""), 160))
    v1 = next((h for h in rec.hypotheses if h.get("id") == "V1"), None)
    v9 = next((h for h in rec.hypotheses if h.get("id") == "V9"), None)
    if v1 and v9:
        out.append("- Milestone G0 asked for the go / no-go on XP through V1 (Rust toolchain) and V9 "
                   "(MFC on XP). Both questions are superseded: Rust and MFC no longer exist in "
                   "the project (hypotheses V1 and V9). The XP question is now \"does the Win32 "
                   "C++11 executable run on XP\", which no trial answers.")
    out.append("")
    out.append("**Not known**: whether the executable loads on XP (SP1, SP2, SP3, x64), whether "
               "the greeting is displayed and updated, the fonts and visual styles, the end-of-life "
               "banner, memory and display time, and every XP-specific behaviour.%s" % (
                   " Spike S9 (Windows 7 to 11 accept the Windows 2000 executable) is %s." %
                   s9.get("status") if s9 else ""))
    out.append("")
    out.append("**Conclusion (Windows XP): NOT CONCLUDED, no trial.** Neither OK, OK with "
               "restrictions nor KO can be recorded. An inference of the kind accepted for "
               "Windows 2000 SP4 would be \"likely OK\", but it is not a measurement and no "
               "verdict is recorded from it. To conclude: one XP SP3 virtual machine, the same "
               "executable and driver as the Windows 2000 trial.")
    out.append("")
    return out


def sec_platforms(rec):
    out = ["## 3. Platforms tried", ""]
    rows = []
    for p in rec.platforms.values():
        rows.append((p["id"], FAMILY_NAME.get(p.get("family"), p.get("family")),
                     "%s %s" % (p.get("os_name"), clip(p.get("os_version"), 70)), p.get("arch"),
                     p.get("release_date"), p.get("age_years"), p.get("environment"),
                     "yes" if p.get("eol") else "no", "yes" if p.get("is_floor") else "no"))
    out += table(["Platform", "Family", "System", "Arch", "Released", "Age (years)",
                  "Environment", "End of life", "Decided floor"], rows)
    out.append("")
    out.append("Environment details are in the platform records. A `vm` is an emulated machine: "
               "its timings and memory are those of the virtual machine, not of real hardware.")
    out.append("")
    return out


def sec_matrix(rec):
    out = ["## 4. Platform x variant x verdict", ""]
    rows = []
    for p in rec.platforms.values():
        for variant in VARIANTS:
            vs = rec.verdicts_of(p["id"], variant)
            system = "%s %s" % (p.get("os_name"), clip(p.get("os_version"), 55))
            if vs:
                for v in vs:
                    wp = v.get("web_path")
                    if v.get("status") == "not_tested":
                        rows.append((p["id"], system, variant, label(v.get("status")), "-", "-", "-",
                                     "not attempted, justified: " + clip(v.get("reason", ""), 330)
                                     + " (full text in section 12)"))
                        continue
                    rows.append((p["id"], system, variant + (" (%s)" % wp if wp else ""),
                                 label(v.get("status")), v.get("decision", "-"),
                                 evidence_summary(v),
                                 v.get("reproduction") or "MISSING: no procedure recorded",
                                 "%d restriction(s), %d disabled feature(s)" % (
                                     len(v.get("restrictions") or []),
                                     len(v.get("disabled_features") or []))))
                continue
            d = rec.deliverables.get((p["id"], variant))
            if d:
                rows.append((p["id"], system, variant, "no verdict (built, never judged)", "-",
                             "none", "MISSING", "deliverable %s, %s bytes" % (
                                 (d.get("artifact") or {}).get("name"),
                                 num((d.get("artifact") or {}).get("size", 0)))))
                continue
            reason = variant_reason(rec, p, variant)
            rows.append((p["id"], system, variant, "not attempted", "-", "-", "-",
                         ("justified: " + reason) if reason else "MISSING: no justification recorded"))
    for fam in FAMILIES:
        if not any(p.get("family") == fam for p in rec.platforms.values()):
            note = "no platform record: no trial"
            rows.append(("(none)", FAMILY_NAME[fam], "both", NOT_TESTED, "-", "-", "-", note))
    out += table(["Platform", "System", "Variant", "Verdict", "Decision", "Evidence",
                  "Reproduction", "Notes"], rows)
    out.append("")
    out.append("`web_path` (`wasm` or `js`) is shown for web verdicts; no web platform was tried. "
               "Other systems named by the requirements document or the spec floors with no "
               "record here are not tested: Windows XP, Windows 10, Windows 11 ARM64 and every "
               "Windows Modern UI, macOS, iOS, Android and the Web.")
    out.append("")
    return out


def sec_oldest(rec):
    out = ["## 5. Oldest working platform per family and variant (FR-005)", ""]
    rows = []
    for fam in FAMILIES:
        paths = (("wasm", "js") if fam == "web" else (None,))
        for variant in VARIANTS:
            for wp in paths:
                found = oldest_validated(rec, fam, variant, wp)
                name = FAMILY_NAME[fam] + (" (%s path)" % wp if wp else "")
                if found is None:
                    rows.append((name, variant, NOT_TESTED, "-", "-", "-", "-"))
                else:
                    p, v = found
                    rows.append((name, variant, oldest_ids(rec, fam, variant, wp),
                                 p.get("release_date"), p.get("age_years"),
                                 label(v["status"]), evidence_summary(v)))
    out += table(["Family", "Variant", "Oldest platform that works", "Released", "Age (years)",
                  "Verdict", "Evidence"], rows)
    out.append("")
    out.append("Several ids in one cell are platform records of the same release date (for example "
               "the 32-bit build run on a 64-bit system of the same distribution): none is older than "
               "the other, and the verdict and evidence shown are those with the most evidence.")
    out.append("")
    return out


def sec_measurements(rec):
    out = ["## 6. Measurements", "",
           "Artifact size comes from the deliverable record; display time and peak memory from "
           "the `measurements` of the verdict (the range over the series of the current build; "
           "the verdict may describe an earlier build, read its notes). `display_ms` is the time "
           "to the first display, `peak_rss_kb` the application's own peak, both read from the "
           "probe line; SC-001 asks for under 3,000 ms.", ""]
    rows = []
    for p in rec.platforms.values():
        for variant in VARIANTS:
            d = rec.deliverables.get((p["id"], variant))
            vs = [v for v in rec.verdicts_of(p["id"], variant) if v.get("status") != "not_tested"]
            if not d and not vs:
                continue
            art = (d or {}).get("artifact") or {}
            size = ("%s: %s bytes" % (art.get("name"), num(art["size"]))) if "size" in art \
                else "not recorded (no deliverable record)"
            if vs:
                disp = measurement(vs[0], "display_ms")
                peak = measurement(vs[0], "peak_rss_kb")
                disp = (disp + " ms") if disp else "not measured: the verdict has no display_ms value"
                peak = (peak + " kB") if peak else "not measured: the verdict has no peak_rss_kb value"
            else:
                disp = peak = "not measured: no verdict, no trial run recorded"
            rows.append((p["id"], variant, size, disp, peak))
    out += table(["Platform", "Variant", "Artifact", "display_ms", "peak_rss_kb"], rows)
    out.append("")
    return out


def sec_fallbacks(rec):
    out = ["## 7. Fallbacks and disabled features", "", "### Fallbacks (FR-021)", ""]
    users = {}
    for (pid, variant), d in rec.deliverables.items():
        for fid in d.get("fallbacks") or []:
            users.setdefault(fid, []).append("%s %s" % (pid, variant))
    rows = []
    for f in rec.fallbacks:
        pids = f.get("platform_ids") or []
        rows.append((f.get("id"), f.get("macro"), clip(f.get("description", ""), 150),
                     ", ".join(pids) if pids else "no platform takes it",
                     ", ".join(users.get(f.get("id"), [])) or "-"))
    out += table(["Fallback", "Macro", "What it replaces", "Platforms taking it (fallback record)",
                  "Deliverables listing it"], rows)
    out.append("")
    out.append("### Disabled features (FR-025)")
    out.append("")
    rows = []
    for v in rec.verdicts:
        if v.get("status") == "not_tested":
            continue
        feats = list(v.get("disabled_features") or [])
        d = rec.deliverables.get((v.get("platform_id"), v.get("ui_variant")), {})
        for f in d.get("disabled_features") or []:
            if feature_name(f) not in [feature_name(x) for x in feats]:
                feats.append(f)
        if not feats:
            rows.append((v["platform_id"], v["ui_variant"], "none recorded", "-"))
        for f in feats:
            rows.append((v["platform_id"], v["ui_variant"], feature_name(f),
                         clip(feature_reason(f), 260)))
    out += table(["Platform", "Variant", "Disabled feature", "Reason"], rows)
    out.append("")
    return out


def sec_spikes(rec):
    out = ["## 8. Spikes", ""]
    for s in rec.spikes:
        res = str(s.get("result") or "")
        if s.get("status") == "open":
            text = "open, no result recorded"
        else:
            text = clip(res.split(" TRIAL (")[0], 1500) if s.get("id") == "S1" else clip(res, 700)
        out.append("- **%s** (%s): %s Result: %s%s" % (
            s.get("id"), s.get("status"), clip(s.get("question", ""), 200), text,
            (" Decision: `%s`." % s["adr"]) if s.get("adr") else ""))
    out.append("")
    out.append("S4 was removed on 2026-11-14 (ADR 0007: Linux binaries are built on the machine "
               "that runs them).")
    out.append("")
    return out


def sec_hypotheses(rec):
    out = ["## 9. Hypotheses V1 to V20 (requirements document, section 15.1.4)", ""]
    counts = OrderedDict()
    for h in rec.hypotheses:
        key = "not covered by this probe" if h.get("status") == "not_covered" else label(
            h.get("status"))
        counts[key] = counts.get(key, 0) + 1
    out.append("Status: " + ", ".join("%d %s" % (n, k) for k, n in counts.items()) + ".")
    out.append("")
    for h in rec.hypotheses:
        status = "NOT COVERED by this probe" if h.get("status") == "not_covered" else label(
            h.get("status"))
        plats = ", ".join(h.get("platform_ids") or []) or "none"
        out.append("- **%s %s**: %s. Platforms: %s. %s" % (h.get("id"), h.get("title"), status,
                                                          plats, h.get("note", "")))
    out.append("")
    return out


def sec_recommendations(rec):
    out = ["## 10. Recommendations for the requirements document", "",
           "### Floors (FR-008, SC-009)", "",
           "\"Contradicted\" means a system older than the floor of the requirements document "
           "works; \"not contradicted\" means only newer systems were tried; neither confirms "
           "the floor. Proposals are the report's, for the owner to decide.", ""]
    rows = []
    for fam, variant, req, decided, year in REQ_FLOORS:
        found = oldest_validated(rec, fam, variant)
        if fam == "web":
            found = oldest_validated(rec, fam, variant, "js") or oldest_validated(
                rec, fam, variant, "wasm")
        tried = ["%s %s: %s" % (p["id"], v["ui_variant"], label(v["status"]))
                 for v in rec.verdicts for p in [rec.platforms.get(v.get("platform_id"), {})]
                 if p.get("family") == fam and v.get("ui_variant") == variant
                 and v.get("status") != "not_tested"]
        if not tried:
            rows.append((FAMILY_NAME[fam] + " " + variant, req, decided, NOT_TESTED,
                         "neither confirmed nor contradicted",
                         "keep the floor wording open until a trial; no change proposed"))
            continue
        p, v = found if found else (None, None)
        if p is not None and int(str(p.get("release_date"))[:4]) < year:
            reading = "contradicted: %s works (%s)" % (
                oldest_ids(rec, fam, variant) or p["id"], label(v["status"]))
            change = "lower the floor of the requirements document to %s, as decided (%s); "                      "the verdict is %s: read its restrictions (section 12) before G0" % (
                         p.get("os_name"), decided, label(v["status"]))
        elif p is not None:
            reading = "not contradicted: only newer systems tried (oldest working: %s)" % (
                oldest_ids(rec, fam, variant) or p["id"])
            change = "reword the floor as decided (%s); older systems are not tried" % decided
        else:
            reading = "no validated platform"
            change = "see the verdicts"
        rows.append((FAMILY_NAME[fam] + " " + variant, req, decided, "; ".join(tried), reading,
                     change))
    out += table(["Family and UI", "Requirements document", "Decided floor (spec)", "Trials",
                  "Reading", "Proposed change"], rows)
    out.append("")
    out.append("The Linux GTK 3.10 floor of the Modern UI is a compile-time rule: the floor build "
               "compiles with the 3.10 symbols only, but only GTK 3.24 was run (verdict of the "
               "Linux Modern UI, restriction on GTK versions).")
    out.append("")
    out.append("### Tiers A / B / C (section 2.1.1)")
    out.append("")
    tier_rows = []
    w2k = [p for p in rec.platforms.values() if "Windows 2000" in p.get("os_name", "")]
    lin = [p for p in rec.platforms.values() if p.get("family") == "linux"]
    tier_a = ["%s %s: %s" % (v["platform_id"], v["ui_variant"], label(v["status"]))
              for v in rec.verdicts
              if rec.platforms.get(v["platform_id"], {}).get("arch") in ("x86_64", "arm64", "arm")]
    tier_rows.append(("A", "complete packaged application (table of section 2.1)",
                      ("; ".join(tier_a) or NOT_TESTED) + ". All plain executables: no package, "
                      "store, Flatpak or AppImage (hypothesis V20 not covered); the other "
                      "platforms of the table: not tested",
                      "tier A is not confirmed for any platform as a packaged application"))
    tier_rows.append(("B", "Windows x86 32-bit, Linux x86 32-bit, ARM 32-bit, RISC-V, PowerPC "
                      "64 LE",
                      ("Windows x86 (%s): %s; " % (w2k[0]["id"], ", ".join(
                          label(v["status"]) for v in rec.verdicts if v["platform_id"] == w2k[0]["id"]))
                       if w2k else "Windows x86: not tested; ") +
                      ("Linux i686 built and run as a 32-bit program on a 64-bit kernel (not a "
                       "native 32-bit system), %s" % (
                           ", ".join("%s %s" % (v["ui_variant"], label(v["status"]))
                                     for v in rec.verdicts if v["platform_id"] == "linux-i686")
                           or "no verdict")
                       if any(p["id"] == "linux-i686" for p in lin) else "Linux i686: not tested")
                      + "; ARM 32-bit, RISC-V, PowerPC: out of scope or not tested",
                      "Windows x86 is reachable by the Legacy UI down to Windows 2000; the tier "
                      "no longer depends on a Rust toolchain, so it should be redefined by "
                      "\"a C++11 compiler and the platform libraries exist\""))
    tier_rows.append(("C", "PowerPC big-endian, s390x, LoongArch64, FreeBSD: CPU engine, CLI, server",
                      "not tested; PowerPC and other architectures are out of scope (spec "
                      "Clarifications)", "no change proposed"))
    out += table(["Tier", "Requirements document", "Trials", "Proposed change"], tier_rows)
    out.append("")
    out.append("### Disabled features (section 2.3, section 2.1.2)")
    out.append("")
    out.append("The disabled features listed for XP in section 2.1.2 (GPU, BLE, TLS, simplified "
               "server, voice, PWA) concern functions the probe does not have: not covered. The "
               "disabled features recorded by the trials are in section 7. Proposed change: add "
               "them to the per-platform list of section 2.3 with their reason, and keep the "
               "other entries unchanged until an extended probe exercises them.")
    out.append("")
    rows = []
    for v in rec.verdicts:
        for f in v.get("disabled_features") or []:
            rows.append((v["platform_id"], v["ui_variant"], feature_name(f),
                         "add to section 2.3 for this platform (reason in the verdict)"))
    out += table(["Platform", "Variant", "Disabled feature", "Proposed change"], rows)
    out.append("")
    out.append("### Decisions of section 16")
    out.append("")
    out += table(["No.", "Decision", "After the probe", "Remark"], DECISIONS)
    out.append("")
    return out


def sec_deviations(rec):
    out = ["## 11. Deviations from the requirements document", ""]
    out += table(["Requirements document", "This project", "Decided in"],
                 [(a, b, c) for a, b, c, _ in DEVIATIONS])
    out.append("")
    return out


def sec_restrictions(rec):
    out = ["## 12. Restrictions and causes as recorded (US3 scenario 2)", ""]
    for v in rec.verdicts:
        if v.get("status") == "not_tested":
            out.append("### %s %s: %s (not attempted)" % (v.get("platform_id"), v.get("ui_variant"),
                                                         label(v.get("status"))))
            out.append("")
            out.append("Justification recorded in the verdict: %s" % v.get("reason", "MISSING"))
            out.append("")
            continue
        out.append("### %s %s: %s (decision: %s)" % (v.get("platform_id"), v.get("ui_variant"),
                                                      label(v.get("status")),
                                                      v.get("decision", "none recorded")))
        out.append("")
        out.append("Evidence: %s. Reproduction: %s." % (
            evidence_summary(v), v.get("reproduction") or "MISSING (no procedure recorded)"))
        out.append("")
        for r in v.get("restrictions") or []:
            out.append("- " + clip(r, 300))
        if v.get("cause"):
            out.append("- Cause: " + clip(v["cause"], 300))
        out.append("")
    return out


def _fallback_mentions(rec, ui, name):
    """Ids of the fallback records whose text names the file ui/<ui>/<name>, either with its whole
    path or in a list after the directory ("ui/legacy/linux-xlib/main.cpp, xftfonts.cpp")."""
    pattern = re.compile(re.escape("ui/%s/" % ui) + r"([\w.\-]+)((?:,\s*[\w\-]+\.(?:cpp|hpp|cc|c|h))*)")
    ids = []
    for f in rec.fallbacks:
        text = " ".join(str(f.get(k, "")) for k in ("description", "reason", "min_compiler"))
        for m in pattern.finditer(text):
            names = [m.group(1).rstrip(".")] + [x.strip() for x in m.group(2).split(",")
                                                if x.strip()]
            if name in names and f.get("id") not in ids:
                ids.append(f.get("id"))
    return ids


def sec_sharing(rec):
    out = ["## 13. Shared code per platform and variant (FR-011, FR-012, SC-007)", ""]
    rows = sharing_rows(rec)
    out.append("Shared code is the backend `core/` (the C++11 library and its C API), glue is the code "
               "between the C API and a UI language (`bridge/jni`, `bridge/wasm`: none exists yet), "
               "platform-specific code is the native UI of the platform and variant "
               "(`ui/<variant>/<platform>/`). Counted: non-empty, non-comment source lines, without "
               "tests, generated code, build and packaging files. Share = shared / (shared + glue + "
               "platform-specific). The numbers are the fields `shared_loc`, `glue_loc`, `ui_loc` and "
               "`sharing_ratio` of the deliverable records, written by "
               "`python tools/measure_sharing.py --write`; SC-007 asks for at least %d%%." % round(
                   SHARING_MIN * 100))
    out.append("")
    table_rows = []
    for r in rows:
        d = r["record"]
        if r["measured"]:
            table_rows.append((r["pid"], r["variant"], r["ui"], clip(d.get("ui_toolkit", ""), 60),
                               num(r["shared"]), num(r["glue"]), num(r["uiloc"]), pct(r["ratio"]),
                               "met" if r["ratio"] >= SHARING_MIN else "BELOW %d%%" % round(
                                   SHARING_MIN * 100)))
        else:
            table_rows.append((r["pid"], r["variant"], r["ui"], clip(d.get("ui_toolkit", ""), 60),
                               "-", "-", "-", "not measured", "not measured"))
    out += table(["Platform", "Variant", "UI directory", "UI toolkit", "Shared lines", "Glue lines",
                  "Platform-specific lines", "Share", "SC-007"], table_rows)
    out.append("")
    groups = OrderedDict()
    for r in rows:
        groups.setdefault(r["ui"], []).append(r["pid"])
    same = ["`ui/%s/`: %s" % (ui, ", ".join(pids)) for ui, pids in groups.items() if len(pids) > 1]
    if same:
        out.append("Records that run the same UI sources have the same numbers, the code is counted once "
                   "per record: " + "; ".join(same) + ".")
        out.append("")
    absent = [FAMILY_NAME[f] for f in FAMILIES if not any(p.get("family") == f
                                                          for p in rec.platforms.values())]
    unmeasured = sorted(set("%s %s" % (p, v) for p in rec.platforms for v in VARIANTS
                            if (p, v) not in rec.deliverables))
    out.append("Not measured: %s have no platform record and no UI code; the variants without a "
               "deliverable record (%s) have no row." % (", ".join(absent) or "no family",
                                                          ", ".join(unmeasured) or "none"))
    out.append("")

    uis = []
    for r in rows:
        if r["measured"] and r["ui"] not in uis:
            uis.append(r["ui"])
    out.append("### Platform-specific code and why it is platform-specific")
    out.append("")
    out.append("Every file counted as platform-specific, with its line count from the stored listing "
               "of the code-sharing evidence (`validation/evidence/code-sharing/`). Reason: the UI of a "
               "platform is native (FR-001, FR-020) and limited to system integration, without "
               "application logic (FR-010); a file that a fallback record names also holds the code of "
               "that fallback (FR-012: the cost of an old or optional platform case in non-shared "
               "code), next to the rest of the native UI.")
    out.append("")
    file_rows = []
    for ui in uis:
        files = rec.code_files["ui"].get(ui)
        if not files:
            file_rows.append(("ui/%s/" % ui, "no file listing stored", "-", "-"))
            continue
        for name, n in files.items():
            ids = _fallback_mentions(rec, ui, name)
            why = ("named by fallback record(s) %s, besides the native UI code" % ", ".join(
                "`%s`" % i for i in ids)) if ids else (
                "native UI of the platform (FR-001, FR-010, FR-020); the text of no fallback record names "
                "this file")
            file_rows.append(("ui/%s/%s" % (ui, name), num(n), why,
                              ", ".join(sorted(set(r["pid"] for r in rows if r["ui"] == ui)))))
    out += table(["File", "Lines", "Why platform-specific", "Used by"], file_rows)
    out.append("")

    exceptions = [r for r in rows if r["measured"] and r["ratio"] < SHARING_MIN]
    out.append("### Exceptions: shares below %d%% (SC-007)" % round(SHARING_MIN * 100))
    out.append("")
    if not exceptions:
        out.append("No measured row is below %d%%." % round(SHARING_MIN * 100))
        out.append("")
        return out
    out.append("%d of %d measured rows are below %d%%. Each reason below is taken from the numbers, the "
               "file listing and the fallback records above; none is a claim about code that was not "
               "measured." % (len(exceptions), len([r for r in rows if r["measured"]]),
                              round(SHARING_MIN * 100)))
    out.append("")
    core = rec.code_files["core"]
    if core:
        biggest = sorted(core.items(), key=lambda kv: (-kv[1], kv[0]))[:3]
        out.append("Common to every row: the shared part is the whole backend, %s lines in %d file(s) "
                   "(largest: %s). The application of this project is the probe, a text field and "
                   "the greeting (`Hello Loomora` reduced on purpose: section 11, spec assumptions, "
                   "FR-001), so there is little shared logic to count, and every platform pays its own "
                   "native UI on top." % (num(sum(core.values())), len(core), ", ".join(
                       "`%s` %s" % (k, num(v)) for k, v in biggest)))
        out.append("")
    for r in exceptions:
        specific = r["glue"] + r["uiloc"]
        percent = int(round(SHARING_MIN * 100))
        limit = r["shared"] * (100 - percent) // percent
        out.append("- **%s %s** (`ui/%s/`): %s shared. %s shared lines, %s platform-specific." % (
            r["pid"], r["variant"], r["ui"], pct(r["ratio"]), num(r["shared"]), num(specific)))
        out.append("  - Arithmetic: for %d%% the platform-specific code could not exceed a quarter of "
                   "the shared code, %s lines; it is %s, %s times that." % (
                       round(SHARING_MIN * 100), num(limit), num(specific),
                       ("%.1f" % (float(specific) / limit)) if limit else "many"))
        files = rec.code_files["ui"].get(r["ui"])
        if files:
            top = sorted(files.items(), key=lambda kv: (-kv[1], kv[0]))[0]
            out.append("  - Platform-specific code: %d files, %s lines; the largest is `%s` with %s "
                       "lines (%d%% of it)." % (len(files), num(sum(files.values())), top[0],
                                                num(top[1]), round(100.0 * top[1] / sum(files.values()))))
            named = [(name, n, _fallback_mentions(rec, r["ui"], name)) for name, n in files.items()]
            named = [x for x in named if x[2]]
            if named:
                total = sum(n for _, n, _ in named)
                out.append("  - Files that a fallback record names (%s): %s lines, %d%% of the "
                           "platform-specific code; these files hold the code of fallbacks (FR-012, "
                           "the old or optional platform cases) and also ordinary UI code, so this "
                           "is not the cost of the fallbacks alone." % (
                               ", ".join("`%s` %s" % (name, num(n)) for name, n, _ in named),
                               num(total), round(100.0 * total / sum(files.values()))))
        same_ui = [o["pid"] for o in rows if o["ui"] == r["ui"] and o is not r]
        if same_ui:
            out.append("  - The same sources as %s: one code base, counted again in this record." %
                       ", ".join(same_ui))
        fb = r["record"].get("fallbacks") or []
        out.append("  - Fallbacks listed by the deliverable record: %s." % (
            ", ".join("`%s`" % f for f in fb) or "none"))
    out.append("")
    out.append("Reading, not measured: the platform-specific part is a native window with its text "
               "display and input, which every platform needs whatever the application does, against "
               "a shared part that is only the probe logic. The records do not say what the share "
               "would be for a larger application, so the report does not claim that %d%% would be "
               "reached: SC-007 is not met by this probe, and the gap is explained here, not closed."
               % round(SHARING_MIN * 100))
    out.append("")
    return out


def sec_changes(rec, comparison):
    out = ["## 14. Verdict changes since a baseline (FR-018)", ""]
    if comparison is None:
        out.append("No baseline was given, so no comparison is made. Generate the report with "
                   "`python tools/report.py --baseline REV` (REV: a git revision of the docs "
                   "submodule, or a folder of verdicts) to list every change of `status` and every "
                   "other difference of the verdicts and deliverable records since REV "
                   "(`tools/diff_verdicts.py`). The committed report is generated without a baseline "
                   "so that it is reproducible; the baseline is not recorded in it.")
        out.append("")
        return out
    out.append("Baseline (old): `%s`. Compared with: %s. Source: `tools/diff_verdicts.py`." % (
        comparison.old.label, comparison.new.label))
    out.append("")
    out.append("`%s`" % comparison.summary())
    out.append("")
    if comparison.status_changes:
        rows = []
        for key, old, new in comparison.status_changes:
            record = comparison.new.verdicts.get(key) or comparison.old.verdicts.get(key) or {}
            rows.append((diff_verdicts._title(key, record), label(old), label(new),
                         diff_verdicts.status_hint(old, new).strip("()") or "-"))
        out += table(["Platform / variant", "Status before", "Status now", "Direction"], rows)
    else:
        out.append("No `status` changed between the baseline and these records.")
    out.append("")
    if comparison.added:
        out.append("Verdicts added: %s." % ", ".join("`%s`" % k for k in comparison.added))
        out.append("")
    if comparison.removed:
        out.append("Verdicts removed: %s." % ", ".join("`%s`" % k for k in comparison.removed))
        out.append("")
    out.append("### Differences, per platform and variant")
    out.append("")
    out.append("```text")
    out += comparison.lines
    out.append("```")
    out.append("")
    return out


def load_comparison(baseline, validation_dir, repo=None):
    """The diff_verdicts Comparison of the baseline (a git revision or a folder) with the records of
    `validation_dir`. Raises diff_verdicts.SetError when a set cannot be read."""
    old = diff_verdicts.load_set(baseline, repo)
    new = diff_verdicts.load_set("dir:" + str(validation_dir))
    new.label = "the records of this report"  # no path in the output: the report stays reproducible
    return diff_verdicts.compare_sets(old, new)


def build(rec, comparison=None):
    checks = run_checks(rec)
    out = ["# Loomora validation report", "",
           "Generated by `python tools/report.py` from the records of "
           "`docs/implementation/validation/` (platforms, verdicts, deliverables, fallbacks, "
           "spikes, hypotheses, sources). Do not edit by hand: change a record and regenerate. "
           "Where no record exists the report says \"%s\"." % NOT_TESTED, ""]
    for part in (sec_checks(rec, checks), sec_conclusion(rec), sec_platforms(rec),
                 sec_matrix(rec), sec_oldest(rec), sec_measurements(rec), sec_fallbacks(rec),
                 sec_spikes(rec), sec_hypotheses(rec), sec_recommendations(rec),
                 sec_deviations(rec), sec_restrictions(rec), sec_sharing(rec),
                 sec_changes(rec, comparison)):
        out += part
    return "\n".join(out).rstrip("\n") + "\n"


def missing_sources(rec):
    """Documents cited by DEVIATIONS that do not exist (repository or docs paths)."""
    missing = []
    for _, _, _, paths in DEVIATIONS:
        for rel in paths:
            ok = (common.ROOT / rel).exists() or (
                rec.docs_root is not None and (rec.docs_root / rel).exists())
            if not ok:
                missing.append(rel)
    return missing


# ---- command line ----------------------------------------------------------------------------

def report_path(validation_dir):
    return Path(validation_dir) / "report.md"


def check(validation_dir, output=None, comparison=None):
    rec = Records.load(validation_dir)
    status = 0
    for c in run_checks(rec):
        print("%-7s %-10s %s" % (c.ident, c.result, c.summary))
    gone = missing_sources(rec) if rec.docs_root and (rec.docs_root / "adr").is_dir() else []
    for rel in gone:
        print("MISSING cited document: %s" % rel)
        status = 1
    path = Path(output) if output else report_path(validation_dir)
    if not path.is_file():
        print("report.md does not exist: run python tools/report.py")
        return 1
    with open(str(path), encoding="utf-8", newline="") as f:
        current = f.read()
    if current.replace("\r\n", "\n") != build(rec, comparison):
        print("report.md is stale: run python tools/report.py")
        return 1
    print("report.md is up to date")
    return status


def generate(validation_dir, output=None, to_stdout=False, comparison=None):
    rec = Records.load(validation_dir)
    text = build(rec, comparison)
    if to_stdout:
        sys.stdout.write(text)
        return 0
    path = Path(output) if output else report_path(validation_dir)
    with open(str(path), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d lines)" % (path, text.count("\n")))
    return 0


# ---- self-test -------------------------------------------------------------------------------

def _p(pid, family, release, age, os_name="Test OS", **extra):
    rec = OrderedDict([("id", pid), ("family", family), ("os_name", os_name),
                       ("os_version", "1.0"), ("arch", "x86"), ("release_date", release),
                       ("age_years", age), ("is_floor", False), ("in_scope", True),
                       ("environment", "vm"), ("environment_detail", "hv"), ("eol", False)])
    rec.update(extra)
    return rec


def _v(pid, variant, status, **extra):
    rec = OrderedDict([("platform_id", pid), ("ui_variant", variant), ("status", status),
                       ("evidence", [{"path": "x", "sha256": "0" * 64, "kind": "screenshot"}]),
                       ("reproduction", "validation/procedures/p.md"), ("decision", "keep")])
    if status == "ok_restricted":
        rec["restrictions"] = ["r"]
    rec.update(extra)
    return rec


def _hyp(i, status="not_covered", note="reason"):
    return OrderedDict([("id", "V%d" % i), ("title", "t"), ("criterion", "c"), ("fallback", "f"),
                        ("covered_by_probe", status != "not_covered"), ("platform_ids", []),
                        ("status", status), ("note", note)])


def _records(platforms, verdicts, hyps=None, deliverables=(), fallbacks=(), spikes=(),
             sources=()):
    hyps = list(hyps) if hyps is not None else [_hyp(i) for i in range(1, 21)]
    return Records(platforms, verdicts, list(deliverables), list(fallbacks), list(spikes), hyps,
                   list(sources))


def self_test():
    failures = []

    def expect(name, condition):
        print("%s %s" % ("ok  " if condition else "FAIL", name))
        if not condition:
            failures.append(name)

    def result(rec, ident):
        return [c for c in run_checks(rec) if c.ident == ident][0]

    old_win = _p("win-old", "windows", "2000-02-17", 26.7, "Windows 2000 Professional")
    lin = _p("lin-1", "linux", "2012-01-01", 14.9)
    mac = _p("mac-1", "macos", "2010-01-01", 16.9)
    new = _p("new-1", "linux", "2024-01-01", 2.5)

    rec = _records([old_win, new], [_v("win-old", "legacy", "ok_restricted"),
                                    _v("new-1", "legacy", "ok")])
    c = result(rec, "SC-002")
    expect("SC-002 not met with one old family", c.result == "NOT MET" and "1 of 3" in c.summary)
    expect("SC-003 met by a family of 15 years", result(rec, "SC-003").result == "MET")

    rec = _records([old_win, lin, mac], [_v("win-old", "legacy", "ok"), _v("lin-1", "legacy", "ok"),
                                         _v("mac-1", "legacy", "ok_restricted")])
    expect("SC-002 met with three old families", result(rec, "SC-002").result == "MET")

    rec = _records([new], [_v("new-1", "legacy", "ok")])
    expect("SC-003 not met without an old family or two attempts",
           result(rec, "SC-003").result == "NOT MET")
    ko = _v("new-1", "legacy", "ko", cause="c", attempts=["a", "b"])
    rec = _records([_p("w", "windows", "2000-01-01", 26.0, "Windows 2000"),
                    _p("l", "linux", "2024-01-01", 2.0), _p("m", "macos", "2010-01-01", 16.0),
                    _p("a", "android", "2010-01-01", 16.0), _p("i", "ios", "2011-01-01", 15.0),
                    _p("b", "web", "2011-01-01", 15.0)],
                   [dict(ko, platform_id=pid) for pid in "wlmaib"])
    expect("SC-003 met by demonstrated impossibility", result(rec, "SC-003").result == "MET")

    rec = _records([new], [_v("new-1", "legacy", "ok", reproduction="")])
    s4 = result(rec, "SC-004")
    expect("SC-004 flags a missing reproduction", s4.result == "INCOMPLETE" and any(
        "missing reproduction" in d for d in s4.details))
    rec = _records([new], [_v("new-1", "legacy", "ok", evidence=[])])
    expect("SC-004 flags missing evidence", any("no evidence" in d
                                                for d in result(rec, "SC-004").details))
    rec = _records([new], [], deliverables=[OrderedDict([("platform_id", "new-1"),
                                                         ("ui_variant", "legacy")])])
    expect("SC-004 flags a built variant with no verdict", any(
        "no verdict record" in d for d in result(rec, "SC-004").details))
    expect("SC-010 flags an absent variant without justification", any(
        "new-1 modern" in d for d in result(rec, "SC-010").details))
    rec = _records([old_win], [_v("win-old", "legacy", "ok")],
                   sources=[{"id": "winappsdk-support", "claim": "WinUI 3 needs Windows 10"}])
    expect("SC-010 accepts a justified absent variant",
           result(rec, "SC-010").result == "MET")

    rec = _records([new], [], hyps=[_hyp(i) for i in range(1, 20)])
    expect("SC-005 flags a missing hypothesis", any("V20" in d for d in result(rec, "SC-005").details))
    rec = _records([new], [], hyps=[_hyp(1, note="")] + [_hyp(i) for i in range(2, 21)])
    expect("SC-005 flags a not_covered hypothesis without reason",
           any("without a reason" in d for d in result(rec, "SC-005").details))

    rec = _records([old_win], [_v("win-old", "legacy", "ok_restricted")])
    text = build(rec)
    expect("Windows 2000 concluded OK with restrictions",
           "### Windows 2000: GO: OK with restrictions" in text)
    expect("Windows XP inferred from Windows 2000 without a trial",
           "### Windows XP: GO by inference: OK with restrictions" in text)
    text = build(_records([new], [_v("new-1", "legacy", "ok")]))
    expect("Windows XP not concluded without any trial",
           "### Windows XP: NOT CONCLUDED: no trial" in text and "no verdict is recorded" in text)
    xp = _p("xp", "windows", "2001-10-25", 25.0, "Windows XP")
    for status, want in (("ok", "GO: OK"), ("ko", "NO-GO: KO")):
        extra = {"cause": "c", "attempts": ["a", "b"]} if status == "ko" else {}
        text = build(_records([xp], [_v("xp", "legacy", status, **extra)]))
        expect("Windows XP %s verdict gives %s" % (status, want),
               "### Windows XP: %s" % want in text)
    expect("every family without a platform says not tested",
           text.count(NOT_TESTED) >= 5 and "macOS" in text)
    expect("the report is deterministic", build(rec) == build(rec))
    expect("the report never prints None", "None" not in build(rec))
    expect("clip cuts at a word", clip("alpha beta gamma", 12) == "alpha beta ...")
    expect("measurement range and median", measurement(
        {"platform_id": "p", "ui_variant": "legacy", "measurements": {"display_ms": {
            "all_20_runs": {"min": 3, "median": 9, "max": 13}}}}, "display_ms")
           == "3 to 13 (median 9)")
    expect("measurement of an earlier build is ignored", measurement(
        {"platform_id": "p", "ui_variant": "legacy", "measurements": {"history": {
            "display_ms": {"min": 1, "max": 2}}}}, "display_ms") is None)

    # a not_tested verdict is the recorded justification of a variant that was not attempted
    rec = _records([new], [_v("new-1", "legacy", "ok"),
                           _v("new-1", "modern", "not_tested", reason="no 32-bit toolkit installed",
                              evidence=[], reproduction=None)])
    expect("a not_tested verdict is not an attempt", rec.attempted() == [("new-1", "legacy")])
    expect("SC-004 and SC-010 accept a not_tested variant with a reason",
           result(rec, "SC-004").result == "MET" and result(rec, "SC-010").result == "MET")
    text = build(rec)
    expect("the matrix shows the justification of a not_tested variant",
           "not attempted, justified: no 32-bit toolkit installed" in text
           and "Justification recorded in the verdict: no 32-bit toolkit installed" in text)
    expect("a not_tested variant has no row in the measurements",
           "| new-1 | modern |" not in "\n".join(sec_measurements(rec)))
    rec = _records([new], [_v("new-1", "legacy", "ok"), _v("new-1", "modern", "not_tested")])
    expect("SC-010 flags a not_tested variant without a reason",
           result(rec, "SC-010").result == "INCOMPLETE")

    # platforms of the same release date are all named as the oldest
    twin = _p("lin-twin", "linux", "2024-01-01", 2.5)
    rec = _records([new, twin], [_v("new-1", "legacy", "ok"), _v("lin-twin", "legacy", "ok")])
    expect("oldest platform: a tie on the release date names both",
           oldest_ids(rec, "linux", "legacy") == "lin-twin, new-1")
    rec = _records([new, twin], [_v("new-1", "legacy", "ok"), _v("lin-twin", "legacy", "ok", evidence=[
        {"path": "a", "sha256": "0" * 64}, {"path": "b", "sha256": "0" * 64}])])
    expect("oldest platform: a tie keeps the verdict with the most evidence",
           oldest_validated(rec, "linux", "legacy")[0]["id"] == "lin-twin")
    rec = _records([new, twin], [_v("lin-twin", "legacy", "ok"), _v("new-1", "legacy", "ok", evidence=[
        {"path": "a", "sha256": "0" * 64}, {"path": "b", "sha256": "0" * 64}])])
    expect("oldest platform: the choice does not depend on the order of the records",
           oldest_validated(rec, "linux", "legacy")[0]["id"] == "new-1")

    # shared code (T107): table, SC-007 check, exceptions
    def deliv(pid, variant, shared, glue, ui, ratio):
        return OrderedDict([("platform_id", pid), ("ui_variant", variant), ("ui_toolkit", "toolkit"),
                            ("fallbacks", ["fb-x"]), ("shared_loc", shared), ("glue_loc", glue),
                            ("ui_loc", ui), ("sharing_ratio", ratio)])

    fb = [{"id": "fb-x", "description": "code in ui/legacy/linux-xlib/main.cpp, extra.cpp. See "
           "ui/legacy/linux-xlib/last.cpp.", "reason": "r",
           "min_compiler": "m", "platform_ids": []}]
    rec = _records([new], [_v("new-1", "legacy", "ok")], fallbacks=fb,
                   deliverables=[deliv("new-1", "legacy", 80, 0, 20, 0.8)])
    expect("SC-007 met at exactly 80%", result(rec, "SC-007").result == "MET")
    rec = _records([new], [_v("new-1", "legacy", "ok")], fallbacks=fb,
                   deliverables=[deliv("new-1", "legacy", 40, 0, 60, 0.4)])
    c = result(rec, "SC-007")
    expect("SC-007 not met below 80% and the row is listed",
           c.result == "NOT MET" and "new-1 legacy: 40.0%" in c.details[0])
    rec.code_files = {"core": OrderedDict([("core/src/a.cpp", 40)]),
                      "ui": OrderedDict([("legacy/linux-xlib", OrderedDict([("main.cpp", 50),
                                                                           ("extra.cpp", 6),
                                                                           ("last.cpp", 3),
                                                                           ("other.cpp", 4)]))])}
    text = build(rec)
    expect("exceptions: the shortfall is computed from the numbers",
           "a quarter of the shared code, 10 lines; it is 60, 6.0 times that" in text)
    expect("exceptions: files named by a fallback record are listed (whole path or list)",
           "(`main.cpp` 50, `extra.cpp` 6, `last.cpp` 3): 59 lines, 94% of the platform-specific code" in text
           and "| ui/legacy/linux-xlib/other.cpp | 4 | native UI of the platform" in text)
    expect("exceptions: no claim beyond the data", "none is a claim about code that was not measured" in text)
    rec = _records([new], [_v("new-1", "legacy", "ok")],
                   deliverables=[OrderedDict([("platform_id", "new-1"), ("ui_variant", "legacy")])])
    c = result(rec, "SC-007")
    expect("SC-007 incomplete when a record has no measured numbers",
           c.result == "INCOMPLETE" and "not measured" in c.details[0])
    rec = _records([new], [_v("new-1", "legacy", "ok")],
                   deliverables=[deliv("new-1", "legacy", 90, 0, 10, 0.5)])
    expect("SC-007 flags a stored ratio that does not match the line counts",
           any("differs" in d for d in result(rec, "SC-007").details))
    tmp_cs = Path(tempfile.mkdtemp(prefix="loomora-report-cs-"))
    try:
        d = tmp_cs / "code-sharing" / "p"
        d.mkdir(parents=True)
        (d / "measure-sharing.txt").write_text(
            "backend core/   5 lines in 1 file(s)\n  core/src/a.cpp      5\n"
            "  ui/modern/linux-gtk3/m/n.cpp    7\n  ui/legacy/x-y/z.h   3\n", encoding="utf-8")
        got = _read_code_sharing(tmp_cs / "code-sharing")
        expect("the file listing of the code-sharing evidence is parsed",
               got["core"] == {"core/src/a.cpp": 5}
               and got["ui"]["modern/linux-gtk3"] == {"m/n.cpp": 7}
               and got["ui"]["legacy/x-y"] == {"z.h": 3})
    finally:
        shutil.rmtree(str(tmp_cs), ignore_errors=True)
    expect("the shared-code section is part of the report", "## 13. Shared code" in build(rec))

    # verdict changes since a baseline (T112)
    rec = _records([new], [_v("new-1", "legacy", "ok")])
    expect("no baseline: the section says so",
           "No baseline was given" in build(rec) and "## 14. Verdict changes" in build(rec))

    tmp = Path(tempfile.mkdtemp(prefix="loomora-report-"))
    try:
        common.write_json(tmp / "validation" / "platforms" / "n.json", new)
        common.write_json(tmp / "validation" / "verdicts" / "n-legacy.json",
                          _v("new-1", "legacy", "ok"))
        out = tmp / "validation" / "report.md"
        expect("--check fails when the report is missing", check(str(tmp / "validation")) == 1)
        generate(str(tmp / "validation"))
        expect("--check passes right after the generation", check(str(tmp / "validation")) == 0)
        common.write_json(tmp / "validation" / "verdicts" / "n-legacy.json",
                          _v("new-1", "legacy", "ko", cause="c", attempts=["a", "b"]))
        expect("--check fails when a record changed", check(str(tmp / "validation")) == 1)
        expect("the report file is written", out.is_file())

        # --baseline: the section holds the output of diff_verdicts.compare_sets()
        old = tmp / "old"
        common.write_json(old / "validation" / "verdicts" / "n-legacy.json",
                          _v("new-1", "legacy", "ok"))
        common.write_json(old / "validation" / "verdicts" / "gone-legacy.json",
                          _v("gone-1", "legacy", "ok"))
        try:
            cmp_ = load_comparison("dir:" + str(old), str(tmp / "validation"))
        except diff_verdicts.SetError as e:
            cmp_ = None
            print("load_comparison failed: %s" % e)
        expect("--baseline: a comparison is built", cmp_ is not None)
        if cmp_ is not None:
            text = build(Records.load(str(tmp / "validation")), cmp_)
            expect("--baseline: the status change is listed with its direction",
                   "| new-1 / legacy | OK | KO | worse |" in text)
            expect("--baseline: a removed verdict is listed", "Verdicts removed: `gone-legacy`" in text)
            expect("--baseline: the differences of diff_verdicts follow",
                   "STATUS CHANGE ok -> ko (worse)" in text and "```text" in text)
            expect("--baseline: the records of the report are not named by a path",
                   "Compared with: the records of this report" in text)
            expect("--baseline: the report is deterministic", text == build(
                Records.load(str(tmp / "validation")), cmp_))
            generate(str(tmp / "validation"), None, False, cmp_)
            expect("--check with the same baseline passes", check(str(tmp / "validation"), None, cmp_) == 0)
            expect("--check without the baseline notices the difference",
                   check(str(tmp / "validation")) == 1)
            generate(str(tmp / "validation"))
        expect("an unreadable baseline stops with status 2", main(
            ["--validation-dir", str(tmp / "validation"), "--baseline", "dir:" + str(tmp / "nothing")])
               == EXIT_NO_DOCS)
    finally:
        shutil.rmtree(str(tmp), ignore_errors=True)

    print("self-test %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


def main(argv=None):
    common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Generate validation/report.md from the records.")
    ap.add_argument("--check", action="store_true", help="print the checks, verify report.md")
    ap.add_argument("--stdout", action="store_true", help="print the report, write nothing")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--validation-dir", default=str(common.VALIDATION))
    ap.add_argument("--output", help="report file (default: <validation-dir>/report.md)")
    ap.add_argument("--baseline", help="compare the verdicts with this git revision of the docs "
                    "submodule or this folder (section 14); without it the section says that no "
                    "baseline was given")
    ap.add_argument("--baseline-repo", help="git repository of the revisions (default "
                    "docs/implementation)")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not Path(args.validation_dir).is_dir():
        print(common.docs_missing_message("no report to generate"))
        return EXIT_NO_DOCS
    comparison = None
    if args.baseline:
        try:
            comparison = load_comparison(args.baseline, args.validation_dir, args.baseline_repo)
        except diff_verdicts.SetError as e:
            print("report: cannot read the baseline: %s" % e, file=sys.stderr)
            return EXIT_NO_DOCS
    if args.check:
        return check(args.validation_dir, args.output, comparison)
    return generate(args.validation_dir, args.output, args.stdout, comparison)


if __name__ == "__main__":
    sys.exit(main())
