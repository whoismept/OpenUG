/* world.h — OpenUG World module: multi-region city loading (STREAM .BUN
 * stitching into one scene), per-region texture binding, per-mesh bounds
 * for draw culling, and a grid-accelerated ground query. Owns the region
 * file buffers between load and texture upload. */
#ifndef OPENUG_WORLD_H
#define OPENUG_WORLD_H

#include "nfsu2.h"
#include "render.h"

#define WORLD_MAXREG 16

typedef struct {
    char name[64];
    unsigned char *data; long len;   /* file buffer, freed after texture bind */
    N2Tpk tpk;
    int mesh0, mesh1;                /* this region's mesh range in the scene */
} WRegion;

typedef struct {
    N2Scene scene;
    WRegion rgn[WORLD_MAXREG]; int nreg;
    unsigned char *loc4; long loc4len;                       /* shared tex library */
    unsigned char *master; long masterlen; N2Tpk mastertpk;  /* single-region mode */
    N2Tex grass; int have_grass;                             /* terrain fallback */
    float (*mbb)[4];   /* per-mesh XY bbox (x0,y0,x1,y1) for culling + ground grid */
} World;

/* Load trackname ("ALL" = every STREAM*.BUN under troot, else one region)
 * into w->scene. Builds per-mesh bounds and the ground grid. Returns the
 * mesh count (0 = nothing readable). */
int world_load(World *w, const char *troot, const char *trackname);

/* Decode + upload every distinct mesh texture (own TPK -> LOC4 -> master),
 * writing the key->GL map, then free the region buffers. Needs a GL context.
 * Returns the number of textures bound. */
int world_bind_textures(World *w, uint32_t *keys, GLuint *texs, int cap);

/* Ground height at (x,y): same contract as n2_ground_z but only tests the
 * road/terrain meshes whose bbox covers the point (grid lookup). */
float world_ground_z(const N2Scene *s, float x, float y, float fallback);

#endif
