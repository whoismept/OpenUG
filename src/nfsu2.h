/* nfsu2.h — single-header loader for NFSU2 track assets.
 * Ports the reverse-engineered .BUN chunk format + DXT1 texture decode to C.
 * Public domain, stdlib only. See README for the format notes.
 */
#ifndef NFSU2_H
#define NFSU2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* One drawable submesh: interleaved [px,py,pz,u,v] verts + u16 triangle list. */
enum { N2_ROAD = 0, N2_TERRAIN = 1, N2_OTHER = 2, N2_SKY = 3, N2_GLOW = 4,
       /* car mesh classes, from material name */
       N2_CAR_BODY = 10, N2_CAR_GLASS = 11, N2_CAR_LIGHT = 12,
       N2_CAR_TIRE = 13, N2_CAR_MISC = 14, N2_CAR_BRAKELIGHT = 15, N2_CAR_MECH = 16 };
typedef struct {
    float   *verts;   /* 5 floats per vertex: pos.xyz, uv */
    int      nverts;
    uint16_t *idx;
    int      nidx;
    int      cat;     /* N2_ROAD / N2_TERRAIN / N2_OTHER, from material name */
    uint32_t texkey;  /* car meshes: bound TPK diffuse key (0x134012), 0 if none */
} N2Mesh;

typedef struct {
    N2Mesh *meshes;
    int     count, cap;
} N2Scene;

/* Decoded RGB texture (3 bytes/pixel, top-left origin). alpha is a separate
 * w*h plane, only non-NULL for DXT3 car textures (decal/badge masks). */
typedef struct { int w, h; unsigned char *rgb; unsigned char *alpha; } N2Tex;

/* ---- file I/O ---- */
static unsigned char *n2_read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(n);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

static uint32_t n2_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Skip the run of 0x11 filler bytes that prefixes a vertex/leaf payload. */
static int n2_skip_filler(const unsigned char *p, int n) {
    int i = 0; while (i < n && p[i] == 0x11) i++; return i;
}

/* ---- mesh extraction ---- */
static void n2_push_mesh(N2Scene *s, N2Mesh m) {
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->meshes = (N2Mesh *)realloc(s->meshes, s->cap * sizeof(N2Mesh));
    }
    s->meshes[s->count++] = m;
}

/* Collect leaf chunk (start,size) pairs of a given magic within [beg,end). */
typedef struct { long off; uint32_t size; } N2Leaf;
static void n2_find_leaves(const unsigned char *d, long beg, long end,
                           uint32_t want, N2Leaf *out, int *n, int cap) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == want && *n < cap) { out[*n].off = ds; out[*n].size = size; (*n)++; }
        else if (magic != 0 && (magic >> 28) == 8) n2_find_leaves(d, ds, ds + size, want, out, n, cap);
        o = ds + size;
    }
}

/* substring search within an unterminated byte run */
static int n2_contains(const unsigned char *hay, long n, const char *needle) {
    long m = (long)strlen(needle);
    for (long i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0) return 1;
    return 0;
}

/* case-insensitive substring search: retail's world-object material names
 * are NOT uniformly cased like the ROAD/TERRAIN/SKY prefixes below — e.g.
 * "XO_BulbAOrange_1a_00", "XB_CasinoF_Neon_1a_LL_00" — so glow/neon/flare
 * detection can't reuse the uppercase-run scan those checks rely on. */
static int n2_icontains(const unsigned char *hay, long n, const char *needle) {
    long m = (long)strlen(needle);
    for (long i = 0; i + m <= n; i++) {
        long k = 0;
        while (k < m) {
            unsigned char a = hay[i+k], b = (unsigned char)needle[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
            k++;
        }
        if (k == m) return 1;
    }
    return 0;
}

/* Classify a 0x80134010 mesh by the material name in its first 0x134011 leaf.
 * Names look like TRN_ROADA_CHOP_*, TRN_TERRAINA_*, XO_TRAFFICCONE_* ... */
static int n2_mesh_category(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        /* neon signs, bulbs, lens flares: real light *emitters*, distinct
         * from opaque fixtures (StreetLightCbWALL, LightPoleB, TrafficLightC
         * are the pole/housing mesh — matching bare LIGHT/LAMP would also
         * catch those and wrongly make solid poles additive-transparent).
         * Checked case-insensitively: unlike SKY/ROAD/TERRAIN below, retail
         * casing for these is inconsistent per-region (mixed vs. upper). */
        if (n2_icontains(p, s, "NEON") || n2_icontains(p, s, "GLOW") ||
            n2_icontains(p, s, "FLARE") || n2_icontains(p, s, "BULB"))
            return N2_GLOW;
        for (long i = 0; i + 5 < s; i++) {
            /* start of an uppercase A-Z name run */
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
                    if (n2_contains(n, L, "SKYDOME") || n2_contains(n, L, "SKY")) return N2_SKY;
                    if (n2_contains(n, L, "ROAD")) return N2_ROAD;
                    if (n2_contains(n, L, "TERRAIN")) return N2_TERRAIN;
                    return N2_OTHER;
                }
                i = j;
            }
        }
    }
    return N2_OTHER;
}

/* Extract one (vertex,index) leaf pair. stride = 24 (scenery, uv@16) or 36
 * (car, normal@12 uv@24). cull_skybox drops huge shells (tracks only). */
