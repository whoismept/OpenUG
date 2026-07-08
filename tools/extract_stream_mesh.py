#!/usr/bin/env python3
"""Extract 0x134xxx-family mesh objects from a STREAM/GEOMETRY .BUN/.BIN
file and export to Wavefront OBJ (+ a dependency-free SVG wireframe
preview). Vertex format is the solved car/scenery FVF:
  - 24-byte stride: pos(3f) + color(u32) + uv(2f)   [scenery, no normal]
  - 36-byte stride: pos(3f) + normal(3f) + color(u32) + uv(2f)  [car]
Index buffer is 0x134b03 (u16 triangle list). A run of 0x11 filler bytes
precedes the real vertex array in 0x134b01.
"""
import struct
import sys
import math


def iter_chunks(data, start, end):
    off = start
    while off + 8 <= end:
        magic, size = struct.unpack_from("<II", data, off)
        ds = off + 8
        yield off, magic, size, ds
        off = ds + size


def find_leaves(data, start, end, want, out):
    for _, magic, size, ds in iter_chunks(data, start, end):
        if magic == want:
            out.append((ds, size))
        elif magic != 0 and (magic >> 28) == 8:
            find_leaves(data, ds, ds + size, want, out)


def skip_filler(buf):
    i = 0
    while i < len(buf) and buf[i] == 0x11:
        i += 1
    return i


def decode_vertices(buf):
    """Try 24- then 36-byte stride; pick the one whose color field decodes
    to opaque-ish ARGB (high alpha byte) for most records."""
    start = skip_filler(buf)
    body = buf[start:]
    best = None
    for stride, color_off in ((24, 12), (36, 24)):
        if len(body) % stride:
            continue
        n = len(body) // stride
        if n < 3:
            continue
        good = 0
        for i in range(n):
            a = body[i * stride + color_off + 3]  # ARGB -> alpha is high byte
            if a in (0xFF, 0xFE, 0xBE, 0x3E, 0x00):  # common alpha values seen
                good += 1
        score = good / n
        if best is None or score > best[0]:
            verts = []
            for i in range(n):
                px, py, pz = struct.unpack_from("<3f", body, i * stride)
                verts.append((px, py, pz))
            best = (score, stride, verts)
    return best[2] if best else []


def extract(path):
    with open(path, "rb") as f:
        data = f.read()
    objects = []
    # each top-level 0x80134000 is one object; grab its vtx + idx leaves
    for _, magic, size, ds in iter_chunks(data, 0, len(data)):
        if magic != 0x80134000:
            continue
        vtx_leaves, idx_leaves = [], []
        find_leaves(data, ds, ds + size, 0x00134B01, vtx_leaves)
        find_leaves(data, ds, ds + size, 0x00134B03, idx_leaves)
        for (vo, vs), (io, isz) in zip(vtx_leaves, idx_leaves):
            verts = decode_vertices(data[vo:vo + vs])
            idx = struct.unpack_from(f"<{isz // 2}H", data, io)
            tris = [idx[i:i + 3] for i in range(0, len(idx) - 2, 3)]
            tris = [t for t in tris if max(t) < len(verts)]
            if verts and tris:
                objects.append((verts, tris))
    return objects


def write_obj(objects, path):
    with open(path, "w") as f:
        base = 1
        for oi, (verts, tris) in enumerate(objects):
            f.write(f"o object_{oi}\n")
            for x, y, z in verts:
                f.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
            for a, b, c in tris:
                f.write(f"f {a + base} {b + base} {c + base}\n")
            base += len(verts)


def write_svg(objects, path, size=800):
    # orthographic projection of X/Y, all objects overlaid
    xs = [v[0] for verts, _ in objects for v in verts]
    ys = [v[1] for verts, _ in objects for v in verts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    spanx, spany = (maxx - minx) or 1, (maxy - miny) or 1
    span = max(spanx, spany)
    pad = 20

    def proj(x, y):
        sx = pad + (x - minx) / span * (size - 2 * pad)
        sy = size - (pad + (y - miny) / span * (size - 2 * pad))
        return sx, sy

    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" '
             f'height="{size}" style="background:#111">']
    palette = ["#4fc3f7", "#81c784", "#ffb74d", "#e57373", "#ba68c8",
               "#fff176", "#4db6ac", "#f06292"]
    for oi, (verts, tris) in enumerate(objects):
        col = palette[oi % len(palette)]
        for a, b, c in tris:
            pa, pb, pc = proj(*verts[a][:2]), proj(*verts[b][:2]), proj(*verts[c][:2])
            lines.append(
                f'<polygon points="{pa[0]:.1f},{pa[1]:.1f} {pb[0]:.1f},'
                f'{pb[1]:.1f} {pc[0]:.1f},{pc[1]:.1f}" fill="none" '
                f'stroke="{col}" stroke-width="0.5" stroke-opacity="0.7"/>')
    lines.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "TRACKS/STREAML4RH.BUN"
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    objs = extract(src)[:limit]
    tv = sum(len(v) for v, _ in objs)
    tt = sum(len(t) for _, t in objs)
    print(f"{src}: {len(objs)} mesh objects, {tv} verts, {tt} triangles")
    write_obj(objs, "mesh_out.obj")
    write_svg(objs, "mesh_out.svg")
    print("wrote mesh_out.obj and mesh_out.svg")
