#!/usr/bin/env python
"""Generate the invalid, hostile, valid and large `.loomora` corpus of spec 002 (T065).

Usage:
  python tools/gen_invalid_corpus.py OUTDIR [--skip-truncate]   write the corpus and OUTDIR/corpus.json
  python tools/gen_invalid_corpus.py --selftest                 build in memory and check every file with
                                                                tools/check_loomora.py (no output files)

Standard library only; nothing is committed (OUTDIR is a build directory). The files are built by the
project's own small Python builder (tools/loomora_pyfmt.py): the basic 2D reference (one frame of eight
nails, one spool, one image with a tiny PNG, one result with a seven-step sequence) and a small 3D frame
with a tetrahedron mesh, each file changed in exactly one respect so that exactly one rule is violated.

File names: `<kind>-<code>[-<variant>].loomora` with kind one of
  invalid     a rule of the data model or of the binary entries is broken (exit status 1)
  hostile     a container-level attack: unsafe names, links, overlap, bomb, bad ZIP64 ... (exit status 1)
  warning     valid project that must open with the named warning (exit status 0)
  valid       valid project, no issue at all (exit status 0)
  large       valid project above a warning threshold; open it with the threshold of the name,
              `--set-limit NAME=VALUE` of the CLI or the limits() setters of the tests (exit status 0)
  truncate    the small reference cut after NNNN bytes, every byte (rejected: exit status 1 or 2)

`corpus.json` indexes every file: expected `code` (null for valid and truncate), `severity`, `entry`
(the entry the issue names), `also` (incidental warnings that the file carries), `set_limit` (for
large-*), `exit` (accepted exit statuses), `family` (the part of the code before the dot, for the
`zip.*`, `manifest.*` and `schema.*` families whose sub-codes the contract does not list) and
`contract_code` (false when the sub-code is this generator's reading, see tools/check_loomora.py SUBCODES).
"""
import argparse
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import zipfile
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import loomora_pyfmt as pf  # noqa: E402

# Codes named literally in contracts/c-api-project.md or in quickstart.md (the rest are sub-codes of the
# `zip.*`, `manifest.*` and `schema.*` families, or codes this reading adds).
CONTRACT_CODES = set("""zip.bad_name entry.size entry.sha256 entry.crc entry.missing entry.unlisted entry.bomb
version.major feature.unsupported id.duplicate ref.missing ref.layout_of_other_frame ref.spool_not_selected
num.not_finite nail.outside_frame nail.duplicate_id seq.header seq.crc seq.count mesh.crc mesh.index_range
spool.length_over_available warn.count warn.nail_distance warn.origin_offset warn.thumbnail_size
warn.light_project warn.unknown_entry""".split())

CASES = []


class Case(object):
    def __init__(self, kind, code, variant, build, severity, entry, also, limits, note):
        self.kind, self.code, self.variant, self.build = kind, code, variant, build
        self.severity, self.entry, self.also, self.limits, self.note = severity, entry, also or [], limits, note

    @property
    def filename(self):
        parts = [self.kind]
        if self.code:
            parts.append(self.code)
        stem = "-".join(parts)
        if self.variant:
            stem += "-" + self.variant
        return stem + ".loomora"

    def meta(self):
        d = OrderedDict()
        d["kind"] = self.kind
        d["code"] = self.code
        d["severity"] = self.severity
        d["family"] = self.code.split(".")[0] if self.code else None
        d["contract_code"] = bool(self.code) and self.code in CONTRACT_CODES
        d["entry"] = self.entry
        d["also"] = list(self.also)
        d["exit"] = [1] if self.severity == "error" else [0]
        if self.limits:
            d["set_limit"] = ["%s=%d" % kv for kv in sorted(self.limits.items())]
        d["note"] = self.note
        return d


def case(kind, code=None, variant=None, severity="error", entry="", also=None, limits=None, note=""):
    def deco(fn):
        CASES.append(Case(kind, code, variant, fn, severity, entry, also, limits, note))
        return fn
    return deco


# ---- editing helpers -------------------------------------------------------------------------
def edit(p, name, fn):
    d = p.doc(name)
    r = fn(d)
    p.set(name, r if isinstance(r, bytes) else d)


def raw_replace(p, name, old, new):
    b = p.raw(name)
    assert old in b, (name, old)
    p.set(name, b.replace(old, new, 1))


def manifest_edit(fn):
    return lambda m: fn(m)


def item(items, name):
    return next(i for i in items if i.name == name)


def seq_records(p, rid=1, **kw):
    return pf.sequence_bin(kw.pop("records", pf.SEQ_2D), **kw)


def build_info(p, zkw=None, **kw):
    items = p.render(**kw)
    return pf.write_zip(items, **(zkw or {}))


# ---- valid ----------------------------------------------------------------------------------
@case("valid", variant="basic", severity=None, note="the basic 2D reference (full profile)")
def _():
    return pf.basic_project().build()


@case("valid", variant="mesh3d", severity=None, note="a 3D frame with a tetrahedron mesh, normals, one view")
def _():
    return pf.mesh3d_project().build()


@case("valid", variant="mesh3d-uv", severity=None, note="3D full file whose mesh.bin carries a UV section")
def _():
    return pf.mesh3d_project(uvs=True).build()


@case("valid", variant="two-frames", severity=None, note="basic reference plus a second frame and layout")
def _():
    return pf.two_frames_project().build()


@case("valid", variant="all-stored", severity=None, note="writer option all_stored: no entry is compressed")
def _():
    return pf.basic_project().build(all_stored=True)


