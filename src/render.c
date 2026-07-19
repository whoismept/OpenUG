/* render.c — OpenUG Renderer module implementation. Everything here was
 * extracted verbatim from the original monolithic main.c; the parsing logic
 * (nfsu2.h) is untouched ground truth. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <zlib.h>   /* screenshot PNG (dev only) */

#include "render.h"

#ifdef N2_GLES
#  define GLSL_HEADER "precision mediump float;\n"
#else
#  define GLSL_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#endif

static const char *VS =
    GLSL_HEADER
    "attribute vec3 aPos; attribute vec2 aUV; attribute vec3 aNor;\n"
    "uniform mat4 uMVP; varying vec2 vUV; varying vec3 vN; varying float vDepth;\n"
    "varying vec3 vPos;\n"
    /* clip.w == view-space depth under a perspective projection, for world
       (P*V) and car (P*V*M) alike — no view matrix or extra uniforms needed.
       NDC-drawn HUD quads have w==1, so fog leaves them alone. */
    "void main(){ vUV=aUV; vN=aNor; vPos=aPos; gl_Position=uMVP*vec4(aPos,1.0);\n"
    "  vDepth=gl_Position.w; }\n";

static const char *FS =
    GLSL_HEADER
    "varying vec2 vUV; varying vec3 vN; varying float vDepth; varying vec3 vPos;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex; uniform vec3 uColor; uniform float uUnlit; uniform float uAlpha; uniform float uSoft; uniform float uSpec; uniform float uDecal;\n"
    "uniform float uAmbient; uniform float uDiffuse; uniform vec3 uLight;\n"
    "uniform vec3 uFogColor; uniform float uFogDensity;\n"
    "uniform vec3 uCamPos; uniform float uEnv; uniform float uUVCheck;\n"
    "uniform float uGloss; uniform float uFlipN;\n"
    /* exp^2 distance fog: fades far batches into the sky colour (which is
       cleared to uFogColor, so the horizon and the haze always agree) */
    "void main(){\n"
    /* diagnostic UV visualization: bypasses lighting/texture/fog entirely.
       R=u, G=v (raw, unclamped -- a mesh whose UVs run outside [0,1] shows
       as a flat saturated patch instead of a gradient, exposing tiling
       regions at a glance) with a darkened 10x10 grid overlaid so uneven
       cell spacing reveals stretching and grid-line direction reveals
       flips/mirrors. Checked first so it overrides every other path. */
    "  if(uUVCheck>0.5){\n"
    "    vec2 g=abs(fract(vUV*10.0-0.5)-0.5);\n"
    "    float grid=smoothstep(0.0,0.04,min(g.x,g.y));\n"
    "    gl_FragColor=vec4(vec3(vUV.x,vUV.y,0.2)*mix(0.35,1.0,grid),1.0); return;\n"
    "  }\n"
    "  float fog = clamp(exp(-pow(vDepth*uFogDensity, 2.0)), 0.0, 1.0);\n"
    "  if(uUnlit>0.5){ float a=uAlpha;\n"
    "    if(uSoft>0.5){ float d=length(vUV-vec2(0.5)); a*=clamp(1.0-d*2.0,0.0,1.0); a*=a; }\n"
    "    gl_FragColor=vec4(mix(uFogColor,uColor,fog),a); return; }\n"
    /* uLight is the sun direction in the OBJECT's model space: normals stay
       model-space (no per-vertex transform), so a rotated object (the car)
       must counter-rotate the light or its lit side turns with it. */
    "  vec3 L=normalize(uLight); vec3 N=normalize(vN);\n"
    "  if(uFlipN>0.5) N = -N;\n"
    "  vec3 V=normalize(uCamPos - vPos);\n"
    "  float nl=max(dot(N,L),0.0);\n"
    "  float d=uAmbient+uDiffuse*nl;\n"   /* directional; reveals body form */
    /* uAmbient/uDiffuse are one shared per-frame pair (world+cars both read
       them), so retuning the defaults to fix a car panel would also retune
       every building and road. Cars get their own lower ambient floor /
       sharper diffuse swing right here instead, gated the same way as the
       cavity term above — world batches always set uSpec=0, so they never
       take this branch. */
    "  if(uSpec>0.001) d=uAmbient*0.55+uDiffuse*1.2*nl;\n"
    /* uDecal: paint under an alpha-masked decal atlas (badges/vinyls) —
       texture RGB shows only where its alpha says so, paint elsewhere */
    "  vec4 t = texture2D(uTex,vUV);\n"
    "  vec3 base = uUseTex>0.5 ? (uDecal>0.5 ? mix(uColor,t.rgb,t.a) : t.rgb) : uColor;\n"
    /* cheap view-angle cavity darkening (cars only, uSpec>0 — the world
       batches always set uSpec=0): panel gaps/creases have no texture data
       to show them (verified: body meshes carry no diffuse map at all), so
       this fakes the early-2000s baked-AO look by darkening paint where the
       surface turns away from the camera, instead of claiming detail that
       isn't in the asset. */
    "  if(uSpec>0.001){\n"
    "    float edge=pow(1.0-clamp(dot(N,V),0.0,1.0), 4.0);\n"
    "    base *= mix(1.0, 0.6, edge);\n"
    "  }\n"
    /* Phong: reflect the light about the normal and test it against the VIEW
       vector. The old form used dot(N,L) with no V term at all, so it was a
       sharpened diffuse -- the highlight could not travel across a panel as
       the camera moved, which is what made the paint read flat/matte. */
    "  float sp = pow(max(dot(reflect(-L,N), V), 0.0), uGloss)*uSpec;\n"
    "  float rim = pow(1.0-abs(N.z), 3.0)*uSpec*0.4;\n"        /* fresnel-ish edge sheen */
    "  vec3 lit = base*d*1.35 + sp + rim;\n"
    /* environment reflection (cars only, uEnv>0): a procedural night-city
       sphere — dark ground, warm city-glow horizon band, dim blue sky —
       sampled with the model-space reflection vector, fresnel-weighted.
       uCamPos is the camera in the SAME space as vPos/vN. */
    "  if(uEnv>0.001){\n"
    "    vec3 R = reflect(-V, N);\n"
    "    float up = clamp(R.z*0.5+0.5, 0.0, 1.0);\n"
    /* stronger, wider night-city sphere: brighter sky dome and a much broader
       warm horizon band (pow 8 -> 4). The old values topped out at 0.11, so
       even at uEnv 0.5 the reflection never rose above the paint and the body
       read as plastic. This gives the clear-coat something bright enough to
       actually mirror. */
    "    vec3 env = mix(vec3(0.03,0.03,0.05), vec3(0.10,0.14,0.24), up)\n"
    "             + vec3(0.85,0.66,0.42)*pow(1.0-abs(R.z), 4.0);\n"
    "    float fres = 0.35 + 0.65*pow(1.0-clamp(dot(N,V),0.0,1.0), 3.0);\n"
    "    lit += env * (uEnv * fres);\n"
    "  }\n"
    /* lit alpha = uAlpha (1 everywhere but the blended glass pass), so
       translucent glass keeps its specular highlight */
    "  gl_FragColor=vec4(mix(uFogColor, lit, fog), uAlpha);\n"
    "}\n";