static void n2_add_pair(const unsigned char *d, N2Leaf vtx, N2Leaf idx,
                        int cat, N2Scene *scene,
                        int stride, int uvoff, int cull_skybox, uint32_t texkey,
                        const float *mtx) {
    const unsigned char *vb = d + vtx.off;
    int vlen = (int)vtx.size;
    int pad = n2_skip_filler(vb, vlen);
    int body = vlen - pad;
    if (body <= 0 || body % stride) return;
    int n = body / stride;
    const unsigned char *rec = vb + pad;

    if (cull_skybox) {
        float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
        for (int i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) {
                float v; memcpy(&v, rec + i*stride + c*4, 4);
                if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v;
            }
        float span = 0;
        for (int c = 0; c < 3; c++) if (mx[c]-mn[c] > span) span = mx[c]-mn[c];
        /* the skybox is dropped by material name now; keep only an absurd-span
           safety so big city ground/buildings (thousands of units) still draw. */
        if (span >= 60000.0f) return;
    }

    N2Mesh m; memset(&m, 0, sizeof(m));
    m.cat = cat; m.texkey = texkey; m.nverts = n;
    m.verts = (float *)malloc(n * 5 * sizeof(float));
    for (int i = 0; i < n; i++) {
        float px, py, pz;
        memcpy(&px, rec+i*stride,   4);
        memcpy(&py, rec+i*stride+4, 4);
        memcpy(&pz, rec+i*stride+8, 4);
        if (mtx) {   /* place local-space geometry into the world (track props) */
            m.verts[i*5+0] = px*mtx[0]+py*mtx[4]+pz*mtx[8] +mtx[12];
            m.verts[i*5+1] = px*mtx[1]+py*mtx[5]+pz*mtx[9] +mtx[13];
            m.verts[i*5+2] = px*mtx[2]+py*mtx[6]+pz*mtx[10]+mtx[14];
        } else { m.verts[i*5+0]=px; m.verts[i*5+1]=py; m.verts[i*5+2]=pz; }
        memcpy(m.verts + i*5 + 3, rec + i*stride + uvoff, 8);
    }
    /* the index leaf carries the same 0x11 filler prefix as the vertex leaf
       (as whole 0x1111 u16 words). Skipping it is essential: when the filler is
       not a multiple of 6 bytes (e.g. 4 or 8), leaving it in shifts the triangle
       grouping and shears every mesh — car wheels into urchins, buildings into
       loose planes. */
    const unsigned char *ib0 = d + idx.off; int ibytes = (int)idx.size, ip = 0;
    while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
    const unsigned char *ib = ib0 + ip;
    int nidx = (ibytes - ip) / 2;
    m.idx = (uint16_t *)malloc(nidx * sizeof(uint16_t));
    m.nidx = 0;
    for (int i = 0; i + 2 < nidx; i += 3) {
        uint16_t a = (uint16_t)(ib[i*2]     | ib[i*2+1]     << 8);
        uint16_t b = (uint16_t)(ib[(i+1)*2] | ib[(i+1)*2+1] << 8);
        uint16_t c = (uint16_t)(ib[(i+2)*2] | ib[(i+2)*2+1] << 8);
        if (a < n && b < n && c < n) {
            m.idx[m.nidx++] = a; m.idx[m.nidx++] = b; m.idx[m.nidx++] = c;
        }
    }
    if (m.nidx == 0) { free(m.verts); free(m.idx); return; }
    n2_push_mesh(scene, m);
}

/* Pick a track mesh's diffuse key from its 0x134012 slot list. Road/terrain use
 * the LAST slot (their own surface texture; missing -> asphalt/grass fallback,
 * never an earlier grass base slot). Props/buildings prefer the last slot that
 * actually RESOLVES (is in `keys`) — a building often lists a detail slot last
 * that lives in an unshipped pack, with the real facade in an earlier slot. */
static uint32_t n2_mesh_texkey_cat(const unsigned char *d, long beg, long end,
                               int cat, const uint32_t *keys, int nkeys) {
    N2Leaf t12[4]; int n12 = 0;
    n2_find_leaves(d, beg, end, 0x00134012u, t12, &n12, 4);
    uint32_t last = 0, lastres = 0;
    for (int a = 0; a < n12; a++) {
        const unsigned char *p = d + t12[a].off; long ls = t12[a].size;
        for (long b = 0; b + 4 <= ls; b += 4) {
            uint32_t v = n2_u32(p + b); if (!v) continue;
            last = v;
            for (int c = 0; c < nkeys; c++) if (keys[c] == v) { lastres = v; break; }
        }
    }
    if (cat == N2_ROAD || cat == N2_TERRAIN) return last;
    return lastres ? lastres : last;
}

/* A 0x80134010 object's 4x4 world transform, from its 0x134011 header (after
 * that header's own 0x11 filler): row-major, translation in row 3 (+0x40..+0x7f).
 * Track props are modelled in local space and placed by this matrix — without it
 * every building piles at the origin. Fills identity + returns 0 on miss. */
static int n2_obj_matrix(const unsigned char *d, long beg, long end, float *m) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    N2Leaf h[2]; int nh = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, h, &nh, 2);
    if (!nh) return 0;
    const unsigned char *p = d + h[0].off; int s = (int)h[0].size;
    int pad = n2_skip_filler(p, s);
    if (pad + 0x40 + 64 > s) return 0;
    for (int i = 0; i < 16; i++) memcpy(&m[i], p + pad + 0x40 + i*4, 4);
    if (m[15] < 0.5f || m[15] > 1.5f) {          /* not a sane affine matrix */
        for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return 0;
    }
    return 1;
}

/* Walk to every 0x80134010 mesh, classify it, extract its vtx/idx pairs. `keys`
 * is the set of texture keys resolvable for this region (local TPK + shared
 * pack), used to pick each mesh's diffuse slot. */
