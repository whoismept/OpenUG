/* zfight_audit — Phase 47. Is the --track ALL corruption duplicate geometry,
   or a low-poly-vista / high-poly-ground clash?

   Answer, measured: duplicate geometry. 51.5% of solid meshes across the 8
   bundles are exact duplicates (same texkey, bbox, tri and vert count).

   Excludes SKY/GLOW: the skydome spans the whole map, so leaving it in makes
   every region's bounds look identical and hides the real overlap pattern.

   Build: cc -O2 -Wno-unused-function -I src -o zfight_audit \
              tools/zfight_audit.c -lm
          (-Wno-unused-function matches the Makefile: nfsu2.h is header-only,
           so every helper this tool does not call warns without it.)
   Run:   ./zfight_audit /path/to/nfsu2/data                              */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "nfsu2.h"

static const char *REG[] = {"STREAML4RA","STREAML4RB","STREAML4RC","STREAML4RD",
                            "STREAML4RF","STREAML4RG","STREAML4RH","STREAML4RR"};
#define NREG ((int)(sizeof REG / sizeof *REG))
typedef struct { float bb[6]; uint32_t tex; int reg, ntri, nv; } MB;

static int cmp(const void *a, const void *b) {
    const MB *x = (const MB*)a, *y = (const MB*)b;
    if (x->tex != y->tex) return x->tex < y->tex ? -1 : 1;
    for (int k = 0; k < 6; k++)
        if (x->bb[k] != y->bb[k]) return x->bb[k] < y->bb[k] ? -1 : 1;
    if (x->ntri != y->ntri) return x->ntri - y->ntri;
    if (x->nv   != y->nv)   return x->nv   - y->nv;
    return x->reg - y->reg;
}
static int same(const MB *x, const MB *y) {
    if (x->tex != y->tex) return 0;
    if (x->ntri != y->ntri || x->nv != y->nv) return 0;
    for (int k = 0; k < 6; k++) if (fabsf(x->bb[k]-y->bb[k]) > 1e-3f) return 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    char p[1024]; long l4len = 0;
    snprintf(p, sizeof p, "%s/TRACKS/LOC4DYNTEX.BIN", root);
    unsigned char *loc4 = n2_read_file(p, &l4len);
    static uint32_t tkeys[16384];
    MB *mb = NULL; int nmb = 0, cap = 0;

    printf("%-13s %6s %6s   solid-geometry bounds (SKY/GLOW excluded)\n",
           "region","solid","sky");
    for (int r = 0; r < NREG; r++) {
        long len = 0;
        snprintf(p, sizeof p, "%s/TRACKS/%s.BUN", root, REG[r]);
        unsigned char *d = n2_read_file(p, &len);
        if (!d) { printf("%-13s MISSING\n", REG[r]); continue; }
        N2Tpk tpk = n2_tpk_open(d, len);
        int ntk = n2_tpk_keys(d, tpk, tkeys, 16384);
        if (loc4 && ntk < 16384)
            ntk += n2_car_tex_keys(loc4, l4len, tkeys+ntk, 16384-ntk);
        N2Scene s; memset(&s, 0, sizeof s);
        n2_walk_meshes(d, 0, len, &s, tkeys, ntk);
        float b0[6] = {1e30f,-1e30f,1e30f,-1e30f,1e30f,-1e30f};
        int nsolid = 0, nsky = 0;
        for (int i = 0; i < s.count; i++) {
            int c = s.meshes[i].cat;
            if (c == N2_SKY || c == N2_GLOW) { nsky++; continue; }
            nsolid++;
            float b[6]; n2_mesh_bbox(&s.meshes[i], b);
            for (int k = 0; k < 6; k += 2) {
                if (b[k] < b0[k]) b0[k] = b[k];
                if (b[k+1] > b0[k+1]) b0[k+1] = b[k+1];
            }
            if (nmb == cap) { cap = cap?cap*2:4096; mb = (MB*)realloc(mb,(size_t)cap*sizeof *mb); }
            memcpy(mb[nmb].bb, b, sizeof b); mb[nmb].tex = s.meshes[i].texkey;
            mb[nmb].reg = r; mb[nmb].ntri = s.meshes[i].nidx/3;
            mb[nmb].nv = s.meshes[i].nverts; nmb++;
        }
        printf("%-13s %6d %6d   X[%8.1f..%8.1f] Y[%8.1f..%8.1f] Z[%7.1f..%7.1f]\n",
               REG[r], nsolid, nsky, b0[0],b0[1],b0[2],b0[3],b0[4],b0[5]);
        n2_free_scene(&s); free(d);
    }

    qsort(mb, (size_t)nmb, sizeof *mb, cmp);
    puts("\n=== exact duplicate solid meshes (same texkey AND same bbox) ===");
    int dupgroups = 0, dupmeshes = 0, duptris = 0, shown = 0, crossreg = 0;
    for (int i = 0; i < nmb; ) {
        int j = i+1; while (j < nmb && same(&mb[i], &mb[j])) j++;
        int n = j - i;
        if (n > 1) {
            int multi = 0;
            for (int k = i+1; k < j; k++) if (mb[k].reg != mb[i].reg) multi = 1;
            dupgroups++; dupmeshes += n-1; duptris += (n-1)*mb[i].ntri;
            if (multi) crossreg++;
            if (shown < 12 && multi) { shown++;
                printf("  x%d tex %08x %5d tri  X[%8.1f..%8.1f] Z[%7.2f..%7.2f]  regions:",
                       n, mb[i].tex, mb[i].ntri, mb[i].bb[0],mb[i].bb[1],mb[i].bb[4],mb[i].bb[5]);
                for (int k = i; k < j; k++) printf(" %s", REG[mb[k].reg]+6);
                puts("");
            }
        }
        i = j;
    }
    printf("\nsolid meshes total        : %d\n", nmb);
    printf("duplicate groups          : %d (%d span >1 region)\n", dupgroups, crossreg);
    printf("redundant duplicate meshes: %d (%.1f%% of all solid meshes)\n",
           dupmeshes, 100.0*dupmeshes/nmb);
    printf("redundant triangles       : %d\n", duptris);
    return 0;
}