/* ---- tiny 4x4 matrix (column-major) ---- */
void mat_mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k*4+j]*b[i*4+k]; o[i*4+j] = s;
    }
}
void mat_persp(float f, float ar, float n, float fa, float *m) {
    float t = 1.0f/tanf(f/2);
    memset(m, 0, 16*sizeof(float));
    m[0]=t/ar; m[5]=t; m[10]=(fa+n)/(n-fa); m[11]=-1; m[14]=2*fa*n/(n-fa);
}
static void vnorm(float *v){ float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l<1e-6f)l=1;
    v[0]/=l; v[1]/=l; v[2]/=l; }
void mat_trans(float x,float y,float z,float *m){
    float r[16]={1,0,0,0,0,1,0,0,0,0,1,0,x,y,z,1}; memcpy(m,r,sizeof r); }
void mat_rotz(float a, float *m){ float c=cosf(a),s=sinf(a);
    float r[16]={c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1}; memcpy(m,r,sizeof r); }
/* right-handed lookAt, column-major, up = world +Z */
void mat_lookat(const float *eye, const float *fwd, float *m) {
    float f[3]={fwd[0],fwd[1],fwd[2]}; vnorm(f);
    float up[3]={0,0,1};
    float s[3]={ f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] }; vnorm(s);
    float u[3]={ s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0] };
    float r[16]={ s[0],u[0],-f[0],0,  s[1],u[1],-f[1],0,  s[2],u[2],-f[2],0,
        -(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]),
        -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]),
          f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2], 1 };
    memcpy(m, r, sizeof r);
}

