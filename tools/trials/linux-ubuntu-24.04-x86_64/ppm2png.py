#!/usr/bin/env python3
"""Convert binary PPM (P6) files to PNG with the standard library only (zlib).

Usage: ppm2png.py [--crop X0,Y0,X1,Y1] [--scale N] file.ppm [file.png]   (the png defaults to the same name)
A scale above 1 enlarges the picture by repeating pixels (a magnified copy for reading small text).
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    assert parts[0] == b"P6" and parts[3] == b"255"
    return int(parts[1]), int(parts[2]), data[pos:]


def write_png(path, w, h, rgb):
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def scaled(w, h, rgb, n):
    if n == 1:
        return w, h, rgb
    out = bytearray()
    for y in range(h):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        big = b"".join(row[x * 3:x * 3 + 3] * n for x in range(w))
        out += big * n
    return w * n, h * n, bytes(out)


def cropped(w, h, rgb, box):
    x0, y0, x1, y1 = box
    out = b"".join(rgb[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1))
    return x1 - x0, y1 - y0, out


def main(argv):
    scale = 1
    box = None
    while argv and argv[0] in ("--scale", "--crop"):
        if argv[0] == "--scale":
            scale = int(argv[1])
        else:
            box = [int(v) for v in argv[1].split(",")]
        argv = argv[2:]
    if not argv:
        sys.exit(__doc__)
    src = argv[0]
    dst = argv[1] if len(argv) > 1 else src.rsplit(".", 1)[0] + ".png"
    w, h, rgb = read_ppm(src)
    if box:
        w, h, rgb = cropped(w, h, rgb, box)
    write_png(dst, *scaled(w, h, rgb, scale))
    print("%s -> %s (%dx%d)" % (src, dst, w * scale, h * scale))


if __name__ == "__main__":
    main(sys.argv[1:])
