/* world.c — OpenUG World module implementation. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "world.h"
#include "resource.h"

static void grid_build(World *w);

/* ---- load-time duplicate stripper (Phase 48) ----
   The 8 STREAM*.BUN bundles are overlapping per-race supersets, not adjacent
   tiles, so stitching them (and even a single bundle on its own) stacks exact
   copies of the same surface at dz=0.000 — the source of the --track ALL
   z-fighting and a big chunk of wasted geometry (audit: 14551 of 28270 solid
   meshes, tools/zfight_audit.c). This drops every mesh whose
   (texkey, xyz-bbox, tri, vert) key was already registered, keeping the first
   occurrence. Runs on the whole scene BEFORE mbb/grid/batches are built, so
   the ground grid, the physics ground query, and the GPU batches all see one
   clean surface layer with no further plumbing.

   Identity is exact: two meshes that survive share a texkey, an identical
   bounding box (all three axes) and the same tri+vert counts. An instanced
   prop reused at a DIFFERENT position has a different bbox, so it is NOT a
   duplicate and is kept — only same-place, same-asset copies are removed. */
static const N2Scene *dd_s;      /* comparator context; load is single-threaded */
static const float (*dd_bb)[6];
static int dd_cmp(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    const N2Mesh *ma = &dd_s->meshes[a], *mb = &dd_s->meshes[b];
    if (ma->texkey != mb->texkey) return ma->texkey < mb->texkey ? -1 : 1;
    for (int k = 0; k < 6; k++)
        if (dd_bb[a][k] != dd_bb[b][k]) return dd_bb[a][k] < dd_bb[b][k] ? -1 : 1;
    if (ma->nidx   != mb->nidx)   return ma->nidx   < mb->nidx   ? -1 : 1;
    if (ma->nverts != mb->nverts) return ma->nverts < mb->nverts ? -1 : 1;
    return a - b;   /* stable: the earliest original index sorts first, so it
                       is the copy we keep */
}
static int dd_same(int a, int b) {
    const N2Mesh *ma = &dd_s->meshes[a], *mb = &dd_s->meshes[b];
    if (ma->texkey != mb->texkey || ma->nidx != mb->nidx || ma->nverts != mb->nverts)
        return 0;
    for (int k = 0; k < 6; k++) if (dd_bb[a][k] != dd_bb[b][k]) return 0;
    return 1;
}

/* Compact w->scene to unique meshes, fix each region's [mesh0,mesh1) range to
   the compacted positions, free the dropped geometry. Returns meshes removed. */
static int world_dedup(World *w) {
    N2Scene *s = &w->scene;
    int nm = s->count;
    if (nm < 2) return 0;

    float (*bb)[6] = (float (*)[6])malloc((size_t)nm * sizeof *bb);
    int   *ord     = (int *)malloc((size_t)nm * sizeof *ord);
    char  *drop    = (char *)calloc((size_t)nm, 1);
    for (int i = 0; i < nm; i++) { n2_mesh_bbox(&s->meshes[i], bb[i]); ord[i] = i; }

    dd_s = s; dd_bb = (const float (*)[6])bb;
    qsort(ord, (size_t)nm, sizeof *ord, dd_cmp);

    long droptri = 0;
    for (int i = 0; i < nm; ) {
        int j = i + 1; while (j < nm && dd_same(ord[i], ord[j])) j++;
        for (int k = i + 1; k < j; k++) { drop[ord[k]] = 1; droptri += s->meshes[ord[k]].nidx / 3; }
        i = j;
    }

    /* new index = count of survivors before this original slot */
    int *keptbefore = (int *)malloc((size_t)(nm + 1) * sizeof *keptbefore);
    keptbefore[0] = 0;
    for (int i = 0; i < nm; i++) keptbefore[i + 1] = keptbefore[i] + (drop[i] ? 0 : 1);
    for (int r = 0; r < w->nreg; r++) {
        w->rgn[r].mesh0 = keptbefore[w->rgn[r].mesh0];
        w->rgn[r].mesh1 = keptbefore[w->rgn[r].mesh1];
    }

    int wr = 0;
    for (int i = 0; i < nm; i++) {
        if (drop[i]) { free(s->meshes[i].verts); free(s->meshes[i].idx); continue; }
        if (wr != i) s->meshes[wr] = s->meshes[i];
        wr++;
    }
    s->count = wr;

    /* region ranges must stay well-formed and cover exactly the survivors */
    assert(wr == keptbefore[nm]);
    for (int r = 0; r < w->nreg; r++)
        assert(w->rgn[r].mesh0 <= w->rgn[r].mesh1 && w->rgn[r].mesh1 <= wr);

    /* z-fight proof: rebuild the key over survivors and confirm none collide.
       Cheap (load-time, one pass) and it is the actual deliverable — if this
       ever fires, duplicates slipped through and --track ALL will z-fight. */
    int residual = 0;
    {
        float (*bb2)[6] = (float (*)[6])malloc((size_t)wr * sizeof *bb2);
        int   *ord2     = (int *)malloc((size_t)wr * sizeof *ord2);
        for (int i = 0; i < wr; i++) { n2_mesh_bbox(&s->meshes[i], bb2[i]); ord2[i] = i; }
        dd_s = s; dd_bb = (const float (*)[6])bb2;
        qsort(ord2, (size_t)wr, sizeof *ord2, dd_cmp);
        for (int i = 1; i < wr; i++) if (dd_same(ord2[i-1], ord2[i])) residual++;
        free(bb2); free(ord2);
    }
    assert(residual == 0);

    free(bb); free(ord); free(drop); free(keptbefore);
    printf("dedup: %d -> %d meshes (dropped %d duplicates, %ld triangles); "
           "residual coplanar duplicates: %d\n",
           nm, wr, nm - wr, droptri, residual);
    return nm - wr;
}