/* minimal PNG writer (dev screenshot). rgb is top-left origin, w*h*3. */
static void png_chunk(FILE *f, const char *tag, const unsigned char *d, uLong n) {
    unsigned char len[4] = { n>>24, n>>16, n>>8, n };
    fwrite(len, 1, 4, f);
    uLong crc = crc32(0, (const Bytef*)tag, 4);
    crc = crc32(crc, d, n);
    fwrite(tag, 1, 4, f); fwrite(d, 1, n, f);
    unsigned char c[4] = { crc>>24, crc>>16, crc>>8, crc };
    fwrite(c, 1, 4, f);
}
void write_png(const char *path, int w, int h, const unsigned char *rgb) {
    /* raw scanlines with filter byte 0 */
    uLong raw_n = (uLong)h * (1 + w*3);
    unsigned char *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[y*(1+w*3)] = 0;
        memcpy(raw + y*(1+w*3) + 1, rgb + (size_t)y*w*3, w*3);
    }
    uLong comp_n = compressBound(raw_n);
    unsigned char *comp = malloc(comp_n);
    compress2(comp, &comp_n, raw, raw_n, 9);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        unsigned char ihdr[13] = { w>>24,w>>16,w>>8,w, h>>24,h>>16,h>>8,h, 8,2,0,0,0 };
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", comp, comp_n);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(comp);
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
        fprintf(stderr, "shader: %s\n", log); }
    return s;
}

