#!/usr/bin/env python3
"""props_audit.py — Phase 2 audit: verify the 0x80134010 object transform
pipeline (n2_obj_matrix / n2_add_pair) against real track data, WITHOUT
touching the parser.

For every 0x80134010 object in a STREAM .BUN it reports:
  - material/name class (ROAD / TERRAIN / OTHER)
  - whether a 4x4 matrix was found at 0x134011+0x40 (post-filler)
  - translation read the CURRENT way (m[12..14], row-major/D3D row-vector)
  - translation read the TRANSPOSED way (m[3],m[7],m[11])
  - the rotation block's orthonormality error (a real placement matrix has
    orthonormal 3x3 rows; garbage/transposed reads usually don't)
  - local-vertex bbox centre + span, and the world centre under the current
    interpretation

Usage: props_audit.py TRACKS/STREAML4RA.BUN [--limit N]
"""
import struct, sys

def u32(d, o): return struct.unpack_from("<I", d, o)[0]
def f32(d, o): return struct.unpack_from("<f", d, o)[0]

def find_leaves(d, beg, end, want, out):
    o = beg
    while o + 8 <= end:
        magic, size = u32(d, o), u32(d, o + 4)
        ds = o + 8
        if magic == want: out.append((ds, size))
        elif magic != 0 and (magic >> 28) == 8:
            find_leaves(d, ds, ds + size, want, out)
        o = ds + size

def skip_filler(d, o, n):
    i = 0
    while i < n and d[o + i] == 0x11: i += 1
    return i

def name_of(d, off, size):
    p = d[off:off + size]
    i = 0
    while i < len(p) - 5:
        if 65 <= p[i] <= 90:
            j = i
            while j < len(p) and (p[j] == 95 or 65 <= p[j] <= 90 or 48 <= p[j] <= 57): j += 1
            if j - i >= 5: return p[i:j].decode()
            i = j
        i += 1
    return "?"

def ortho_err(m, transposed):
    """max deviation of the 3x3 basis rows from orthonormal (0 = perfect)."""
    if transposed:
        rows = [(m[0], m[1], m[2]), (m[4], m[5], m[6]), (m[8], m[9], m[10])]
    else:  # current reading: basis vectors are m[0..2], m[4..6], m[8..10] too —
        rows = [(m[0], m[4], m[8]), (m[1], m[5], m[9]), (m[2], m[6], m[10])]
    err = 0.0
    for a in range(3):
        for b in range(3):
            dot = sum(rows[a][k] * rows[b][k] for k in range(3))
            err = max(err, abs(dot - (1.0 if a == b else 0.0)))
    return err

def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    d = open(path, "rb").read()
    objs = []
    # top-level walk for 0x80134010 containers
    def walk(beg, end):
        o = beg
        while o + 8 <= end:
            magic, size = u32(d, o), u32(d, o + 4)
            ds = o + 8
            if magic == 0x80134010: objs.append((ds, size))
            elif magic != 0 and (magic >> 28) == 8: walk(ds, ds + size)
            o = ds + size
    walk(0, len(d))
    print(f"{len(objs)} objects in {path}")

    stats = {"nomtx": 0, "identity": 0, "placed": 0}
    shown = 0
    for ds, size in objs:
        hdrs = []; find_leaves(d, ds, ds + size, 0x00134011, hdrs)
        vtxs = []; find_leaves(d, ds, ds + size, 0x00134B01, vtxs)
        if not vtxs: continue
        name = name_of(d, hdrs[0][0], hdrs[0][1]) if hdrs else "?"
        m = None
        if hdrs:
            ho, hs = hdrs[0]
            pad = skip_filler(d, ho, hs)
            if pad + 0x40 + 64 <= hs:
                m = [f32(d, ho + pad + 0x40 + i * 4) for i in range(16)]
                if not (0.5 <= m[15] <= 1.5): m = None
        # local bbox of first vertex leaf (stride 24, pos at +0)
        vo, vs = vtxs[0]
        pad = skip_filler(d, vo, vs)
        body = vs - pad
        if body <= 0 or body % 24: continue
        n = body // 24
        mn = [1e30] * 3; mx = [-1e30] * 3
        for i in range(n):
            for c in range(3):
                v = f32(d, vo + pad + i * 24 + c * 4)
                mn[c] = min(mn[c], v); mx[c] = max(mx[c], v)
        lc = [(mn[c] + mx[c]) / 2 for c in range(3)]
        span = max(mx[c] - mn[c] for c in range(3))
        if m is None:
            stats["nomtx"] += 1
            tag = "NOMTX"
            t_cur = t_tr = (0, 0, 0); wc = lc; oe = oe_t = -1
        else:
            t_cur = (m[12], m[13], m[14])
            t_tr = (m[3], m[7], m[11])
            ident = all(abs(m[i] - (1.0 if i % 5 == 0 else 0.0)) < 1e-4 for i in range(16))
            stats["identity" if ident else "placed"] += 1
            tag = "IDENT" if ident else "PLACED"
            wc = [lc[0]*m[0]+lc[1]*m[4]+lc[2]*m[8]+m[12],
                  lc[0]*m[1]+lc[1]*m[5]+lc[2]*m[9]+m[13],
                  lc[0]*m[2]+lc[1]*m[6]+lc[2]*m[10]+m[14]]
            oe = ortho_err(m, False); oe_t = ortho_err(m, True)
        if shown < limit and (tag != "IDENT" or shown < 8):
            print(f"[{tag}] {name[:28]:28s} loc=({lc[0]:8.1f},{lc[1]:8.1f},{lc[2]:7.1f}) span={span:7.1f} "
                  f"t_cur=({t_cur[0]:8.1f},{t_cur[1]:8.1f},{t_cur[2]:7.1f}) t_tr=({t_tr[0]:8.1f},{t_tr[1]:8.1f},{t_tr[2]:7.1f}) "
                  f"world=({wc[0]:8.1f},{wc[1]:8.1f},{wc[2]:7.1f}) oerr={oe:.3f}/{oe_t:.3f}")
            shown += 1
    print("summary:", stats)

if __name__ == "__main__":
    main()
