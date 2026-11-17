#!/usr/bin/env python
"""Independent checker of `.loomora` project files (spec 002, T037).

Reads a file with the standard library only (`struct`, `zlib` for CRC-32 and inflate, `hashlib`,
`json`; `jsonschema` is used for the schema check when it is installed, a small built-in Draft-07
subset validator otherwise) and verifies it against contracts/project-format.md, data-model.md and
contracts/schemas/. It imports no Loomora code: it proves the claim "readable without Loomora" and
cross-checks the C++ implementation with an unrelated reader.

Usage:
  python tools/check_loomora.py [--json] [--quiet] [--canonical] [--set-limit NAME=VALUE]
                                FILE_OR_DIR [FILE_OR_DIR ...]
  python tools/check_loomora.py --expect EXPECTED.json DIR  also compare multi.loomora with tools/gen_reference.py's JSON
  python tools/check_loomora.py --compare DIR_A DIR_B     byte-identical .loomora files, names matched
  python tools/check_loomora.py --compare-mesh FULL LIGHT  mesh.bin of a full file and of its light copy
  python tools/check_loomora.py --selftest                 built-in references and corpus, fast

Checks, in the order of section 8 of the contract: container (ZIP rules, `mimetype`), manifest,
version and required features, per-entry size / SHA-256 / ZIP CRC-32 and the decompression ratio,
JSON schemas, identifiers and references, numbers, nails against frames, `sequence.bin` and
`mesh.bin` decoding, light-profile rules, thresholds and nail distances. Output follows
contracts/cli-validate.md (one line per issue, one summary line per file; `--json` one object per
file). `--canonical` also checks the deterministic writer rules of section 1.1 (header values,
entry order, compression method per entry).

Exit status (ADR 0015): 0 every file valid (warnings allowed), 1 at least one error (or, for the
comparisons, a difference), 2 usage error or a file that cannot be read.
Importable: check_bytes(data, name="", limits=None, canonical=False) -> Report.

Issue codes follow contracts/c-api-project.md. That table names `zip.*`, `manifest.*` and `schema.*`
only as families; the sub-codes used here are listed in SUBCODES below and are this checker's
reading (the C++ implementation may name them differently within the family).
"""
import argparse
import copy
import hashlib
import json
import math
import os
import re
import struct
import sys
import zlib

try:
    import common
    SCHEMA_DIR = str(common.ROOT / "specs" / "002-loomora-file-format" / "contracts" / "schemas")
except ImportError:  # pragma: no cover - used outside the repository
    common = None
    SCHEMA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "specs",
                              "002-loomora-file-format", "contracts", "schemas")

MIMETYPE = b"application/vnd.loomora+zip"
MAX_MAJOR = 1                      # the format version this checker reads (1.0)
KNOWN_FEATURES = ("surface_mesh", "string_levels", "multi_view_target")
BLOCK = 1 << 20
MIB = 1 << 20
DEFAULT_LIMITS = {                 # data-model.md "Constants"
    "warn_nails": 100000, "warn_steps": 5000000, "warn_triangles": 5000000,
    "warn_total_bytes": 256 * MIB, "warn_items": 1000, "bomb_ratio": 250,
}
NAIL_TOL = 0.01                    # nail inside frame
OVERLAP_TOL = 1e-6                 # nail distance
ORIGIN_TOL = 0.01                  # frame origin (warning)
THUMB_SIDE, THUMB_BYTES = 256, 64 * 1024
JSON_DEPTH = 64
MAX_NAME = 512
SUBCODES = {
    # ZIP container (reading of zip.*)
    "zip.not_zip": "not a ZIP archive (no end of central directory record)",
    "zip.multi_disk": "multi-disk archive",
    "zip.comment": "archive or entry comment (the format has none)",
    "zip.trailing_data": "bytes after the end of central directory record",
    "zip.central_directory": "central directory truncated, inconsistent or out of bounds",
    "zip.zip64": "inconsistent or missing ZIP64 information",
    "zip.bad_name": "unsafe or malformed entry name",
    "zip.duplicate_name": "two names equal after ASCII case folding",
    "zip.symlink": "Unix mode marks a symbolic link, device, socket or FIFO",
    "zip.method": "compression method other than stored (0) and deflate (8)",
    "zip.encrypted": "encrypted entry",
    "zip.local_header": "local header differs from the central directory",
    "zip.overlap": "entries overlap or lie beyond the central directory",
    "zip.inflate": "deflate stream corrupt or longer/shorter than recorded",
    "zip.mimetype": "`mimetype` missing, not first, compressed, with an extra field or wrong",
    # manifest
    "manifest.missing": "manifest.json missing", "manifest.counts": "counts differ from the content",
    "manifest.next_id": "next_id not above an identifier", "manifest.order": "order is not a permutation",
    "manifest.entries": "entries lists mimetype or manifest.json",
    "manifest.features": "required_features differ from the content",
    "manifest.profile": "profile and editable disagree", "manifest.min_reader": "min_reader above the format version",
    # schema
    "schema.parse": "not valid UTF-8 JSON", "schema.duplicate_key": "duplicate key",
    "schema.depth": "JSON deeper than 64", "schema.number": "number out of range in manifest.json",
    "schema.required": "missing field", "schema.type": "wrong type", "schema.enum": "value not allowed",
    "schema.pattern": "pattern mismatch", "schema.range": "value out of range", "schema.invalid": "other schema violation",
    # others
    "id.mismatch": "identifier differs from the entry path", "nail.count": "nail_count differs from nails.json",
    "nail.axis": "axis is not a unit vector", "frame.exclusion_outside": "exclusion outside the outline",
    "seq.record": "invalid record", "seq.order": "segment or end without a previous record of the spool",
    "mesh.header": "header does not match frame.json", "mesh.size": "entry length does not match the header",
    "mesh.normal": "normal is not unit length",
    "light.forbidden_entry": "entry not allowed in a light project", "light.forbidden_field": "field not allowed in a light project",
    "light.mesh_uvs": "light mesh.bin holds UVs", "writer.header": "ZIP header value differs from section 1.1",
    "writer.order": "entry order differs from section 2", "writer.method": "compression method differs from the default",
}


# ---- report ----------------------------------------------------------------------------------
class Issue(object):
    def __init__(self, severity, code, entry, kind, ident, message):
        self.severity, self.code, self.entry = severity, code, entry
        self.kind, self.id, self.message = kind, ident, message

    def as_dict(self):
        item = {"kind": self.kind or "none"}
        if self.id is not None:
            item["id"] = self.id
        return {"severity": self.severity, "code": self.code, "entry": self.entry or "",
                "item": item, "message": self.message}

    def line(self):
        where = "entry=%s" % self.entry if self.entry else ""
        if self.kind and self.id is not None:
            where += "  %s=%s" % (self.kind, self.id)
        return "%-7s %-26s %s  %s" % (self.severity, self.code, where.strip(), self.message)


class Report(object):
    def __init__(self, name=""):
        self.name = name
        self.issues = []
        self._seen = {}

    def err(self, code, entry, message, kind=None, ident=None):
        self.issues.append(Issue("error", code, entry, kind, ident, message))

    def warn(self, code, entry, message, kind=None, ident=None):
        self.issues.append(Issue("warning", code, entry, kind, ident, message))

    def capped(self, code, entry, message, kind=None, ident=None, cap=20, severity="error"):
        """Report at most `cap` issues of one (code, entry); the rest is summarised by finish_caps."""
        key = (code, entry, severity)
        n = self._seen.get(key, 0) + 1
        self._seen[key] = n
        if n <= cap:
            self.issues.append(Issue(severity, code, entry, kind, ident, message))

    def finish_caps(self):
        for (code, entry, severity), n in sorted(self._seen.items()):
            if n > 20:
                self.issues.append(Issue(severity, code, entry, None, None,
                                         "%d more of the same kind not listed" % (n - 20)))

    @property
    def errors(self):
        return [i for i in self.issues if i.severity == "error"]

    @property
    def warnings(self):
        return [i for i in self.issues if i.severity == "warning"]

    @property
    def valid(self):
        return not self.errors

    def codes(self, severity=None):
        return [i.code for i in self.issues if severity is None or i.severity == severity]

    def as_json(self):
        return {"file": self.name, "valid": self.valid, "errors": len(self.errors),
                "warnings": len(self.warnings), "issues": [i.as_dict() for i in self.issues]}

    def summary(self):
        e, w = len(self.errors), len(self.warnings)
        return "%s %s: %d error%s, %d warning%s" % ("OK  " if e == 0 else "FAIL", self.name,
                                                    e, "" if e == 1 else "s", w, "" if w == 1 else "s")


# ---- JSON schema (jsonschema when present, a Draft-07 subset otherwise) ----------------------
_schema_cache = {}


def _load_schemas(schema_dir):
    key = os.path.abspath(schema_dir)
    if key in _schema_cache:
        return _schema_cache[key]

    def rd(n):
        with open(os.path.join(schema_dir, n), encoding="utf-8") as f:
            return json.load(f)
    out = {}
    man = rd("manifest.schema.json")
    # Unknown required features are reported as feature.unsupported (section 8: before schemas).
    man["properties"]["required_features"]["items"].pop("enum", None)
    out["manifest"] = man
    for k in ("frame", "image", "result", "spools"):
        out[k] = rd(k + ".schema.json")
    lay = rd("layout.schema.json")
    for k in ("layout", "nails"):
        s = copy.deepcopy(lay["definitions"][k])
        s["$schema"] = lay.get("$schema", "")
        out[k] = s
    _schema_cache[key] = out
    return out


def _is_type(v, t):
    if t == "object":
        return isinstance(v, dict)
    if t == "array":
        return isinstance(v, list)
    if t == "string":
        return isinstance(v, str)
    if t == "boolean":
        return isinstance(v, bool)
    if t == "null":
        return v is None
    if t == "integer":
        return (isinstance(v, int) and not isinstance(v, bool)) or (isinstance(v, float) and v.is_integer())
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    return True