@case("valid", variant="zip64", severity=None, note="ZIP64 fields in every entry and ZIP64 end record (a reader accepts ZIP64 whenever present)")
def _():
    items = pf.basic_project().render()
    for it in items[1:]:          # `mimetype` has no extra field, not even a ZIP64 one
        it.zip64 = True
    return pf.write_zip(items, zip64_eocd=True)[0]


@case("valid", variant="data-descriptor", severity=None, note="deflated entries followed by data descriptors (accepted from other writers)")
def _():
    def edit_items(items):
        for it in items:
            if it.method == 8:
                it.descriptor = True
    return pf.basic_project().build(zip_edit=edit_items)


@case("valid", variant="dir-entry", severity=None, note="a directory entry (name ending with /) is accepted and ignored")
def _():
    def edit_items(items):
        items.append(pf.ZipItem("frames/", b"", 0, ext_attr=0x41ED0000))
    return pf.basic_project().build(zip_edit=edit_items)


@case("valid", variant="newer-minor", severity=None, note="format 1.3 with unknown fields and an unknown extension key: opens, ignores them")
def _():
    p = pf.basic_project()
    p.minor = 3
    p.manifest_extra = OrderedDict([("extensions", OrderedDict([("acme.hint", OrderedDict([("a", 1)]))])),
                                    ("future_field", [1, 2, 3])])
    edit(p, "frames/1/frame.json", lambda d: d.update({"future": OrderedDict([("note", "ignored")])}))
    return p.build()


# ---- warnings -------------------------------------------------------------------------------
@case("warning", "warn.light_project", severity="warning", entry="manifest.json", note="a light copy (no source images, no settings)")
def _():
    return pf.basic_project("light").build()


@case("warning", "warn.light_project", "mesh3d", severity="warning", entry="manifest.json", note="light copy with a mesh (no UV section)")
def _():
    return pf.mesh3d_project("light", uvs=True).build()


@case("warning", "warn.unknown_entry", severity="warning", entry="x-acme/note.txt", note="an entry of a namespace this version does not know, listed in the manifest")
def _():
    p = pf.basic_project()
    p.set("x-acme/note.txt", b"hello\n")
    return p.build()


@case("warning", "warn.nail_distance", severity="warning", entry="frames/1/layouts/1/nails.json",
      note="nails 0 and 7 are 1 mm apart, closer than the sum of their radii (2 mm)")
def _():
    p = pf.basic_project()

    def fn(d):
        d["nails"][7]["x"], d["nails"][7]["y"] = -250, -249
    edit(p, "frames/1/layouts/1/nails.json", fn)
    return p.build()


@case("warning", "warn.origin_offset", severity="warning", entry="frames/1/frame.json",
      note="the outline (and its nails) is shifted by 10 mm: bounding-box centre not at the origin")
def _():
    return pf.basic_project(offset=(10, 0)).build()


@case("warning", "warn.thumbnail_size", severity="warning", entry="images/1/thumb.png", note="thumbnail of 300 x 2 px (limit 256 x 256 px)")
def _():
    p = pf.basic_project()
    p.set("images/1/thumb.png", pf.png(300, 2))
    return p.build()


# ---- large (valid, above a lowered threshold) -------------------------------------------------
@case("large", "warn.count", "warn_nails-20", severity="warning", entry="frames/1/layouts/1/nails.json",
      limits={"warn_nails": 20}, note="a layout of 30 nails")
def _():
    p = pf.basic_project()
    pts = [(-250 + 17 * i, 250) for i in range(30)]
    p.set("frames/1/layouts/1/nails.json", pf.nails2d(pts))
    p.set("frames/1/layouts/1/layout.json", pf.layout(1, 1, 30))
    return p.build()


@case("large", "warn.count", "warn_steps-50", severity="warning", entry="results/1/sequence.bin",
      limits={"warn_steps": 50}, note="a result of 60 steps")
def _():
    p = pf.basic_project()
    recs = [(0, 0, 1, 0)] + [((i % 6) + 1, i % 2, 0, 0) for i in range(58)] + [(3, 0, 2, 0)]
    p.set("results/1/sequence.bin", pf.sequence_bin(recs))
    edit(p, "results/1/result.json", lambda d: d.update({"steps": len(recs)}))
    return p.build()


@case("large", "warn.count", "warn_triangles-10", severity="warning", entry="frames/1/mesh.bin",
      limits={"warn_triangles": 10}, note="a cube mesh of 12 triangles")
def _():
    return pf.mesh3d_project(cube=True).build()


@case("large", "warn.count", "warn_total_bytes-32768", severity="warning", entry="",
      limits={"warn_total_bytes": 32768}, also=["warn.unknown_entry"],
      note="40000 bytes of incompressible data in x-demo/blob.bin (ratio far below the bomb guard)")
def _():
    p = pf.basic_project()
    p.set("x-demo/blob.bin", pf.pseudo_bytes(40000))
    return p.build()


@case("large", "warn.count", "warn_items-10", severity="warning", entry="manifest.json",
      limits={"warn_items": 10}, note="twelve spools")
def _():
    p = pf.basic_project()
    p.set("spools.json", OrderedDict([("spools", [pf.spool(i, "Spool %d" % i) for i in range(1, 13)])]))
    return p.build()


# ---- entries ---------------------------------------------------------------------------------
@case("invalid", "entry.size", entry="spools.json", note="the manifest records a size one byte too large")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m["entries"]["spools.json"].update(size=m["entries"]["spools.json"]["size"] + 1))


