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
enum { N2_ROAD = 0, N2_TERRAIN = 1, N2_OTHER = 2,
       /* car mesh classes, from material name */
       N2_CAR_BODY = 10, N2_CAR_GLASS = 11, N2_CAR_LIGHT = 12,
       N2_CAR_TIRE = 13, N2_CAR_MISC = 14 };
typedef struct {
    float   *verts;   /* 5 floats per vertex: pos.xyz, uv */
    int      nverts;
    uint16_t *idx;
    int      nidx;
    int      cat;     /* N2_ROAD / N2_TERRAIN / N2_OTHER, from material name */
} N2Mesh;

typedef struct {
    N2Mesh *meshes;
    int     count, cap;
} N2Scene;

/* Decoded RGB texture (3 bytes/pixel, top-left origin). */
typedef struct { int w, h; unsigned char *rgb; } N2Tex;

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

/* Classify a 0x80134010 mesh by the material name in its first 0x134011 leaf.
 * Names look like TRN_ROADA_CHOP_*, TRN_TERRAINA_*, XO_TRAFFICCONE_* ... */
static int n2_mesh_category(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            /* start of an uppercase A-Z name run */
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
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
                        int stride, int uvoff, int cull_skybox) {
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
        if (span >= 2000.0f) return;
    }

    N2Mesh m; memset(&m, 0, sizeof(m));
    m.cat = cat; m.nverts = n;
    m.verts = (float *)malloc(n * 5 * sizeof(float));
    for (int i = 0; i < n; i++) {
        memcpy(m.verts + i*5,     rec + i*stride,          12);
        memcpy(m.verts + i*5 + 3, rec + i*stride + uvoff,  8);
    }
    const unsigned char *ib = d + idx.off;
    int nidx = (int)idx.size / 2;
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

/* Walk to every 0x80134010 mesh, classify it, extract its vtx/idx pairs. */
static void n2_walk_meshes(const unsigned char *d, long beg, long end, N2Scene *scene) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == 0x80134010u) {
            int cat = n2_mesh_category(d, ds, ds + size);
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + size, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + size, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;
            for (int k = 0; k < pairs; k++)
                n2_add_pair(d, vtx[k], idx[k], cat, scene, 24, 16, 1);
        } else if (magic != 0 && (magic >> 28) == 8) {
            n2_walk_meshes(d, ds, ds + size, scene);
        }
        o = ds + size;
    }
}

/* Parse a STREAM .BUN into categorized world-space meshes. Returns mesh count. */
static int n2_load_scene(const unsigned char *d, long len, N2Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    n2_walk_meshes(d, 0, len, scene);
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
                    if (n2_contains(n,L,"LIGHT") || n2_contains(n,L,"LAMP"))   return N2_CAR_LIGHT;
                    if (n2_contains(n,L,"TIRE") || n2_contains(n,L,"WHEEL"))   return N2_CAR_TIRE;
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

/* Parse a car GEOMETRY.BIN (36-byte verts w/ normals), tagging each mesh with
 * a class from its material name (body/glass/light/tire/misc). */
static void n2_walk_car(const unsigned char *d, long beg, long end, N2Scene *scene) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4);
        long ds = o + 8;
        if (m == 0x80134010u) {
            int cat = n2_car_category(d, ds, ds + s);
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + s, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + s, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;
            for (int k = 0; k < pairs; k++)
                n2_add_pair(d, vtx[k], idx[k], cat, scene, 36, 28, 0);
        } else if (m != 0 && (m >> 28) == 8) {
            n2_walk_car(d, ds, ds + s, scene);
        }
        o = ds + s;
    }
}
static int n2_load_car(const unsigned char *d, long len, N2Scene *scene) {
    memset(scene, 0, sizeof(*scene));
    n2_walk_car(d, 0, len, scene);
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

static void n2_dxt3(const unsigned char *src, int w, int h, unsigned char *out) {
    int bi = 0;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            /* 8-byte 4-bit alpha (skipped), then 8-byte DXT1 colour */
            uint16_t c0 = src[bi+8] | src[bi+9] << 8, c1 = src[bi+10] | src[bi+11] << 8;
            uint32_t bits = n2_u32(src + bi + 12); bi += 16;
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
                }
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

/* Load a car's main body texture from CARS/<car>/TEXTURES.BIN: parse the TPK
 * offset slots, take the largest (the 512x512 body atlas), JDLZ-decompress,
 * DXT3-decode. Approximation — one texture, no per-mesh binding. Returns 1. */
static int n2_load_car_texture(const unsigned char *d, long len, N2Tex *t) {
    /* container 0xb3300000 -> header 0xb3310000, pixel block 0xb3320000 */
    long hoff = -1;
    for (long i = 0; i + 4 < len; i++)
        if (d[i]==0 && d[i+1]==0 && d[i+2]==0x31 && d[i+3]==0xb3) { hoff = i + 8; break; }
    if (hoff < 0) return 0;
    /* offset-slot table = 3rd info sub-chunk (0x33310003) inside the header block */
    long o = hoff, p3 = -1; uint32_t hsz = n2_u32(d + hoff - 4);
    long hend = hoff + hsz;
    while (o + 8 <= hend) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4);
        if (m == 0x33310003u) { p3 = o + 8; break; }
        o += 8 + s;
    }
    if (p3 < 0) return 0;
    uint32_t p3sz = n2_u32(d + p3 - 4);
    /* pick the slot with the largest DecodedSize */
    long best_off = 0; int best_enc = 0, best_dec = 0;
    for (uint32_t i = 0; i + 0x18 <= p3sz; i += 0x18) {
        int absoff = (int)n2_u32(d + p3 + i + 4);
        int enc = (int)n2_u32(d + p3 + i + 8);
        int dec = (int)n2_u32(d + p3 + i + 12);
        if (dec > best_dec && absoff >= 0 && absoff + enc <= len) {
            best_dec = dec; best_enc = enc; best_off = absoff;
        }
    }
    if (best_dec <= 0) return 0;
    unsigned char *raw = (unsigned char *)malloc(best_dec);
    int got = n2_jdlz(d + best_off, best_enc, raw, best_dec);
    int W = 512, H = 512;             /* body atlas dims (from DecodedSize analysis) */
    if (got < W*H) { free(raw); return 0; }
    t->w = W; t->h = H;
    t->rgb = (unsigned char *)malloc((long)W*H*3);
    n2_dxt3(raw, W, H, t->rgb);
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
            n2_dxt1(d + pix + off, w, hh, t->rgb);
            return 1;
        }
    }
    return 0;
}

#endif /* NFSU2_H */