def _mini(schema, v, path, out, root):
    """Validate `v` against a Draft-07 subset; append (keyword, path, message) to `out`."""
    if "$ref" in schema:
        node = root
        for part in schema["$ref"].lstrip("#/").split("/"):
            node = node[part]
        _mini(node, v, path, out, root)
        return
    if "type" in schema:
        ts = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_is_type(v, t) for t in ts):
            out.append(("type", path, "%r is not of type %s" % (v, "/".join(ts))))
            return
    if "const" in schema and v != schema["const"]:
        out.append(("const", path, "%r was expected" % (schema["const"],)))
    if "enum" in schema and v not in schema["enum"]:
        out.append(("enum", path, "%r is not one of %r" % (v, schema["enum"])))
    isnum = isinstance(v, (int, float)) and not isinstance(v, bool)
    if isnum:
        if "minimum" in schema and v < schema["minimum"]:
            out.append(("minimum", path, "%r is less than the minimum of %r" % (v, schema["minimum"])))
        if "maximum" in schema and v > schema["maximum"]:
            out.append(("maximum", path, "%r is greater than the maximum of %r" % (v, schema["maximum"])))
        if "exclusiveMinimum" in schema and v <= schema["exclusiveMinimum"]:
            out.append(("exclusiveMinimum", path, "%r is not greater than %r" % (v, schema["exclusiveMinimum"])))
        if "exclusiveMaximum" in schema and v >= schema["exclusiveMaximum"]:
            out.append(("exclusiveMaximum", path, "%r is not less than %r" % (v, schema["exclusiveMaximum"])))
    if isinstance(v, str):
        if "maxLength" in schema and len(v) > schema["maxLength"]:
            out.append(("maxLength", path, "string is longer than %d" % schema["maxLength"]))
        if "minLength" in schema and len(v) < schema["minLength"]:
            out.append(("minLength", path, "string is shorter than %d" % schema["minLength"]))
        if "pattern" in schema and not re.search(schema["pattern"], v):
            out.append(("pattern", path, "%r does not match %r" % (v, schema["pattern"])))
    if isinstance(v, dict):
        for k in schema.get("required", []):
            if k not in v:
                out.append(("required", path, "%r is a required property" % k))
        for k, sub in schema.get("properties", {}).items():
            if k in v:
                _mini(sub, v[k], path + [k], out, root)
        ap = schema.get("additionalProperties")
        if isinstance(ap, dict):
            for k, val in v.items():
                if k not in schema.get("properties", {}):
                    _mini(ap, val, path + [k], out, root)
        if "propertyNames" in schema:
            for k in v:
                _mini(schema["propertyNames"], k, path, out, root)
    if isinstance(v, list):
        if "minItems" in schema and len(v) < schema["minItems"]:
            out.append(("minItems", path, "%r is too short" % (v,)))
        if "maxItems" in schema and len(v) > schema["maxItems"]:
            out.append(("maxItems", path, "%r is too long" % (v,)))
        if schema.get("uniqueItems") and len(set(json.dumps(x, sort_keys=True) for x in v)) != len(v):
            out.append(("uniqueItems", path, "%r has non-unique elements" % (v,)))
        if isinstance(schema.get("items"), dict):
            for i, x in enumerate(v):
                _mini(schema["items"], x, path + [i], out, root)
    for sub in schema.get("allOf", []):
        _mini(sub, v, path, out, root)
    if "if" in schema:
        probe = []
        _mini(schema["if"], v, path, probe, root)
        if not probe and "then" in schema:
            _mini(schema["then"], v, path, out, root)
        elif probe and "else" in schema:
            _mini(schema["else"], v, path, out, root)
    if "not" in schema:
        probe = []
        _mini(schema["not"], v, path, probe, root)
        if not probe:
            out.append(("not", path, "%r should not be valid under the forbidden schema" % (v,)))


def schema_errors(schema, instance, engine="auto"):
    """List of (keyword, path, message). engine: "auto" (jsonschema if importable), "jsonschema", "mini"."""
    if engine in ("auto", "jsonschema"):
        try:
            import jsonschema
            cls = jsonschema.validators.validator_for(schema)
            return [(e.validator, list(e.absolute_path), e.message)
                    for e in sorted(cls(schema).iter_errors(instance), key=lambda e: list(map(str, e.absolute_path)))]
        except ImportError:
            if engine == "jsonschema":
                raise
    out = []
    _mini(schema, instance, [], out, schema)
    return out


_RANGE_KW = ("minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minItems", "maxItems",
             "maxLength", "minLength", "uniqueItems")


def schema_code(keyword):
    if keyword == "required":
        return "schema.required"
    if keyword == "type":
        return "schema.type"
    if keyword in ("enum", "const"):
        return "schema.enum"
    if keyword == "pattern":
        return "schema.pattern"
    if keyword in _RANGE_KW:
        return "schema.range"
    return "schema.invalid"


# ---- strict JSON -----------------------------------------------------------------------------
_STRING_RX = re.compile(rb'"(?:[^"\\]|\\.)*"', re.S)
_BRACKET_RX = re.compile(rb"[\[\{\]\}]")


def json_depth(raw):
    depth = peak = 0
    for m in _BRACKET_RX.finditer(_STRING_RX.sub(b'""', raw)):
        if m.group() in (b"[", b"{"):
            depth += 1
            peak = max(peak, depth)
        else:
            depth -= 1
    return peak


class JsonResult(object):
    def __init__(self):
        self.obj = None
        self.ok = False
        self.nonfinite = 0


def parse_json(rep, entry, raw, manifest=False, kind=None, ident=None):
    """Strict parse (section 4): UTF-8 without BOM, no duplicate keys, finite numbers, depth <= 64."""
    res = JsonResult()
    if raw.startswith(b"\xef\xbb\xbf"):
        rep.err("schema.parse", entry, "JSON starts with a byte order mark", kind, ident)
        return res
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        rep.err("schema.parse", entry, "not valid UTF-8: %s" % e, kind, ident)
        return res
    depth = json_depth(raw)
    if depth > JSON_DEPTH:
        rep.err("schema.depth", entry, "JSON nested %d deep (limit %d)" % (depth, JSON_DEPTH), kind, ident)
        return res
    dups = []
    bad = []

    def pairs(items):
        d = {}
        for k, v in items:
            if k in d:
                dups.append(k)
            d[k] = v
        return d

    def pfloat(s):
        v = float(s)
        if math.isinf(v) or math.isnan(v):
            bad.append(s)
        return v

    def pconst(s):
        raise ValueError("%s is not valid JSON" % s)
    try:
        res.obj = json.loads(text, object_pairs_hook=pairs, parse_float=pfloat, parse_constant=pconst)
    except (ValueError, RecursionError) as e:
        rep.err("schema.parse", entry, "invalid JSON: %s" % e, kind, ident)
        return res
    if dups:
        rep.err("schema.duplicate_key", entry, "duplicate key %r" % dups[0], kind, ident)
        return res
    res.ok = True
    res.nonfinite = len(bad)
    if bad:
        if manifest:
            rep.err("schema.number", entry, "number %s is out of range" % bad[0], kind, ident)
            res.ok = False
        else:
            rep.err("num.not_finite", entry, "%d number(s) not finite (first: %s)" % (len(bad), bad[0]), kind, ident)
    return res


# ---- ZIP container ---------------------------------------------------------------------------
class ZEntry(object):
    def __init__(self):
        self.name = ""
        self.name_bytes = b""
        self.tainted = False          # a per-entry container error was reported: content not used
        self.is_dir = False
        self.data = None              # decompressed bytes (kept for .json and .bin of defined entries)
        self.size = None              # actual decompressed size
        self.sha = None
        self.head = b""


_NAME_RX = [
    ("frame", re.compile(r"^frames/(0|[1-9][0-9]*)/frame\.json$")),
    ("mesh", re.compile(r"^frames/(0|[1-9][0-9]*)/mesh\.bin$")),
    ("meshsrc", re.compile(r"^frames/(0|[1-9][0-9]*)/mesh-source\.[A-Za-z0-9]+$")),
    ("layout", re.compile(r"^frames/(0|[1-9][0-9]*)/layouts/(0|[1-9][0-9]*)/layout\.json$")),
    ("nails", re.compile(r"^frames/(0|[1-9][0-9]*)/layouts/(0|[1-9][0-9]*)/nails\.json$")),
    ("image", re.compile(r"^images/(0|[1-9][0-9]*)/image\.json$")),
    ("imgsource", re.compile(r"^images/(0|[1-9][0-9]*)/source\.(?:png|jpg|jpeg|webp)$")),
    ("imgtarget", re.compile(r"^images/(0|[1-9][0-9]*)/target\.png$")),
    ("imgthumb", re.compile(r"^images/(0|[1-9][0-9]*)/thumb\.png$")),
    ("result", re.compile(r"^results/(0|[1-9][0-9]*)/result\.json$")),
    ("sequence", re.compile(r"^results/(0|[1-9][0-9]*)/sequence\.bin$")),
    ("resthumb", re.compile(r"^results/(0|[1-9][0-9]*)/thumb\.png$")),
    ("settings", re.compile(r"^results/(0|[1-9][0-9]*)/settings\.json$")),
]
_ORDER_SUB = {"frame": 0, "mesh": 1, "meshsrc": 2, "layout": 3, "nails": 4, "image": 0, "imgsource": 1,
              "imgtarget": 2, "imgthumb": 3, "result": 0, "sequence": 1, "resthumb": 2, "settings": 3}


def classify(name):
    """(kind, ids) of a defined entry, (None, ()) for an unknown one."""
    if name in ("mimetype", "manifest.json", "spools.json"):
        return name.split(".")[0], ()
    for kind, rx in _NAME_RX:
        m = rx.match(name)
        if m:
            return kind, tuple(int(g) for g in m.groups())
    return None, ()


def order_key(name, index):
    kind, ids = classify(name)
    if kind == "mimetype":
        return (0,)
    if kind == "manifest":
        return (1,)
    if kind == "spools":
        return (2,)
    if kind in ("layout", "nails"):
        return (3, ids[0], 3, ids[1], _ORDER_SUB[kind])
    if kind in ("frame", "mesh", "meshsrc"):
        return (3, ids[0], _ORDER_SUB[kind])
    if kind in ("image", "imgsource", "imgtarget", "imgthumb"):
        return (4, ids[0], _ORDER_SUB[kind])
    if kind in ("result", "sequence", "resthumb", "settings"):
        return (5, ids[0], _ORDER_SUB[kind])
    return (9, index)


def bad_name_reason(nb):
    if len(nb) > MAX_NAME:
        return "longer than %d bytes" % MAX_NAME
    try:
        s = nb.decode("utf-8")
    except UnicodeDecodeError:
        return "not valid UTF-8"
    if not s:
        return "empty"
    if "\\" in s:
        return "contains a backslash"
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in s):
        return "contains a NUL or control character"
    if s.startswith("/"):
        return "absolute path"
    parts = (s[:-1] if s.endswith("/") else s).split("/")
    for p in parts:
        if p == "":
            return "empty path component"
        if p == ".":
            return "'.' component"
        if p == "..":
            return "'..' component"
    if ":" in parts[0]:
        return "':' in the first component (drive letter)"
    return None


def _ascii_fold(s):
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


class ZipInfo(object):
    pass


def find_eocd(data):
    n = len(data)
    sig = b"PK\x05\x06"
    i = data.rfind(sig)
    lowest = max(0, n - 22 - 65535)
    while i >= lowest and i + 22 > n:
        i = data.rfind(sig, 0, i)
    return i if i >= lowest else -1