@case("invalid", "entry.sha256", entry="frames/1/frame.json", note="the manifest records another SHA-256")
def _():
    def fn(m):
        h = m["entries"]["frames/1/frame.json"]["sha256"]
        m["entries"]["frames/1/frame.json"]["sha256"] = h[:-1] + ("0" if h[-1] != "0" else "1")
    return pf.basic_project().build(edit_manifest=fn)


@case("invalid", "entry.crc", entry="spools.json", note="the ZIP CRC-32 (directory and local header) is wrong, the data is intact")
def _():
    def fn(items):
        it = item(items, "spools.json")
        it.crc = pf.crc32(it.data) ^ 1
    return pf.basic_project().build(zip_edit=fn)


@case("invalid", "entry.missing", entry="x-demo/ghost.bin", note="the manifest lists an entry that is not in the archive")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m["entries"].update(
        {"x-demo/ghost.bin": OrderedDict([("size", 4), ("sha256", "0" * 64)])}))


@case("invalid", "entry.unlisted", entry="x-demo/extra.txt", also=["warn.unknown_entry"],
      note="an archive entry that the manifest does not list")
def _():
    p = pf.basic_project()
    p.set("x-demo/extra.txt", b"not in the manifest")
    return p.build(edit_manifest=lambda m: m["entries"].pop("x-demo/extra.txt"))


@case("hostile", "entry.bomb", entry="x-demo/bomb.bin", note="2 MiB of zeros deflated to about 2 KB: ratio above 250:1 over 1 MiB")
def _():
    p = pf.basic_project()
    p.set("x-demo/bomb.bin", bytes(2 * 1024 * 1024))

    def fn(items):
        item(items, "x-demo/bomb.bin").method = 8
    return p.build(zip_edit=fn)


# ---- manifest --------------------------------------------------------------------------------
@case("invalid", "manifest.counts", entry="manifest.json", note="counts.frames is 2, the project holds one frame")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m["counts"].update(frames=2))


@case("invalid", "manifest.next_id", entry="manifest.json", note="next_id.frame is 1, not above frame 1")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m["next_id"].update(frame=1))


@case("invalid", "manifest.order", entry="manifest.json", note="order.spools is [2], not a permutation of [1]")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m["order"].update(spools=[2]))


@case("invalid", "manifest.entries", entry="manifest.json", note="entries lists mimetype (it must list everything but mimetype and manifest.json)")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m["entries"].update(
        {"mimetype": OrderedDict([("size", len(pf.MIMETYPE)), ("sha256", pf.sha256(pf.MIMETYPE))])}))


@case("invalid", "manifest.min_reader", entry="manifest.json", note="min_reader 1.1 above format 1.0")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m.update(min_reader="1.1"))


@case("invalid", "manifest.profile", entry="manifest.json", note="profile full with editable false")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m.update(editable=False))


@case("invalid", "manifest.features", entry="manifest.json", note="a mesh frame but required_features is empty")
def _():
    return pf.mesh3d_project().build(edit_manifest=lambda m: m.update(required_features=[]))


@case("invalid", "manifest.missing", entry="manifest.json", note="no manifest.json in the archive")
def _():
    def fn(items):
        items.remove(item(items, "manifest.json"))
    return pf.basic_project().build(zip_edit=fn)


@case("invalid", "version.major", entry="manifest.json", note="format major 2, min_reader 2.0")
def _():
    def fn(m):
        m["format"]["major"] = 2
        m["min_reader"] = "2.0"
    return pf.basic_project().build(edit_manifest=fn)


@case("invalid", "feature.unsupported", entry="manifest.json", note="required_features names a feature this version does not know")
def _():
    return pf.basic_project().build(edit_manifest=lambda m: m.update(required_features=["hologram"]))


# ---- schema ----------------------------------------------------------------------------------
@case("invalid", "schema.required", entry="frames/1/frame.json", note="frame.json without `background`")
def _():
    p = pf.basic_project()
    edit(p, "frames/1/frame.json", lambda d: d.pop("background"))
    return p.build()


@case("invalid", "schema.enum", entry="images/1/image.json", note="color_mode 'sepia'")
def _():
    p = pf.basic_project()
    edit(p, "images/1/image.json", lambda d: d.update(color_mode="sepia"))
    return p.build()


@case("invalid", "schema.duplicate_key", entry="frames/1/frame.json", note="the key `name` appears twice")
def _():
    p = pf.basic_project()
    raw_replace(p, "frames/1/frame.json", b'"name":"Square 50"', b'"name":"Square 50","name":"Again"')
    return p.build()


@case("invalid", "schema.depth", entry="results/1/settings.json", note="settings.json nested 65 deep (limit 64)")
def _():
    p = pf.basic_project()
    p.set("results/1/settings.json", b"[" * 65 + b"]" * 65)
    return p.build()


@case("invalid", "schema.number", entry="manifest.json", note="counts.frames is 1e999 (not finite once parsed)")
def _():
    def fn(m):
        return pf.jdump(m).replace(b'"frames":1,', b'"frames":1e999,', 1)
    return pf.basic_project().build(edit_manifest=fn)


@case("invalid", "schema.parse", "syntax", entry="frames/1/frame.json", note="truncated JSON")
def _():
    p = pf.basic_project()
    p.set("frames/1/frame.json", b'{"id":1,"name":')
    return p.build()


