#!/usr/bin/env python3
"""Collect all 0x8003b601 mesh-chunk groups (b602 header, b603/b604 index
buffers, b605 vertex buffer, optional b607) from a .BUN file, skipping
degenerate (all-zero payload) samples, so we have real data to reverse
the vertex/index layout from.
"""
import struct
import sys


def iter_chunks(f, start, end):
    f.seek(start)
    while f.tell() < end:
        pos = f.tell()
        hdr = f.read(8)
        if len(hdr) < 8:
            break
        magic, size = struct.unpack("<II", hdr)
        data_start = f.tell()
        yield pos, magic, size, data_start
        f.seek(data_start + size)


def walk_groups(path):
    groups = []
    with open(path, "rb") as f:
        f.seek(0, 2)
        filesize = f.tell()

        def recurse(start, end):
            for pos, magic, size, data_start in iter_chunks(f, start, end):
                if magic == 0x8003b601:
                    group = {}
                    for _, m2, s2, ds2 in iter_chunks(f, data_start, data_start + size):
                        f.seek(ds2)
                        group[m2] = f.read(s2)
                    groups.append(group)
                elif magic != 0 and (magic >> 28) == 8:
                    recurse(data_start, data_start + size)

        recurse(0, filesize)
    return groups


def is_degenerate(group):
    vtx = group.get(0x0003b605, b"")
    return len(vtx) == 0 or vtx == b"\x00" * len(vtx)


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "TRACKS/L4RH.BUN"
    groups = walk_groups(path)
    real = [g for g in groups if not is_degenerate(g)]
    print(f"{path}: {len(groups)} mesh groups total, {len(real)} non-degenerate")
    for i, g in enumerate(real[:10]):
        sizes = {hex(k): len(v) for k, v in sorted(g.items())}
        print(f"  [{i}] sizes={sizes}")