/* Sector/streaming-trigger audit (Phase 20): TRACKS/ holds 8 named city
 * districts (L4RA/B/C/D/F/G/H/R), each a standalone STREAM<name>.BUN
 * geometry bundle (2 MB to 115 MB) plus a small companion <name>.BUN that
 * — per its strings dump (ZCV_ and ZCS_ prefixed names: trains,
 * drawbridges, gates, garbage cans) — holds scripted/animated set-
 * dressing, not spatial data.
 * Grepped every file under TRACKS/, GLOBAL/, FRONTEND/ for trigger,
 * portal, volume, sector, zone, hub, and region-bound keywords: zero
 * hits. There is no portal graph, trigger-volume list, or district
 * connectivity table anywhere in this data — retail's PS2-era district
 * streaming was almost certainly just "load whichever district bundle
 * the camera's fixed district ID names," not a spatial trigger system.
 *
 * CORRECTION (Phase 47, measured). This comment used to claim the
 * bundles' "mesh bounds abut with no overlap/gap", so a district
 * boundary was just where one bundle ends and the next begins. That is
 * FALSE. Measuring solid-geometry bounds per bundle — excluding
 * SKY/GLOW, whose skydome spans the whole map and masks the effect:
 *   L4RA and L4RD have IDENTICAL solid bounds; so do L4RB and L4RG.
 * The bundles are OVERLAPPING SUPERSETS, not adjacent tiles: each is a
 * per-race-route working set that re-ships whatever geometry its route
 * touches. Across all 8, 51.5% of solid meshes (14551 of 28270, 1.25M
 * triangles) are exact duplicates — identical texkey, bbox, and
 * tri/vert count. Adding tri+vert to the identity key moved the total
 * by only 4 groups, so these are true duplicates, not LOD tiers.
 *
 * So "ALL" below concatenating every STREAM*.BUN is itself the cause of
 * that mode's z-fighting: it stacks coplanar copies of one surface
 * (measured dz = 0.000 between same-texkey pairs). The fix is a
 * load-time dedupe on (texkey, bbox, tri, vert) — NOT a depth bias and
 * NOT a vista/ground poly filter, because the conflicting layers are
 * the same asset twice, not low-poly vista clashing with high-poly
 * ground.
 *
 * Phase 48: that dedupe is now world_dedup(), run below right after the
 * region loop and before mbb/grid/batch build, so the ground grid, the
 * physics ground query, and the GPU batches all see one clean surface
 * layer. It keeps the first copy of each (texkey, xyz-bbox, tri, vert)
 * and drops the rest. */