static void n2_walk_meshes(const unsigned char *d, long beg, long end, N2Scene *scene,
                           const uint32_t *keys, int nkeys) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == 0x80134010u) {
            int cat = n2_mesh_category(d, ds, ds + size);
            uint32_t tk = n2_mesh_texkey_cat(d, ds, ds + size, cat, keys, nkeys);
            float objm[16]; n2_obj_matrix(d, ds, ds + size, objm);   /* world placement */
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + size, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + size, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;
            /* the skybox shell is kept now (Phase 21: rendered camera-locked,
               depth-write off) so its absurd span must skip the size-safety
               cull that still guards every other category. */
            int cull = (cat != N2_SKY);
            for (int k = 0; k < pairs; k++)
                n2_add_pair(d, vtx[k], idx[k], cat, scene, 24, 16, cull, tk, objm);
        } else if (magic != 0 && (magic >> 28) == 8) {
            n2_walk_meshes(d, ds, ds + size, scene, keys, nkeys);
        }
        o = ds + size;
    }
}

/* Parse a STREAM .BUN into categorized world-space meshes. Returns mesh count. */
static int n2_load_scene(const unsigned char *d, long len, N2Scene *scene,
                         const uint32_t *keys, int nkeys) {
    memset(scene, 0, sizeof(*scene));
    n2_walk_meshes(d, 0, len, scene, keys, nkeys);
    return scene->count;
}

/* Classify a car 0x80134010 mesh by its material name (part name). */
static int n2_car_category(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
                    if (n2_contains(n,L,"WINDOW") || n2_contains(n,L,"GLASS")) return N2_CAR_GLASS;
                    /* no diffuse texture exists anywhere in the extracted data
                       for ANY light part (verified: not this car's own TPK, not
                       the shared CARS/TEXTURES.BIN, not any file under GLOBAL —
                       exhaustive search) — housings are chrome+lens by material
                       colour, same treatment as glass. BRAKE first: "BRAKELIGHT"
                       contains "LIGHT" too. */
                    if (n2_contains(n,L,"BRAKE") && n2_contains(n,L,"LIGHT"))  return N2_CAR_BRAKELIGHT;
                    if (n2_contains(n,L,"LIGHT") || n2_contains(n,L,"LAMP"))   return N2_CAR_LIGHT;
                    if (n2_contains(n,L,"TIRE") || n2_contains(n,L,"WHEEL"))   return N2_CAR_TIRE;
                    /* mechanical compartment detail (engine bay, exhaust pipe):
                       unpainted metal/plastic, not glossy body shell — checked
                       before the generic KIT/BODY catch-all below, since these
                       names also contain "KIT##" and would otherwise match it.
                       Parts that already carry their own texture (e.g. GOLF's
                       engine bay atlas) still use it unchanged; this only
                       changes the flat-colour FALLBACK for cars where the part
                       has no texture of its own (e.g. Miata's engine bay). */
                    if (n2_contains(n,L,"ENGINE") || n2_contains(n,L,"EXHAUST")) return N2_CAR_MECH;
                    if (n2_contains(n,L,"BASE") || n2_contains(n,L,"BODY") ||
                        n2_contains(n,L,"KIT")  || n2_contains(n,L,"STYLE"))   return N2_CAR_BODY;
                    return N2_CAR_MISC;
                }
                i = j;
            }
        }
    }
    return N2_CAR_MISC;
}

/* A car GEOMETRY.BIN holds EVERY customization part (stock body + all the body
 * kits, styles and widebodies) overlaid. Rendering them all is a mess, so skip
 * the alternates and keep only the stock config: drop STYLE*, KITW* (widebody)
 * and KIT01..KIT99, keeping KIT00 and un-kitted shared parts. Returns 1 = skip. */
static int n2_car_is_variant(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
                    if (n2_contains(n,L,"STYLE") || n2_contains(n,L,"KITW") ||
                        n2_contains(n,L,"WIDE")  ||   /* widebody variants (WIDE1..4) */
                        n2_contains(n,L,"DECAL"))     /* decal mount shells (used only
                                                         when a sticker is applied) */
                        return 1;
                    for (long q = 0; q + 4 < L; q++)          /* KIT01..KIT99, not KIT00 */
                        if (n[q]=='K'&&n[q+1]=='I'&&n[q+2]=='T'&&
                            n[q+3]>='0'&&n[q+3]<='9'&&n[q+4]>='0'&&n[q+4]<='9'&&
                            !(n[q+3]=='0'&&n[q+4]=='0')) return 1;
                    return 0;
                }
                i = j;
            }
        }
    }
    return 0;
}

/* Find this mesh's bound diffuse: scan its 0x134012 texture-slot list (8-byte
 * entries: key + 0) for a key present in the car's TPK (keys[]). 0 if none. */
static uint32_t n2_mesh_texkey(const unsigned char *d, long beg, long end,
                               const uint32_t *keys, int nkeys) {
    if (!nkeys) return 0;
    N2Leaf t12[4]; int n12 = 0;
    n2_find_leaves(d, beg, end, 0x00134012u, t12, &n12, 4);
    for (int a = 0; a < n12; a++) {
        const unsigned char *p = d + t12[a].off; long ls = t12[a].size;
        for (long b = 0; b + 4 <= ls; b += 4) {
            uint32_t v = n2_u32(p + b);
            for (int c = 0; c < nkeys; c++) if (v == keys[c]) return v;
        }
    }
    return 0;
}

