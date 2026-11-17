#!/usr/bin/env python
"""Independent Python builder of `.loomora` files (spec 002, contracts/project-format.md).

Shared by tools/gen_invalid_corpus.py (hostile and invalid files) and by the self-test of
tools/check_loomora.py (valid references). Standard library only, Python 3.8+. It imports no
Loomora C++ code and is written from the contract alone: byte identity with the C++ writer is not
a goal, validity is (the deterministic writer rules of section 1.1 are followed where they cost
nothing: version made 0x0314, flags 0, DOS time 1980-01-01, external attributes 0x81A40000, the
entry order of section 2, deflate level 6 only for the entries that are compressed).

Two layers:
  * `ZipItem` / `write_zip`: a byte-level ZIP writer that can also write what a conforming writer
    never would (wrong sizes, local header mismatch, ZIP64 in every shape, entry aliasing...);
  * `Project`: the content of a project (JSON documents and binary entries) with the derivation of
    the manifest, so a test can change one field and keep everything else consistent.
"""
import hashlib
import json
import math
import re
import struct
import zlib
from collections import OrderedDict

MIMETYPE = b"application/vnd.loomora+zip"
BLOCK = 1 << 20  # mesh.bin block size (section 5.2)


# ---- small helpers -------------------------------------------------------------------------
def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def deflate_raw(data, level=6):
    """Raw deflate with the fixed parameters of research R3 (level 6, window 15, memLevel 8)."""
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY)
    return c.compress(data) + c.flush()


def jdump(obj):
    """Compact UTF-8 JSON (section 4): no whitespace, keys in insertion order."""
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def f32(values):
    return struct.pack("<%df" % len(values), *values)


def u32(values):
    return struct.pack("<%dI" % len(values), *values)


# ---- binary entries ------------------------------------------------------------------------
def sequence_bin(records, levels=False, count=None, magic=b"LMSQ", version=1, header_flags=None,
                 crc=True, raw_records=None):
    """sequence.bin (section 5.1). `records`: tuples (nail, rotation, type, spool[, level]);
    type 0 segment, 1 start, 2 end; a spool above 30 uses the escape (flags bits 3-7 = 31, then a
    varint index right after the flags byte). `raw_records` replaces the encoded records."""
    if header_flags is None:
        header_flags = 1 if levels else 0
    body = bytearray()
    body += magic + bytes([version, header_flags])
    body += varint(len(records) if count is None else count)
    if raw_records is not None:
        body += raw_records
    else:
        for rec in records:
            nail, rot, typ, spool = rec[:4]
            level = rec[4] if len(rec) > 4 else 0
            body += varint(nail)
            if spool < 31:
                body.append((rot & 1) | (typ << 1) | (spool << 3))
            else:
                body.append((rot & 1) | (typ << 1) | (31 << 3))
                body += varint(spool)
            if header_flags & 1:
                body.append(level)
    if crc:
        body += struct.pack("<I", crc32(bytes(body)))
    return bytes(body)


def mesh_table(data):
    """Block table of section 5.2 for `data` (everything before the table): one CRC-32 per 1 MiB
    block, then the CRC-32 of those block CRCs (reading: 'table CRC32 covers the block CRCs')."""
    table = b"".join(struct.pack("<I", crc32(data[i:i + BLOCK])) for i in range(0, len(data), BLOCK))
    return table + struct.pack("<I", crc32(table))


def mesh_bin(positions, indices, normals=None, uvs=None, magic=b"LMMS", version=1, flags=None,
             reserved=0, vcount=None, tcount=None, table=True):
    """mesh.bin (section 5.2). positions: flat list of floats (3 per vertex); indices: flat list of
    ints (3 per triangle); normals: flat floats; uvs: flat floats (2 per vertex)."""
    if flags is None:
        flags = (1 if normals is not None else 0) | (2 if uvs is not None else 0)
    nv = len(positions) // 3 if vcount is None else vcount
    nt = len(indices) // 3 if tcount is None else tcount
    data = magic + struct.pack("<BBHQQ", version, flags, reserved, nv, nt)
    data += f32(positions) + u32(indices)
    if normals is not None:
        data += f32(normals)
    if uvs is not None:
        data += f32(uvs)
    return data + mesh_table(data) if table else data