RProg render_program(void) {
    RProg r;
    r.prog = glCreateProgram();
    glAttachShader(r.prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(r.prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(r.prog, 0, "aPos");
    glBindAttribLocation(r.prog, 1, "aUV");
    glBindAttribLocation(r.prog, 2, "aNor");
    glLinkProgram(r.prog); glUseProgram(r.prog);
    r.uMVP     = glGetUniformLocation(r.prog, "uMVP");
    r.uUseTex  = glGetUniformLocation(r.prog, "uUseTex");
    r.uColor   = glGetUniformLocation(r.prog, "uColor");
    r.uUnlit   = glGetUniformLocation(r.prog, "uUnlit");
    r.uAlpha   = glGetUniformLocation(r.prog, "uAlpha");
    r.uSoft    = glGetUniformLocation(r.prog, "uSoft");
    r.uSpec    = glGetUniformLocation(r.prog, "uSpec");
    r.uDecal   = glGetUniformLocation(r.prog, "uDecal");
    r.uFogColor   = glGetUniformLocation(r.prog, "uFogColor");
    r.uFogDensity = glGetUniformLocation(r.prog, "uFogDensity");
    r.uCamPos = glGetUniformLocation(r.prog, "uCamPos");
    r.uEnv    = glGetUniformLocation(r.prog, "uEnv");
    r.uUVCheck = glGetUniformLocation(r.prog, "uUVCheck");
    r.uAmbient = glGetUniformLocation(r.prog, "uAmbient");
    r.uDiffuse = glGetUniformLocation(r.prog, "uDiffuse");
    r.uLight   = glGetUniformLocation(r.prog, "uLight");
    r.uGloss   = glGetUniformLocation(r.prog, "uGloss");
    r.uFlipN   = glGetUniformLocation(r.prog, "uFlipN");
    glUniform1f(r.uAlpha, 1.0f); glUniform1f(r.uSoft, 0.0f); glUniform1f(r.uSpec, 0.0f);
    glUniform1f(r.uDecal, 0.0f);
    glUniform3f(r.uFogColor, 0.06f, 0.07f, 0.11f); glUniform1f(r.uFogDensity, 0.0f);
    glUniform3f(r.uCamPos, 0, 0, 0); glUniform1f(r.uEnv, 0.0f);
    glUniform1f(r.uUVCheck, 0.0f);
    glUniform1f(r.uGloss, 20.0f);
    glUniform1f(r.uFlipN, 0.0f);
    glUniform3f(r.uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);
    return r;
}

/* smoothed per-vertex normals for one mesh (area-weighted face accumulation);
 * nor must hold nverts*3 floats, zeroed by the caller. */
static void mesh_normals(const N2Mesh *m, float *nor) {
    for (int t = 0; t + 2 < m->nidx; t += 3) {
        int a=m->idx[t], b=m->idx[t+1], c=m->idx[t+2];
        float *pa=m->verts+a*5,*pb=m->verts+b*5,*pc=m->verts+c*5;
        float ux=pb[0]-pa[0],uy=pb[1]-pa[1],uz=pb[2]-pa[2];
        float vx=pc[0]-pa[0],vy=pc[1]-pa[1],vz=pc[2]-pa[2];
        float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
        int ii[3]={a,b,c};
        for(int e=0;e<3;e++){ nor[ii[e]*3]+=nx; nor[ii[e]*3+1]+=ny; nor[ii[e]*3+2]+=nz; }
    }
    for (int v=0;v<m->nverts;v++){ float*np=nor+v*3;
        float l=sqrtf(np[0]*np[0]+np[1]*np[1]+np[2]*np[2]); if(l<1e-6f)l=1;
        np[0]/=l; np[1]/=l; np[2]/=l; }
}

/* upload a scene's meshes to GPU buffers, computing per-vertex normals. */
GpuMesh *upload_scene(N2Scene *s) {
    GpuMesh *gm = (GpuMesh *)calloc(s->count, sizeof(GpuMesh));
    for (int i = 0; i < s->count; i++) {
        N2Mesh *m = &s->meshes[i];
        float *nor = (float *)calloc(m->nverts * 3, sizeof(float));
        mesh_normals(m, nor);
        glGenBuffers(1,&gm[i].vbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].vbo);
        glBufferData(GL_ARRAY_BUFFER, m->nverts*5*sizeof(float), m->verts, GL_STATIC_DRAW);
        glGenBuffers(1,&gm[i].nbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].nbo);
        glBufferData(GL_ARRAY_BUFFER, m->nverts*3*sizeof(float), nor, GL_STATIC_DRAW);
        glGenBuffers(1,&gm[i].ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gm[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m->nidx*sizeof(uint16_t), m->idx, GL_STATIC_DRAW);
        gm[i].nidx = m->nidx; gm[i].cat = m->cat; gm[i].texkey = m->texkey;
        gm[i].trim = m->trim; gm[i].roof = m->roof;
        free(nor);
    }
    return gm;
}

/* ---- static-world batching ---- */

typedef struct { uint64_t key; int idx; } BSortEnt;   /* (cell,tex) -> mesh */
static int bsort_cmp(const void *a, const void *b) {
    uint64_t ka = ((const BSortEnt *)a)->key, kb = ((const BSortEnt *)b)->key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}
static int btex_cmp(const void *a, const void *b) {   /* final texture order */
    GLuint ta = ((const N2Batch *)a)->tex, tb = ((const N2Batch *)b)->tex;
    return ta < tb ? -1 : ta > tb ? 1 : 0;
}

#define BATCH_CELL     256.0f   /* grid cell edge, metres */
#define BATCH_MAXVERTS 65535    /* u16 indices (GLES2: no u32 without ext) */

/* merge meshes [i0,i1) of the sort array into one uploaded batch */
static void batch_emit(const N2Scene *s, const BSortEnt *ent, int i0, int i1,
                       GLuint tex, N2Batch *b) {
    int nv = 0, ni = 0;
    for (int k = i0; k < i1; k++) {
        nv += s->meshes[ent[k].idx].nverts; ni += s->meshes[ent[k].idx].nidx;
    }
    BatchedVertex *bv = (BatchedVertex *)malloc((size_t)nv * sizeof *bv);
    uint16_t *bi = (uint16_t *)malloc((size_t)ni * sizeof *bi);
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    int vo = 0, io = 0;
    for (int k = i0; k < i1; k++) {
        const N2Mesh *m = &s->meshes[ent[k].idx];
        float *nor = (float *)calloc((size_t)m->nverts * 3, sizeof(float));
        mesh_normals(m, nor);          /* per source mesh: no cross-mesh smoothing */
        for (int v = 0; v < m->nverts; v++) {
            BatchedVertex *o = &bv[vo + v]; const float *p = m->verts + v*5;
            o->pos[0]=p[0]; o->pos[1]=p[1]; o->pos[2]=p[2];
            o->uv[0]=p[3];  o->uv[1]=p[4];
            o->normal[0]=nor[v*3]; o->normal[1]=nor[v*3+1]; o->normal[2]=nor[v*3+2];
            for (int c = 0; c < 3; c++) {
                if (p[c] < mn[c]) mn[c] = p[c];
                if (p[c] > mx[c]) mx[c] = p[c];
            }
        }
        for (int t = 0; t < m->nidx; t++) bi[io + t] = (uint16_t)(m->idx[t] + vo);
        vo += m->nverts; io += m->nidx;
        free(nor);
    }
    memset(b, 0, sizeof *b);
    glGenBuffers(1, &b->vbo); glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, (long)nv * (long)sizeof *bv, bv, GL_STATIC_DRAW);
    glGenBuffers(1, &b->ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)ni * 2, bi, GL_STATIC_DRAW);
    b->index_count = ni; b->tex = tex; b->nmesh = i1 - i0;
    b->texkey = s->meshes[ent[i0].idx].texkey;
    for (int c = 0; c < 3; c++) { b->bbox_min[c] = mn[c]; b->bbox_max[c] = mx[c]; }
    free(bv); free(bi);
}

