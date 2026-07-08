#!/usr/bin/env python3
"""Decode a DXT1/BC1 texture from an NFSU2 STREAM TPK and write a PNG.
Pure stdlib (zlib for PNG deflate) — no external deps."""
import struct
import zlib
import sys


def rgb565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def dxt1_decode(block_data, w, h):
    """Return list of (r,g,b) rows*cols for a DXT1 image."""
    out = bytearray(w * h * 3)
    bi = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from("<HHI", block_data, bi)
            bi += 8
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1:
                p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
                p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
            else:
                p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
                p3 = (0, 0, 0)
            pal = (p0, p1, p2, p3)
            for py in range(4):
                for px in range(4):
                    x, y = bx + px, by + py
                    if x >= w or y >= h:
                        continue
                    idx = (bits >> (2 * (py * 4 + px))) & 3
                    r, g, b = pal[idx]
                    o = (y * w + x) * 3
                    out[o], out[o + 1], out[o + 2] = r, g, b
    return out


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    with open("TRACKS/STREAML4RH.BUN", "rb") as f:
        data = f.read()
    base = 0x4000 + 8
    pixel_base = base + 0x378 + 8  # 0xb3320000 payload start
    htab = base + 0x38 + 8  # 0xb3310000 payload start

    # read texture records straight from the header table
    texs = []
    for name, pos in [("RDP_PARKING", 0xc0), ("TRN_GRASSC", 0x13c),
                      ("OBJ_BLKPLAS", 0x1b8), ("OBJ_PYLON", 0x234)]:
        off = struct.unpack_from("<I", data, htab + pos + 0x24)[0]
        w, h = struct.unpack_from("<HH", data, htab + pos + 0x38)
        texs.append((name, off, w, h))
    want = sys.argv[1] if len(sys.argv) > 1 else "TRN_GRASSC"
    for name, off, w, h in texs:
        if name != want and want != "all":
            continue
        need = w * h // 2  # DXT1 base level
        blk = data[pixel_base + off: pixel_base + off + need]
        rgb = dxt1_decode(blk, w, h)
        # crude sanity: average color
        n = w * h
        ar = sum(rgb[0::3]) / n
        ag = sum(rgb[1::3]) / n
        ab = sum(rgb[2::3]) / n
        out = f"tex_{name}.png"
        write_png(out, w, h, rgb)
        print(f"{name}: {w}x{h} avg RGB=({ar:.0f},{ag:.0f},{ab:.0f}) -> {out}")