def parse_zip(data, rep, limits, canonical=False):
    """Container checks (section 1). Returns (entries, cd_offset) or None when nothing can be read."""
    n = len(data)
    e = find_eocd(data)
    if e < 0:
        rep.err("zip.not_zip", "", "not a ZIP archive (no end of central directory record)")
        return None
    disk, cd_disk, n_disk, n_total, cd_size, cd_off, clen = struct.unpack_from("<HHHHIIH", data, e + 4)
    if e + 22 + clen > n:
        rep.err("zip.not_zip", "", "end of central directory record is truncated")
        return None
    if clen:
        rep.err("zip.comment", "", "archive comment of %d bytes" % clen)
    if e + 22 + clen < n:
        rep.err("zip.trailing_data", "", "%d bytes after the end of central directory record" % (n - e - 22 - clen))
    if disk != 0 or cd_disk != 0 or (n_disk != n_total and n_disk != 0xFFFF):
        rep.err("zip.multi_disk", "", "multi-disk archive")
        return None
    struct_start = e
    sat = (n_total == 0xFFFF, cd_size == 0xFFFFFFFF, cd_off == 0xFFFFFFFF)
    loc = e - 20
    if loc >= 0 and data[loc:loc + 4] == b"PK\x06\x07":
        off64 = struct.unpack_from("<Q", data, loc + 8)[0]
        if off64 + 56 > n or data[off64:off64 + 4] != b"PK\x06\x06":
            rep.err("zip.zip64", "", "ZIP64 locator does not lead to a ZIP64 end record")
            return None
        t64, s64, o64 = struct.unpack_from("<QQQ", data, off64 + 32)
        for label, classic, big, saturated in (("entry count", n_total, t64, sat[0]),
                                               ("directory size", cd_size, s64, sat[1]),
                                               ("directory offset", cd_off, o64, sat[2])):
            if not saturated and classic != big:
                rep.err("zip.zip64", "", "%s: %d in the classic record, %d in the ZIP64 record" % (label, classic, big))
                return None
        n_total, cd_size, cd_off = t64, s64, o64
        struct_start = off64
    elif any(sat):
        rep.err("zip.zip64", "", "end record fields are saturated but there is no ZIP64 end record")
        return None
    if cd_off > n or cd_off + cd_size > struct_start:
        rep.err("zip.central_directory", "", "central directory (offset %d, size %d) lies outside the archive" % (cd_off, cd_size))
        return None
    if n_total * 46 > cd_size:
        rep.err("zip.central_directory", "", "%d entries declared but the directory holds only %d bytes" % (n_total, cd_size))
        return None
    cd_end = cd_off + cd_size
    pos = cd_off
    raw = []
    for _ in range(n_total):
        if pos + 46 > cd_end or data[pos:pos + 4] != b"PK\x01\x02":
            rep.err("zip.central_directory", "", "central directory ends early or has a bad record at offset %d" % pos)
            return None
        (made, needed, flags, method, mtime, mdate, crc, csize, usize, nlen, elen, clen2, dstart, iattr,
         eattr, off) = struct.unpack_from("<HHHHHHIIIHHHHHII", data, pos + 4)
        end = pos + 46 + nlen + elen + clen2
        if end > cd_end:
            rep.err("zip.central_directory", "", "central directory record runs past the directory")
            return None
        name_b = data[pos + 46:pos + 46 + nlen]
        extra = data[pos + 46 + nlen:pos + 46 + nlen + elen]
        raw.append(dict(made=made, needed=needed, flags=flags, method=method, time=mtime, date=mdate,
                        crc=crc, csize=csize, usize=usize, extra=extra, comment=clen2, eattr=eattr,
                        off=off, name_b=name_b, cdpos=pos, dstart=dstart))
        pos = end
    if pos != cd_end:
        rep.err("zip.central_directory", "", "%d unexplained bytes inside the central directory" % (cd_end - pos))
    entries = []
    seen = {}
    for r in raw:
        z = ZEntry()
        z.name_bytes = r["name_b"]
        z.name = r["name_b"].decode("utf-8", "replace")
        z.cd = r
        z.is_dir = z.name.endswith("/")
        entries.append(z)
        # ZIP64 extra field: values come in the order usize, csize, offset for saturated fields
        r["z64"] = False
        need = [k for k, v in (("usize", r["usize"]), ("csize", r["csize"]), ("off", r["off"])) if v == 0xFFFFFFFF]
        ex = r["extra"]
        fields = {}
        p = 0
        while p + 4 <= len(ex):
            hid, hsz = struct.unpack_from("<HH", ex, p)
            fields[hid] = ex[p + 4:p + 4 + hsz]
            p += 4 + hsz
        if need:
            body = fields.get(1)
            if body is None or len(body) < 8 * len(need):
                rep.err("zip.zip64", z.name, "field saturated (0xFFFFFFFF) without a matching ZIP64 extra field")
                z.tainted = True
            else:
                for k, v in zip(need, struct.unpack_from("<%dQ" % len(need), body)):
                    r[k] = v
                r["z64"] = True
        if r["comment"]:
            rep.err("zip.comment", z.name, "entry comment of %d bytes" % r["comment"])
        reason = bad_name_reason(r["name_b"])
        if reason:
            rep.err("zip.bad_name", z.name, "unsafe entry name: " + reason)
            z.tainted = True
            continue
        fold = _ascii_fold(z.name[:-1] if z.is_dir else z.name)
        if fold in seen:
            rep.err("zip.duplicate_name", z.name, "equal to %r after ASCII case folding" % seen[fold])
            z.tainted = True
        else:
            seen[fold] = z.name
        if (r["made"] >> 8) == 3:
            mode = (r["eattr"] >> 16) & 0o170000
            if mode not in (0, 0o100000, 0o040000):
                rep.err("zip.symlink", z.name, "Unix file type %o (link, device, socket or FIFO)" % mode)
                z.tainted = True
        if r["flags"] & 0x1 or r["flags"] & 0x40 or r["method"] == 99:
            rep.err("zip.encrypted", z.name, "entry is encrypted")
            z.tainted = True
        elif r["method"] not in (0, 8):
            rep.err("zip.method", z.name, "compression method %d" % r["method"])
            z.tainted = True
    # local headers, bounds and overlaps
    spans = []
    for z in entries:
        r = z.cd
        if z.tainted:
            continue
        off = r["off"]
        if off + 30 > cd_off or data[off:off + 4] != b"PK\x03\x04":
            rep.err("zip.local_header", z.name, "no local header at offset %d" % off)
            z.tainted = True
            continue
        (lneeded, lflags, lmethod, lt, ld, lcrc, lcs, lus, lnl, lel) = struct.unpack_from("<HHHHHIIIHH", data, off + 4)
        lname = data[off + 30:off + 30 + lnl]
        problems = []
        if lname != r["name_b"]:
            problems.append("name")
        if lmethod != r["method"]:
            problems.append("method")
        if not lflags & 0x08:
            lex = data[off + 30 + lnl:off + 30 + lnl + lel]
            if lcs == 0xFFFFFFFF or lus == 0xFFFFFFFF:
                q = 0
                got = None
                while q + 4 <= len(lex):
                    hid, hsz = struct.unpack_from("<HH", lex, q)
                    if hid == 1 and hsz >= 16:
                        got = struct.unpack_from("<QQ", lex, q + 4)
                    q += 4 + hsz
                if got is None:
                    problems.append("ZIP64 sizes")
                else:
                    lus, lcs = got
            if lcrc != r["crc"]:
                problems.append("CRC-32")
            if lcs != r["csize"] or lus != r["usize"]:
                problems.append("sizes")
        if problems:
            rep.err("zip.local_header", z.name, "local header differs from the central directory (%s)" % ", ".join(problems))
            z.tainted = True
            continue
        z.data_off = off + 30 + lnl + lel
        z.local_extra_len = lel
        end = z.data_off + r["csize"]
        if end > cd_off:
            rep.err("zip.overlap", z.name, "entry data ends at %d, beyond the central directory (%d)" % (end, cd_off))
            z.tainted = True
            continue
        spans.append((off, end, z))
    spans.sort(key=lambda t: (t[0], t[1]))
    top = -1
    for off, end, z in spans:
        if off < top:
            rep.err("zip.overlap", z.name, "entry starting at %d overlaps the previous entry (ending at %d)" % (off, top))
            z.tainted = True
        top = max(top, end)
    # mimetype
    files = [z for z in entries if not z.is_dir]
    first = files[0] if files else None
    mt = None
    for z in files:
        if z.name == "mimetype":
            mt = z
    if mt is None:
        rep.err("zip.mimetype", "mimetype", "`mimetype` entry is missing")
    else:
        why = []
        if first is not mt or mt.cd["off"] != 0:
            why.append("not the first entry at offset 0")
        if mt.cd["method"] != 0:
            why.append("not stored")
        if mt.cd["extra"] or getattr(mt, "local_extra_len", 0):
            why.append("has an extra field")
        if not mt.tainted and mt.cd["method"] == 0:
            body = data[mt.data_off:mt.data_off + mt.cd["csize"]]
            if body != MIMETYPE:
                why.append("content is %r" % body[:40])
        if why:
            rep.err("zip.mimetype", "mimetype", "`mimetype` " + "; ".join(why))
    if canonical:
        _canonical_zip(rep, files)
    # content: inflate, CRC, size, SHA-256
    ratio = limits["bomb_ratio"]
    total = 0
    for z in files:
        if z.tainted:
            continue
        _read_entry(z, data, rep, ratio)
        if z.size is not None:
            total += z.size
    return entries, total


def _read_entry(z, data, rep, ratio):
    r = z.cd
    comp = data[z.data_off:z.data_off + r["csize"]]
    keep = classify(z.name)[0] is not None and z.name.endswith((".json", ".bin")) and z.name != "mimetype"
    sha = hashlib.sha256()
    crc = 0
    size = 0
    buf = bytearray() if keep else None
    head = bytearray()
    if r["method"] == 0:
        chunks = [comp]
        size, crc = len(comp), zlib.crc32(comp) & 0xFFFFFFFF
        sha.update(comp)
        head = bytearray(comp[:64])
        if keep:
            buf = bytearray(comp)
        if r["usize"] != r["csize"]:
            rep.err("entry.size", z.name, "stored entry: compressed size %d differs from size %d" % (r["csize"], r["usize"]))
    else:
        d = zlib.decompressobj(-15)
        pending = comp
        try:
            while pending:
                out = d.decompress(pending, 1 << 16)
                pending = d.unconsumed_tail
                if out:
                    size += len(out)
                    crc = zlib.crc32(out, crc)
                    sha.update(out)
                    if len(head) < 64:
                        head += out[:64 - len(head)]
                    if keep:
                        buf += out
                    if size > MIB and size > ratio * max(len(comp), 1):
                        rep.err("entry.bomb", z.name, "decompression ratio above %d:1 after %d bytes (compressed %d)"
                                % (ratio, size, len(comp)))
                        z.tainted = True
                        return
                if d.eof:
                    break
            if not d.eof:
                tail = d.flush()
                if tail:
                    size += len(tail)
                    crc = zlib.crc32(tail, crc)
                    sha.update(tail)
                    if keep:
                        buf += tail
            if not d.eof:
                raise zlib.error("deflate stream ends early")
            if d.unused_data:
                raise zlib.error("%d bytes after the end of the deflate stream" % len(d.unused_data))
        except zlib.error as ex:
            rep.err("zip.inflate", z.name, "corrupt deflate stream: %s" % ex)
            z.tainted = True
            return
        if r["usize"] != size:
            rep.err("entry.size", z.name, "ZIP records %d bytes, the entry holds %d" % (r["usize"], size))
    crc &= 0xFFFFFFFF
    if crc != r["crc"]:
        rep.err("entry.crc", z.name, "CRC-32 expected %08x found %08x" % (r["crc"], crc))
    z.size = size
    z.sha = sha.hexdigest()
    z.head = bytes(head)
    z.data = bytes(buf) if keep else None