@case("invalid", "schema.parse", "bom", entry="frames/1/frame.json", note="UTF-8 byte order mark before the JSON")
def _():
    p = pf.basic_project()
    p.set("frames/1/frame.json", b"\xef\xbb\xbf" + p.raw("frames/1/frame.json"))
    return p.build()


@case("invalid", "schema.type", entry="frames/1/layouts/1/nails.json", note="a nail with x as a string")
def _():
    p = pf.basic_project()
    raw_replace(p, "frames/1/layouts/1/nails.json", b'"x":-250,', b'"x":"abc",')
    return p.build()


@case("invalid", "schema.range", entry="spools.json", note="opacity 0 (must be above 0)")
def _():
    p = pf.basic_project()
    edit(p, "spools.json", lambda d: d["spools"][0].update(opacity=0))
    return p.build()


@case("invalid", "schema.pattern", entry="spools.json", note="color 'red' instead of #rrggbb")
def _():
    p = pf.basic_project()
    edit(p, "spools.json", lambda d: d["spools"][0].update(color="red"))
    return p.build()


# ---- identifiers, references, numbers, nails -------------------------------------------------
@case("invalid", "id.duplicate", entry="spools.json", note="two spools with id 1 (counts.spools is 2, order lists 1 once)")
def _():
    p = pf.basic_project()
    p.set("spools.json", OrderedDict([("spools", [pf.spool(1), pf.spool(1, "Duplicate")])]))
    return p.build(edit_manifest=lambda m: m["counts"].update(spools=2))


@case("invalid", "ref.missing", "image", entry="results/1/result.json", note="the result uses image 9")
def _():
    p = pf.basic_project()
    edit(p, "results/1/result.json", lambda d: d["images"][0].update(image=9))
    return p.build()


@case("invalid", "ref.missing", "frame", entry="frames/1/layouts/1/layout.json", note="the layout names frame 9")
def _():
    p = pf.basic_project()
    edit(p, "frames/1/layouts/1/layout.json", lambda d: d.update(frame=9))
    return p.build()


@case("invalid", "ref.missing", "layout", entry="results/1/result.json", note="the result uses layout 9")
def _():
    p = pf.basic_project()
    edit(p, "results/1/result.json", lambda d: d.update(layout=9))
    return p.build()


@case("invalid", "ref.missing", "spool", entry="results/1/result.json", note="the selection names spool 7")
def _():
    p = pf.basic_project()
    edit(p, "results/1/result.json", lambda d: d["spools"][0].update(spool=7))
    return p.build()


@case("invalid", "ref.missing", "entry", entry="images/1/image.json", note="the thumbnail entry does not exist")
def _():
    p = pf.basic_project()
    edit(p, "images/1/image.json", lambda d: d["thumbnail"].update(entry="images/1/missing.png"))
    return p.build()


@case("invalid", "ref.missing", "step-nail", entry="results/1/sequence.bin", note="a step uses nail 99, the layout has nails 0 to 7")
def _():
    p = pf.basic_project()
    recs = list(pf.SEQ_2D)
    recs[3] = (99, 0, 0, 0)
    p.set("results/1/sequence.bin", pf.sequence_bin(recs))
    return p.build()


@case("invalid", "ref.layout_of_other_frame", entry="results/1/result.json", note="result of frame 2 uses layout 1, which belongs to frame 1")
def _():
    p = pf.two_frames_project()
    edit(p, "results/1/result.json", lambda d: d.update(frame=2))
    return p.build()


@case("invalid", "ref.spool_not_selected", entry="results/1/sequence.bin", note="a step starts spool index 1, the selection has one spool (spool 2 exists but is not selected)")
def _():
    p = pf.basic_project()
    p.set("spools.json", OrderedDict([("spools", [pf.spool(1), pf.spool(2, "Red", "#ff0000")])]))
    recs = list(pf.SEQ_2D)
    recs[1] = (4, 0, 1, 1)
    p.set("results/1/sequence.bin", pf.sequence_bin(recs))
    return p.build()


@case("invalid", "num.not_finite", entry="frames/1/layouts/1/nails.json", note="a nail with x = 1e999")
def _():
    p = pf.basic_project()
    raw_replace(p, "frames/1/layouts/1/nails.json", b'"x":-250,', b'"x":1e999,')
    return p.build()


@case("invalid", "nail.outside_frame", "2d", entry="frames/1/layouts/1/nails.json", note="nail 7 at x = -400, outside the 500 mm square")
def _():
    p = pf.basic_project()
    edit(p, "frames/1/layouts/1/nails.json", lambda d: d["nails"][7].update(x=-400))
    return p.build()


@case("invalid", "nail.outside_frame", "exclusion", entry="frames/1/layouts/1/nails.json", note="nail 7 at the centre of an exclusion")
def _():
    p = pf.basic_project()
    hole = OrderedDict([("type", "rectangle"), ("params", OrderedDict()),
                        ("polyline", [[-50, -50], [50, -50], [50, 50], [-50, 50]])])
    edit(p, "frames/1/frame.json", lambda d: d.update(exclusions=[hole]))
    edit(p, "frames/1/layouts/1/nails.json", lambda d: d["nails"][7].update(x=0, y=0, kind="interior"))
    return p.build()


@case("invalid", "nail.outside_frame", "3d-face", entry="frames/1/layouts/1/nails.json", note="a 3D nail on triangle 99 of a 4-triangle mesh")
def _():
    p = pf.mesh3d_project()
    edit(p, "frames/1/layouts/1/nails.json", lambda d: d["nails"][3]["face"].update(triangle=99))
    return p.build()


