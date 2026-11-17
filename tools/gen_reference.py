#!/usr/bin/env python
"""Independent description of the content of the `multi` reference project (spec 002, T054).

Writes core/tests/format/multi.expected.json from the SPECIFICATION of `multi` (3 frames incl. a 3D
cube, 5 layouts, 4 images, 6 spools, 5 results: core/tests/multi_project.hpp is its C++ builder), with
no Loomora code and no C++ involved: the nails (a circle, a heart, rings, a ring on the top face of the
cube) and the steps (the formulas of the builder, restated here) are computed again with the Python
standard library. The Catch2 test core/tests/format/test_multi_project.cpp compares what the library
reads from `multi` to this file, and `tools/check_loomora.py --expect` compares what the independent
checker reads to it.

Usage:
  python tools/gen_reference.py            write core/tests/format/multi.expected.json
  python tools/gen_reference.py --check    exit 1 when the committed file is not what this script writes
  python tools/gen_reference.py --stdout   print it
"""
import json
import math
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "core", "tests", "format", "multi.expected.json")

TWO_PI = 6.283185307179586


def round3(v):
    """Rounding to 0.001 mm used by the builder: floor(v * 1000 + 0.5) / 1000."""
    return math.floor(v * 1000.0 + 0.5) / 1000.0


def clean(v):
    """JSON number: an integer when the value is whole."""
    return int(v) if float(v).is_integer() else v


def ring(count, radius):
    return [[clean(round3(radius * math.cos(TWO_PI * i / count))), clean(round3(radius * math.sin(TWO_PI * i / count)))]
            for i in range(count)]


def heart(count, size):
    k = size / 32.0
    out = []
    for i in range(count):
        t = TWO_PI * i / count
        s = math.sin(t)
        out.append([clean(round3(k * 16.0 * s * s * s)),
                    clean(round3(k * (13.0 * math.cos(t) - 5.0 * math.cos(2 * t) - 2.0 * math.cos(3 * t) - math.cos(4 * t))))])
    return out


def ring3d(count):
    return [[clean(round3(30.0 * math.cos(TWO_PI * i / count))), clean(round3(30.0 * math.sin(TWO_PI * i / count))), 50]
            for i in range(count)]


def steps(count, spools, nails):
    """The steps of a result: every spool starts, goes round, ends (formulas of the builder)."""
    n = len(spools)
    out = []
    for i in range(count):
        first = i < n
        last = i >= count - n
        s = i if first else (i - (count - n) if last else (i * 5 + 1) % n)
        out.append((((i * 7 + s * 3) % nails), s, i & 1, 1 if first else (2 if last else 0)))
    return out


def packed_crc(st):
    """CRC-32 over the 12-byte little-endian records (nail u32, spool_index u32, rotation, type, level, 0)."""
    raw = b"".join(struct.pack("<IIBBBB", nail, sp, rot, typ, 0, 0) for nail, sp, rot, typ in st)
    return "%08x" % (zlib.crc32(raw) & 0xFFFFFFFF)