/* Parse a car GEOMETRY.BIN (36-byte verts w/ normals), tagging each mesh with
 * a class from its material name and its per-mesh diffuse texture key.
 *
 * INVESTIGATED (car submesh materials, Golf, all findings verified against
 * real bytes): 0x134B02 DOES exist per car mesh object and DOES hold
 * multiple 60-byte records (same 0x11-filler-prefix convention as every
 * other leaf in this format; skip filler, then size/60 is exact) with
 * varying mat_id/flag fields — e.g. GOLF_KIT00_FRONT_BUMPER_A has 5 records,
 * mat_id 0-3, flag 0-4. Splitting the index buffer at these record
 * boundaries and checking each record's vertex bbox confirms they ARE real,
 * spatially-distinct sub-groups (a small asymmetric bracket vs. the big
 * symmetric shell, etc.) — this part of the directive was right.
 *
 * BUT: the chain that would make this useful for TEXTURE routing —
 * mat_id -> 0x134003 hash list -> a DIFFERENT 0x134011/0x134012 per submesh
 * — does not exist for car objects. Checked every Golf mesh: zero objects
 * have more than one 0x134011 material block or more than one 0x134012
 * texture-slot list. There is exactly one texture key per whole object,
 * full stop; mat_id/flag never select among alternatives because no
 * alternatives are stored. flag's value set is a small, consistent {0..4}
 * across unrelated meshes and mirrors (L/R copies of the same part keep the
 * same flag, different mat_id) — looks like a small built-in render-state
 * enum (cull mode / blend mode / vertex-color-source, guessing), not a
 * material-lookup key. So "bind a different texture per submesh" is not
 * implementable from this data — there is nothing per-submesh to bind.
 * Splitting meshes at these boundaries anyway (same texture on every
 * resulting piece) would add draw calls for a pixel-identical result, so
 * it isn't done. If a future car is found with >1 material/texslot block
 * per object, THAT would be the real signal this is worth revisiting.
 * removed vinyl/badge fallback, not a real submesh material). */
static void n2_walk_car(const unsigned char *d, long beg, long end, N2Scene *scene,
                        const uint32_t *keys, int nkeys) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4);
        long ds = o + 8;
        if (m == 0x80134010u) {
            if (n2_car_is_variant(d, ds, ds + s)) { o = ds + s; continue; }  /* stock only */
            int cat = n2_car_category(d, ds, ds + s);
            uint32_t tk = n2_mesh_texkey(d, ds, ds + s, keys, nkeys);
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + s, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + s, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;
            for (int k = 0; k < pairs; k++)   /* car parts have identity transforms */
                n2_add_pair(d, vtx[k], idx[k], cat, scene, 36, 28, 0, tk, NULL);
        } else if (m != 0 && (m >> 28) == 8) {
            n2_walk_car(d, ds, ds + s, scene, keys, nkeys);
        }
        o = ds + s;
    }
}
static int n2_load_car(const unsigned char *d, long len, N2Scene *scene,
                       const uint32_t *keys, int nkeys) {
    memset(scene, 0, sizeof(*scene));
    n2_walk_car(d, 0, len, scene, keys, nkeys);
    return scene->count;
}

static void n2_free_scene(N2Scene *s) {
    for (int i = 0; i < s->count; i++) { free(s->meshes[i].verts); free(s->meshes[i].idx); }
    free(s->meshes); memset(s, 0, sizeof(*s));
}

/* ---- AI racing-line path (ROUTES.../Paths...bin, chunk 0x34148) ---- */
/* 24-byte records: x(f32) y(f32) then unknown; a smooth 2D centerline. */
typedef struct { float *xy; int n; } N2Path;
static int n2_load_path(const unsigned char *d, long len, N2Path *p) {
    p->xy = NULL; p->n = 0;
    N2Leaf leaf[8]; int nl = 0;
    n2_find_leaves(d, 0, len, 0x00034148u, leaf, &nl, 8);
    if (!nl) return 0;
    int n = (int)leaf[0].size / 24;
    p->xy = (float *)malloc((size_t)n * 2 * sizeof(float));
    int c = 0;
    for (int i = 0; i < n; i++) {
        float x, y;
        memcpy(&x, d + leaf[0].off + i*24,     4);
        memcpy(&y, d + leaf[0].off + i*24 + 4, 4);
        if (x==x && y==y && x>-1e6f && x<1e6f && y>-1e6f && y<1e6f) {
            p->xy[c*2] = x; p->xy[c*2+1] = y; c++;
        }
    }
    p->n = c;
    return c;
}

/* Index of the racing-line waypoint nearest (x,y) — a car's progress metric. */
static int n2_nearest_wp(const N2Path *p, float x, float y) {
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < p->n; i++) {
        float dx=p->xy[i*2]-x, dy=p->xy[i*2+1]-y, d=dx*dx+dy*dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Ground height at (x,y): highest road/terrain triangle surface under the
 * point (barycentric z), or `fallback` if none covers it. Brute force over
 * track triangles — fine for ~10k tris/frame. */
static float n2_ground_z(N2Scene *s, float x, float y, float fallback) {
    float best = -1e30f; int found = 0;
    for (int m = 0; m < s->count; m++) {
        if (s->meshes[m].cat == N2_OTHER) continue;   /* road + terrain only */
        N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx; t += 3) {
            float *a = me->verts + me->idx[t]*5;
            float *b = me->verts + me->idx[t+1]*5;
            float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < -0.01f || v < -0.01f || w < -0.01f) continue;
            float z = u*a[2] + v*b[2] + w*c[2];
            if (z > best) { best = z; found = 1; }
        }
    }
    return found ? best : fallback;
}

/* ---- DXT1 / BC1 decode ---- */
static void n2_rgb565(uint16_t c, unsigned char *o) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
}
static void n2_dxt1(const unsigned char *src, int w, int h, unsigned char *out) {
    int bi = 0;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            uint16_t c0 = src[bi] | src[bi+1] << 8, c1 = src[bi+2] | src[bi+3] << 8;
            uint32_t bits = n2_u32(src + bi + 4); bi += 8;
            unsigned char pal[4][3];
            n2_rgb565(c0, pal[0]); n2_rgb565(c1, pal[1]);
            for (int j = 0; j < 3; j++) {
                if (c0 > c1) { pal[2][j] = (2*pal[0][j]+pal[1][j])/3; pal[3][j] = (pal[0][j]+2*pal[1][j])/3; }
                else         { pal[2][j] = (pal[0][j]+pal[1][j])/2; pal[3][j] = 0; }
            }
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    int x = bx+px, y = by+py; if (x >= w || y >= h) continue;
                    int idx = (bits >> (2*(py*4+px))) & 3;
                    memcpy(out + (y*w+x)*3, pal[idx], 3);
                }
        }
}