int upload_world_batches(const N2Scene *s, const float (*mbb)[4],
                         const GLuint *mtex, GLuint texTerr, N2Batch **out) {
    int n = s->count;
    /* world extent -> grid coords */
    float x0 = 1e30f, y0 = 1e30f;
    for (int i = 0; i < n; i++) {
        if (mbb[i][0] < x0) x0 = mbb[i][0];
        if (mbb[i][1] < y0) y0 = mbb[i][1];
    }
    /* sort meshes by (cell, resolved texture) — sky/glow meshes are pulled by
       upload_cat_batches instead: batching them in here would let an ordinary
       cell+texture run silently absorb a skybox shell or a neon sign, so
       they'd draw with the wrong depth/blend state at the wrong time. */
    BSortEnt *ent = (BSortEnt *)malloc((size_t)n * sizeof *ent);
    int m = 0;
    for (int i = 0; i < n; i++) {
        const N2Mesh *mesh = &s->meshes[i];
        if (mesh->cat == N2_SKY || mesh->cat == N2_GLOW) continue;
        GLuint tex = mtex[i];
        if (!tex && mesh->cat == N2_TERRAIN) tex = texTerr;   /* fallback baked in */
        float cx = (mbb[i][0]+mbb[i][2])*0.5f, cy = (mbb[i][1]+mbb[i][3])*0.5f;
        uint64_t cell = (uint64_t)(uint32_t)((int)((cy-y0)/BATCH_CELL)*4096
                                           + (int)((cx-x0)/BATCH_CELL));
        ent[m].key = cell << 32 | tex; ent[m].idx = i; m++;
    }
    qsort(ent, (size_t)m, sizeof *ent, bsort_cmp);
    /* walk key runs, splitting a run at the u16 vertex ceiling */
    int cap = 256, nb = 0;
    N2Batch *bat = (N2Batch *)malloc((size_t)cap * sizeof *bat);
    int i0 = 0, verts = 0;
    for (int i = 0; i <= m; i++) {
        int flush = (i == m) || (i > i0 && ent[i].key != ent[i0].key) ||
                    (i > i0 && verts + s->meshes[ent[i].idx].nverts > BATCH_MAXVERTS);
        if (flush && i > i0) {
            if (nb == cap) { cap *= 2; bat = (N2Batch *)realloc(bat, (size_t)cap * sizeof *bat); }
            batch_emit(s, ent, i0, i, (GLuint)(ent[i0].key & 0xffffffffu), &bat[nb++]);
            i0 = i; verts = 0;
        }
        if (i < m) verts += s->meshes[ent[i].idx].nverts;
    }
    free(ent);
    qsort(bat, (size_t)nb, sizeof *bat, btex_cmp);   /* minimise texture binds */
    *out = bat;
    return nb;
}