def build():
    colors = ["#000000", "#ff0000", "#0000ff", "#00aa00", "#ffaa00", "#808080"]
    spools = [{"id": i + 1, "name": "Spool %d" % (i + 1), "color": colors[i], "opacity": 1, "diameter_mm": 0.3,
               "available_length_mm": 200000, "material": "cotton", "domain": "exterior"} for i in range(6)]
    frames = [
        {"id": 1, "name": "Square 50", "kind": "2d", "width_mm": 500, "height_mm": 500},
        {"id": 2, "name": "Wide 60", "kind": "2d", "width_mm": 600, "height_mm": 400},
        {"id": 3, "name": "Cube 100", "kind": "3d", "width_mm": 100, "height_mm": 100, "depth_mm": 100,
         "mesh": {"vertices": 8, "triangles": 12, "normals": True, "uvs": True}},
    ]
    lay = [  # id, frame, name, nails, labels
        (1, 1, "Circle 200", ring(120, 200), {"0": "first"}),
        (2, 1, "Heart 190", heart(96, 380), {"0": "cleft"}),
        (3, 2, "Ring 100", ring(80, 100), {}),
        (4, 2, "Ring 50", ring(40, 50), {}),
        (5, 3, "Top ring", ring3d(24), {}),
    ]
    layouts = []
    for ident, frame, name, nails, labels in lay:
        d = {"id": ident, "frame": frame, "name": name, "nail_count": len(nails), "nails": nails}
        if labels:
            d["labels"] = labels
        layouts.append(d)
    images = [
        {"id": 1, "name": "Portrait", "color_mode": "bw", "shape": "circle", "width_px": 8, "height_px": 8},
        {"id": 2, "name": "Landscape", "color_mode": "gray", "shape": "landscape", "width_px": 8, "height_px": 8},
        {"id": 3, "name": "Color", "color_mode": "color", "shape": "portrait", "width_px": 8, "height_px": 8},
        {"id": 4, "name": "Square", "color_mode": "color", "shape": "square", "width_px": 8, "height_px": 8},
    ]
    nails_of = dict((l["id"], l["nail_count"]) for l in layouts)
    res = [  # id, name, frame, layout, images, spool ids, length base, steps
        (1, "Heart", 1, 2, [1], [1, 2], 1000, 400),
        (2, "Circle", 1, 1, [2], [2, 3, 4], 2000, 600),
        (3, "Ring", 2, 3, [1], [1, 4], 1500, 300),
        (4, "Cube", 3, 5, [3, 4], [5, 6], 800, 240),
        (5, "Circle again", 1, 1, [3], [1, 3, 5, 6], 3000, 500),
    ]
    results = []
    for ident, name, frame, layout, imgs, sp, base, count in res:
        st = steps(count, sp, nails_of[layout])
        results.append({
            "id": ident, "name": name, "frame": frame, "layout": layout, "images": imgs,
            "views": ident == 4,
            "spools": [[s, base + 10 * i] for i, s in enumerate(sp)],
            "steps": count, "steps_crc32": packed_crc(st),
            "first_steps": [list(x) for x in st[:12]], "last_steps": [list(x) for x in st[-12:]],
        })
    used = set((r["frame"], r["layout"]) for r in results)
    return {
        "about": "Expected content of the `multi` reference project; written by tools/gen_reference.py",
        "title": "Several items",
        "counts": {"frames": 3, "layouts": 5, "images": 4, "spools": 6, "results": 5},
        "required_features": ["surface_mesh", "multi_view_target"],
        "spools": spools, "frames": frames, "layouts": layouts, "images": images, "results": results,
        "combinations_without_result": [[l["frame"], l["id"]] for l in layouts if (l["frame"], l["id"]) not in used],
    }


def render(doc):
    """One line per item (a nail list stays on its item's line): readable and a small diff."""
    def one(v):
        return json.dumps(v, separators=(",", ":"), ensure_ascii=True)
    lines = []
    for key, value in doc.items():
        if isinstance(value, list) and value and isinstance(value[0], dict):
            lines.append('%s: [\n  %s\n ]' % (one(key), ",\n  ".join(one(x) for x in value)))
        else:
            lines.append("%s: %s" % (one(key), one(value)))
    return "{\n " + ",\n ".join(lines) + "\n}\n"


def main(argv):
    text = render(build())
    if "--stdout" in argv:
        sys.stdout.write(text)
        return 0
    if "--check" in argv:
        try:
            with open(OUT, "rb") as f:
                same = f.read().decode("utf-8").replace("\r\n", "\n") == text  # autocrlf checkouts
        except OSError:
            same = False
        print("multi.expected.json is %s" % ("up to date" if same else "NOT what gen_reference.py writes"))
        return 0 if same else 1
    with open(OUT, "wb") as f:
        f.write(text.encode("utf-8"))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