/* alf: optional w*h output for the block's 4-bit explicit alpha (may be NULL) */
static void n2_dxt3(const unsigned char *src, int w, int h, unsigned char *out,
                    unsigned char *alf) {
    int bi = 0;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            /* 8-byte 4-bit alpha, then 8-byte DXT1 colour */
            uint16_t c0 = src[bi+8] | src[bi+9] << 8, c1 = src[bi+10] | src[bi+11] << 8;
            uint32_t bits = n2_u32(src + bi + 12);
            unsigned char pal[4][3];
            n2_rgb565(c0, pal[0]); n2_rgb565(c1, pal[1]);
            for (int j = 0; j < 3; j++) {
                pal[2][j] = (2*pal[0][j]+pal[1][j])/3;
                pal[3][j] = (pal[0][j]+2*pal[1][j])/3;
            }
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    int x = bx+px, y = by+py; if (x >= w || y >= h) continue;
                    int idx = (bits >> (2*(py*4+px))) & 3;
                    memcpy(out + (y*w+x)*3, pal[idx], 3);
                    if (alf) {
                        int a4 = (src[bi + (py*4+px)/2] >> (((py*4+px)&1)*4)) & 15;
                        alf[y*w+x] = (unsigned char)(a4 * 17);
                    }
                }
            bi += 16;
        }
}

/* JDLZ decompress (EA NFS). Writes up to out_cap bytes; returns bytes written. */
static int n2_jdlz(const unsigned char *s, int slen, unsigned char *out, int out_cap) {
    if (slen < 16 || memcmp(s, "JDLZ", 4)) return 0;
    int usize = (int)n2_u32(s + 8);
    if (usize > out_cap) usize = out_cap;
    int op = 0, ip = 16, flags1 = 1, flags2 = 1;
    while (ip < slen && op < usize) {
        if (flags1 == 1) flags1 = s[ip++] | 0x100;
        if (flags2 == 1) flags2 = s[ip++] | 0x100;
        if (flags1 & 1) {
            int len, dist;
            if (flags2 & 1) { len = (s[ip+1] | ((s[ip] & 0xF0) << 4)) + 3; dist = (s[ip] & 0x0F) + 1; }
            else            { dist = (s[ip+1] | ((s[ip] & 0xE0) << 3)) + 17; len = (s[ip] & 0x1F) + 3; }
            ip += 2;
            for (int k = 0; k < len && op < usize; k++) { out[op] = out[op - dist]; op++; }
            flags2 >>= 1;
        } else out[op++] = s[ip++];
        flags1 >>= 1;
    }
    return op;
}

/* ---- EA "HUFF" (EAC canonical-Huffman, methods 30FBh..35FBh) ----
 * Used by CARS/ * /VINYLS.BIN blobs ("HUFF" wrapper + EAC stream). Format per
 * Martin Korth's documentation (problemkaputt.de/psx-spx.htm, "CDROM File
 * Compression EA Methods (Huffman)"), reimplemented from the spec:
 * big-endian bitstream; header = u16 method + optional 3B + u24 size + u8
 * escape; canonical code widths until the Kraft sum fills the code space;
 * symbols delta-assigned over not-yet-used values; ESC = {varlen 0: EOS-bit
 * or raw literal; varlen n: repeat previous byte n times}. */

typedef struct { const unsigned char *b; long len, pos; } N2Bits;
static uint32_t n2_bits(N2Bits *s, int n) {
    uint32_t v = 0;
    while (n-- > 0) {
        if (s->pos >= s->len * 8) return v << (n + 1);   /* starved: zeros */
        v = v << 1 | ((s->b[s->pos >> 3] >> (7 - (s->pos & 7))) & 1);
        s->pos++;
    }
    return v;
}
static uint32_t n2_huff_varlen(N2Bits *s) {
    int num = 2;
    while (n2_bits(s, 1) == 0 && num < 24) num++;
    return n2_bits(s, num) + (1u << num) - 4;
}