int world_load(World *w, const char *troot, const char *trackname) {
    memset(w, 0, sizeof *w);

    /* Region set: ALL stitches every STREAM*.BUN into one scene — they share
       one world coordinate system, so appending them yields the whole city. */
    static char regs[WORLD_MAXREG][64]; int nreg = 0;
    if (!strcmp(trackname, "ALL")) {
        int dummy = 0;
        nreg = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
    }
    if (!nreg) { snprintf(regs[0], sizeof regs[0], "%s", trackname); nreg = 1; }
    w->nreg = nreg;

    char loc4p[1024]; snprintf(loc4p, sizeof loc4p, "%s/LOC4DYNTEX.BIN", troot);
    w->loc4 = n2_read_file(loc4p, &w->loc4len);

    /* per-region: own TPK keys + shared LOC4 keys pick each mesh's diffuse
       slot; single-region mode also pulls a big "master" neighbour's TPK for
       mid-size regions whose buildings reference it. */
    static uint32_t tkeys[16384];
    for (int r = 0; r < nreg; r++) {
        WRegion *g = &w->rgn[r];
        snprintf(g->name, sizeof g->name, "%s", regs[r]);
        char trackp[1024]; snprintf(trackp, sizeof trackp, "%s/%s.BUN", troot, regs[r]);
        g->data = n2_read_file(trackp, &g->len);
        g->mesh0 = g->mesh1 = w->scene.count;
        if (!g->data) { fprintf(stderr, "cannot read %s\n", trackp); continue; }
        g->tpk = n2_tpk_open(g->data, g->len);
        int ntk = n2_tpk_keys(g->data, g->tpk, tkeys, 16384);
        if (w->loc4 && ntk < 16384)
            ntk += n2_car_tex_keys(w->loc4, w->loc4len, tkeys + ntk, 16384 - ntk);
        if (nreg == 1 && g->len > 8L*1024*1024 && g->len < 60L*1024*1024) {
            /* Location-4 shared library; use the other big region if we ARE it */
            const char *mn = strcmp(regs[r], "STREAML4RD") ? "STREAML4RD" : "STREAML4RA";
            char mp[1024]; snprintf(mp, sizeof mp, "%s/%s.BUN", troot, mn);
            w->master = res_map_file(mp, &w->masterlen);
            if (w->master) {
                w->mastertpk = n2_tpk_open(w->master, w->masterlen);
                if (ntk < 16384)
                    ntk += n2_tpk_keys(w->master, w->mastertpk, tkeys + ntk, 16384 - ntk);
            }
        }
        n2_walk_meshes(g->data, 0, g->len, &w->scene, tkeys, ntk);
        g->mesh1 = w->scene.count;
        if (!w->have_grass)
            w->have_grass = n2_load_texture(g->data, g->len, "TRN_GRASSC", &w->grass);
        printf("region %-12s: %3ld MB, %5d meshes, %d tex keys\n",
               regs[r], g->len >> 20, g->mesh1 - g->mesh0, ntk);
    }

    /* strip coplanar duplicates before anything downstream sees the scene */
    world_dedup(w);

    /* per-mesh XY bounds — the draw cull and the ground grid both key off it */
    int nm = w->scene.count;
    w->mbb = (float (*)[4])malloc((size_t)nm * 4 * sizeof(float));
    for (int i = 0; i < nm; i++) {
        N2Mesh *m = &w->scene.meshes[i];
        float x0=1e30f, y0=1e30f, x1=-1e30f, y1=-1e30f;
        for (int v = 0; v < m->nverts; v++) {
            float *p = m->verts + v*5;
            if (p[0]<x0)x0=p[0]; if (p[0]>x1)x1=p[0];
            if (p[1]<y0)y0=p[1]; if (p[1]>y1)y1=p[1];
        }
        w->mbb[i][0]=x0; w->mbb[i][1]=y0; w->mbb[i][2]=x1; w->mbb[i][3]=y1;
    }
    grid_build(w);
    return nm;
}

int world_bind_textures(World *w, uint32_t *keys, GLuint *texs, int cap) {
    int n = 0;
    for (int r = 0; r < w->nreg; r++) {
        WRegion *g = &w->rgn[r];
        if (!g->data) continue;
        for (int i = g->mesh0; i < g->mesh1; i++) {
            uint32_t tk = w->scene.meshes[i].texkey; if (!tk) continue;
            int seen = 0; for (int j = 0; j < n; j++) if (keys[j] == tk) seen = 1;
            if (seen || n >= cap) continue;
            N2Tex tt; int ok = n2_tpk_decode(g->data, g->len, g->tpk, tk, &tt);
            if (!ok && w->loc4)
                ok = n2_load_car_tex_by_key(w->loc4, w->loc4len, tk, &tt);
            if (!ok && w->master)
                ok = n2_tpk_decode(w->master, w->masterlen, w->mastertpk, tk, &tt);
            if (ok && !n2_tex_noise(&tt)) { keys[n] = tk; texs[n] = upload_tex(&tt); n++; }
            if (ok) free(tt.rgb);
        }
        free(g->data); g->data = NULL;   /* meshes + textures live on the GPU now */
    }
    return n;
}