@case("invalid", "nail.duplicate_id", entry="frames/1/layouts/1/nails.json", note="nail 7 renamed 0 (nail 0 exists; no step uses nail 7)")
def _():
    p = pf.basic_project()
    edit(p, "frames/1/layouts/1/nails.json", lambda d: d["nails"][7].update(id=0))
    return p.build()


@case("invalid", "spool.length_over_available", entry="results/1/result.json", note="300000 mm selected, 200000 mm available")
def _():
    p = pf.basic_project()
    edit(p, "results/1/result.json", lambda d: d["spools"][0].update(length_mm=300000))
    return p.build()


# ---- sequence.bin ----------------------------------------------------------------------------
def _set_seq(data, steps=None):
    p = pf.basic_project()
    p.set("results/1/sequence.bin", data)
    if steps is not None:
        edit(p, "results/1/result.json", lambda d: d.update(steps=steps))
    return p


@case("invalid", "seq.header", "magic", entry="results/1/sequence.bin", note="magic LMSX (CRC recomputed)")
def _():
    return _set_seq(pf.sequence_bin(pf.SEQ_2D, magic=b"LMSX")).build()


@case("invalid", "seq.header", "version", entry="results/1/sequence.bin", note="version 2 (CRC recomputed)")
def _():
    return _set_seq(pf.sequence_bin(pf.SEQ_2D, version=2)).build()


@case("invalid", "seq.header", "levels", entry="results/1/sequence.bin", note="levels flag set in the header, result.levels is false")
def _():
    return _set_seq(pf.sequence_bin([r + (0,) for r in pf.SEQ_2D], header_flags=1)).build()


@case("invalid", "seq.crc", "trailer", entry="results/1/sequence.bin", note="last byte of the CRC-32 flipped")
def _():
    d = bytearray(pf.sequence_bin(pf.SEQ_2D))
    d[-1] ^= 0xFF
    return _set_seq(bytes(d)).build()


@case("invalid", "seq.crc", "body", entry="results/1/sequence.bin", note="one record byte flipped, CRC not recomputed")
def _():
    d = bytearray(pf.sequence_bin(pf.SEQ_2D))
    d[8] ^= 0x01
    return _set_seq(bytes(d)).build()


@case("invalid", "seq.count", "steps", entry="results/1/sequence.bin", note="result.steps is 9, the sequence holds 7 records")
def _():
    return _set_seq(pf.sequence_bin(pf.SEQ_2D), steps=9).build()


@case("invalid", "seq.count", "header", entry="results/1/sequence.bin", note="header count 3 (steps 3) but seven records follow")
def _():
    return _set_seq(pf.sequence_bin(pf.SEQ_2D, count=3), steps=3).build()


@case("invalid", "seq.record", "reserved-type", entry="results/1/sequence.bin", note="a record of the reserved type 3")
def _():
    recs = list(pf.SEQ_2D)
    recs[2] = (2, 1, 3, 0)
    return _set_seq(pf.sequence_bin(recs)).build()


@case("invalid", "seq.record", "varint", entry="results/1/sequence.bin", note="first nail id encoded non-minimally (0x80 0x00)")
def _():
    body = bytearray()
    for k, (nail, rot, typ, spool) in enumerate(pf.SEQ_2D):
        body += (b"\x80\x00" if k == 0 else pf.varint(nail)) + bytes([rot | (typ << 1) | (spool << 3)])
    return _set_seq(pf.sequence_bin(pf.SEQ_2D, raw_records=bytes(body))).build()


# ---- mesh.bin --------------------------------------------------------------------------------
def _mesh_project(data=None, frame_edit=None):
    p = pf.mesh3d_project()
    if data is not None:
        p.set("frames/1/mesh.bin", data)
    if frame_edit:
        edit(p, "frames/1/frame.json", frame_edit)
    return p


def _tetra():
    return pf.mesh_arrays(pf.TETRA_V, pf.TETRA_T, True, False)


@case("invalid", "mesh.crc", "block", entry="frames/1/mesh.bin", note="a position byte flipped, block CRC not recomputed")
def _():
    d = bytearray(pf.mesh3d_project().raw("frames/1/mesh.bin"))
    d[30] ^= 0x01
    return _mesh_project(bytes(d)).build()


@case("invalid", "mesh.crc", "table", entry="frames/1/mesh.bin", note="the table CRC-32 flipped")
def _():
    d = bytearray(pf.mesh3d_project().raw("frames/1/mesh.bin"))
    d[-1] ^= 0xFF
    return _mesh_project(bytes(d)).build()


@case("invalid", "mesh.index_range", entry="frames/1/mesh.bin", note="a triangle index 99 with 4 vertices (CRCs recomputed)")
def _():
    pos, idx, nrm, _uv = _tetra()
    idx[3] = 99
    return _mesh_project(pf.mesh_bin(pos, idx, nrm)).build()


@case("invalid", "mesh.header", "counts", entry="frames/1/mesh.bin", note="frame.json says 5 vertices, mesh.bin holds 4")
def _():
    return _mesh_project(frame_edit=lambda d: d["mesh"].update(vertices=5)).build()


@case("invalid", "mesh.header", "flags", entry="frames/1/mesh.bin", note="frame.json has_normals false, mesh.bin has a normals section")
def _():
    return _mesh_project(frame_edit=lambda d: d["mesh"].update(has_normals=False)).build()