def _canonical_zip(rep, files):
    names = [z.name for z in files]
    want = [n for _, n in sorted((order_key(n, i), n) for i, n in enumerate(names))]
    if names != want:
        rep.err("writer.order", "", "entry order differs from section 2 (first difference: %s)"
                % next(a for a, b in zip(names, want) if a != b))
    # the writer option all_stored stores every entry, JSON included: no per-entry method rule then
    all_stored = all(z.cd["method"] == 0 for z in files)
    for z in files:
        r = z.cd
        bad = []
        nonascii = any(b >= 0x80 for b in z.name_bytes)
        if r["made"] != 0x0314:
            bad.append("version made %#06x" % r["made"])
        if r["flags"] & ~0x800 or bool(r["flags"] & 0x800) != nonascii:
            bad.append("flags %#06x" % r["flags"])
        if r["time"] != 0 or r["date"] != 0x21:
            bad.append("DOS time/date")
        if r["eattr"] != 0x81A40000:
            bad.append("external attributes %#010x" % r["eattr"])
        if r["extra"] and not r.get("z64"):
            bad.append("extra field")
        if bad:
            rep.err("writer.header", z.name, "; ".join(bad))
        kind = classify(z.name)[0]
        if kind is not None and not all_stored:
            deflate = z.name.endswith(".json")
            if r["method"] != (8 if deflate else 0):
                rep.err("writer.method", z.name, "method %d (default: %s)" % (r["method"], "deflate" if deflate else "stored"))


# ---- the checker -----------------------------------------------------------------------------
_KINDS = (("frames", "frame"), ("layouts", "layout"), ("images", "image"), ("spools", "spool"), ("results", "result"))


def _png_dims(head):
    if head[:8] == b"\x89PNG\r\n\x1a\n" and head[12:16] == b"IHDR" and len(head) >= 24:
        return struct.unpack(">II", head[16:24])
    return None


def _inside(poly, x, y):
    ins = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            ins = not ins
        j = i
    return ins


def _edge_dist(poly, x, y):
    best = float("inf")
    j = len(poly) - 1
    for i in range(len(poly)):
        x1, y1 = poly[j]
        x2, y2 = poly[i]
        j = i
        dx, dy = x2 - x1, y2 - y1
        L = dx * dx + dy * dy
        t = 0.0 if L == 0 else max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / L))
        best = min(best, math.hypot(x - (x1 + t * dx), y - (y1 + t * dy)))
    return best


def _poly_ok(p):
    return (isinstance(p, list) and all(isinstance(q, list) and len(q) == 2 and
                                        all(isinstance(c, (int, float)) and math.isfinite(c) for c in q) for q in p))


def _finite(*vals):
    return all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in vals)


