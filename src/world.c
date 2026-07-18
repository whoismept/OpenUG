/* world.c — OpenUG World module implementation. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "world.h"
#include "resource.h"

static void grid_build(World *w);

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
 * the camera's fixed district ID names," not a spatial trigger system,
 * because there's nothing to look up: all district bundles already
 * share one continuous world coordinate system (confirmed by their
 * mesh bounds abutting with no overlap/gap), so a district boundary is
 * just wherever one bundle's geometry ends and its neighbour's begins.
 * That's why "ALL" below just concatenates every STREAM*.BUN into one
 * scene at load time instead of paging them — on modern hardware the
 * whole city (28k+ meshes, confirmed in Phase 19) comfortably fits
 * resident at once, so the retail streaming problem doesn't exist here
 * and there is no trigger structure to wire up. */
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