@case("invalid", "mesh.size", "uv", entry="frames/1/mesh.bin", note="UV flag set (and has_uvs true) but the UV section is missing")
def _():
    pos, idx, nrm, _uv = _tetra()
    return _mesh_project(pf.mesh_bin(pos, idx, nrm, flags=3), lambda d: d["mesh"].update(has_uvs=True)).build()


# ---- ZIP container ---------------------------------------------------------------------------
def _with_items(extra=None, listed=None, edit_items=None, zkw=None):
    """The basic project plus `listed` unknown entries (in the manifest) and `extra` raw ZipItems
    (not in the manifest); `edit_items` may change the items; returns the archive bytes."""
    p = pf.basic_project()
    for name, data in (listed or []):
        p.set(name, data)

    def fn(items):
        for it in extra or []:
            items.append(it)
        if edit_items:
            edit_items(items)
    return pf.write_zip(p.render(zip_edit=fn), **(zkw or {}))[0]


_BAD_NAMES = [
    ("absolute", b"/etc/evil.txt", "absolute path"),
    ("dotdot", b"../evil.txt", "'..' component at the start"),
    ("dotdot-nested", b"x-demo/../../evil.txt", "'..' components in the middle"),
    ("backslash", b"x-demo\\evil.txt", "backslash separator"),
    ("drive", b"C:/evil.txt", "drive letter"),
    ("nul", b"x-demo/evil\x00.txt", "NUL in the name"),
    ("control", b"x-demo/ev\x01il.txt", "control character"),
    ("dot", b"x-demo/./evil.txt", "'.' component"),
    ("empty-component", b"x-demo//evil.txt", "empty component"),
    ("too-long", b"x-demo/" + b"a" * 520, "name above 512 bytes"),
    ("not-utf8", b"x-demo/\xff\xfe.txt", "name that is not UTF-8"),
]


def _bad_name_case(variant, name, why):
    @case("hostile", "zip.bad_name", variant, entry=name.decode("utf-8", "replace"), note=why)
    def _():
        return _with_items(extra=[pf.ZipItem(name, b"payload", 0)])
    return _


for _v, _n, _w in _BAD_NAMES:
    _bad_name_case(_v, _n, _w)


@case("hostile", "zip.duplicate_name", "case", entry="x-demo/a.txt", also=["warn.unknown_entry"],
      note="x-demo/A.txt and x-demo/a.txt: equal after ASCII case folding")
def _():
    return _with_items(listed=[("x-demo/A.txt", b"one")], extra=[pf.ZipItem("x-demo/a.txt", b"two", 0)])


@case("hostile", "zip.duplicate_name", "exact", entry="x-demo/A.txt", also=["warn.unknown_entry"],
      note="the same name twice")
def _():
    return _with_items(listed=[("x-demo/A.txt", b"one")], extra=[pf.ZipItem("x-demo/A.txt", b"two", 0)])


@case("hostile", "zip.symlink", "symlink", entry="x-demo/link", note="Unix mode 0120777 (symbolic link)")
def _():
    return _with_items(extra=[pf.ZipItem("x-demo/link", b"/etc/passwd", 0, ext_attr=0xA1FF0000)])


@case("hostile", "zip.symlink", "device", entry="x-demo/dev", note="Unix mode 060644 (block device)")
def _():
    return _with_items(extra=[pf.ZipItem("x-demo/dev", b"", 0, ext_attr=0x61A40000)])


@case("hostile", "zip.overlap", entry="x-demo/inner.bin", also=["warn.unknown_entry"],
      note="x-demo/inner.bin is a complete local record stored inside the data of x-demo/outer.bin")
def _():
    inner = pf.ZipItem("x-demo/inner.bin", b"inner payload", 0)
    outer_data = pf.local_record(inner)

    def fn(items):
        inner.alias = (item(items, "x-demo/outer.bin"), 0)
        items.append(inner)
    return _with_items(listed=[("x-demo/outer.bin", outer_data)], edit_items=fn)


@case("hostile", "zip.method", "77", entry="x-demo/odd.bin", note="method 77 (unknown; method 99 is WinZip AES, reported as encrypted)")
def _():
    return _with_items(extra=[pf.ZipItem("x-demo/odd.bin", b"payload", 77, raw=b"payload")])


@case("hostile", "zip.method", "bzip2", entry="x-demo/odd.bin", note="method 12 (bzip2)")
def _():
    return _with_items(extra=[pf.ZipItem("x-demo/odd.bin", b"payload", 12, raw=b"payload")])


@case("hostile", "zip.encrypted", entry="x-demo/secret.bin", note="general purpose flag bit 0 (encrypted)")
def _():
    return _with_items(extra=[pf.ZipItem("x-demo/secret.bin", b"payload", 0, flags=1)])


def _mimetype_case(variant, why, fn, **kw):
    @case("hostile", "zip.mimetype", variant, entry="mimetype", note=why)
    def _():
        return pf.basic_project().build(**dict(kw, zip_edit=fn) if fn else kw)
    return _


_mimetype_case("missing", "no mimetype entry", None, with_mimetype=False)
_mimetype_case("misplaced", "mimetype after manifest.json", lambda items: items.insert(1, items.pop(0)))
_mimetype_case("wrong", "content application/x-loomora", lambda items: setattr(items[0], "data", b"application/x-loomora"))
_mimetype_case("newline", "content followed by a newline", lambda items: setattr(items[0], "data", pf.MIMETYPE + b"\n"))
_mimetype_case("deflated", "mimetype compressed", lambda items: setattr(items[0], "method", 8))


def _extra_field(items):
    items[0].extra = items[0].local_extra = b"\xca\xfe\x00\x00"