/* Decompress one EAC Huffman stream into out. Returns bytes written (0=fail). */
static long n2_huff(const unsigned char *src, long slen, unsigned char *out, long cap) {
    N2Bits s = { src, slen, 0 };
    uint32_t method = n2_bits(&s, 16);
    if ((method & 0xF0FF) != 0x30FB) return 0;
    if (method & 0x100) n2_bits(&s, 24);
    long dsize = (long)n2_bits(&s, 24);
    if (dsize <= 0 || dsize > cap) return 0;
    int esc = (int)n2_bits(&s, 8);
    int numcodes[17] = {0}, width = 0, total = 0;
    uint32_t code = 0;
    while (width < 16 && (code << (16 - width)) < 0x10000u) {
        uint32_t n = n2_huff_varlen(&s);
        width++;
        if (n > 256) return 0;
        numcodes[width] = (int)n; total += (int)n;
        code = code * 2 + n;
    }
    if (total < 1 || total > 256) return 0;
    unsigned char vals[256], defined[256];
    memset(defined, 0, sizeof defined);
    int dat = 0xFF;
    for (int i = 0; i < total; i++) {
        long n = (long)n2_huff_varlen(&s) + 1;
        if (n > 256) return 0;
        while (n > 0) {
            dat = (dat + 1) & 0xFF;
            if (!defined[dat]) n--;
        }
        defined[dat] = 1; vals[i] = (unsigned char)dat;
    }
    /* canonical decode tables */
    uint32_t first_code[18]; int first_idx[18];
    uint32_t c = 0; int idx = 0;
    for (int w = 1; w <= width; w++) {
        first_code[w] = c; first_idx[w] = idx;
        c = (c + (uint32_t)numcodes[w]) << 1; idx += numcodes[w];
    }
    long op = 0;
    while (op < dsize && s.pos < s.len * 8) {
        uint32_t v = 0; int w = 0, sym = -1;
        while (w < width) {
            v = v << 1 | n2_bits(&s, 1); w++;
            if (v - first_code[w] < (uint32_t)numcodes[w]) {
                sym = vals[first_idx[w] + (int)(v - first_code[w])]; break;
            }
        }
        if (sym < 0) return 0;                    /* corrupt tree/stream */
        if (sym != esc) { out[op++] = (unsigned char)sym; continue; }
        uint32_t n = n2_huff_varlen(&s);
        if (n == 0) {
            if (n2_bits(&s, 1) == 1) break;       /* end of stream */
            out[op++] = (unsigned char)n2_bits(&s, 8);
        } else {
            unsigned char prev = op ? out[op-1] : 0;
            while (n-- > 0 && op < dsize) out[op++] = prev;
        }
    }
    if (op != dsize) return 0;
    if ((method & 0xFEFF) == 0x32FB) {            /* optional delta filters */
        unsigned char x = 0;
        for (long i = 0; i < dsize; i++) { x = (unsigned char)(x + out[i]); out[i] = x; }
    } else if ((method & 0xFEFF) == 0x34FB) {
        unsigned char x = 0, y = 0;
        for (long i = 0; i < dsize; i++) { x = (unsigned char)(x + out[i]);
                                           y = (unsigned char)(y + x); out[i] = y; }
    }
    return op;
}

/* Locate the car TPK offset-slot table (0x33310003, 24-byte records) inside the
 * 0xb3310000 header block. Returns the payload pointer + size, or NULL. */
static const unsigned char *n2_tpk_slots(const unsigned char *d, long len, uint32_t *outsz) {
    long hoff = -1;
    for (long i = 0; i + 4 < len; i++)
        if (d[i]==0 && d[i+1]==0 && d[i+2]==0x31 && d[i+3]==0xb3) { hoff = i + 8; break; }
    if (hoff < 0) return NULL;
    uint32_t hsz = n2_u32(d + hoff - 4); long hend = hoff + hsz, o = hoff;
    while (o + 8 <= hend) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4);
        if (m == 0x33310003u) { *outsz = s; return d + o + 8; }
        o += 8 + s;
    }
    return NULL;
}

/* All texture keys present in the car TPK (offset-slot table). Returns count. */
static int n2_car_tex_keys(const unsigned char *d, long len, uint32_t *keys, int maxk) {
    uint32_t sz; const unsigned char *p = n2_tpk_slots(d, len, &sz);
    if (!p) return 0;
    int n = 0;
    for (uint32_t i = 0; i + 0x18 <= sz && n < maxk; i += 0x18) keys[n++] = n2_u32(p + i);
    return n;
}

/* Byte size of a full mip chain for a w×h texture (bpb = 8 DXT1 / 16 DXT3). */
static int n2_mipbytes2(int w, int h, int bpb) {
    int t = 0;
    for (;;) {
        int bw = w < 4 ? 1 : w/4, bh = h < 4 ? 1 : h/4;
        t += bw*bh*bpb;
        if (w == 1 && h == 1) break;
        if (w > 1) w /= 2; if (h > 1) h /= 2;
    }
    return t;
}
static int n2_mipbytes(int s, int bpb) { return n2_mipbytes2(s, s, bpb); }

/* Decode ONE car texture by its TPK key: find the slot, JDLZ-decompress, then
 * recover square dims + DXT1/DXT3 by matching the mip-chain size to DecodedSize
 * (car textures are square; format isn't stored, so it's inferred). Returns 1. */