def png(width, height, gray=128):
    """A tiny valid 8-bit grayscale PNG."""
    def chunk(typ, payload):
        return struct.pack(">I", len(payload)) + typ + payload + struct.pack(">I", crc32(typ + payload))
    raw = b"".join(b"\x00" + bytes([gray]) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pseudo_bytes(n, seed=b"loomora"):
    """Deterministic incompressible-ish bytes (a SHA-256 counter chain)."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(seed + struct.pack("<Q", i)).digest()
        i += 1
    return bytes(out[:n])


# ---- ZIP writer ----------------------------------------------------------------------------
class ZipItem(object):
    """One archive entry. Every attribute that a conforming writer fixes can be overridden to
    write a hostile or inconsistent entry. `data` is the uncompressed content."""

    def __init__(self, name, data=b"", method=8, **kw):
        self.name = name                      # str or bytes (raw name bytes)
        self.data = data
        self.method = method                  # 0 stored, 8 deflate (anything else: data written raw)
        self.flags = 0
        self.version_made = 0x0314
        self.version_needed = 20
        self.time = 0x0000
        self.date = 0x0021
        self.ext_attr = 0x81A40000
        self.crc = None                       # None: CRC-32 of data
        self.csize = None                     # None: length of the written data
        self.usize = None                     # None: len(data)
        self.raw = None                       # compressed bytes to write instead of the computed ones
        self.extra = b""                      # central directory extra field
        self.local_extra = b""
        self.comment = b""
        self.local_name = None                # local header values that differ from the directory
        self.local_method = None
        self.local_crc = None
        self.local_csize = None
        self.local_usize = None
        self.local_flags = None
        self.zip64 = False                    # True | "no-extra": saturate the fields (see write_zip)
        self.descriptor = False               # general flag bit 3 and a data descriptor after the data
        self.alias = None                     # (other item, delta): not written, offset points inside it
        self.offset = None                    # directory offset override
        for k, v in kw.items():
            if not hasattr(self, k):
                raise AttributeError(k)
            setattr(self, k, v)

    def name_bytes(self):
        return self.name if isinstance(self.name, bytes) else self.name.encode("utf-8")


def _payload(item):
    if item.raw is not None:
        return item.raw
    if item.method == 8:
        return deflate_raw(item.data)
    return item.data


def local_record(item):
    """Local header + data (+ descriptor) bytes of `item` as write_zip would write them."""
    payload = _payload(item)
    crc = crc32(item.data) if item.crc is None else item.crc
    csize = len(payload) if item.csize is None else item.csize
    usize = len(item.data) if item.usize is None else item.usize
    name = item.name_bytes() if item.local_name is None else (
        item.local_name if isinstance(item.local_name, bytes) else item.local_name.encode("utf-8"))
    method = item.method if item.local_method is None else item.local_method
    flags = item.flags if item.local_flags is None else item.local_flags
    lcrc = crc if item.local_crc is None else item.local_crc
    lcs = csize if item.local_csize is None else item.local_csize
    lus = usize if item.local_usize is None else item.local_usize
    extra = item.local_extra
    needed = item.version_needed
    desc = b""
    if item.descriptor:
        flags |= 0x08
        desc = struct.pack("<IIII", 0x08074B50, crc, csize, usize)
        lcrc = lcs = lus = 0
    if item.zip64:
        needed = 45
        if item.zip64 is True:
            extra = struct.pack("<HHQQ", 1, 16, usize, csize) + extra
        lcs = lus = 0xFFFFFFFF
    head = struct.pack("<IHHHHHIIIHH", 0x04034B50, needed, flags, method, item.time, item.date,
                       lcrc, lcs, lus, len(name), len(extra))
    return head + name + extra + payload + desc


def write_zip(items, comment=b"", zip64_eocd=False, saturate_eocd=True, eocd_locator=True):
    """Serialize `items` in order. Returns (bytes, info). `info` holds `cd_offset`, `cd_size`,
    `eocd_offset`, `eocd64_offset` and, per item, `entries[i] = {"lho", "data_off", "cdo"}`.

    zip64 per item: True writes the sizes (local) and sizes+offset (directory) in a ZIP64 extra
    field and saturates the classic fields; "no-extra" saturates them without any extra field.
    zip64_eocd writes the ZIP64 end record and locator (the classic record is saturated when
    saturate_eocd is true; eocd_locator=False omits the ZIP64 record and locator altogether)."""
    out = bytearray()
    info = {"entries": []}
    for it in items:
        if it.alias is not None:
            other, delta = it.alias
            info["entries"].append({"lho": info["entries"][items.index(other)]["data_off"] + delta,
                                    "data_off": None, "cdo": None})
            continue
        lho = len(out)
        rec = local_record(it)
        nlen = struct.unpack_from("<H", rec, 26)[0]
        elen = struct.unpack_from("<H", rec, 28)[0]
        info["entries"].append({"lho": lho, "data_off": lho + 30 + nlen + elen, "cdo": None})
        out += rec
    cd_off = len(out)
    for i, it in enumerate(items):
        payload = _payload(it) if it.alias is None else _payload(it)
        crc = crc32(it.data) if it.crc is None else it.crc
        csize = len(payload) if it.csize is None else it.csize
        usize = len(it.data) if it.usize is None else it.usize
        name = it.name_bytes()
        flags = it.flags | (0x08 if it.descriptor else 0)
        off = info["entries"][i]["lho"] if it.offset is None else it.offset
        extra = it.extra
        needed = it.version_needed
        c_cs, c_us, c_off = csize, usize, off
        if it.zip64:
            needed = 45
            c_cs = c_us = c_off = 0xFFFFFFFF
            if it.zip64 is True:
                extra = struct.pack("<HHQQQ", 1, 24, usize, csize, off) + extra
        info["entries"][i]["cdo"] = len(out)
        out += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, it.version_made, needed, flags, it.method,
                           it.time, it.date, crc, c_cs, c_us, len(name), len(extra), len(it.comment),
                           0, 0, it.ext_attr, c_off)
        out += name + extra + it.comment
    cd_size = len(out) - cd_off
    n = len(items)
    info["cd_offset"], info["cd_size"] = cd_off, cd_size
    info["eocd64_offset"] = None
    if zip64_eocd and eocd_locator:
        e64 = len(out)
        info["eocd64_offset"] = e64
        out += struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 0x0314, 45, 0, 0, n, n, cd_size, cd_off)
        out += struct.pack("<IIQI", 0x07064B50, 0, e64, 1)
    info["eocd_offset"] = len(out)
    if zip64_eocd and saturate_eocd:
        out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, len(comment))
    else:
        out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, n, n, cd_size, cd_off, len(comment))
    out += comment
    return bytes(out), info


# ---- project content -------------------------------------------------------------------------
_NAME_PATTERNS = [
    (re.compile(r"^frames/(0|[1-9][0-9]*)/frame\.json$"), "frame"),
    (re.compile(r"^frames/(0|[1-9][0-9]*)/mesh\.bin$"), "mesh"),
    (re.compile(r"^frames/(0|[1-9][0-9]*)/mesh-source\.[A-Za-z0-9]+$"), "meshsrc"),
    (re.compile(r"^frames/(0|[1-9][0-9]*)/layouts/(0|[1-9][0-9]*)/layout\.json$"), "layout"),
    (re.compile(r"^frames/(0|[1-9][0-9]*)/layouts/(0|[1-9][0-9]*)/nails\.json$"), "nails"),
    (re.compile(r"^images/(0|[1-9][0-9]*)/image\.json$"), "image"),
    (re.compile(r"^images/(0|[1-9][0-9]*)/source\.(png|jpg|jpeg|webp)$"), "imgsource"),
    (re.compile(r"^images/(0|[1-9][0-9]*)/target\.png$"), "imgtarget"),
    (re.compile(r"^images/(0|[1-9][0-9]*)/thumb\.png$"), "imgthumb"),
    (re.compile(r"^results/(0|[1-9][0-9]*)/result\.json$"), "result"),
    (re.compile(r"^results/(0|[1-9][0-9]*)/sequence\.bin$"), "sequence"),
    (re.compile(r"^results/(0|[1-9][0-9]*)/thumb\.png$"), "resthumb"),
    (re.compile(r"^results/(0|[1-9][0-9]*)/settings\.json$"), "settings"),
]
_SUB = {"frame": 0, "mesh": 1, "meshsrc": 2, "layout": 3, "nails": 4, "image": 0, "imgsource": 1,
        "imgtarget": 2, "imgthumb": 3, "result": 0, "sequence": 1, "resthumb": 2, "settings": 3}


def classify(name):
    """(kind, [ids]) of a defined entry name, or (None, []) for an unknown entry."""
    if name == "mimetype":
        return "mimetype", []
    if name == "manifest.json":
        return "manifest", []
    if name == "spools.json":
        return "spools", []
    for rx, kind in _NAME_PATTERNS:
        m = rx.match(name)
        if m:
            return kind, [int(g) for g in m.groups() if g.isdigit()]
    return None, []


def canonical_key(name, index):
    """Sort key of the entry order of section 2; unknown entries keep their relative order."""
    kind, ids = classify(name)
    if kind == "mimetype":
        return (0,)
    if kind == "manifest":
        return (1,)
    if kind == "spools":
        return (2,)
    if kind in ("frame", "mesh", "meshsrc", "layout", "nails"):
        if kind in ("layout", "nails"):
            return (3, ids[0], 3, ids[1], _SUB[kind])
        return (3, ids[0], _SUB[kind])
    if kind in ("image", "imgsource", "imgtarget", "imgthumb"):
        return (4, ids[0], _SUB[kind])
    if kind in ("result", "sequence", "resthumb", "settings"):
        return (5, ids[0], _SUB[kind])
    return (9, index)


class Project(object):
    """The content of a project: `entries` maps an entry name to bytes or to a JSON-able object
    (encoded compactly on render). Everything but `mimetype` and `manifest.json` lives there."""

    def __init__(self, profile="full", title="Reference"):
        self.profile = profile
        self.title = title
        self.entries = OrderedDict()
        self.manifest_extra = {}              # keys merged into the manifest (e.g. extensions)
        self.minor = 0

    # -- access
    def doc(self, name, safe=False):
        v = self.entries[name]
        if isinstance(v, bytes):
            try:
                return json.loads(v.decode("utf-8"))
            except ValueError:
                if safe:
                    return {}
                raise
        return v

    def set(self, name, value):
        self.entries[name] = value
        return self

    def remove(self, name):
        del self.entries[name]
        return self

    def raw(self, name):
        v = self.entries[name]
        return v if isinstance(v, bytes) else jdump(v)

    # -- manifest derivation
    def ids(self, kind):
        found = set()
        for name in self.entries:
            k, ids = classify(name)
            if k == kind:
                found.add(ids[-1] if kind in ("layout",) else ids[0])
        return sorted(found)

    def derive_manifest(self, blobs):
        spools = self.doc("spools.json")["spools"] if "spools.json" in self.entries else []
        sp_ids = []
        for s in spools:
            if s["id"] not in sp_ids:
                sp_ids.append(s["id"])
        frames, layouts = self.ids("frame"), self.ids("layout")
        images, results = self.ids("image"), self.ids("result")
        feats = []
        for name in self.entries:
            kind, _ = classify(name)
            if kind == "frame":
                d = self.doc(name, True)
                if d.get("kind") == "3d" and "mesh" in d and "surface_mesh" not in feats:
                    feats.append("surface_mesh")
            elif kind == "result":
                d = self.doc(name, True)
                if d.get("levels") and "string_levels" not in feats:
                    feats.append("string_levels")
                if len(d.get("images", [])) > 1 and "multi_view_target" not in feats:
                    feats.append("multi_view_target")
        feats.sort(key=["surface_mesh", "string_levels", "multi_view_target"].index)

        def nxt(ids):
            return (max(ids) + 1) if ids else 1
        entries = OrderedDict()
        for name, blob in blobs:
            if name not in ("mimetype", "manifest.json"):
                entries[name] = OrderedDict([("size", len(blob)), ("sha256", sha256(blob))])
        m = OrderedDict()
        m["format"] = OrderedDict([("name", "loomora"), ("major", 1), ("minor", self.minor)])
        m["min_reader"] = "1.0"
        m["project_id"] = "9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d"
        m["title"] = self.title
        m["created_at"] = "2026-11-17T09:00:00Z"
        m["modified_at"] = "2026-11-17T09:00:00Z"
        m["generator"] = OrderedDict([("application", "loomora-pytools"), ("version", "0.1.0"),
                                      ("platform", "python"), ("architecture", "any")])
        m["profile"] = self.profile
        m["editable"] = self.profile == "full"
        m["counts"] = OrderedDict([("frames", len(frames)), ("layouts", len(layouts)),
                                   ("images", len(images)), ("spools", len(sp_ids)),
                                   ("results", len(results))])
        m["next_id"] = OrderedDict([("frame", nxt(frames)), ("layout", nxt(layouts)),
                                    ("image", nxt(images)), ("spool", nxt(sp_ids)),
                                    ("result", nxt(results))])
        m["order"] = OrderedDict([("frames", frames), ("layouts", layouts), ("images", images),
                                  ("spools", sp_ids), ("results", results)])
        m["required_features"] = feats
        m["entries"] = entries
        m["extensions"] = OrderedDict()
        for k, v in self.manifest_extra.items():
            m[k] = v
        return m

    # -- rendering
    def render(self, edit_manifest=None, all_stored=False, canonical=True, with_mimetype=True,
               zip_edit=None):
        """Return the list of ZipItem of the project. `edit_manifest(dict)` may mutate the manifest
        and may return replacement bytes. `zip_edit(items)` may mutate or reorder the items."""
        names = list(self.entries.keys())
        if canonical:
            names = [n for _, n in sorted((canonical_key(n, i), n) for i, n in enumerate(names))]
        blobs = [(n, self.raw(n)) for n in names]
        m = self.derive_manifest(blobs)
        mbytes = None
        if edit_manifest is not None:
            r = edit_manifest(m)
            if isinstance(r, bytes):
                mbytes = r
        if mbytes is None:
            mbytes = jdump(m)
        items = []
        if with_mimetype:
            items.append(ZipItem("mimetype", MIMETYPE, 0))
        items.append(ZipItem("manifest.json", mbytes, 8))
        for n, blob in blobs:
            stored = all_stored or n.endswith((".bin", ".png", ".jpg", ".jpeg", ".webp"))
            items.append(ZipItem(n, blob, 0 if stored else 8))
        if all_stored:
            for it in items:
                it.method = 0
        if zip_edit is not None:
            zip_edit(items)
        return items

    def build(self, **kw):
        """Serialize to .loomora bytes. Extra keywords for write_zip: `comment`, `zip64_eocd`..."""
        zkw = {k: kw.pop(k) for k in ("comment", "zip64_eocd", "saturate_eocd", "eocd_locator") if k in kw}
        data, _ = write_zip(self.render(**kw), **zkw)
        return data


# ---- reference projects ----------------------------------------------------------------------
SQUARE = [[-250, -250], [250, -250], [250, 250], [-250, 250]]
# Eight nails on the boundary of the 500 mm square (corners and edge midpoints); nail 7 is not used
# by the reference sequence so that a test can change it without touching a step.
NAILS_2D = [(-250, -250), (250, -250), (250, 250), (-250, 250), (0, -250), (250, 0), (0, 250), (-250, 0)]
# start 0, then segments to 4, 2, 6, 1, 5, then end at 3 (nail 7 stays unused)
SEQ_2D = [(0, 0, 1, 0), (4, 0, 0, 0), (2, 1, 0, 0), (6, 0, 0, 0), (1, 1, 0, 0), (5, 0, 0, 0), (3, 0, 2, 0)]


def spool(i, name="Black", color="#000000", available=200000):
    d = OrderedDict([("id", i), ("name", name), ("color", color), ("opacity", 1), ("diameter_mm", 0.3)])
    if available is not None:
        d["available_length_mm"] = available
    d["material"] = "cotton"
    d["domain"] = "exterior"
    return d


def frame2d(fid, name="Square 50", polyline=SQUARE, exclusions=None, width=500, height=500):
    return OrderedDict([
        ("id", fid), ("name", name), ("kind", "2d"),
        ("dimensions", OrderedDict([("width_mm", width), ("height_mm", height)])),
        ("background", "#ffffff"),
        ("outline", OrderedDict([("type", "rectangle"),
                                 ("params", OrderedDict([("width_mm", width), ("height_mm", height)])),
                                 ("polyline", polyline)])),
        ("exclusions", exclusions or []), ("surface", "plane")])


def nails2d(points, first_id=0, diameter=2):
    return OrderedDict([("nails", [OrderedDict([("id", first_id + i), ("x", x), ("y", y),
                                                ("diameter", diameter), ("kind", "boundary")])
                                   for i, (x, y) in enumerate(points)])])


def layout(lid, fid, count, name="Layout"):
    return OrderedDict([("id", lid), ("name", name), ("frame", fid), ("nail_count", count)])


def image_doc(iid, source=None, target=None, thumb_entry=None, size=8):
    d = OrderedDict([("id", iid), ("name", "Portrait"), ("color_mode", "bw"), ("shape", "circle"),
                     ("width_px", size), ("height_px", size)])
    if source:
        d["source"] = OrderedDict([("entry", "images/%d/source.png" % iid), ("format", "png"), ("size", source)])
    if target:
        d["target"] = OrderedDict([("entry", "images/%d/target.png" % iid), ("format", "png"), ("size", target)])
    d["thumbnail"] = OrderedDict([("entry", thumb_entry or "images/%d/thumb.png" % iid)])
    return d


def result_doc(rid, fid, lid, images, spools, steps, levels=False, settings=True):
    d = OrderedDict([("id", rid), ("name", "Portrait on square"), ("created_at", "2026-11-17T09:30:00Z"),
                     ("frame", fid), ("layout", lid), ("images", images), ("spools", spools),
                     ("steps", steps), ("levels", levels),
                     ("thumbnail", OrderedDict([("entry", "results/%d/thumb.png" % rid)]))])
    if settings:
        d["settings"] = OrderedDict([("entry", "results/%d/settings.json" % rid)])
    return d


def add_image(p, iid, full=True, thumb=None):
    src = png(8, 8, 100)
    if full:
        p.set("images/%d/source.png" % iid, src)
    p.set("images/%d/image.json" % iid, image_doc(iid, len(src) if full else None))
    p.set("images/%d/thumb.png" % iid, thumb or png(8, 8, 140))


def add_result(p, rid, fid, lid, images, spools, records, full=True, levels=False):
    p.set("results/%d/result.json" % rid,
          result_doc(rid, fid, lid, images, spools, len(records), levels, settings=full))
    p.set("results/%d/sequence.bin" % rid, sequence_bin(records, levels))
    p.set("results/%d/thumb.png" % rid, png(8, 8, 200))
    if full:
        p.set("results/%d/settings.json" % rid,
              OrderedDict([("algorithm", "greedy"), ("seed", 1), ("resolution", 64)]))


def basic_project(profile="full", offset=(0, 0)):
    """The basic 2D reference: one square frame, one layout of 8 nails, one spool, one image with a
    tiny PNG and one result with a 7-step sequence. `offset` shifts the frame and its nails."""
    dx, dy = offset
    p = Project(profile)
    p.set("spools.json", OrderedDict([("spools", [spool(1)])]))
    poly = [[x + dx, y + dy] for x, y in SQUARE]
    p.set("frames/1/frame.json", frame2d(1, polyline=poly))
    p.set("frames/1/layouts/1/layout.json", layout(1, 1, len(NAILS_2D)))
    p.set("frames/1/layouts/1/nails.json", nails2d([(x + dx, y + dy) for x, y in NAILS_2D]))
    add_image(p, 1, full=profile == "full")
    add_result(p, 1, 1, 1, [OrderedDict([("image", 1)])],
               [OrderedDict([("spool", 1), ("length_mm", 1000)])], SEQ_2D, full=profile == "full")
    return p


def add_frame2(p):
    """A second 2D frame (100 mm square, id 2) with layout 2 of four corner nails."""
    sq = [[-50, -50], [50, -50], [50, 50], [-50, 50]]
    p.set("frames/2/frame.json", frame2d(2, "Square 10", sq, width=100, height=100))
    p.set("frames/2/layouts/2/layout.json", layout(2, 2, 4))
    p.set("frames/2/layouts/2/nails.json", nails2d([(-50, -50), (50, -50), (50, 50), (-50, 50)]))
    return p


def two_frames_project():
    return add_frame2(basic_project())


# A closed tetrahedron whose bounding box is centred on the origin, and a cube.
TETRA_V = [(50, 50, 50), (50, -50, -50), (-50, 50, -50), (-50, -50, 50)]
TETRA_T = [(0, 1, 2), (0, 3, 1), (0, 2, 3), (1, 3, 2)]
CUBE_V = [(x, y, z) for x in (-50, 50) for y in (-50, 50) for z in (-50, 50)]
CUBE_T = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1),
          (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)]


def _unit(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def mesh_arrays(verts, tris, normals=True, uvs=False):
    pos = [float(c) for v in verts for c in v]
    idx = [i for t in tris for i in t]
    nrm = [c for v in verts for c in _unit(v)] if normals else None
    uv = [u for k in range(len(verts)) for u in (k / 10.0, 0.5)] if uvs else None
    return pos, idx, nrm, uv


def mesh3d_project(profile="full", cube=False, uvs=False):
    """A 3D frame with a closed mesh, one layout of 4 nails on triangles, a result with one view."""
    verts, tris = (CUBE_V, CUBE_T) if cube else (TETRA_V, TETRA_T)
    uvs = uvs and profile == "full"
    pos, idx, nrm, uv = mesh_arrays(verts, tris, True, uvs)
    p = Project(profile)
    p.set("spools.json", OrderedDict([("spools", [spool(1)])]))
    mesh = OrderedDict([("vertices", len(verts)), ("triangles", len(tris)), ("watertight", True),
                        ("manifold", True), ("has_normals", True), ("has_uvs", bool(uvs))])
    p.set("frames/1/frame.json", OrderedDict([
        ("id", 1), ("name", "Tetra"), ("kind", "3d"),
        ("dimensions", OrderedDict([("width_mm", 100), ("height_mm", 100), ("depth_mm", 100)])),
        ("background", "#ffffff"),
        ("outline", OrderedDict([("type", "polygon"), ("params", OrderedDict()), ("polyline", [])])),
        ("exclusions", []), ("surface", "mesh"), ("body", "solid"), ("mesh", mesh)]))
    p.set("frames/1/mesh.bin", mesh_bin(pos, idx, nrm, uv))
    nails = []
    for i in range(4):
        tri = tris[i]
        pts = [verts[k] for k in tri]
        c = [sum(q[j] for q in pts) / 3.0 for j in range(3)]
        ax = _unit(c)
        nails.append(OrderedDict([("id", i), ("x", round(c[0], 6)), ("y", round(c[1], 6)), ("z", round(c[2], 6)),
                                  ("diameter", 2), ("kind", "boundary"), ("axis", [round(a, 9) for a in ax]),
                                  ("length", 10), ("side", "exterior"),
                                  ("face", OrderedDict([("triangle", i), ("u", 0.333333), ("v", 0.333333)]))]))
    p.set("frames/1/layouts/1/layout.json", layout(1, 1, 4))
    p.set("frames/1/layouts/1/nails.json", OrderedDict([("nails", nails)]))
    add_image(p, 1, full=profile == "full")
    view = OrderedDict([("position", [0, 0, 300]), ("target", [0, 0, 0]), ("up", [0, 1, 0]),
                        ("projection", "orthographic"), ("size", 200), ("weight", 1.0)])
    add_result(p, 1, 1, 1, [OrderedDict([("image", 1), ("view", view)])],
               [OrderedDict([("spool", 1), ("length_mm", 500)])],
               [(0, 0, 1, 0), (1, 0, 0, 0), (2, 1, 0, 0), (3, 0, 2, 0)], full=profile == "full")
    return p


def small_reference():
    """The 'small reference' of the truncation sweep: the basic project without the optional
    full-only entries, so that the file stays a few KB."""
    p = basic_project("full")
    p.remove("images/1/source.png")
    img = p.doc("images/1/image.json")
    del img["source"]
    p.set("images/1/image.json", img)
    return p
