#!/usr/bin/env python3
"""Brute-force candidate (header_size, stride) pairs for the 0x3b608 chunk
by requiring: (a) (chunk_size - header) % stride == 0 for every sample
across multiple files, and (b) the resulting element count is consistent
with the max index referenced in the sibling 0x3b603/0x3b604 index buffers
(read as u16), which lower-bounds the real vertex count.
"""
import struct
from extract_mesh_groups import walk_groups, is_degenerate

FILES = ["TRACKS/L4RA.BUN", "TRACKS/L4RH.BUN", "TRACKS/L4RD.BUN"]

samples = []
for path in FILES:
    for g in walk_groups(path):
        if is_degenerate(g):
            continue
        b608 = g.get(0x0003b608)
        b603 = g.get(0x0003b603, b"")
        b604 = g.get(0x0003b604, b"")
        if not b608:
            continue
        max_idx = 0
        for buf in (b603, b604):
            n = len(buf) // 2
            if n:
                vals = struct.unpack(f"<{n}H", buf[: n * 2])
                max_idx = max(max_idx, max(vals))
        samples.append((path, len(b608), max_idx, b608))

print(f"{len(samples)} samples with b608 across files")
for path, size, max_idx, _ in samples:
    print(f"  {path}: b608_size={size} max_index_seen={max_idx}")

print("\nsearching (header, stride) pairs valid for ALL samples...")
sizes = [s[1] for s in samples]
max_idxs = [s[2] for s in samples]
candidates = []
for header in range(0, 64, 4):
    for stride in range(4, 80, 4):
        ok = True
        counts = []
        for size, max_idx in zip(sizes, max_idxs):
            rem = size - header
            if rem <= 0 or rem % stride != 0:
                ok = False
                break
            count = rem // stride
            if count <= max_idx:  # vertex buffer must cover every referenced index
                ok = False
                break
            counts.append(count)
        if ok:
            candidates.append((header, stride, counts))

for header, stride, counts in candidates:
    print(f"  header={header} stride={stride}  counts={counts}")