static int n2_load_car_tex_by_key(const unsigned char *d, long len, uint32_t key, N2Tex *t) {
    uint32_t sz; const unsigned char *p = n2_tpk_slots(d, len, &sz);
    if (!p) return 0;
    int absoff = 0, enc = 0, dec = 0;
    for (uint32_t i = 0; i + 0x18 <= sz; i += 0x18)
        if (n2_u32(p + i) == key) {
            absoff = (int)n2_u32(p + i + 4); enc = (int)n2_u32(p + i + 8);
            dec = (int)n2_u32(p + i + 12); break;
        }
    if (dec <= 0 || absoff < 0 || (long)absoff + enc > len) return 0;
    unsigned char *raw = (unsigned char *)malloc(dec);
    if (enc >= 20 && memcmp(d + absoff, "HUFF", 4) == 0) {
        /* "HUFF"-wrapped blob (16-byte wrapper + EAC Huffman stream) — used
           by every VINYLS.BIN slot and by some TEXTURES.BIN slots. If the
           payload carries a vinyl record it's decoded here; otherwise it's
           raw DXT and falls through to the square solver below. */
        if (n2_huff(d + absoff + 16, enc - 16, raw, dec) != dec)
            { free(raw); return 0; }
        /* HUFF payload = pixel data (base level only) + a 144-byte trailing
           record: name[24], key u32 @+0x18 (validates the slot), w,h u16s
           @+0x38, format u32 @+0x84 — ASCII "DXT1"/"DXT3", or a code
           (0x29 etc.) for P8-style palette indices (vinyls). */
        if (dec >= 144) {
            const unsigned char *rec = raw + dec - 144;
            int w = rec[0x38] | rec[0x39] << 8, h = rec[0x3a] | rec[0x3b] << 8;
            uint32_t fmt = n2_u32(rec + 0x84);
            if (w >= 8 && h >= 8 && w <= 2048 && h <= 2048 &&
                n2_u32(rec + 0x18) == key && rec[0] >= 'A' && rec[0] <= 'Z') {
                long n = (long)w * h;
                if (fmt == 0x31545844 && n/2 + 144 <= dec) {        /* "DXT1" */
                    t->w = w; t->h = h; t->alpha = NULL;
                    t->rgb = (unsigned char *)malloc(n * 3);
                    n2_dxt1(raw, w, h, t->rgb);
                    free(raw); return 1;
                }
                if (fmt == 0x33545844 && n + 144 <= dec) {          /* "DXT3" */
                    t->w = w; t->h = h;
                    t->rgb = (unsigned char *)malloc(n * 3);
                    t->alpha = (unsigned char *)malloc(n);
                    n2_dxt3(raw, w, h, t->rgb, t->alpha);
                    free(raw); return 1;
                }
                if (n + 144 <= dec) {
                    /* palette indices (vinyls). The palette ships ALL-ZERO —
                       the game recolors vinyls from the player's colours —
                       so synthesize a dark cut-vinyl look: most-frequent
                       index = transparent background. */
                    long hist[256] = {0};
                    for (long i = 0; i < n; i++) hist[raw[i]]++;
                    int bg = 0;
                    for (int i = 1; i < 256; i++) if (hist[i] > hist[bg]) bg = i;
                    t->w = w; t->h = h;
                    t->rgb = (unsigned char *)malloc(n * 3);
                    t->alpha = (unsigned char *)malloc(n);
                    for (long i = 0; i < n; i++) {
                        int ix = raw[i];
                        unsigned char v = (unsigned char)(20 + (ix & 15) * 8);
                        t->rgb[i*3] = v; t->rgb[i*3+1] = v; t->rgb[i*3+2] = v;
                        t->alpha[i] = ix == bg ? 0 : 255;
                    }
                    free(raw); return 1;
                }
            }
        }
    } else
        n2_jdlz(d + absoff, enc, raw, dec);
    /* recover dims by matching the mip-chain byte total: square first (the
       common case), then rectangular pairs (headlight strips, banners) —
       orientation of a w!=h tie is unknowable from size alone, so wider-
       than-tall wins (matches the rect atlases seen in the data). */
    int tw = 0, th = 0, dxt3 = 0, bestd = 1 << 30;
    for (int s = 8; s <= 1024; s <<= 1) {
        int d1 = dec - n2_mipbytes2(s, s, 8), d3 = dec - n2_mipbytes2(s, s, 16);
        if (d1 < 0) d1 = -d1; if (d3 < 0) d3 = -d3;
        if (d1 < bestd) { bestd = d1; tw = th = s; dxt3 = 0; }
        if (d3 < bestd) { bestd = d3; tw = th = s; dxt3 = 1; }
    }
    if (bestd > 256) {
        for (int w = 16; w <= 1024; w <<= 1)
            for (int h = 8; h < w; h <<= 1) {
                int d1 = dec - n2_mipbytes2(w, h, 8), d3 = dec - n2_mipbytes2(w, h, 16);
                if (d1 < 0) d1 = -d1; if (d3 < 0) d3 = -d3;
                if (d1 < bestd) { bestd = d1; tw = w; th = h; dxt3 = 0; }
                if (d3 < bestd) { bestd = d3; tw = w; th = h; dxt3 = 1; }
            }
    }
    if (tw == 0 || bestd > 256) { free(raw); return 0; }
    t->w = tw; t->h = th;
    t->rgb = (unsigned char *)malloc((long)tw*th*3);
    /* keep DXT3's explicit alpha: it masks the body decals/badges */
    t->alpha = dxt3 ? (unsigned char *)malloc((long)tw*th) : NULL;
    if (dxt3) n2_dxt3(raw, tw, th, t->rgb, t->alpha);
    else      n2_dxt1(raw, tw, th, t->rgb);
    free(raw);
    return 1;
}

/* Find a named texture in the file's local TPK and decode it. Returns 1 on hit. */
static int n2_load_texture(const unsigned char *d, long len, const char *name, N2Tex *t) {
    /* TPK header block marker: magic 0xb3310000 (LE bytes 00 00 31 b3) */
    long hdr = -1;
    for (long i = 0; i + 4 < len; i++)
        if (d[i]==0x00 && d[i+1]==0x00 && d[i+2]==0x31 && d[i+3]==0xb3) { hdr = i; break; }
    if (hdr < 0) return 0;
    /* pixel block 0xb3320000 (LE 00 00 32 b3) after the header */
    long pix = -1;
    for (long i = hdr; i + 4 < len; i++)
        if (d[i]==0x00 && d[i+1]==0x00 && d[i+2]==0x32 && d[i+3]==0xb3) { pix = i + 8; break; }
    if (pix < 0) return 0;
    long hbeg = hdr + 8, hsize = n2_u32(d + hdr + 4);
    long nlen = (long)strlen(name);
    for (long i = hbeg; i + 0x3c < hbeg + hsize; i++) {
        if (memcmp(d + i, name, nlen) == 0 && d[i + nlen] == 0) {
            uint32_t off = n2_u32(d + i + 0x24);
            uint16_t w = d[i+0x38] | d[i+0x39] << 8, hh = d[i+0x3a] | d[i+0x3b] << 8;
            if (w == 0 || hh == 0 || w > 2048 || hh > 2048) return 0;
            t->w = w; t->h = hh;
            t->rgb = (unsigned char *)malloc((long)w * hh * 3);
            t->alpha = NULL;
            n2_dxt1(d + pix + off, w, hh, t->rgb);
            return 1;
        }
    }
    return 0;
}