class Checker(object):
    def __init__(self, data, name, limits, canonical, schema_dir, engine):
        self.data, self.name, self.limits, self.canonical = data, name, limits, canonical
        self.rep = Report(name)
        self.schemas = _load_schemas(schema_dir)
        self.engine = engine
        self.docs = {}               # entry -> parsed document (valid against its schema)
        self.byname = {}

    # -- helpers
    def err(self, *a, **k):
        self.rep.err(*a, **k)

    def warn(self, *a, **k):
        self.rep.warn(*a, **k)

    def load(self, entry, schema_key, kind=None, ident=None, manifest=False):
        """Parse an entry, check it against its schema; returns the document or None."""
        z = self.byname.get(entry)
        if z is None or z.tainted or z.data is None:
            return None
        res = parse_json(self.rep, entry, z.data, manifest, kind, ident)
        if not res.ok:
            return None
        if schema_key is None:
            return res.obj
        errs = schema_errors(self.schemas[schema_key], res.obj, self.engine)
        for kw, path, msg in errs:
            self.err(schema_code(kw), entry, "%s: %s" % (".".join(str(p) for p in path) or "<root>", msg), kind, ident)
        if errs:
            return None
        res.obj["__nonfinite__"] = res.nonfinite if isinstance(res.obj, dict) else 0
        return res.obj

    # -- phases
    def run(self):
        rep = self.rep
        parsed = parse_zip(self.data, rep, self.limits, self.canonical)
        if parsed is None:
            return rep
        entries, total = parsed
        files = [z for z in entries if not z.is_dir]
        self.files = files
        self.byname = dict((z.name, z) for z in files)
        self.total = total
        if "manifest.json" not in self.byname:
            self.err("manifest.missing", "manifest.json", "manifest.json is missing")
            return rep
        mz = self.byname["manifest.json"]
        if mz.tainted or mz.data is None:
            return rep
        mres = parse_json(rep, "manifest.json", mz.data, manifest=True)
        if not mres.ok:
            return rep
        man = mres.obj
        if not isinstance(man, dict):
            self.err("schema.type", "manifest.json", "manifest is not a JSON object")
            return rep
        if not self.version_and_features(man):
            return rep
        merrs = schema_errors(self.schemas["manifest"], man, self.engine)
        for kw, path, msg in merrs:
            self.err(schema_code(kw), "manifest.json", "%s: %s" % (".".join(str(p) for p in path) or "<root>", msg))
        self.man = man
        self.man_ok = not merrs
        self.cross_check_entries(man)
        self.items()
        if self.man_ok:
            self.manifest_consistency(man)
        self.thresholds()
        rep.finish_caps()
        return rep

    def version_and_features(self, man):
        fmt = man.get("format")
        major = fmt.get("major") if isinstance(fmt, dict) else None
        minor = fmt.get("minor") if isinstance(fmt, dict) else None
        if isinstance(major, int) and not isinstance(major, bool) and major > MAX_MAJOR:
            need = man.get("min_reader") if isinstance(man.get("min_reader"), str) else "%d.%s" % (major, minor)
            self.err("version.major", "manifest.json", "This file needs Loomora format version %s or newer" % need)
            return False
        feats = man.get("required_features")
        if isinstance(feats, list):
            unknown = [f for f in feats if f not in KNOWN_FEATURES]
            if unknown:
                self.err("feature.unsupported", "manifest.json", "required feature %s is not supported" % ", ".join(map(str, unknown)))
                return False
        return True

    def cross_check_entries(self, man):
        listed = man.get("entries") if isinstance(man.get("entries"), dict) else None
        if listed is None:
            return
        for k in ("mimetype", "manifest.json"):
            if k in listed:
                self.err("manifest.entries", "manifest.json", "entries lists %s (it must list every entry except mimetype and manifest.json)" % k)
        for z in self.files:
            if z.name in ("mimetype", "manifest.json") or z.tainted and z.size is None:
                continue
            info = listed.get(z.name)
            if info is None:
                if not z.tainted:
                    self.err("entry.unlisted", z.name, "archive entry is not listed in the manifest")
                continue
            if z.size is None or not isinstance(info, dict):
                continue
            if info.get("size") != z.size:
                self.err("entry.size", z.name, "manifest says %s bytes, the entry holds %d" % (info.get("size"), z.size))
            if info.get("sha256") != z.sha:
                self.err("entry.sha256", z.name, "expected %s found %s" % (info.get("sha256"), z.sha))
        present = set(z.name for z in self.files)
        for k in listed:
            if k not in ("mimetype", "manifest.json") and k not in present:
                self.err("entry.missing", k, "listed in the manifest but not in the archive")

    # -- items -------------------------------------------------------------------------------
    def items(self):
        rep = self.rep
        self.light = bool(isinstance(self.man.get("profile"), str) and self.man["profile"] == "light")
        profile_known = self.man.get("profile") in ("full", "light")
        if self.light:
            self.warn("warn.light_project", "manifest.json", "light project: source images, settings and UVs are not included")
        self.failed = set()          # kinds with a document that failed its schema (counts are then unreliable)
        self.bad_ids = {"frame": set(), "layout": set(), "image": set(), "result": set()}
        # spools
        self.spools = {}
        self.spool_list = []
        sp = self.load("spools.json", "spools")
        if sp is None and "spools.json" in self.byname:
            self.failed.add("spool")
        if sp is not None:
            self.spool_list = sp["spools"]
            seen = set()
            for s in self.spool_list:
                if s["id"] in seen:
                    self.err("id.duplicate", "spools.json", "spool %d appears twice" % s["id"], "spool", s["id"])
                seen.add(s["id"])
                self.spools[s["id"]] = s
        # unknown entries and light-forbidden entries
        for z in self.files:
            kind, ids = classify(z.name)
            if z.name in ("mimetype", "manifest.json"):
                continue
            if kind is None:
                if not z.tainted:
                    self.warn("warn.unknown_entry", z.name, "entry unknown to this version (kept)")
            elif self.light and kind in ("imgsource", "imgtarget", "settings", "meshsrc"):
                self.err("light.forbidden_entry", z.name, "not allowed in a light project")
        # frames, layouts, images, results
        self.frames, self.layouts, self.images, self.results = {}, {}, {}, {}
        idcount = {"frame": {}, "layout": {}, "image": {}, "result": {}}
        for z in self.files:
            kind, ids = classify(z.name)
            if kind == "frame":
                d = self.load(z.name, "frame", "frame", ids[0])
                if d is None:
                    self.failed.add("frame")
                    self.bad_ids["frame"].add(ids[0])
                    continue
                self._register("frame", self.frames, idcount, d, z, ids[0])
            elif kind == "layout":
                d = self.load(z.name, "layout", "layout", ids[1])
                if d is None:
                    self.failed.add("layout")
                    self.bad_ids["layout"].add(ids[1])
                    continue
                d["__path_frame__"] = ids[0]
                self._register("layout", self.layouts, idcount, d, z, ids[1])
            elif kind == "image":
                d = self.load(z.name, "image", "image", ids[0])
                if d is None:
                    self.failed.add("image")
                    self.bad_ids["image"].add(ids[0])
                    continue
                self._register("image", self.images, idcount, d, z, ids[0])
            elif kind == "result":
                d = self.load(z.name, "result", "result", ids[0])
                if d is None:
                    self.failed.add("result")
                    self.bad_ids["result"].add(ids[0])
                    continue
                self._register("result", self.results, idcount, d, z, ids[0])
            elif kind == "settings":
                self.load(z.name, None, "result", ids[0])
        self.idcount = idcount
        self.all_ids = {"frame": sorted(self.frames), "layout": sorted(self.layouts), "image": sorted(self.images),
                        "spool": sorted(self.spools), "result": sorted(self.results)}
        for fid in sorted(self.frames):
            self.check_frame(self.frames[fid])
        for lid in sorted(self.layouts):
            self.check_layout(self.layouts[lid])
        for iid in sorted(self.images):
            self.check_image(self.images[iid])
        for rid in sorted(self.results):
            self.check_result(self.results[rid])

    def _register(self, kind, table, idcount, doc, z, path_id):
        ident = doc["id"]
        cnt = idcount[kind]
        cnt[ident] = cnt.get(ident, 0) + 1
        if cnt[ident] > 1:
            self.err("id.duplicate", z.name, "%s %d is defined twice" % (kind, ident), kind, ident)
        elif ident != path_id:
            self.err("id.mismatch", z.name, "%s says id %d but its folder is %d" % (kind, ident, path_id), kind, ident)
        doc["__entry__"] = z.name
        doc["__path_id__"] = path_id
        table.setdefault(ident, doc)

    def entry_data(self, name):
        z = self.byname.get(name)
        return None if z is None or z.tainted else z

    def need_entry(self, entry, ref_from, what, kind, ident):
        if entry not in self.byname:
            self.err("ref.missing", ref_from, "%s %r does not exist" % (what, entry), kind, ident)
            return None
        return self.entry_data(entry)

    # frames
    def check_frame(self, f):
        fid = f["id"]
        ename = f["__entry__"]
        light = self.light
        mesh = f.get("mesh")
        f["__bbox__"] = None
        f["__mesh_triangles__"] = None
        if f["kind"] == "2d":
            poly = f["outline"]["polyline"]
            if len(poly) < 3:
                self.err("schema.range", ename, "outline.polyline needs at least 3 points", "frame", fid)
            elif _poly_ok(poly):
                xs, ys = [p[0] for p in poly], [p[1] for p in poly]
                f["__bbox__"] = ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
                for ex in f["exclusions"]:
                    pts = ex["polyline"]
                    if _poly_ok(pts) and len(pts) >= 3 and len(poly) >= 3:
                        out = [q for q in pts if not (_inside(poly, q[0], q[1]) or _edge_dist(poly, q[0], q[1]) <= NAIL_TOL)]
                        if out:
                            self.err("frame.exclusion_outside", ename, "an exclusion has a vertex outside the outline", "frame", fid)
        else:
            if f["surface"] == "mesh" and mesh is None:
                self.err("schema.required", ename, "surface is 'mesh' but the frame has no mesh description", "frame", fid)
        if mesh is not None:
            mname = "frames/%d/mesh.bin" % f["__path_id__"]
            if light:
                for key in ("construction", "attached_source"):
                    if key in mesh:
                        self.err("light.forbidden_field", ename, "mesh.%s in a light project" % key, "frame", fid)
                if mesh.get("has_uvs"):
                    self.err("light.forbidden_field", ename, "mesh.has_uvs is true in a light project", "frame", fid)
            att = mesh.get("attached_source")
            if att is not None and att not in self.byname:
                self.err("ref.missing", ename, "attached_source %r does not exist" % att, "frame", fid)
            z = self.need_entry(mname, ename, "mesh entry", "frame", fid)
            if z is not None and z.data is not None:
                self.check_mesh(f, z)
        elif "frames/%d/mesh.bin" % f["__path_id__"] in self.byname:
            self.err("mesh.header", "frames/%d/mesh.bin" % f["__path_id__"], "mesh.bin present but frame.json has no mesh", "frame", fid)
        if f["__bbox__"] is not None:
            cx, cy = f["__bbox__"][:2]
            if abs(cx) > ORIGIN_TOL or abs(cy) > ORIGIN_TOL or (len(f["__bbox__"]) > 2 and abs(f["__bbox__"][2]) > ORIGIN_TOL):
                self.warn("warn.origin_offset", ename, "bounding-box centre is at (%s) mm, not at the origin"
                          % ", ".join("%.4g" % c for c in f["__bbox__"]), "frame", fid)

    def check_mesh(self, f, z):
        data = z.data
        fid, mname = f["id"], z.name
        info = f["mesh"]
        L = len(data)

        def bad(code, msg):
            self.err(code, mname, msg, "frame", fid)
        if L < 24 or data[:4] != b"LMMS":
            return bad("mesh.header", "bad magic or entry shorter than the 24-byte header")
        version, flags, reserved, nv, nt = struct.unpack_from("<BBHQQ", data, 4)
        if version != 1 or reserved != 0 or flags & ~3:
            return bad("mesh.header", "version %d, flags %#x, reserved %d" % (version, flags, reserved))
        if nv != info["vertices"] or nt != info["triangles"]:
            return bad("mesh.header", "header counts %d vertices / %d triangles, frame.json says %d / %d"
                       % (nv, nt, info["vertices"], info["triangles"]))
        if bool(flags & 1) != bool(info["has_normals"]) or bool(flags & 2) != bool(info.get("has_uvs", False)):
            return bad("mesh.header", "flags %#x do not match has_normals=%s has_uvs=%s" % (flags, info["has_normals"], info.get("has_uvs", False)))
        data_len = 24 + nv * 12 + nt * 12 + (nv * 12 if flags & 1 else 0) + (nv * 8 if flags & 2 else 0)
        blocks = (data_len + BLOCK - 1) // BLOCK
        if L != data_len + 4 * blocks + 4:
            return bad("mesh.size", "entry is %d bytes, the header implies %d%s" % (L, data_len + 4 * blocks + 4,
                       " (UV section length)" if flags & 2 else ""))
        table = data[data_len:data_len + 4 * blocks]
        (tcrc,) = struct.unpack_from("<I", data, data_len + 4 * blocks)
        if zlib.crc32(table) & 0xFFFFFFFF != tcrc:
            return bad("mesh.crc", "block table CRC-32 mismatch")
        for b in range(blocks):
            (want,) = struct.unpack_from("<I", table, 4 * b)
            if zlib.crc32(data[b * BLOCK:min((b + 1) * BLOCK, data_len)]) & 0xFFFFFFFF != want:
                return bad("mesh.crc", "CRC-32 mismatch in block %d" % b)
        mv = memoryview(data)
        pos = 24
        xs = [float("inf")] * 3
        xe = [float("-inf")] * 3
        nonfinite = False
        for x, y, zc in struct.iter_unpack("<3f", mv[pos:pos + nv * 12]):
            if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(zc)):
                nonfinite = True
                continue
            for k, c in enumerate((x, y, zc)):
                xs[k] = min(xs[k], c)
                xe[k] = max(xe[k], c)
        pos += nv * 12
        if nv and not nonfinite:
            f["__bbox__"] = tuple((xs[k] + xe[k]) / 2.0 for k in range(3))
        badidx = 0
        for (a, b2, c) in struct.iter_unpack("<3I", mv[pos:pos + nt * 12]):
            if a >= nv or b2 >= nv or c >= nv:
                badidx += 1
        pos += nt * 12
        if badidx:
            bad("mesh.index_range", "%d triangle(s) index a vertex >= %d" % (badidx, nv))
        if flags & 1:
            badn = 0
            for x, y, zc in struct.iter_unpack("<3f", mv[pos:pos + nv * 12]):
                if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(zc)):
                    nonfinite = True
                elif abs(math.sqrt(x * x + y * y + zc * zc) - 1.0) > 1e-3:
                    badn += 1
            pos += nv * 12
            if badn:
                bad("mesh.normal", "%d normal(s) not of unit length (tolerance 1e-3)" % badn)
        if flags & 2:
            for u, v in struct.iter_unpack("<2f", mv[pos:pos + nv * 8]):
                if not (math.isfinite(u) and math.isfinite(v)):
                    nonfinite = True
            if self.light:
                bad("light.mesh_uvs", "UV section present in a light project")
        if nonfinite:
            bad("num.not_finite", "mesh holds a non-finite float")
        f["__mesh_triangles__"] = nt
        f["__mesh_ok__"] = True

    # layouts and nails
    def check_layout(self, lay):
        lid = lay["id"]
        ename = lay["__entry__"]
        fid = lay["frame"]
        lay["__nails__"] = None
        frame = self.frames.get(fid)
        if frame is None:
            if fid not in self.bad_ids["frame"]:
                self.err("ref.missing", ename, "frame %d does not exist" % fid, "layout", lid)
        elif frame["__path_id__"] != lay["__path_frame__"]:
            self.err("id.mismatch", ename, "layout says frame %d but is stored under frame %d" % (fid, lay["__path_frame__"]), "layout", lid)
        nname = "frames/%d/layouts/%d/nails.json" % (lay["__path_frame__"], lay["__path_id__"])
        if nname not in self.byname:
            self.err("ref.missing", ename, "layout has no %s" % nname, "layout", lid)
            return
        nd = self.load(nname, "nails", "layout", lid)
        if nd is None:
            return
        nails = nd["nails"]
        lay["__nails__"] = nails
        if lay["nail_count"] != len(nails):
            self.err("nail.count", ename, "nail_count is %d, nails.json holds %d" % (lay["nail_count"], len(nails)), "layout", lid)
        ids = set()
        dup = 0
        for n in nails:
            if n["id"] in ids:
                dup += 1
                self.rep.capped("nail.duplicate_id", nname, "nail id %d appears more than once" % n["id"], "layout", lid)
            ids.add(n["id"])
        lay["__nail_ids__"] = ids
        if len(nails) > self.limits["warn_nails"]:
            self.warn("warn.count", nname, "%d nails (threshold %d)" % (len(nails), self.limits["warn_nails"]), "layout", lid)
        if frame is None:
            return
        pts = []
        three = frame["kind"] == "3d"
        for n in nails:
            if three:
                for key in ("z", "axis", "length", "side", "face"):
                    if key not in n:
                        self.rep.capped("schema.required", nname, "3D nail %d has no %r" % (n["id"], key), "layout", lid)
                if not all(k in n for k in ("z", "axis", "length", "side", "face")):
                    continue
                ax = n["axis"]
                if _finite(*ax) and abs(math.sqrt(sum(a * a for a in ax)) - 1.0) > 1e-6:
                    self.rep.capped("nail.axis", nname, "nail %d axis is not a unit vector" % n["id"], "layout", lid)
                fc = n["face"]
                tri = frame.get("__mesh_triangles__")
                if tri is not None and fc["triangle"] >= tri:
                    self.rep.capped("nail.outside_frame", nname, "nail %d: face.triangle %d >= %d triangles" % (n["id"], fc["triangle"], tri), "layout", lid)
                if _finite(fc["u"], fc["v"]) and (fc["u"] < -1e-6 or fc["v"] < -1e-6 or fc["u"] + fc["v"] > 1 + 1e-6):
                    self.rep.capped("nail.outside_frame", nname, "nail %d: barycentric (u, v) outside the triangle" % n["id"], "layout", lid)
                if _finite(n["x"], n["y"], n["z"], n["diameter"]):
                    pts.append((n["x"], n["y"], n["z"], n["diameter"], n["id"]))
            elif _finite(n["x"], n["y"], n["diameter"]):
                x, y = n["x"], n["y"]
                poly = frame["outline"]["polyline"]
                if _poly_ok(poly) and len(poly) >= 3:
                    if not (_inside(poly, x, y) or _edge_dist(poly, x, y) <= NAIL_TOL):
                        self.rep.capped("nail.outside_frame", nname, "nail %d at (%s, %s) is outside the outline" % (n["id"], x, y), "layout", lid)
                    else:
                        for ex in frame["exclusions"]:
                            ep = ex["polyline"]
                            if _poly_ok(ep) and len(ep) >= 3 and _inside(ep, x, y) and _edge_dist(ep, x, y) > NAIL_TOL:
                                self.rep.capped("nail.outside_frame", nname, "nail %d at (%s, %s) is inside an exclusion" % (n["id"], x, y), "layout", lid)
                                break
                pts.append((x, y, 0.0, n["diameter"], n["id"]))
        self.nail_distance(nname, lid, pts)

    def nail_distance(self, nname, lid, pts):
        if len(pts) < 2:
            return
        cell = max(p[3] for p in pts) or 1.0
        grid = {}
        pairs = 0
        first = None
        for p in pts:
            cx, cy, cz = int(math.floor(p[0] / cell)), int(math.floor(p[1] / cell)), int(math.floor(p[2] / cell))
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for q in grid.get((cx + dx, cy + dy, cz + dz), ()):
                            if math.sqrt((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2) < (p[3] + q[3]) / 2.0 - OVERLAP_TOL:
                                pairs += 1
                                if first is None:
                                    first = (q[4], p[4])
            grid.setdefault((cx, cy, cz), []).append(p)
        if pairs:
            self.warn("warn.nail_distance", nname, "%d nail pair(s) closer than the sum of their radii (first: nails %d and %d)"
                      % (pairs, first[0], first[1]), "layout", lid)

    # images
    def check_image(self, im):
        iid, ename = im["id"], im["__entry__"]
        for key in ("source", "target"):
            if key in im:
                if self.light:
                    self.err("light.forbidden_field", ename, "image.%s in a light project" % key, "image", iid)
                z = self.need_entry(im[key]["entry"], ename, "image %s" % key, "image", iid)
                if z is not None and "size" in im[key] and z.size is not None and im[key]["size"] != z.size:
                    self.err("entry.size", im[key]["entry"], "image.json says %d bytes, the entry holds %d" % (im[key]["size"], z.size), "image", iid)
        self.thumb(im["thumbnail"]["entry"], ename, "image", iid)

    def thumb(self, entry, ref_from, kind, ident):
        z = self.need_entry(entry, ref_from, "thumbnail", kind, ident)
        if z is None or z.size is None:
            return
        dims = _png_dims(z.head)
        if z.size > THUMB_BYTES or (dims and (dims[0] > THUMB_SIDE or dims[1] > THUMB_SIDE)):
            self.warn("warn.thumbnail_size", entry, "thumbnail is %s px, %d bytes (limit %dx%d px, %d bytes)"
                      % ("x".join(map(str, dims)) if dims else "?", z.size, THUMB_SIDE, THUMB_SIDE, THUMB_BYTES), kind, ident)

    # results
    def check_result(self, r):
        rid, ename = r["id"], r["__entry__"]
        frame = self.frames.get(r["frame"])
        lay = self.layouts.get(r["layout"])
        if frame is None and r["frame"] not in self.bad_ids["frame"]:
            self.err("ref.missing", ename, "frame %d does not exist" % r["frame"], "result", rid)
        if lay is None:
            if r["layout"] not in self.bad_ids["layout"]:
                self.err("ref.missing", ename, "layout %d does not exist" % r["layout"], "result", rid)
        elif frame is not None and lay["frame"] != r["frame"] and lay["frame"] in self.frames:
            self.err("ref.layout_of_other_frame", ename, "layout %d belongs to frame %d, not to frame %d" % (r["layout"], lay["frame"], r["frame"]), "result", rid)
        for k, it in enumerate(r["images"]):
            if it["image"] not in self.images and it["image"] not in self.bad_ids["image"]:
                self.err("ref.missing", ename, "image %d does not exist" % it["image"], "result", rid)
            if frame is not None:
                if frame["kind"] == "2d" and "view" in it:
                    self.err("schema.invalid", ename, "images[%d]: a 2D frame takes no view" % k, "result", rid)
                if frame["kind"] == "3d":
                    v = it.get("view")
                    if v is None:
                        self.err("schema.required", ename, "images[%d]: a 3D frame needs a view" % k, "result", rid)
                    elif (v["projection"] == "orthographic" and "size" not in v) or (v["projection"] == "perspective" and "fov_deg" not in v):
                        self.err("schema.required", ename, "images[%d].view: %s needs %s" % (k, v["projection"], "size" if v["projection"] == "orthographic" else "fov_deg"), "result", rid)
        if frame is not None and frame["kind"] == "2d" and len(r["images"]) != 1:
            self.err("schema.range", ename, "a 2D frame takes exactly one image (found %d)" % len(r["images"]), "result", rid)
        seen = set()
        for k, s in enumerate(r["spools"]):
            sp = self.spools.get(s["spool"])
            if sp is None and "spool" not in self.failed:
                self.err("ref.missing", ename, "spool %d does not exist" % s["spool"], "result", rid)
            elif sp is not None and "available_length_mm" in sp and s["length_mm"] > sp["available_length_mm"]:
                self.err("spool.length_over_available", ename, "spool %d: %s mm selected, %s mm available"
                         % (s["spool"], s["length_mm"], sp["available_length_mm"]), "result", rid)
            if s["spool"] in seen:
                self.err("id.duplicate", ename, "spool %d selected twice" % s["spool"], "result", rid)
            seen.add(s["spool"])
        self.thumb(r["thumbnail"]["entry"], ename, "result", rid)
        if "settings" in r:
            if self.light:
                self.err("light.forbidden_field", ename, "result.settings in a light project", "result", rid)
            self.need_entry(r["settings"]["entry"], ename, "settings", "result", rid)
        sname = "results/%d/sequence.bin" % r["__path_id__"]
        z = self.need_entry(sname, ename, "sequence", "result", rid)
        if z is not None and z.data is not None:
            self.check_sequence(r, lay, z)

    def check_sequence(self, r, lay, z):
        data, name, rid = z.data, z.name, r["id"]

        def bad(code, msg):
            self.err(code, name, msg, "result", rid)
        if len(data) < 11 or data[:4] != b"LMSQ":
            return bad("seq.header", "bad magic or entry shorter than header and CRC")
        if data[4] != 1:
            return bad("seq.header", "version %d (expected 1)" % data[4])
        (want,) = struct.unpack_from("<I", data, len(data) - 4)
        if zlib.crc32(data[:-4]) & 0xFFFFFFFF != want:
            return bad("seq.crc", "CRC-32 expected %08x found %08x" % (want, zlib.crc32(data[:-4]) & 0xFFFFFFFF))
        levels = bool(data[5] & 1)
        if levels != bool(r["levels"]):
            bad("seq.header", "header levels flag is %s, result.levels is %s" % (levels, r["levels"]))
        end = len(data) - 4

        def rd_varint(p):
            val, shift, start = 0, 0, p
            while True:
                if p >= end:
                    return None, p
                b = data[p]
                p += 1
                val |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    break
                if p - start >= 5:
                    return -1, p
            if p - start > 1 and data[p - 1] == 0:
                return -1, p            # non-minimal
            if val > 0xFFFFFFFF:
                return -1, p
            return val, p
        count, p = rd_varint(6)
        if count is None or count < 0:
            return bad("seq.header", "record count is not a valid varint")
        nails = lay.get("__nail_ids__") if lay else None
        nsel = len(r["spools"])
        prev = {}
        recs = 0
        agg = {}

        def note(code, msg):
            c = agg.setdefault(code, [0, msg])
            c[0] += 1
        while recs < count:
            nid, p = rd_varint(p)
            if nid is None or p >= end:
                return bad("seq.count", "header says %d records but the data ends after %d" % (count, recs))
            if nid < 0:
                return bad("seq.record", "record %d: nail id is not a minimal varint of at most 5 bytes" % recs)
            fl = data[p]
            p += 1
            typ = (fl >> 1) & 3
            sidx = fl >> 3
            if sidx == 31:
                sidx, p = rd_varint(p)
                if sidx is None or sidx < 0:
                    return bad("seq.record", "record %d: bad spool escape" % recs)
            if levels:
                if p >= end:
                    return bad("seq.count", "header says %d records but the data ends after %d" % (count, recs))
                p += 1
            if typ == 3:
                return bad("seq.record", "record %d: reserved type 3" % recs)
            if nails is not None and nid not in nails:
                note("ref.missing", "step %d refers to nail %d, which the layout does not have" % (recs, nid))
            if sidx >= nsel:
                note("ref.spool_not_selected", "step %d uses spool index %d, the selection has %d spool(s)" % (recs, sidx, nsel))
            if typ in (0, 2) and sidx not in prev:
                note("seq.order", "step %d (%s) has no previous record of spool index %d" % (recs, "segment" if typ == 0 else "end", sidx))
            prev[sidx] = typ
            recs += 1
        if p != end:
            return bad("seq.count", "header says %d records but %d bytes follow them" % (count, end - p))
        if count != r["steps"]:
            bad("seq.count", "sequence holds %d records, result.steps is %d" % (count, r["steps"]))
        for code, (n, msg) in sorted(agg.items()):
            bad(code, msg if n == 1 else "%s (and %d more)" % (msg, n - 1))
        if count > self.limits["warn_steps"]:
            self.warn("warn.count", name, "%d steps (threshold %d)" % (count, self.limits["warn_steps"]), "result", rid)

    # -- manifest consistency ------------------------------------------------------------------
    def manifest_consistency(self, man):
        have = {"frame": len(self.frames) + sum(c - 1 for c in self.idcount["frame"].values()),
                "layout": len(self.layouts) + sum(c - 1 for c in self.idcount["layout"].values()),
                "image": len(self.images) + sum(c - 1 for c in self.idcount["image"].values()),
                "spool": len(self.spool_list),
                "result": len(self.results) + sum(c - 1 for c in self.idcount["result"].values())}
        # items that failed their schema are not counted above: count entries instead
        for kind, rx in (("frame", "frame"), ("layout", "layout"), ("image", "image"), ("result", "result")):
            have[kind] = sum(1 for z in self.files if classify(z.name)[0] == rx)
        for plural, kind in _KINDS:
            if kind in self.failed or (kind == "spool" and "spools.json" not in self.byname):
                continue
            if man["counts"][plural] != have[kind]:
                self.err("manifest.counts", "manifest.json", "counts.%s is %d, the project holds %d" % (plural, man["counts"][plural], have[kind]))
            ids = self.all_ids[kind]
            top = max(ids) if ids else 0
            nx = man["next_id"][kind]
            if ids and nx <= top:
                self.err("manifest.next_id", "manifest.json", "next_id.%s is %d, not above identifier %d" % (kind, nx, top))
            if sorted(man["order"][plural]) != ids:
                self.err("manifest.order", "manifest.json", "order.%s %s is not a permutation of %s" % (plural, man["order"][plural], ids))
        derived = set()
        for f in self.frames.values():
            if f["kind"] == "3d" and "mesh" in f:
                derived.add("surface_mesh")
        for r in self.results.values():
            if r["levels"]:
                derived.add("string_levels")
            if len(r["images"]) > 1:
                derived.add("multi_view_target")
        if set(man["required_features"]) != derived:
            self.err("manifest.features", "manifest.json", "required_features %s, the content needs %s"
                     % (sorted(man["required_features"]), sorted(derived)))
        if man["editable"] != (man["profile"] == "full"):
            self.err("manifest.profile", "manifest.json", "profile %s with editable=%s" % (man["profile"], man["editable"]))
        mr = tuple(int(x) for x in man["min_reader"].split("."))
        if mr > (man["format"]["major"], man["format"]["minor"]):
            self.err("manifest.min_reader", "manifest.json", "min_reader %s is above the format version %d.%d"
                     % (man["min_reader"], man["format"]["major"], man["format"]["minor"]))

    def thresholds(self):
        lim = self.limits
        if self.total > lim["warn_total_bytes"]:
            self.warn("warn.count", "", "%d bytes decompressed (threshold %d)" % (self.total, lim["warn_total_bytes"]))
        counts = {"frames": len(self.frames), "layouts": len(self.layouts), "images": len(self.images),
                  "spools": len(self.spool_list), "results": len(self.results)}
        for plural, n in counts.items():
            if n > lim["warn_items"]:
                self.warn("warn.count", "manifest.json", "%d %s (threshold %d)" % (n, plural, lim["warn_items"]))
        for f in self.frames.values():
            nt = f.get("__mesh_triangles__")
            if nt is not None and nt > lim["warn_triangles"]:
                self.warn("warn.count", "frames/%d/mesh.bin" % f["__path_id__"], "%d triangles (threshold %d)" % (nt, lim["warn_triangles"]), "frame", f["id"])


def check_bytes(data, name="", limits=None, canonical=False, schema_dir=None, engine="auto"):
    """Check the bytes of a .loomora file. Returns a Report."""
    lim = dict(DEFAULT_LIMITS)
    lim.update(limits or {})
    return Checker(data, name, lim, canonical, schema_dir or SCHEMA_DIR, engine).run()


def check_file(path, **kw):
    with open(path, "rb") as f:
        data = f.read()
    return check_bytes(data, os.path.basename(path), **kw)


# ---- expected content (tools/gen_reference.py) -------------------------------------------------
def check_expected(path, expected_path, out=print):
    """Re-derives the counts and the references of a .loomora file with the independent reader and compares
    them with the expected-content JSON of tools/gen_reference.py (core/tests/format/multi.expected.json).
    Returns the number of differences (an invalid file counts as one)."""
    with open(expected_path, "rb") as f:
        want = json.loads(f.read().decode("utf-8"))
    with open(path, "rb") as f:
        data = f.read()
    chk = Checker(data, os.path.basename(path), dict(DEFAULT_LIMITS), False, SCHEMA_DIR, "auto")
    rep = chk.run()
    diffs = []
    if rep.errors:
        diffs.append("the file is not valid: %s" % rep.errors[0].line())
    else:
        def cmp(what, got, exp):
            if got != exp:
                diffs.append("%s: file has %r, expected %r" % (what, got, exp))
        cmp("title", chk.man.get("title"), want["title"])
        cmp("counts", chk.man.get("counts"), want["counts"])
        cmp("required_features", chk.man.get("required_features"), want["required_features"])
        for kind, table, key in (("frame", chk.frames, "frames"), ("layout", chk.layouts, "layouts"),
                                 ("image", chk.images, "images"), ("result", chk.results, "results")):
            cmp("%s ids" % kind, sorted(table), [x["id"] for x in want[key]])
        cmp("spool ids", sorted(chk.spools), [x["id"] for x in want["spools"]])
        for x in want["frames"]:
            d = chk.frames.get(x["id"], {})
            cmp("frame %d name/kind" % x["id"], (d.get("name"), d.get("kind")), (x["name"], x["kind"]))
            if "mesh" in x:
                m = d.get("mesh", {})
                cmp("frame %d mesh counts" % x["id"], (m.get("vertices"), m.get("triangles")),
                    (x["mesh"]["vertices"], x["mesh"]["triangles"]))
        for x in want["layouts"]:
            d = chk.layouts.get(x["id"], {})
            cmp("layout %d name/frame/nail_count" % x["id"], (d.get("name"), d.get("frame"), d.get("nail_count")),
                (x["name"], x["frame"], len(x["nails"])))
        for x in want["images"]:
            d = chk.images.get(x["id"], {})
            cmp("image %d" % x["id"], (d.get("name"), d.get("color_mode"), d.get("shape")),
                (x["name"], x["color_mode"], x["shape"]))
        for x in want["results"]:
            d = chk.results.get(x["id"], {})
            cmp("result %d name/frame/layout/steps" % x["id"], (d.get("name"), d.get("frame"), d.get("layout"), d.get("steps")),
                (x["name"], x["frame"], x["layout"], x["steps"]))
            cmp("result %d images" % x["id"], [i.get("image") for i in d.get("images", [])], x["images"])
            cmp("result %d views" % x["id"], any("view" in i for i in d.get("images", [])), x["views"])
            cmp("result %d spools" % x["id"], [[s["spool"], s["length_mm"]] for s in d.get("spools", [])], x["spools"])
        used = set((d.get("frame"), d.get("layout")) for d in chk.results.values())
        free = sorted([d.get("frame"), i] for i, d in chk.layouts.items() if (d.get("frame"), i) not in used)
        cmp("layouts without result", free, sorted(want["combinations_without_result"]))
    for line in diffs:
        out("expected: %s" % line)
    out("%s: %s" % (path, "matches %s" % os.path.basename(expected_path) if not diffs else "%d difference(s)" % len(diffs)))
    return len(diffs)


# ---- comparisons -----------------------------------------------------------------------------
def compare_dirs(a, b, out=print):
    """Byte-identical .loomora files of two directories, names matched. Returns the number of differences."""
    def listing(d):
        return dict((n, os.path.join(d, n)) for n in sorted(os.listdir(d)) if n.endswith(".loomora"))
    la, lb = listing(a), listing(b)
    diffs = 0
    for n in sorted(set(la) | set(lb)):
        if n not in la or n not in lb:
            out("DIFF %s: only in %s" % (n, "B" if n not in la else "A"))
            diffs += 1
            continue
        with open(la[n], "rb") as f:
            da = f.read()
        with open(lb[n], "rb") as f:
            db = f.read()
        if da == db:
            out("same %s (%d bytes, sha256 %s)" % (n, len(da), hashlib.sha256(da).hexdigest()[:16]))
        else:
            first = next((i for i, (x, y) in enumerate(zip(da, db)) if x != y), min(len(da), len(db)))
            out("DIFF %s: %d vs %d bytes, first difference at offset %d" % (n, len(da), len(db), first))
            diffs += 1
    if not la and not lb:
        out("no .loomora file in either directory")
        diffs += 1
    return diffs


def _mesh_sections(data):
    """(flags, vertices, triangles, positions+indices+normals bytes, uv bytes, whole entry) of a mesh.bin."""
    if len(data) < 24 or data[:4] != b"LMMS":
        raise ValueError("not a mesh.bin")
    _, flags, _, nv, nt = struct.unpack_from("<BBHQQ", data, 4)
    core = 24 + nv * 12 + nt * 12 + (nv * 12 if flags & 1 else 0)
    uv = nv * 8 if flags & 2 else 0
    return flags, nv, nt, data[24:core], data[core:core + uv]


def compare_mesh(full_path, light_path, out=print):
    """Light rule of the contract (sections 5.2 and 7): positions, indices and normals of every mesh are
    byte-for-byte the same in the full file and in its light copy; the entry's SHA-256 is equal when the
    full file has no UVs (nothing to drop) and differs when it has (the light entry is rewritten)."""
    problems = 0
    metas = {}
    for label, path in (("full", full_path), ("light", light_path)):
        with open(path, "rb") as f:
            data = f.read()
        rep = Report(path)
        parsed = parse_zip(data, rep, dict(DEFAULT_LIMITS))
        if parsed is None or rep.errors:
            out("%s: container errors: %s" % (label, ", ".join(rep.codes("error"))))
            return 1
        metas[label] = dict((z.name, z) for z in parsed[0] if not z.is_dir)
        profile = None
        mz = metas[label].get("manifest.json")
        if mz is not None and mz.data:
            profile = json.loads(mz.data.decode("utf-8")).get("profile")
        if profile != label:
            out("%s: manifest profile is %r, expected %r" % (path, profile, label))
            problems += 1
    names = sorted(n for n in metas["full"] if n.endswith("/mesh.bin") and n in metas["light"])
    if not names:
        out("no mesh.bin in common")
        return 1
    for n in names:
        zf, zl = metas["full"][n], metas["light"][n]
        try:
            ff, vf, tf, coref, uvf = _mesh_sections(zf.data)
            fl, vl, tl, corel, uvl = _mesh_sections(zl.data)
        except ValueError as e:
            out("%s: %s" % (n, e))
            problems += 1
            continue
        if fl & 2:
            out("DIFF %s: the light mesh still has UVs" % n)
            problems += 1
        elif (vf, tf, ff & 1) != (vl, tl, fl & 1) or coref != corel:
            out("DIFF %s: positions, indices or normals differ between full and light" % n)
            problems += 1
        elif (zf.sha == zl.sha) == bool(ff & 2):
            out("DIFF %s: SHA-256 is %s although the full mesh %s UVs" % (n, "equal" if zf.sha == zl.sha else "different",
                                                                       "has" if ff & 2 else "has no"))
            problems += 1
        else:
            out("same %s (geometry identical; entry sha256 %s: full %s UVs)" % (n, "equal" if zf.sha == zl.sha else "differs", "has" if ff & 2 else "has no"))
    return problems


# ---- command line ----------------------------------------------------------------------------
def parse_limits(pairs):
    lim = {}
    for p in pairs or []:
        k, _, v = p.partition("=")
        if k not in DEFAULT_LIMITS or not v.isdigit():
            raise ValueError("bad --set-limit %r (names: %s)" % (p, ", ".join(sorted(DEFAULT_LIMITS))))
        lim[k] = int(v)
    return lim


def main(argv=None):
    if common is not None:
        common.setup_utf8_output()
    ap = argparse.ArgumentParser(description="Independent checker of .loomora files (spec 002, T037).")
    ap.add_argument("files", nargs="*", help=".loomora files or directories holding them")
    ap.add_argument("--json", action="store_true", help="one JSON object per file on one line")
    ap.add_argument("--quiet", action="store_true", help="print only the summary lines")
    ap.add_argument("--canonical", action="store_true", help="also check the deterministic writer rules (section 1.1)")
    ap.add_argument("--set-limit", action="append", metavar="NAME=VALUE", help="lower a warning threshold (test only)")
    ap.add_argument("--schemas", default=SCHEMA_DIR, help="directory of the JSON schemas")
    ap.add_argument("--compare", nargs=2, metavar=("DIR_A", "DIR_B"), help="byte-identical .loomora files of two directories")
    ap.add_argument("--compare-mesh", nargs=2, metavar=("FULL", "LIGHT"), help="mesh.bin of a full file and of its light copy")
    ap.add_argument("--expect", metavar="EXPECTED_JSON", help="also compare the file(s) with the expected-content JSON "
                    "of tools/gen_reference.py (the file named multi.loomora only)")
    ap.add_argument("--selftest", action="store_true", help="built-in references and corpus (fast)")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    try:
        limits = parse_limits(args.set_limit)
    except ValueError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    if args.selftest:
        return selftest()
    if args.compare:
        for d in args.compare:
            if not os.path.isdir(d):
                print("error: %s is not a directory" % d, file=sys.stderr)
                return 2
        return 1 if compare_dirs(*args.compare) else 0
    if args.compare_mesh:
        for f in args.compare_mesh:
            if not os.path.isfile(f):
                print("error: cannot read %s" % f, file=sys.stderr)
                return 2
        return 1 if compare_mesh(*args.compare_mesh) else 0
    if not args.files:
        ap.print_usage(sys.stderr)
        return 2
    paths = []
    for f in args.files:
        if os.path.isdir(f):
            paths += [os.path.join(f, n) for n in sorted(os.listdir(f)) if n.endswith(".loomora")]
        else:
            paths.append(f)
    status = 0
    for path in paths:
        try:
            rep = check_file(path, limits=limits, canonical=args.canonical, schema_dir=args.schemas)
        except (OSError, IOError) as e:
            print("error: cannot read %s: %s" % (path, e), file=sys.stderr)
            status = 2
            continue
        except Exception as e:  # a bug of this checker: never let a hostile file end in a traceback
            print("error: internal error while checking %s: %r" % (path, e), file=sys.stderr)
            status = 2
            continue
        rep.name = path
        if args.expect and os.path.basename(path) == "multi.loomora":
            if check_expected(path, args.expect):
                status = max(status, 1)
        if args.json:
            print(json.dumps(rep.as_json(), ensure_ascii=False, separators=(",", ":")))
        else:
            if not args.quiet:
                for i in rep.issues:
                    print(i.line())
            print(rep.summary())
        if rep.errors and status < 1:
            status = 1
    return status


# ---- self-test -------------------------------------------------------------------------------
def selftest():
    """Valid references pass (also with --canonical), hostile ones fail with the expected code, the
    two schema engines agree, comparison helpers work. Quick: no truncation sweep."""
    import io
    import tempfile
    import zipfile
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import loomora_pyfmt as pf
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
            print("FAIL %s" % what)

    refs = [("basic", pf.basic_project().build()), ("mesh3d", pf.mesh3d_project().build()),
            ("mesh3d-uv", pf.mesh3d_project(uvs=True).build()), ("two-frames", pf.two_frames_project().build()),
            ("light", pf.basic_project("light").build()), ("mesh3d-light", pf.mesh3d_project("light").build()),
            ("all-stored", pf.basic_project().build(all_stored=True))]
    for name, data in refs:
        zipfile.ZipFile(io.BytesIO(data)).testzip()
        for canon in (False, True):
            rep = check_bytes(data, name, canonical=canon)
            bad = [i for i in rep.issues if not (i.code == "warn.light_project")]
            check(not bad, "reference %s (canonical=%s) has issues: %s" % (name, canon, [i.line() for i in bad]))
    # both schema engines give the same verdict on a bad document
    try:
        import jsonschema  # noqa: F401
        have_js = True
    except ImportError:
        have_js = False
    schemas = _load_schemas(SCHEMA_DIR)
    p = pf.basic_project()
    doc = p.doc("frames/1/frame.json")
    good = [x for x in schema_errors(schemas["frame"], doc, "mini")]
    check(not good, "mini validator rejects a valid frame: %s" % good)
    bad_doc = dict(doc)
    bad_doc["kind"] = "4d"
    del bad_doc["background"]
    mini = sorted((k, tuple(map(str, pth))) for k, pth, _ in schema_errors(schemas["frame"], bad_doc, "mini"))
    check(len(mini) >= 2, "mini validator finds the two defects")
    if have_js:
        js = sorted((k, tuple(map(str, pth))) for k, pth, _ in schema_errors(schemas["frame"], bad_doc, "jsonschema"))
        check(set(k for k, _ in js if k in ("required", "enum")) == set(k for k, _ in mini if k in ("required", "enum")),
              "schema engines disagree: %s vs %s" % (js, mini))
    else:
        print("note: jsonschema is not installed, the built-in validator is used alone")
    # a mesh of two 1 MiB blocks: valid, then one byte of the second block flipped
    verts = [(i % 100 - 49.5, (i // 100) % 100 - 49.5, 0.0) for i in range(50000)]
    big = pf.mesh3d_project()
    pos = [c for v in verts for c in v]
    idx = [k for t in range(50000) for k in (t, (t + 1) % 50000, (t + 2) % 50000)]
    nrm = [c for _ in verts for c in (0.0, 0.0, 1.0)]
    mb = pf.mesh_bin(pos, idx, nrm)
    big.set("frames/1/mesh.bin", mb)
    d = big.doc("frames/1/frame.json")
    d["mesh"].update(vertices=50000, triangles=50000)
    big.set("frames/1/frame.json", d)
    rep = check_bytes(big.build(), "bigmesh")
    check(not rep.issues, "two-block mesh: %s" % [i.line() for i in rep.issues])
    flipped = bytearray(mb)
    flipped[pf.BLOCK + 5] ^= 1
    big.set("frames/1/mesh.bin", bytes(flipped))
    rep = check_bytes(big.build(), "bigmesh-bad")
    check(rep.codes("error") == ["mesh.crc"] and "block 1" in rep.errors[0].message, "two-block mesh corruption: %s" % rep.codes())
    # spool escape (index above 30), and a segment without a previous record of its spool
    many = pf.basic_project()
    many.set("spools.json", {"spools": [pf.spool(i, "S%d" % i) for i in range(1, 42)]})
    edit_r = many.doc("results/1/result.json")
    edit_r["spools"] = [{"spool": i, "length_mm": 10} for i in range(1, 42)]
    edit_r["steps"] = 3
    many.set("results/1/result.json", edit_r)
    many.set("results/1/sequence.bin", pf.sequence_bin([(0, 0, 1, 40), (4, 0, 0, 40), (2, 0, 2, 40)]))
    rep = check_bytes(many.build(), "escape")
    check(not rep.issues, "spool escape: %s" % [i.line() for i in rep.issues])
    many.set("results/1/sequence.bin", pf.sequence_bin([(4, 0, 0, 3), (2, 0, 2, 3), (0, 0, 1, 3)]))
    rep = check_bytes(many.build(), "order")
    check(rep.codes("error") == ["seq.order"], "segment without start: %s" % rep.codes())
    # canonical writer rules: order and header values
    items = pf.basic_project().render()
    items[2], items[3] = items[3], items[2]
    rep = check_bytes(pf.write_zip(items)[0], "order", canonical=True)
    check(rep.codes("error") == ["writer.order"], "canonical order: %s" % rep.codes())
    items = pf.basic_project().render()
    items[3].time = 1
    rep = check_bytes(pf.write_zip(items)[0], "hdr", canonical=True)
    check(rep.codes("error") == ["writer.header"], "canonical header: %s" % rep.codes())
    check(check_bytes(pf.write_zip(items)[0], "hdr").valid, "header values are not an error without --canonical")
    # JSON output of the command line
    import contextlib
    with tempfile.TemporaryDirectory() as t:
        path = os.path.join(t, "r.loomora")
        with open(path, "wb") as f:
            f.write(refs[4][1])
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            st = main(["--json", path])
        obj = json.loads(buf.getvalue())
        check(st == 0 and obj["valid"] and obj["warnings"] == 1 and obj["issues"][0]["code"] == "warn.light_project",
              "--json output: %s" % buf.getvalue())
    # hostile corpus (without the truncation sweep)
    import gen_invalid_corpus as gic
    n = gic.selftest(quick=True, engines=("mini", "jsonschema") if have_js else ("mini",), say=lambda s: None)
    check(n == 0, "corpus self-test reported %d problem(s)" % n)
    # comparisons
    with tempfile.TemporaryDirectory() as t:
        a, b = os.path.join(t, "a"), os.path.join(t, "b")
        os.mkdir(a)
        os.mkdir(b)
        for d in (a, b):
            with open(os.path.join(d, "x.loomora"), "wb") as f:
                f.write(refs[0][1])
        check(compare_dirs(a, b, out=lambda s: None) == 0, "compare identical directories")
        with open(os.path.join(b, "x.loomora"), "ab") as f:
            f.write(b"\0")
        check(compare_dirs(a, b, out=lambda s: None) == 1, "compare detects a difference")
        full, light = os.path.join(t, "full.loomora"), os.path.join(t, "light.loomora")
        with open(full, "wb") as f:
            f.write(pf.mesh3d_project("full", uvs=True).build())
        with open(light, "wb") as f:
            f.write(pf.mesh3d_project("light", uvs=True).build())
        check(compare_mesh(full, light, out=lambda s: None) == 0, "compare-mesh full(UV)/light")
        with open(full, "wb") as f:
            f.write(pf.mesh3d_project("full", uvs=False).build())
        check(compare_mesh(full, light, out=lambda s: None) == 0, "compare-mesh full(no UV)/light")
    print("check_loomora self-test: %s" % ("OK" if not failures else "%d failure(s)" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