_mimetype_case("extra-field", "mimetype with an extra field", _extra_field)


@case("hostile", "zip.not_zip", "text", note="a text file")
def _():
    return b"This is not a ZIP archive.\n"


@case("hostile", "zip.not_zip", "empty", note="an empty file")
def _():
    return b""


def _patched(items_edit, patch, zkw=None, p=None):
    p = p or pf.basic_project()
    data, info = pf.write_zip(p.render(zip_edit=items_edit) if items_edit else p.render(), **(zkw or {}))
    data = bytearray(data)
    patch(data, info)
    return bytes(data)


@case("hostile", "zip.central_directory", "count", note="the end record announces one entry more than the directory holds (truncated directory)")
def _():
    def patch(d, info):
        n = struct.unpack_from("<H", d, info["eocd_offset"] + 10)[0]
        struct.pack_into("<HH", d, info["eocd_offset"] + 8, n + 1, n + 1)
    return _patched(None, patch)


@case("hostile", "zip.central_directory", "size", note="the directory size runs into the end record")
def _():
    def patch(d, info):
        struct.pack_into("<I", d, info["eocd_offset"] + 12, info["cd_size"] + 100)
    return _patched(None, patch)


@case("hostile", "zip.central_directory", "offset", note="the directory offset is beyond the end of the file")
def _():
    def patch(d, info):
        struct.pack_into("<I", d, info["eocd_offset"] + 16, len(d) + 10)
    return _patched(None, patch)


def _local_case(variant, why, **attrs):
    @case("hostile", "zip.local_header", variant, entry="x-demo/a.bin", note=why)
    def _():
        def fn(items):
            for k, v in attrs.items():
                setattr(item(items, "x-demo/a.bin"), k, v)
        return _with_items(listed=[("x-demo/a.bin", b"0123456789abcdef")], edit_items=fn)
    return _


_local_case("name", "local header names x-demo/b.bin", local_name="x-demo/b.bin")
_local_case("method", "local header says deflate, directory says stored", local_method=8)
_local_case("crc", "local header CRC-32 differs", local_crc=0x12345678)
_local_case("size", "local header compressed size is one byte too large", local_csize=17)


@case("hostile", "zip.zip64", "count-disagree", note="the classic end record says N entries, the ZIP64 end record N+1")
def _():
    def fn(items):
        for it in items[1:]:
            it.zip64 = True

    def patch(d, info):
        n = len(info["entries"])
        struct.pack_into("<HH", d, info["eocd_offset"] + 8, n, n)
        struct.pack_into("<QQ", d, info["eocd64_offset"] + 24, n + 1, n + 1)
    return _patched(fn, patch, {"zip64_eocd": True})


@case("hostile", "zip.zip64", "no-extra", entry="x-demo/a.bin", note="directory fields saturated (0xFFFFFFFF) with no ZIP64 extra field")
def _():
    def fn(items):
        item(items, "x-demo/a.bin").zip64 = "no-extra"
    return _with_items(listed=[("x-demo/a.bin", b"0123456789abcdef")], edit_items=fn)


@case("hostile", "zip.zip64", "no-locator", note="end record fields saturated but there is no ZIP64 end record")
def _():
    return pf.write_zip(pf.basic_project().render(), zip64_eocd=True, eocd_locator=False)[0]


# ---- registry, truncations, output -----------------------------------------------------------
def truncation_source():
    """The 'small reference' cut at every byte by the truncate-NNNN files."""
    return pf.small_reference().build()


def all_files(skip_truncate=False, truncate_step=1):
    """Yield (filename, bytes, meta dict) for every corpus file, in a stable order."""
    for c in CASES:
        yield c.filename, c.build(), c.meta()
    if skip_truncate:
        return
    ref = truncation_source()
    width = max(4, len(str(len(ref))))
    for n in range(0, len(ref), truncate_step):
        yield ("truncate-%0*d.loomora" % (width, n), ref[:n],
               OrderedDict([("kind", "truncate"), ("code", None), ("severity", "error"), ("family", "zip"),
                            ("contract_code", False), ("entry", ""), ("also", []), ("exit", [1, 2]),
                            ("note", "small reference cut after %d of %d bytes" % (n, len(ref)))]))