/* Same merge as above but for exactly one category, grouped by texture only
 * (no spatial grid — a city has a handful of skybox/neon meshes, not tens of
 * thousands, so there's nothing for a cell split to buy here). */
int upload_cat_batches(const N2Scene *s, int cat, const GLuint *mtex, N2Batch **out) {
    int n = s->count;
    BSortEnt *ent = (BSortEnt *)malloc((size_t)(n ? n : 1) * sizeof *ent);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (s->meshes[i].cat == cat) { ent[m].key = mtex[i]; ent[m].idx = i; m++; }
    qsort(ent, (size_t)m, sizeof *ent, bsort_cmp);
    int cap = 8, nb = 0;
    N2Batch *bat = (N2Batch *)malloc((size_t)cap * sizeof *bat);
    int i0 = 0, verts = 0;
    for (int i = 0; i <= m; i++) {
        int flush = (i == m) || (i > i0 && ent[i].key != ent[i0].key) ||
                    (i > i0 && verts + s->meshes[ent[i].idx].nverts > BATCH_MAXVERTS);
        if (flush && i > i0) {
            if (nb == cap) { cap *= 2; bat = (N2Batch *)realloc(bat, (size_t)cap * sizeof *bat); }
            batch_emit(s, ent, i0, i, (GLuint)ent[i0].key, &bat[nb++]);
            i0 = i; verts = 0;
        }
        if (i < m) verts += s->meshes[ent[i].idx].nverts;
    }
    free(ent);
    *out = bat;
    return nb;
}

void draw_batch(const N2Batch *b) {
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchedVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BatchedVertex), (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(BatchedVertex), (void*)20);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glDrawElements(GL_TRIANGLES, b->index_count, GL_UNSIGNED_SHORT, 0);
}

/* Build a clean procedural wheel (short cylinder, axle along Y, disc in X-Z) —
 * the game's rim meshes are sparse spoke shells that read as spiky urchins when
 * drawn solid, so we substitute a simple tyre. Modelled at the origin like the
 * game wheel, so the same wheelT placement transforms apply. */