/* ---- ground grid ----
   Uniform XY grid over the road/terrain meshes; each cell lists the meshes
   whose bbox overlaps it (CSR layout). A query tests only that cell's meshes
   with the exact n2_ground_z triangle scan, so overpasses still resolve to
   the highest surface under the point, same as the brute force.
   ponytail: module-global singleton — the process loads exactly one world. */
#define GCELL 64.0f
static struct {
    const N2Mesh *meshes;          /* identifies the scene the grid was built
                                      for — main copies the N2Scene struct, so
                                      match on the shared meshes array */
    float x0, y0; int gw, gh;
    int *start;                    /* gw*gh+1 prefix offsets into list */
    int *list;                     /* mesh indices */
} g_grid;

static void grid_build(World *w) {
    const N2Scene *s = &w->scene;
    float x0=1e30f, y0=1e30f, x1=-1e30f, y1=-1e30f;
    for (int i = 0; i < s->count; i++) {
        if (s->meshes[i].cat == N2_OTHER) continue;
        if (w->mbb[i][0]<x0)x0=w->mbb[i][0]; if (w->mbb[i][1]<y0)y0=w->mbb[i][1];
        if (w->mbb[i][2]>x1)x1=w->mbb[i][2]; if (w->mbb[i][3]>y1)y1=w->mbb[i][3];
    }
    if (x0 > x1) return;                       /* no ground meshes at all */
    int gw = (int)((x1-x0)/GCELL) + 1, gh = (int)((y1-y0)/GCELL) + 1;
    int *start = (int *)calloc((size_t)gw*gh + 1, sizeof(int));
    /* count pass, prefix sum, fill pass */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->count; i++) {
            if (s->meshes[i].cat == N2_OTHER) continue;
            int cx0=(int)((w->mbb[i][0]-x0)/GCELL), cy0=(int)((w->mbb[i][1]-y0)/GCELL);
            int cx1=(int)((w->mbb[i][2]-x0)/GCELL), cy1=(int)((w->mbb[i][3]-y0)/GCELL);
            for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) {
                if (pass == 0) start[cy*gw+cx + 1]++;
                else           g_grid.list[start[cy*gw+cx]++] = i;
            }
        }
        if (pass == 0) {
            for (int c = 0; c < gw*gh; c++) start[c+1] += start[c];
            g_grid.list = (int *)malloc((size_t)start[gw*gh] * sizeof(int));
        }
    }
    /* fill pass advanced each start[c] to its end; shift back down one cell */
    memmove(start + 1, start, (size_t)gw*gh * sizeof(int));
    start[0] = 0;
    g_grid.meshes = s->meshes; g_grid.x0 = x0; g_grid.y0 = y0;
    g_grid.gw = gw; g_grid.gh = gh; g_grid.start = start;
    printf("ground grid: %dx%d cells, %d mesh refs\n", gw, gh, start[gw*gh]);
}

float world_ground_z(const N2Scene *s, float x, float y, float fallback) {
    if (s->meshes != g_grid.meshes)                        /* not the loaded world */
        return n2_ground_z((N2Scene *)s, x, y, fallback);
    int cx = (int)((x - g_grid.x0) / GCELL), cy = (int)((y - g_grid.y0) / GCELL);
    if (cx < 0 || cy < 0 || cx >= g_grid.gw || cy >= g_grid.gh) return fallback;
    /* borrow the cell's meshes into a scratch scene and reuse the exact scan */
    static N2Mesh scratch[512];
    N2Scene sub = { scratch, 0, 512 };
    int c = cy*g_grid.gw + cx;
    for (int k = g_grid.start[c]; k < g_grid.start[c+1] && sub.count < 512; k++)
        scratch[sub.count++] = s->meshes[g_grid.list[k]];
    return n2_ground_z(&sub, x, y, fallback);
}