def write_corpus(outdir, skip_truncate=False, say=print):
    os.makedirs(outdir, exist_ok=True)
    index = OrderedDict()
    counts = {}
    size = 0
    names = set()
    for name, data, meta in all_files(skip_truncate):
        assert name not in names, name
        names.add(name)
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(data)
        index[name] = meta
        counts[meta["kind"]] = counts.get(meta["kind"], 0) + 1
        size += len(data)
    doc = OrderedDict([("format", "loomora-invalid-corpus"), ("version", 1),
                       ("generator", "tools/gen_invalid_corpus.py"),
                       ("reference_size", len(truncation_source())), ("files", index)])
    with open(os.path.join(outdir, "corpus.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write("{\n")
        head = [k for k in doc if k != "files"]
        for k in head:
            f.write("  %s: %s,\n" % (json.dumps(k), json.dumps(doc[k])))
        f.write('  "files": {\n')
        rows = ['    %s: %s' % (json.dumps(n), json.dumps(m, ensure_ascii=False)) for n, m in index.items()]
        f.write(",\n".join(rows))
        f.write("\n  }\n}\n")
    say("wrote %d files (%d bytes) to %s: %s" % (len(index), size, outdir,
        ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts))))
    return index


# ---- self-test ---------------------------------------------------------------------------------
def verify(name, data, meta, rep):
    """Problems (list of text) between the expectation of `meta` and the checker's report `rep`."""
    errs, warns = rep.codes("error"), rep.codes("warning")
    out = []
    if meta["kind"] == "truncate":
        if not errs or any(not c.startswith("zip.") for c in errs):
            out.append("expected only zip.* errors, got %s" % errs)
        return out
    if meta["severity"] == "error":
        if errs != [meta["code"]]:
            out.append("expected errors [%s], got %s" % (meta["code"], errs))
        elif meta["entry"] and rep.errors[0].entry != meta["entry"]:
            out.append("error names entry %r, expected %r" % (rep.errors[0].entry, meta["entry"]))
        if sorted(warns) != sorted(meta["also"]):
            out.append("expected incidental warnings %s, got %s" % (meta["also"], warns))
    else:
        if errs:
            out.append("expected no error, got %s" % errs)
        want = ([meta["code"]] if meta["code"] else []) + list(meta["also"])
        if sorted(warns) != sorted(want):
            out.append("expected warnings %s, got %s" % (sorted(want), sorted(warns)))
        elif meta["code"] and meta["entry"] and not any(i.code == meta["code"] and i.entry == meta["entry"] for i in rep.issues):
            out.append("warning %s does not name entry %r" % (meta["code"], meta["entry"]))
    return out


def selftest(quick=False, engines=("auto",), say=print):
    """Build every file in memory and check it with tools/check_loomora.py. Returns the number of problems."""
    import check_loomora as cl
    problems = 0
    seen = set()
    step = 23 if quick else 1
    n = 0
    for name, data, meta in all_files(truncate_step=step):
        if name in seen:
            say("PROBLEM duplicate file name %s" % name)
            problems += 1
        seen.add(name)
        n += 1
        limits = cl.parse_limits(meta.get("set_limit"))
        for eng in engines:
            rep = cl.check_bytes(data, name, limits=limits, engine=eng)
            for msg in verify(name, data, meta, rep):
                say("PROBLEM %s [%s]: %s" % (name, eng, msg))
                problems += 1
        if meta["kind"] == "valid":
            try:
                if zipfile.ZipFile(io.BytesIO(data)).testzip() is not None:
                    raise zipfile.BadZipFile("bad member")
            except zipfile.BadZipFile as e:
                say("PROBLEM %s: the standard zipfile module rejects it: %s" % (name, e))
                problems += 1
        if meta["kind"] == "large" and not cl.check_bytes(data, name).valid:
            say("PROBLEM %s: not valid with the default limits" % name)
            problems += 1
        if meta["kind"] == "large" and any(i.code == "warn.count" for i in cl.check_bytes(data, name).issues):
            say("PROBLEM %s: warns with the default limits" % name)
            problems += 1
    # every contract code of the task list is covered
    wanted = """zip.bad_name entry.size entry.sha256 entry.crc entry.missing entry.unlisted entry.bomb manifest.counts
manifest.next_id manifest.order manifest.entries version.major feature.unsupported schema.required schema.enum
schema.duplicate_key schema.depth schema.number id.duplicate ref.missing ref.layout_of_other_frame
ref.spool_not_selected num.not_finite nail.outside_frame nail.duplicate_id seq.header seq.crc seq.count mesh.crc
mesh.index_range spool.length_over_available warn.count warn.nail_distance warn.origin_offset
warn.thumbnail_size warn.light_project warn.unknown_entry""".split()
    have = set(c.code for c in CASES if c.code)
    for w in wanted:
        if w not in have:
            say("PROBLEM no file for %s" % w)
            problems += 1
    # the writer to disk, the index and the CLI
    with tempfile.TemporaryDirectory() as t:
        out = os.path.join(t, "corpus")
        idx = write_corpus(out, skip_truncate=True, say=lambda s: None)
        with open(os.path.join(out, "corpus.json"), encoding="utf-8") as f:
            doc = json.load(f)
        if sorted(doc["files"]) != sorted(idx) or any(not os.path.isfile(os.path.join(out, k)) for k in idx):
            say("PROBLEM corpus.json does not match the files")
            problems += 1
        bad = os.path.join(out, "invalid-entry.sha256.loomora")
        good = os.path.join(out, "valid-basic.loomora")
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            status = [cl.main(["--quiet", bad]), cl.main(["--quiet", good]),
                      cl.main(["--quiet", os.path.join(t, "missing.loomora")]),
                      cl.main(["--json", out]), cl.main(["--set-limit", "bogus=1", good])]
        if status != [1, 0, 2, 1, 2]:
            say("PROBLEM exit statuses of check_loomora.py: %s" % status)
            problems += 1
    say("gen_invalid_corpus self-test: %d files checked, %s" % (n, "OK" if not problems else "%d problem(s)" % problems))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate the invalid/hostile .loomora corpus (spec 002, T065).")
    ap.add_argument("outdir", nargs="?", help="directory to write the corpus into (created if missing)")
    ap.add_argument("--out", dest="out_opt", help="same as OUTDIR (the form used by quickstart.md)")
    ap.add_argument("--skip-truncate", action="store_true", help="do not write the truncate-NNNN files")
    ap.add_argument("--selftest", action="store_true", help="build in memory and check with tools/check_loomora.py")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    if args.selftest:
        return 1 if selftest() else 0
    out = args.outdir or args.out_opt
    if not out:
        ap.print_usage(sys.stderr)
        return 2
    try:
        write_corpus(out, args.skip_truncate)
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