GpuMesh make_wheel(float R, float halfW) {
    const int N = 24;
    int nv = N*2 + 2, nt = N*4;         /* 2 rings + 2 hub centres; sides + 2 caps */
    N2Mesh m; memset(&m, 0, sizeof m);
    m.nverts = nv; m.verts = (float *)malloc(nv*5*sizeof(float));
    for (int i = 0; i < N; i++) {
        float a = 2.0f*(float)M_PI*i/N, cx = R*cosf(a), cz = R*sinf(a);
        float u = cosf(a)*0.5f+0.5f, v = sinf(a)*0.5f+0.5f;
        float *p0 = m.verts + i*5;     p0[0]=cx; p0[1]= halfW; p0[2]=cz; p0[3]=u; p0[4]=v;
        float *p1 = m.verts + (N+i)*5; p1[0]=cx; p1[1]=-halfW; p1[2]=cz; p1[3]=u; p1[4]=v;
    }
    int c0 = 2*N, c1 = 2*N+1;
    float *h0=m.verts+c0*5; h0[0]=0;h0[1]= halfW;h0[2]=0;h0[3]=0.5f;h0[4]=0.5f;
    float *h1=m.verts+c1*5; h1[0]=0;h1[1]=-halfW;h1[2]=0;h1[3]=0.5f;h1[4]=0.5f;
    m.idx = (uint16_t *)malloc(nt*3*sizeof(uint16_t)); int k=0;
    for (int i = 0; i < N; i++) {
        int j = (i+1)%N;
        m.idx[k++]=i;    m.idx[k++]=j;    m.idx[k++]=N+j;      /* tread quad */
        m.idx[k++]=i;    m.idx[k++]=N+j;  m.idx[k++]=N+i;
        m.idx[k++]=c0;   m.idx[k++]=i;    m.idx[k++]=j;         /* +Y face */
        m.idx[k++]=c1;   m.idx[k++]=N+j;  m.idx[k++]=N+i;       /* -Y face */
    }
    m.nidx = k; m.cat = N2_CAR_TIRE;
    N2Scene s; s.meshes = &m; s.count = 1; s.cap = 1;
    GpuMesh *g = upload_scene(&s); GpuMesh out = *g;
    free(g); free(m.verts); free(m.idx);
    return out;
}

/* Procedural alloy-rim texture for the wheel caps: the cap UVs are radial
 * (centre 0.5,0.5, ring at uv-radius 0.5), so paint by relative radius —
 * bright hub disc, spoked metal mid, dark rubber edge. The tread quads
 * sample the outer ring = rubber. */
/* spin_blur: 0 = crisp spokes, 1 = the same rim averaged around its own axis,
 * i.e. what the spokes smear into once the wheel turns faster than the eye can
 * follow. Generated, not loaded: there is no pre-baked blur asset anywhere in
 * CARS/ (checked every TEXTURES.BIN — the only "SPIN" hits are the SPINNER rim
 * accessory), and the rim itself is procedural here anyway. */
static GLuint make_wheel_tex_var(int spin_blur) {
    enum { S = 64 };
    static unsigned char px[S*S*3];
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float dx = (x+0.5f)/S - 0.5f, dy = (y+0.5f)/S - 0.5f;
        float r = sqrtf(dx*dx + dy*dy) * 2.0f;      /* 0 centre .. 1 ring */
        unsigned char v;
        if (r > 0.78f)      v = 14;                                  /* rubber */
        else if (r > 0.30f) {                                        /* spokes */
            /* the crisp term is (0.5+0.5cos)^2, whose mean over a full turn is
               0.25 + 0.25*mean(cos^2) = 0.375 — so the blurred ring carries the
               true angular average of the spokes: same mean brightness, no
               spoke phase left to strobe. */
            float spoke = 0.5f + 0.5f*cosf(5.0f*atan2f(dy, dx));
            v = (unsigned char)(38 + 70.0f*(spin_blur ? 0.375f : spoke*spoke));
        } else               v = r < 0.10f ? 150 : 105;              /* hub */
        unsigned char *o = px + (y*S + x)*3;
        o[0] = v; o[1] = v; o[2] = (unsigned char)(v + v/16);        /* cool metal */
    }
    N2Tex t = { S, S, px, NULL };
    GLuint id = upload_tex(&t);
    /* radial, single-sample cap texture (no tiling intended) — clamp so a
       filter footprint near u/v=0 or 1 can't wrap and bleed in colour from
       the opposite edge, same reasoning as the car atlas/vinyl clamps. */
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}
GLuint make_wheel_tex(void)      { return make_wheel_tex_var(0); }
GLuint make_wheel_blur_tex(void) { return make_wheel_tex_var(1); }