/* Rough "is this decoded image just noise?" test: mean abs colour difference of
 * horizontally-spaced sampled pixels. Real textures are locally coherent; a
 * wrong format/swizzle decodes to high-frequency rainbow. Used to reject a
 * texture we can't decode correctly (e.g. a swizzled surface) so it falls back
 * instead of binding garbage. */
static int n2_tex_noise(const N2Tex *t) {
    long sum = 0, cnt = 0;
    for (int y = 0; y < t->h; y += 4)
        for (int x = 0; x + 4 < t->w; x += 4) {
            const unsigned char *a = t->rgb + ((long)y*t->w + x)*3, *b = a + 12;
            sum += abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2]); cnt++;
        }
    return cnt && (sum / (cnt*3)) > 55;
}

/* A STREAM TPK opened once for repeated by-hash lookups. A region can have MANY
 * 0xb3310000 header blocks (each paired with a following 0x33320002 pixel block);
 * a single O(len) pass records them all, so a huge shared "master" region can be
 * mmap'd and queried without re-scanning it per texture. */
typedef struct { long hbeg, hsize, dbase; } N2TpkBlk;
typedef struct { N2TpkBlk *blk; int nblk; } N2Tpk;
static N2Tpk n2_tpk_open(const unsigned char *d, long len) {
    N2Tpk t; t.blk = NULL; t.nblk = 0; int cap = 0;
    long ph = -1; uint32_t phs = 0;
    for (long i = 0; i + 8 < len; i++) {
        if (d[i]==0 && d[i+1]==0 && d[i+2]==0x31 && d[i+3]==0xb3) {    /* header 0xb3310000 */
            ph = i + 8; phs = n2_u32(d + i + 4);
        } else if (ph >= 0 && d[i]==0x02 && d[i+1]==0x00 && d[i+2]==0x32 && d[i+3]==0x33) {
            long pbase = i + 8, room = len - pbase;       /* pixels 0x33320002 */
            if (room > 0x40000000) room = 0x40000000;
            if (t.nblk == cap) { cap = cap ? cap*2 : 64;
                t.blk = (N2TpkBlk *)realloc(t.blk, (size_t)cap*sizeof(N2TpkBlk)); }
            t.blk[t.nblk].hbeg = ph; t.blk[t.nblk].hsize = phs;
            t.blk[t.nblk].dbase = pbase + n2_skip_filler(d + pbase, (int)room);
            t.nblk++; ph = -1;
        }
    }
    return t;
}
/* Collect every record hash (@+0x18) across all blocks. Returns the count. */
static int n2_tpk_keys(const unsigned char *d, N2Tpk t, uint32_t *keys, int maxk) {
    int n = 0;
    for (int b = 0; b < t.nblk && n < maxk; b++) {
        long hbeg = t.blk[b].hbeg, hend = hbeg + t.blk[b].hsize;
        for (long i = hbeg; i + 0x40 < hend && n < maxk; i++) {
            if (!(d[i] >= 'A' && d[i] <= 'Z')) continue;
            if (d[i+0x17] != 0) continue;                 /* 24-byte name null-terminated */
            keys[n++] = n2_u32(d + i + 0x18);
            i += 0x7b;                                    /* next 0x7c record */
        }
    }
    return n;
}

/* Decode one texture by its record hash, searching all blocks. Fields (Nikki's
 * layout minus a 0x0C name-pad prefix): +0x18 BinKey +0x24 Offset
 * +0x28 PaletteOffset +0x2c Size +0x30 PaletteSize +0x38 W(u16) +0x3a H(u16).
 * P8 when PaletteSize >= 1024 (256-entry RGBA); else DXT1/DXT3 by Size. */
static int n2_tpk_decode(const unsigned char *d, long len, N2Tpk t, uint32_t hash, N2Tex *tex) {
    for (int b = 0; b < t.nblk; b++) {
        long hbeg = t.blk[b].hbeg, hend = hbeg + t.blk[b].hsize, dbase = t.blk[b].dbase;
        for (long i = hbeg; i + 0x40 < hend; i++) {
            if (!(d[i] >= 'A' && d[i] <= 'Z')) continue;
            if (n2_u32(d + i + 0x18) != hash) continue;
            uint32_t off = n2_u32(d + i + 0x24), paloff = n2_u32(d + i + 0x28);
            uint32_t sz = n2_u32(d + i + 0x2c), palsz = n2_u32(d + i + 0x30);
            int w = d[i+0x38] | d[i+0x39]<<8, hh = d[i+0x3a] | d[i+0x3b]<<8;
            if (w<=0 || hh<=0 || w>4096 || hh>4096) continue;
            tex->w = w; tex->h = hh; tex->rgb = (unsigned char *)malloc((long)w*hh*3);
            tex->alpha = NULL;
            if (palsz >= 1024 && dbase+paloff+1024 <= len && dbase+off+(long)w*hh <= len) {
                const unsigned char *pal = d + dbase + paloff, *ix = d + dbase + off;
                for (long p = 0; p < (long)w*hh; p++) {   /* P8: index -> RGBA palette */
                    const unsigned char *c = pal + (long)ix[p]*4;
                    tex->rgb[p*3]=c[0]; tex->rgb[p*3+1]=c[1]; tex->rgb[p*3+2]=c[2];
                }
            } else if (dbase + off + (long)w*hh/2 <= len) {
                int dxt3 = (long)sz > (long)w*hh*9/10;
                if (dxt3) n2_dxt3(d + dbase + off, w, hh, tex->rgb, NULL);
                else      n2_dxt1(d + dbase + off, w, hh, tex->rgb);
            } else { free(tex->rgb); continue; }
            return 1;
        }
    }
    return 0;
}

#endif /* NFSU2_H */