/* unit-quad buffers for the 2D HUD / billboards (drawn in NDC via uMVP) */
GpuMesh make_quad(void) {
    GpuMesh quad; memset(&quad, 0, sizeof quad);
    float qv[] = {0,0,0,0,0, 1,0,0,1,0, 1,1,0,1,1, 0,1,0,0,1};
    float qn[] = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
    uint16_t qi[] = {0,1,2, 0,2,3};
    glGenBuffers(1,&quad.vbo); glBindBuffer(GL_ARRAY_BUFFER,quad.vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof qv,qv,GL_STATIC_DRAW);
    glGenBuffers(1,&quad.nbo); glBindBuffer(GL_ARRAY_BUFFER,quad.nbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof qn,qn,GL_STATIC_DRAW);
    glGenBuffers(1,&quad.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,quad.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof qi,qi,GL_STATIC_DRAW);
    quad.nidx = 6;
    return quad;
}

void draw_gpumesh(GpuMesh *g) {
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, g->nbo);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
    glDrawElements(GL_TRIANGLES, g->nidx, GL_UNSIGNED_SHORT, 0);
}

GLuint upload_tex(const N2Tex *t) {
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    if (t->alpha) {   /* interleave the decal-mask plane -> RGBA */
        unsigned char *px = (unsigned char *)malloc((size_t)t->w * t->h * 4);
        for (long p = 0; p < (long)t->w * t->h; p++) {
            px[p*4]=t->rgb[p*3]; px[p*4+1]=t->rgb[p*3+1];
            px[p*4+2]=t->rgb[p*3+2]; px[p*4+3]=t->alpha[p];
        }
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t->w,t->h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
        free(px);
    } else
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,t->w,t->h,0,GL_RGB,GL_UNSIGNED_BYTE,t->rgb);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    return id;
}

/* Minimal 3x5 bitmap font (uppercase, digits, _ and -), 5 rows x 3 bits each
   (bit 2 = left column). Rendered as one unit-quad per lit pixel — fine for the
   handful of short labels in the menu. Index: 0=space, 1-10='0'-'9', 11-36=A-Z,
   37='_', 38='-'. */
static const unsigned char FONT3x5[][5] = {
    {0,0,0,0,0},                                     /* space */
    {7,5,5,5,7},{2,2,2,2,2},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1}, /* 0 1 2 3 4 */
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}, /* 5 6 7 8 9 */
    {7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,6,4,7}, /* A B C D E */
    {7,4,6,4,4},{7,4,5,5,7},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,7}, /* F G H I J */
    {5,5,6,5,5},{4,4,4,4,7},{5,7,5,5,5},{5,7,7,7,5},{7,5,5,5,7}, /* K L M N O */
    {7,5,7,4,4},{7,5,5,7,1},{7,5,6,5,5},{7,4,7,1,7},{7,2,2,2,2}, /* P Q R S T */
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2}, /* U V W X Y */
    {7,1,2,4,7},                                     /* Z */
    {0,0,0,0,7},{0,0,7,0,0},{1,1,2,4,4},             /* _ - / */
};
static const unsigned char *glyph3x5(char c) {
    if (c >= '0' && c <= '9') return FONT3x5[1 + (c-'0')];
    if (c >= 'A' && c <= 'Z') return FONT3x5[11 + (c-'A')];
    if (c == '_') return FONT3x5[37];
    if (c == '-') return FONT3x5[38];
    if (c == '/') return FONT3x5[39];
    return FONT3x5[0];
}
/* Draw an uppercase string at NDC (x,y = top-left), pixel size (px,py). Colour +
   uUnlit/uUseTex are set by the caller; this only sets uMVP and draws pixels. */
void draw_text(GpuMesh *quad, GLint uMVP, const char *s, float x, float y, float px, float py) {
    for (; *s; s++) {
        const unsigned char *g = glyph3x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col)) {
                    float M[16] = { px*0.85f,0,0,0, 0,py*0.85f,0,0, 0,0,1,0,
                                    x + col*px, y - row*py, 0, 1 };
                    glUniformMatrix4fv(uMVP, 1, GL_FALSE, M);
                    draw_gpumesh(quad);
                }
        x += 4*px;   /* 3 columns + 1 gap */
    }
}
float text_width(const char *s, float px) { return (float)strlen(s) * 4 * px; }
