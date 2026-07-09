/* main.c — OpenUG: an open reimplementation of Need for Speed: Underground 2.
 * Loads the original game data directly (no Wine/box64), assembles a track
 * section, decodes its textures, and drives a textured car with AI opponents
 * around a circuit — SDL2 + OpenGL, portable across x86 and ARM.
 *
 * Desktop (x86/ARM, Linux/macOS/Windows): legacy GL 2.1 + GLSL 120.
 * Embedded / mobile (ARM): OpenGL ES 2.0 + GLSL 100 — build with -DN2_GLES
 * (links -lGLESv2). The shaders are written to compile on both.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <dirent.h>
#include <unistd.h>   /* execvp: menu track-switch re-launches the process */
#include <fcntl.h>
#include <sys/mman.h>  /* mmap the shared "master" region (lazy paging, no 100MB read) */
#include <sys/stat.h>
#include <SDL.h>
#include <zlib.h>   /* screenshot PNG (dev only; desktop build) */

#ifdef N2_GLES
#  include <SDL_opengles2.h>
#  define GLSL_HEADER "precision mediump float;\n"
#elif defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/gl.h>
#  include <OpenGL/glext.h>
#  define GLSL_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#else
#  define GL_GLEXT_PROTOTYPES 1
#  include <SDL_opengl.h>
#  define GLSL_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#endif

#include "nfsu2.h"

static const char *VS =
    GLSL_HEADER
    "attribute vec3 aPos; attribute vec2 aUV; attribute vec3 aNor;\n"
    "uniform mat4 uMVP; varying vec2 vUV; varying vec3 vN;\n"
    "void main(){ vUV=aUV; vN=aNor; gl_Position=uMVP*vec4(aPos,1.0); }\n";

static const char *FS =
    GLSL_HEADER
    "varying vec2 vUV; varying vec3 vN; uniform sampler2D uTex;\n"
    "uniform float uUseTex; uniform vec3 uColor; uniform float uUnlit; uniform float uAlpha; uniform float uSoft;\n"
    "void main(){\n"
    "  if(uUnlit>0.5){ float a=uAlpha;\n"
    "    if(uSoft>0.5){ float d=length(vUV-vec2(0.5)); a*=clamp(1.0-d*2.0,0.0,1.0); a*=a; }\n"
    "    gl_FragColor=vec4(uColor,a); return; }\n"
    "  vec3 L=normalize(vec3(0.4,0.7,0.6));\n"
    "  float d=0.75+0.4*abs(dot(normalize(vN),L));\n"
    "  vec3 base = uUseTex>0.5 ? texture2D(uTex,vUV).rgb : uColor;\n"
    "  gl_FragColor=vec4(base*d*1.35,1.0);\n"  // lift the dark night textures so the city reads
    "}\n";

/* ---- tiny 4x4 matrix (column-major) ---- */
static void mat_mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k*4+j]*b[i*4+k]; o[i*4+j] = s;
    }
}
static void mat_persp(float f, float ar, float n, float fa, float *m) {
    float t = 1.0f/tanf(f/2);
    memset(m, 0, 16*sizeof(float));
    m[0]=t/ar; m[5]=t; m[10]=(fa+n)/(n-fa); m[11]=-1; m[14]=2*fa*n/(n-fa);
}
static void vnorm(float *v){ float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l<1e-6f)l=1;
    v[0]/=l; v[1]/=l; v[2]/=l; }
static void mat_trans(float x,float y,float z,float *m){
    float r[16]={1,0,0,0,0,1,0,0,0,0,1,0,x,y,z,1}; memcpy(m,r,sizeof r); }
static void mat_rotz(float a, float *m){ float c=cosf(a),s=sinf(a);
    float r[16]={c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1}; memcpy(m,r,sizeof r); }
/* right-handed lookAt, column-major, up = world +Z */
static void mat_lookat(const float *eye, const float *fwd, float *m) {
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
static void write_png(const char *path, int w, int h, const unsigned char *rgb) {
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

/* per-mesh GPU buffers + computed normals */
typedef struct { GLuint vbo, nbo, ibo; int nidx, cat; uint32_t texkey; } GpuMesh;

/* an AI racer following the racing line */
#define N_AI 4
typedef struct { float pos[3], head, spd, col[3]; int t, lap, prevrel; } AiCar;

/* upload a scene's meshes to GPU buffers, computing per-vertex normals. */
static GpuMesh *upload_scene(N2Scene *s) {
    GpuMesh *gm = (GpuMesh *)calloc(s->count, sizeof(GpuMesh));
    for (int i = 0; i < s->count; i++) {
        N2Mesh *m = &s->meshes[i];
        float *nor = (float *)calloc(m->nverts * 3, sizeof(float));
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
        glGenBuffers(1,&gm[i].vbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].vbo);
        glBufferData(GL_ARRAY_BUFFER, m->nverts*5*sizeof(float), m->verts, GL_STATIC_DRAW);
        glGenBuffers(1,&gm[i].nbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].nbo);
        glBufferData(GL_ARRAY_BUFFER, m->nverts*3*sizeof(float), nor, GL_STATIC_DRAW);
        glGenBuffers(1,&gm[i].ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gm[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m->nidx*sizeof(uint16_t), m->idx, GL_STATIC_DRAW);
        gm[i].nidx = m->nidx; gm[i].cat = m->cat; gm[i].texkey = m->texkey;
        free(nor);
    }
    return gm;
}

static void draw_gpumesh(GpuMesh *g) {
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

static GLuint upload_tex(const N2Tex *t) {
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,t->w,t->h,0,GL_RGB,GL_UNSIGNED_BYTE,t->rgb);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    return id;
}

/* ---- procedural engine sound (no audio assets) ---- */
/* main thread writes freq/vol, audio thread reads them — benign float races. */
static volatile float g_eng_freq = 55.0f, g_eng_vol = 0.0f;
static volatile float g_road_vol = 0.0f;   /* tyre/wind noise, scales with speed */
static volatile float g_hit = 0.0f;        /* collision thud amplitude (main sets, cb decays) */
static unsigned g_rng = 22222;
static float frand(void) { g_rng = g_rng*1664525u + 1013904223u; return (int)(g_rng>>9)/4194304.0f - 1.0f; }
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    static double phase = 0.0; static float hit = 0.0f, lp = 0.0f;
    int16_t *out = (int16_t *)stream;
    int n = len / 2;                 /* mono S16 */
    float freq = g_eng_freq, vol = g_eng_vol, road = g_road_vol;
    if (g_hit > hit) hit = g_hit; g_hit = 0.0f;   /* latch a new collision */
    for (int i = 0; i < n; i++) {
        phase += freq / 44100.0; if (phase >= 1.0) phase -= 1.0;
        /* engine: sawtooth + octave */
        float saw  = 2.0f*(float)phase - 1.0f;
        double p2 = phase*2.0; p2 -= (int)p2;
        float saw2 = 2.0f*(float)p2 - 1.0f;
        float eng = (saw*0.6f + saw2*0.35f) * vol;
        /* road/wind: low-passed white noise scaled by speed */
        lp += (frand() - lp) * 0.25f;
        float env = lp * road;
        /* collision: short noise burst that decays */
        float thud = frand() * hit; hit *= 0.9985f;
        float s = eng + env + thud*0.8f;
        int v = (int)(s * 11000.0f);
        if (v > 32767) v = 32767; if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
}

/* (Re)load a racing-line circuit and grid the AI cars on it. Returns the AI
   count (0 if the file has no usable loop). Frees any previously loaded path,
   so it is safe to call repeatedly (e.g. when the menu switches circuit). */
static int load_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                        N2Path *aipath, AiCar *ais, float spawn[3],
                        float *heading0, int *start_idx, float cx, float cy) {
    static const float AICOL[N_AI][3] = {
        {0.15f,0.4f,0.95f}, {0.2f,0.8f,0.35f}, {0.95f,0.8f,0.15f}, {0.85f,0.2f,0.8f} };
    free(aipath->xy); aipath->xy = NULL; aipath->n = 0;
    char pathp[1024];
    snprintf(pathp, sizeof pathp, "%s/TRACKS/%s", dataroot, circuit);
    long plen; unsigned char *pdata = n2_read_file(pathp, &plen);
    if (!pdata) return 0;
    int ok = n2_load_path(pdata, plen, aipath) > 4;
    free(pdata);
    if (!ok) { free(aipath->xy); aipath->xy = NULL; aipath->n = 0; return 0; }
    /* Spawn the PLAYER at the densest built-up spot (cx,cy passed in) so the
       opening view frames the city — even if that's off the racing line (the
       lap logic tracks the nearest waypoint, so the race still works). The AI
       grid on the line, nearest that spot. */
    int best = 0; float bestd = 1e30f;
    for (int i = 0; i < aipath->n; i++) {
        float dx = aipath->xy[i*2]-cx, dy = aipath->xy[i*2+1]-cy, dd = dx*dx+dy*dy;
        if (dd < bestd) { bestd = dd; best = i; }
    }
    *start_idx = best;
    spawn[0]=cx; spawn[1]=cy;
    spawn[2]=n2_ground_z(scene, cx, cy, spawn[2]);
    float dwx = aipath->xy[best*2]-cx, dwy = aipath->xy[best*2+1]-cy;
    if (dwx*dwx+dwy*dwy < 9.0f) {                 /* already on the line: face along it */
        int nx = (best+1) % aipath->n;
        *heading0 = atan2f(aipath->xy[nx*2+1]-cy, aipath->xy[nx*2]-cx);
    } else {                                       /* face toward the racing line */
        *heading0 = atan2f(dwy, dwx);
    }
    for (int k = 0; k < N_AI; k++) {
        int t = (*start_idx + 2 + k*2) % aipath->n;
        ais[k].t = t; ais[k].lap = 0; ais[k].prevrel = t;
        ais[k].pos[0]=aipath->xy[t*2]; ais[k].pos[1]=aipath->xy[t*2+1];
        ais[k].pos[2]=spawn[2]; ais[k].head=*heading0;
        ais[k].spd = 3.0f + k*0.12f;
        memcpy(ais[k].col, AICOL[k], sizeof AICOL[k]);
    }
    return N_AI;
}

/* mmap a file read-only. Lazy paging: pages fault in only when touched, so a
   100MB shared "master" region costs just the bytes actually read (TPK header +
   the few textures we decode), not a full load. NULL on failure. */
static unsigned char *n2_map_file(const char *path, long *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *len = (long)st.st_size;
    return (unsigned char *)p;
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
    {0,0,0,0,7},{0,0,7,0,0},                         /* _ - */
};
static const unsigned char *glyph3x5(char c) {
    if (c >= '0' && c <= '9') return FONT3x5[1 + (c-'0')];
    if (c >= 'A' && c <= 'Z') return FONT3x5[11 + (c-'A')];
    if (c == '_') return FONT3x5[37];
    if (c == '-') return FONT3x5[38];
    return FONT3x5[0];
}
/* Draw an uppercase string at NDC (x,y = top-left), pixel size (px,py). Colour +
   uUnlit/uUseTex are set by the caller; this only sets uMVP and draws pixels. */
static void draw_text(GpuMesh *quad, GLint uMVP, const char *s, float x, float y, float px, float py) {
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
static float text_width(const char *s, float px) { return (float)strlen(s) * 4 * px; }

/* Push (pos.xy) out of any wall AABB (expanded by r) it penetrates, along the
   least-penetration axis; zero the into-wall velocity so the car slides along
   the face. Returns the number of walls resolved. */
static int collide_walls(float *pos, float *vel, const float obst[][4], int nobst, float r) {
    int hits = 0;
    for (int o = 0; o < nobst; o++) {
        float x0=obst[o][0]-r, y0=obst[o][1]-r, x1=obst[o][2]+r, y1=obst[o][3]+r;
        if (pos[0]<=x0 || pos[0]>=x1 || pos[1]<=y0 || pos[1]>=y1) continue;
        float pl=pos[0]-x0, pr=x1-pos[0], pd=pos[1]-y0, pu=y1-pos[1], m=pl; int ax=0;
        if (pr<m){m=pr;ax=1;} if (pd<m){m=pd;ax=2;} if (pu<m){m=pu;ax=3;}
        if      (ax==0){ pos[0]=x0; if(vel[0]>0)vel[0]=0; }
        else if (ax==1){ pos[0]=x1; if(vel[0]<0)vel[0]=0; }
        else if (ax==2){ pos[1]=y0; if(vel[1]>0)vel[1]=0; }
        else           { pos[1]=y1; if(vel[1]<0)vel[1]=0; }
        hits++;
    }
    return hits;
}
static void collide_walls_selftest(void) {
    float obst[1][4] = {{0,0,10,10}};
    float p[3]={5,5,0}, v[2]={1,1};
    assert(collide_walls(p, v, obst, 1, 1.0f) == 1);       /* deep inside -> resolved */
    assert(p[0]<=0 || p[0]>=10 || p[1]<=0 || p[1]>=10);    /* ...and now outside the box */
    float p2[3]={0.5f,5,0}, v2[2]={2,0};                   /* near left face, moving +x */
    collide_walls(p2, v2, obst, 1, 1.0f);
    assert(p2[0] <= -1.0f + 1e-4f && v2[0] == 0.0f);       /* pushed left, +x vel killed */
    float p3[3]={100,100,0}, v3[2]={1,0};
    assert(collide_walls(p3, v3, obst, 1, 1.0f) == 0);     /* far outside -> untouched */
}

int main(int argc, char **argv) {
    collide_walls_selftest();
    /* Point at your own NFSU2 install/data directory (contains TRACKS/, CARS/).
       Usage: nfsu2 [DATA_DIR] [options]
         --car NAME       car folder under CARS/ (default HUMMER)
         --track NAME     STREAM .BUN under TRACKS/ (default STREAML4RH)
         --circuit PATH   circuit Paths .bin under TRACKS/ (default ROUTESL4RF/Paths4602.bin)
         --shot out.png   render one frame and exit */
    const char *selfexe = argv[0];   /* for the menu's track-switch re-exec */
    const char *dataroot = ".", *shot = NULL;
    const char *carname = "HUMMER", *trackname = "STREAML4RH";
    const char *circuit = "ROUTESL4RF/Paths4602.bin"; int explicit_circuit = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--shot")    && i+1 < argc) shot      = argv[++i];
        else if (!strcmp(argv[i], "--car")     && i+1 < argc) carname   = argv[++i];
        else if (!strcmp(argv[i], "--track")   && i+1 < argc) trackname = argv[++i];
        else if (!strcmp(argv[i], "--circuit") && i+1 < argc) { circuit = argv[++i]; explicit_circuit = 1; }
        else dataroot = argv[i];
    }
    char trackp[1024], carp[1024], cartexp[1024], pathp[1024];
    snprintf(trackp,  sizeof trackp,  "%s/TRACKS/%s.BUN", dataroot, trackname);
    snprintf(carp,    sizeof carp,    "%s/CARS/%s/GEOMETRY.BIN", dataroot, carname);
    snprintf(cartexp, sizeof cartexp, "%s/CARS/%s/TEXTURES.BIN", dataroot, carname);
    snprintf(pathp,   sizeof pathp,   "%s/TRACKS/%s", dataroot, circuit);

    long len; unsigned char *data = n2_read_file(trackp, &len);
    if (!data) { fprintf(stderr, "cannot read %s\n  (pass your NFSU2 data dir: nfsu2 /path/to/data)\n", trackp); return 1; }

    /* resolvable texture keys for this region = local STREAM TPK record hashes
       + shared LOC4DYNTEX keys + a shared "master" region's TPK (small regions
       reference buildings whose textures live in a big neighbour — the master
       is mmap'd so only the touched pages load). Used to pick each mesh's
       diffuse slot and (later) to decode the textures. */
    char loc4p[1024]; snprintf(loc4p, sizeof loc4p, "%s/TRACKS/LOC4DYNTEX.BIN", dataroot);
    long loc4len; unsigned char *loc4 = n2_read_file(loc4p, &loc4len);
    /* Location-4 shared texture library; use the other big region if we ARE it. */
    /* Only mid-size regions need it: tiny proxies (<8MB) self-contain their
       textures, and the huge master regions (>60MB) already carry their own. */
    const char *mastername = strcmp(trackname, "STREAML4RD") ? "STREAML4RD" : "STREAML4RA";
    char masterp[1024]; snprintf(masterp, sizeof masterp, "%s/TRACKS/%s.BUN", dataroot, mastername);
    long masterlen = 0; unsigned char *master = NULL;
    if (len > 8L*1024*1024 && len < 60L*1024*1024) master = n2_map_file(masterp, &masterlen);
    N2Tpk localtpk = n2_tpk_open(data, len);
    N2Tpk mastertpk = {0};
    static uint32_t tkeys[16384]; int ntk = n2_tpk_keys(data, localtpk, tkeys, 16384);
    if (loc4 && ntk < 16384) ntk += n2_car_tex_keys(loc4, loc4len, tkeys + ntk, 16384 - ntk);
    if (master) {
        mastertpk = n2_tpk_open(master, masterlen);
        if (ntk < 16384) ntk += n2_tpk_keys(master, mastertpk, tkeys + ntk, 16384 - ntk);
    }
    printf("texture keyset: %d (local+LOC4%s)\n", ntk, master ? "+master" : "");

    N2Scene scene;
    int nm = n2_load_scene(data, len, &scene, tkeys, ntk);
    printf("loaded %d submeshes from %s\n", nm, trackp);
    N2Tex grass;
    int have_grass = n2_load_texture(data, len, "TRN_GRASSC", &grass);
    printf("terrain texture TRN_GRASSC: %s\n", have_grass ? "ok" : "-");
    int nroad = 0, nterr = 0;
    for (int i = 0; i < nm; i++) {
        if (scene.meshes[i].cat == N2_ROAD) nroad++;
        else if (scene.meshes[i].cat == N2_TERRAIN) nterr++;
    }
    printf("categories: road=%d terrain=%d other=%d\n", nroad, nterr, nm-nroad-nterr);

    /* scene centroid + extent for the camera */
    float cx=0,cy=0,cz=0; long cnt=0;
    float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
    for (int i=0;i<nm;i++) for (int v=0;v<scene.meshes[i].nverts;v++) {
        float *p = scene.meshes[i].verts + v*5;
        cx+=p[0]; cy+=p[1]; cz+=p[2]; cnt++;
        for (int c=0;c<3;c++){ if(p[c]<mn[c])mn[c]=p[c]; if(p[c]>mx[c])mx[c]=p[c]; }
    }
    if (cnt){ cx/=cnt; cy/=cnt; cz/=cnt; }
    float maxr = 1;
    for (int c=0;c<3;c++) if ((mx[c]-mn[c])/2 > maxr) maxr = (mx[c]-mn[c])/2;

    /* densest built-up spot: bin building (N2_OTHER) mesh positions into a grid
       and take the peak cell. The geometry centroid is often an open hole (the
       city spreads in a ring), so this is where to start the car for a view
       that actually frames buildings. Falls back to the centroid if no props. */
    float densx = cx, densy = cy;
    {
        static int hist[40][40]; memset(hist, 0, sizeof hist);
        float gw = (mx[0]-mn[0])/40.0f, gh = (mx[1]-mn[1])/40.0f;
        if (gw > 1e-3f && gh > 1e-3f) {
            for (int i=0;i<nm;i++) if (scene.meshes[i].cat == N2_OTHER && scene.meshes[i].nverts) {
                float *p = scene.meshes[i].verts;          /* first vertex ~ mesh location */
                int gx=(int)((p[0]-mn[0])/gw), gy=(int)((p[1]-mn[1])/gh);
                if (gx<0)gx=0; if (gx>39)gx=39; if (gy<0)gy=0; if (gy>39)gy=39;
                hist[gx][gy]++;
            }
            int best=0, bx=20, by=20;
            for (int gx=0;gx<40;gx++) for (int gy=0;gy<40;gy++)
                if (hist[gx][gy] > best) { best=hist[gx][gy]; bx=gx; by=gy; }
            if (best > 0) { densx = mn[0]+(bx+0.5f)*gw; densy = mn[1]+(by+0.5f)*gh; }
        }
    }
    printf("dense build-up centre: (%.0f,%.0f)\n", densx, densy);

    /* Building collision footprints: the 2D (XY) bounding box of every TALL
       N2_OTHER mesh (a wall/building — flat props, signs and road paint have
       little z-extent and are skipped, so they don't block the road). The car
       is kept out of these. */
    #define MAXOBST 8192
    static float obst[MAXOBST][4]; int nobst = 0;
    for (int i=0;i<nm && nobst<MAXOBST;i++) {
        if (scene.meshes[i].cat != N2_OTHER || scene.meshes[i].nverts < 3) continue;
        float ox0=1e30f,oy0=1e30f,oz0=1e30f, ox1=-1e30f,oy1=-1e30f,oz1=-1e30f;
        for (int v=0;v<scene.meshes[i].nverts;v++){ float *p=scene.meshes[i].verts+v*5;
            if(p[0]<ox0)ox0=p[0]; if(p[0]>ox1)ox1=p[0];
            if(p[1]<oy0)oy0=p[1]; if(p[1]>oy1)oy1=p[1];
            if(p[2]<oz0)oz0=p[2]; if(p[2]>oz1)oz1=p[2]; }
        if (oz1-oz0 < 2.5f) continue;                        /* flat: not a wall */
        if (ox1-ox0 > 300.0f || oy1-oy0 > 300.0f) continue;  /* skip oversized shells */
        obst[nobst][0]=ox0; obst[nobst][1]=oy0; obst[nobst][2]=ox1; obst[nobst][3]=oy1; nobst++;
    }
    printf("collision obstacles: %d buildings\n", nobst);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }

    /* start the procedural engine synth (optional — continue silently on fail) */
    SDL_AudioDeviceID adev = 0;
    { SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
      want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 1;
      want.samples = 1024; want.callback = audio_cb;
      adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
      if (adev) SDL_PauseAudioDevice(adev, 0); }
    printf("engine audio: %s\n", adev ? "on" : "unavailable");
#ifdef N2_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *win = SDL_CreateWindow("OpenUG",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "GL ctx: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glBindAttribLocation(prog, 2, "aNor");
    glLinkProgram(prog); glUseProgram(prog);
    GLint uMVP = glGetUniformLocation(prog, "uMVP");
    GLint uUseTex = glGetUniformLocation(prog, "uUseTex");
    GLint uColor = glGetUniformLocation(prog, "uColor");
    GLint uUnlit = glGetUniformLocation(prog, "uUnlit");
    GLint uAlpha = glGetUniformLocation(prog, "uAlpha");
    GLint uSoft = glGetUniformLocation(prog, "uSoft");
    glUniform1f(uAlpha, 1.0f); glUniform1f(uSoft, 0.0f);

    GpuMesh *gm = upload_scene(&scene);

    /* Per-mesh track textures: each mesh's diffuse key (0x134012) resolves to a
       texture in the region's own STREAM TPK (grass/road/props) or the shared
       LOC4DYNTEX pack (sky/facades). Decode each distinct key once, reject any
       that decode to noise (wrong format / swizzled surface), map key->GL tex.
       Generalises the old hard-coded TRN_GRASSC/RDP_PARKING lookup across
       regions (L4RR uses ORG_GRASS_001 etc.). */
    static uint32_t tmapkey[512]; static GLuint tmaptex[512]; int ntmap = 0;
    for (int i = 0; i < nm; i++) {
        uint32_t tk = scene.meshes[i].texkey; if (!tk) continue;
        int seen = 0; for (int j = 0; j < ntmap; j++) if (tmapkey[j]==tk) seen = 1;
        if (seen || ntmap >= 512) continue;
        N2Tex tt; int ok = n2_tpk_decode(data, len, localtpk, tk, &tt);
        if (!ok && loc4) ok = n2_load_car_tex_by_key(loc4, loc4len, tk, &tt);
        if (!ok && master) ok = n2_tpk_decode(master, masterlen, mastertpk, tk, &tt);
        if (ok && !n2_tex_noise(&tt)) {
            tmapkey[ntmap] = tk; tmaptex[ntmap] = upload_tex(&tt); ntmap++;
        }
        if (ok) free(tt.rgb);
    }
    printf("track textures bound: %d distinct\n", ntmap);

    /* load a car and drop it on the track (36-byte vertex format) */
    long clen; unsigned char *cdata = n2_read_file(carp, &clen);
    long ctlen; unsigned char *ctdata = n2_read_file(cartexp, &ctlen);
    /* per-mesh car textures: get the TPK keys, then decode each key referenced
       by a mesh into its own GL texture (body, wheel, brake, ... bound by UVs). */
    uint32_t ckeys[64]; int nck = ctdata ? n2_car_tex_keys(ctdata, ctlen, ckeys, 64) : 0;
    uint32_t mapkey[32]; GLuint maptex[32]; int nmap = 0;
    N2Tex cartex; int have_cartex = ctdata && n2_load_car_texture(ctdata, ctlen, &cartex);
    N2Scene car; int ncar = 0; GpuMesh *cgm = NULL;
    float spawn[3] = { cx, cy, cz }, heading0 = 0.0f;
    if (cdata) {
        ncar = n2_load_car(cdata, clen, &car, ckeys, nck);
        cgm = upload_scene(&car);
        /* decode + upload each distinct texture actually bound by a mesh */
        for (int i = 0; i < ncar; i++) {
            uint32_t tk = car.meshes[i].texkey; if (!tk) continue;
            int seen = 0; for (int j = 0; j < nmap; j++) if (mapkey[j]==tk) seen = 1;
            if (seen || nmap >= 32) continue;
            N2Tex ct;
            if (n2_load_car_tex_by_key(ctdata, ctlen, tk, &ct)) {
                mapkey[nmap] = tk; maptex[nmap] = upload_tex(&ct); nmap++;
                free(ct.rgb);
            }
        }
        printf("car textures bound: %d distinct\n", nmap);
        /* spawn on the road mesh nearest the track centre; aim inward so a
           straight run stays on the populated track (user steers in play). */
        float bestd = 1e30f;
        for (int i = 0; i < nm; i++) if (scene.meshes[i].cat == N2_ROAD) {
            float vx = scene.meshes[i].verts[0], vy = scene.meshes[i].verts[1];
            float d = (vx-cx)*(vx-cx) + (vy-cy)*(vy-cy);
            if (d < bestd) { bestd = d;
                spawn[0]=vx; spawn[1]=vy; spawn[2]=scene.meshes[i].verts[2]; }
        }
        heading0 = atan2f(cy - spawn[1], cx - spawn[0]);
        printf("car: %d meshes, spawn (%.1f,%.1f) heading %.2f\n",
               ncar, spawn[0], spawn[1], heading0);
    }

    char troot[1024]; snprintf(troot, sizeof troot, "%s/TRACKS", dataroot);

    /* Enumerate selectable TRACKS (STREAM*.BUN) for the menu. Switching track
       reloads the whole scene, so it re-launches the process (see below). */
    #define MAXTRACK 16
    char tracklist[MAXTRACK][64]; int ntrack = 0, seltrack = 0;
    {
        DIR *td = opendir(troot); struct dirent *de;
        while (td && ntrack < MAXTRACK && (de = readdir(td))) {
            char *dot = strstr(de->d_name, ".BUN");
            if (strncmp(de->d_name, "STREAM", 6) || !dot) continue;
            int L = (int)(dot - de->d_name); if (L > 63) L = 63;
            memcpy(tracklist[ntrack], de->d_name, L); tracklist[ntrack][L] = 0;
            if (!strcmp(tracklist[ntrack], trackname)) seltrack = ntrack;
            ntrack++;
        }
        if (td) closedir(td);
    }

    /* Enumerate selectable CARS (folders under CARS/ with a GEOMETRY.BIN, minus
       the part folders). Switching car re-launches the process, like tracks. */
    #define MAXCARS 64
    char carlist[MAXCARS][64]; int ncars = 0, selcar = 0;
    {
        static const char *parts[] = { "AUDIO","BRAKES","EXHAUST","PLATES","ROOF",
            "WHEELS","SPOILER","SPOILER_HATCH","SPOILER_SUV","MIRRORS_BODY",
            "MIRRORS_HUMMER","MIRRORS_POST","MIRRORS_SUV" };
        char croot[1024]; snprintf(croot, sizeof croot, "%s/CARS", dataroot);
        DIR *cd = opendir(croot); struct dirent *de;
        while (cd && ncars < MAXCARS && (de = readdir(cd))) {
            if (de->d_name[0] == '.') continue;
            int isp = 0;
            for (unsigned p = 0; p < sizeof parts/sizeof parts[0]; p++)
                if (!strcmp(de->d_name, parts[p])) isp = 1;
            if (isp) continue;
            char gp[1200]; snprintf(gp, sizeof gp, "%s/%s/GEOMETRY.BIN", croot, de->d_name);
            FILE *f = fopen(gp, "rb"); if (!f) continue; fclose(f);
            int L = (int)strlen(de->d_name); if (L > 63) L = 63;
            memcpy(carlist[ncars], de->d_name, L); carlist[ncars][L] = 0;
            if (!strcmp(carlist[ncars], carname)) selcar = ncars;
            ncars++;
        }
        if (cd) closedir(cd);
    }

    /* Enumerate selectable circuits for the pre-race menu: closed-loop
       Paths*.bin (first waypoint ~= last) lying within this track's footprint,
       so switching route never flings the cars off the rendered map. */
    #define MAXCIRC 24
    char circlist[MAXCIRC][256]; int ncirc = 0, selcirc = 0;
    {
        DIR *td = opendir(troot); struct dirent *de;
        while (td && ncirc < MAXCIRC && (de = readdir(td))) {
            if (strncmp(de->d_name, "ROUTES", 6)) continue;
            char rdir[1200]; snprintf(rdir, sizeof rdir, "%s/%s", troot, de->d_name);
            DIR *rd = opendir(rdir); struct dirent *re;
            while (rd && ncirc < MAXCIRC && (re = readdir(rd))) {
                if (strncmp(re->d_name, "Paths", 5)) continue;
                char rel[256]; snprintf(rel, sizeof rel, "%s/%s", de->d_name, re->d_name);
                int dup=0; for (int i=0;i<ncirc;i++) if(!strcmp(circlist[i],rel)) dup=1;
                if (dup) continue;
                char pp[1500]; snprintf(pp, sizeof pp, "%s/%s", troot, rel);
                long pl; unsigned char *pd = n2_read_file(pp, &pl); N2Path tp = {0};
                if (pd && n2_load_path(pd, pl, &tp) > 4) {
                    /* closed loop (start ~= end) whose centroid sits in this
                       track's footprint (+30% margin, since a circuit may
                       overhang the mesh bbox). Regions are far apart, so the
                       centroid test keeps other regions' loops off the list. */
                    float dx=tp.xy[0]-tp.xy[(tp.n-1)*2], dy=tp.xy[1]-tp.xy[(tp.n-1)*2+1];
                    float px=0,py=0; for(int i=0;i<tp.n;i++){ px+=tp.xy[i*2]; py+=tp.xy[i*2+1]; }
                    px/=tp.n; py/=tp.n;
                    float ex=(mx[0]-mn[0])*0.3f, ey=(mx[1]-mn[1])*0.3f;
                    if (dx*dx+dy*dy < 900.0f &&
                        px>=mn[0]-ex && px<=mx[0]+ex && py>=mn[1]-ey && py<=mx[1]+ey)
                        strncpy(circlist[ncirc++], rel, 255);
                }
                free(tp.xy); free(pd);
            }
            if (rd) closedir(rd);
        }
        if (td) closedir(td);
    }
    if (explicit_circuit)  /* honor an explicit --circuit if it's valid on this track */
        for (int i=0;i<ncirc;i++) if(!strcmp(circlist[i], circuit)) selcirc=i;
    printf("circuits available: %d (menu: Left/Right)\n", ncirc);

    N2Path aipath = {0};
    AiCar ais[N_AI]; int start_idx = 0;
    int nai = ncirc ? load_circuit(dataroot, circlist[selcirc], &scene, &aipath,
                                   ais, spawn, &heading0, &start_idx, densx, densy) : 0;
    if (nai) printf("circuit: %d-waypoint loop; %d AI racers, lap system on\n",
                    aipath.n, nai);

    GLuint texTerr = have_grass ? upload_tex(&grass) : 0;
    GLuint texCar = have_cartex ? upload_tex(&cartex) : 0;
    printf("car body texture: %s\n", have_cartex ? "decoded" : "-");

    /* unit-quad buffers for the 2D HUD (drawn in NDC via uMVP) */
    GpuMesh quad; memset(&quad, 0, sizeof quad);
    {
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
    }

    glEnable(GL_DEPTH_TEST);
    /* arcade driving: car length axis = local X; world +Z up. */
    float carpos[3] = { spawn[0], spawn[1], spawn[2] };
    float heading = heading0, speed = 0.0f, vel[2] = {0,0};
    float cam[3] = { spawn[0], spawn[1], spawn[2]+5 };
    int p_lap = 0, p_prev = 0;   /* player lap + previous loop-progress */
    const float ACCEL=0.3f, MAXSPD=4.5f, FRICTION=0.95f, TURN=0.045f, GRIP=0.86f;
    /* race flow: 3 = pre-race menu, 0 = countdown, 1 = racing, 2 = finished */
    const int COUNTDOWN = 180, LAP_TARGET = 2;
    int race_state = shot ? 1 : 3, racetimer = 0, finish_place = 0;
    float menuspin = 0.0f;   /* orbit-camera angle on the menu screen */
    int running = 1, shotframe = 0;
    /* tyre skid marks: a ring buffer of oriented ground SEGMENTS (ax,ay,az,
       bx,by,bz, life) — life starts at 1 and decays so marks fade over time.
       bx,by,bz) forming continuous ribbons; plus drift smoke particles. */
    #define MAXSKID 700
    static float skid[MAXSKID][7]; int skidn = 0, skidhead = 0;
    float prevL[3] = {0,0,0}, prevR[3] = {0,0,0}; int skidpen = 0;  /* pen-down state */
    #define MAXSMOKE 512
    static struct { float p[3], v[3], life, size; } smoke[MAXSMOKE]; int smoken = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = 0;
                else if (race_state == 3) {   /* pre-race menu navigation */
                    /* car and track each mean a whole new load, so they re-launch
                       the process; circuit is a cheap in-place reload. */
                    char *na[8]; int a=0;
                    if ((k==SDLK_LEFT || k==SDLK_RIGHT) && ncars > 1) {
                        selcar = (selcar + (k==SDLK_RIGHT?1:ncars-1)) % ncars;
                        na[a++]=(char*)selfexe; na[a++]=(char*)dataroot;
                        na[a++]="--car"; na[a++]=carlist[selcar];
                        na[a++]="--track"; na[a++]=(char*)trackname; na[a]=NULL;
                        SDL_Quit(); execvp(selfexe, na); return 1;
                    } else if ((k==SDLK_UP || k==SDLK_DOWN) && ntrack > 1) {
                        seltrack = (seltrack + (k==SDLK_DOWN?1:ntrack-1)) % ntrack;
                        na[a++]=(char*)selfexe; na[a++]=(char*)dataroot;
                        na[a++]="--car"; na[a++]=(char*)carname;
                        na[a++]="--track"; na[a++]=tracklist[seltrack]; na[a]=NULL;
                        SDL_Quit(); execvp(selfexe, na); return 1;
                    } else if ((k==SDLK_LEFTBRACKET || k==SDLK_RIGHTBRACKET) && ncirc > 1) {
                        selcirc = (selcirc + (k==SDLK_RIGHTBRACKET?1:ncirc-1)) % ncirc;
                        nai = load_circuit(dataroot, circlist[selcirc], &scene,
                                           &aipath, ais, spawn, &heading0, &start_idx, densx, densy);
                        carpos[0]=spawn[0]; carpos[1]=spawn[1]; carpos[2]=spawn[2];
                        heading=heading0; vel[0]=vel[1]=0; speed=0; p_lap=p_prev=0;
                    } else if (k==SDLK_RETURN || k==SDLK_SPACE) {
                        /* snap the player from the showcase spot to the circuit
                           start line so the race itself is fair. */
                        if (aipath.n > 0) {
                            carpos[0]=aipath.xy[start_idx*2]; carpos[1]=aipath.xy[start_idx*2+1];
                            carpos[2]=n2_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
                            int nx=(start_idx+1)%aipath.n;
                            heading=atan2f(aipath.xy[nx*2+1]-carpos[1], aipath.xy[nx*2]-carpos[0]);
                            vel[0]=vel[1]=0; speed=0;
                        }
                        race_state = 0; racetimer = 0;   /* -> 3-2-1 countdown */
                    }
                }
            }
        }
        /* driving with a velocity vector: throttle along the heading, steering
           rotates the heading, and the tyres scrub the sideways component
           (GRIP) — so hard cornering at speed slides/drifts. */
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        float hf[2] = { cosf(heading), sinf(heading) };
        if (race_state == 1) {
            /* throttle tapers as speed builds: punchy off the line, eases near top */
            if (ks[SDL_SCANCODE_W] || shot) {
                float a = ACCEL * (1.15f - 0.55f*(speed<0?-speed:speed)/MAXSPD);
                vel[0]+=hf[0]*a; vel[1]+=hf[1]*a;
            }
            if (ks[SDL_SCANCODE_S])         { vel[0]-=hf[0]*ACCEL*0.7f; vel[1]-=hf[1]*ACCEL*0.7f; }
        }
        vel[0]*=FRICTION; vel[1]*=FRICTION;
        float spd = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]);
        float dir = (vel[0]*hf[0]+vel[1]*hf[1]) < 0 ? -1.f : 1.f;  /* fwd vs reverse */
        float steer = (ks[SDL_SCANCODE_A]?1.f:0.f) - (ks[SDL_SCANCODE_D]?1.f:0.f);
        /* turn authority ramps in by ~35% of top speed then holds — responsive
           from low speed without getting twitchy/spinny flat out. */
        float sfac = spd/(MAXSPD*0.35f); if (sfac > 1) sfac = 1;
        heading += steer * TURN * sfac * dir;
        if (shot && nai > 0) {   /* screenshot autopilot: steer toward nearest AI */
            float da = atan2f(ais[0].pos[1]-carpos[1], ais[0].pos[0]-carpos[0]) - heading;
            while (da> 3.14159f) da-=6.28318f;
            while (da<-3.14159f) da+=6.28318f;
            if (da> 0.06f) da= 0.06f; if (da<-0.06f) da=-0.06f;
            heading += da;
        }
        /* decompose velocity in the new heading frame, clamp forward, scrub side */
        float nf[2] = { cosf(heading), sinf(heading) }, nr[2] = { nf[1], -nf[0] };
        float vf = vel[0]*nf[0]+vel[1]*nf[1], vl = vel[0]*nr[0]+vel[1]*nr[1];
        if (vf >  MAXSPD) vf =  MAXSPD;
        if (vf < -MAXSPD*0.4f) vf = -MAXSPD*0.4f;
        /* handbrake (Space while racing): rear grip lets go, so the lateral
           velocity survives and the car slides — drift it through corners. */
        float grip = (race_state==1 && ks[SDL_SCANCODE_SPACE]) ? 0.985f : GRIP;
        vl *= grip;
        vel[0] = nf[0]*vf + nr[0]*vl; vel[1] = nf[1]*vf + nr[1]*vl;
        speed = vf;                       /* forward speed, for HUD/collision */
        /* engine note follows speed (idle rev during the countdown) */
        { float sp = (speed < 0 ? -speed : speed) / MAXSPD;
          g_eng_freq = 55.0f + sp*185.0f;
          g_eng_vol  = (race_state==0 ? 0.16f : 0.16f + sp*0.5f);
          g_road_vol = sp*sp*0.35f; }   /* tyre/wind roar rises with speed */
        float fwd[3] = { nf[0], nf[1], 0 };
        carpos[0] += vel[0]; carpos[1] += vel[1];
        /* building collision: push the car out of any wall footprint it entered.*/
        if (race_state == 1 && collide_walls(carpos, vel, obst, nobst, 1.3f)) g_hit = 0.5f;
        /* sit on the road/terrain surface (smoothed to avoid jitter) */
        float gz = n2_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
        carpos[2] += (gz - carpos[2]) * 0.35f;

        /* drifting? extend the tyre ribbons + puff smoke at the rear wheels */
        float dmag = vl < 0 ? -vl : vl;
        int drifting = (dmag > 0.9f && speed > 1.0f);
        if (drifting) {
            float rx = carpos[0]-nf[0]*1.6f, ry = carpos[1]-nf[1]*1.6f, rz = carpos[2]+0.05f;
            float curL[3]={rx+nr[0], ry+nr[1], rz}, curR[3]={rx-nr[0], ry-nr[1], rz};
            if (skidpen) {   /* connect last frame's wheel points into ribbon segments */
                float *seg;
                seg=skid[skidhead]; seg[0]=prevL[0];seg[1]=prevL[1];seg[2]=prevL[2];
                seg[3]=curL[0];seg[4]=curL[1];seg[5]=curL[2];seg[6]=1.0f;
                skidhead=(skidhead+1)%MAXSKID; if(skidn<MAXSKID)skidn++;
                seg=skid[skidhead]; seg[0]=prevR[0];seg[1]=prevR[1];seg[2]=prevR[2];
                seg[3]=curR[0];seg[4]=curR[1];seg[5]=curR[2];seg[6]=1.0f;
                skidhead=(skidhead+1)%MAXSKID; if(skidn<MAXSKID)skidn++;
            }
            memcpy(prevL,curL,sizeof curL); memcpy(prevR,curR,sizeof curR); skidpen=1;
            /* dense puffs at both wheels every frame; soft = many low-alpha billboards */
            float wheels[2][2] = {{curL[0],curL[1]},{curR[0],curR[1]}};
            for (int wi = 0; wi < 2; wi++)
                for (int pc = 0; pc < 3 && smoken < MAXSMOKE; pc++) {
                    int si = smoken++;
                    smoke[si].p[0]=wheels[wi][0]+(rand()%100-50)*0.012f;
                    smoke[si].p[1]=wheels[wi][1]+(rand()%100-50)*0.012f;
                    smoke[si].p[2]=rz+0.2f+(rand()%30)*0.01f;
                    smoke[si].v[0]=(rand()%100-50)*0.0025f; smoke[si].v[1]=(rand()%100-50)*0.0025f;
                    smoke[si].v[2]=0.045f+(rand()%40)*0.0015f;
                    smoke[si].life=1.0f; smoke[si].size=0.55f+(rand()%40)*0.01f;
                }
        } else skidpen = 0;   /* lift the pen so the next drift starts a fresh ribbon */
        /* advance smoke particles (rise, expand, fade) */
        for (int i = 0; i < smoken; i++) {
            smoke[i].p[0]+=smoke[i].v[0]; smoke[i].p[1]+=smoke[i].v[1]; smoke[i].p[2]+=smoke[i].v[2];
            smoke[i].v[2]*=0.98f; smoke[i].size+=0.05f; smoke[i].life-=0.012f;
            if (smoke[i].life <= 0) smoke[i]=smoke[--smoken], i--;
        }

        /* AI opponents: each steers toward its next racing-line waypoint */
        for (int k = 0; race_state == 1 && k < nai; k++) {
            AiCar *ai = &ais[k];
            float ax=aipath.xy[ai->t*2]-ai->pos[0], ay=aipath.xy[ai->t*2+1]-ai->pos[1];
            if (ax*ax+ay*ay < 36.0f) ai->t = (ai->t+1) % aipath.n;
            float da = atan2f(ay, ax) - ai->head;
            while (da >  3.14159f) da -= 6.28318f;
            while (da < -3.14159f) da += 6.28318f;
            if (da >  0.06f) da =  0.06f;
            if (da < -0.06f) da = -0.06f;
            ai->head += da;
            ai->pos[0] += cosf(ai->head)*ai->spd;
            ai->pos[1] += sinf(ai->head)*ai->spd;
            float agz = n2_ground_z(&scene, ai->pos[0], ai->pos[1], ai->pos[2]);
            ai->pos[2] += (agz - ai->pos[2]) * 0.35f;
            /* lap: count when loop-progress wraps past the start/finish */
            int rel = (n2_nearest_wp(&aipath, ai->pos[0], ai->pos[1]) - start_idx
                       + aipath.n) % aipath.n;
            if (ai->prevrel > aipath.n*3/4 && rel < aipath.n/4) ai->lap++;
            ai->prevrel = rel;
        }
        if (race_state == 1 && aipath.n > 1) {   /* same lap logic for the player */
            int prel = (n2_nearest_wp(&aipath, carpos[0], carpos[1]) - start_idx
                        + aipath.n) % aipath.n;
            if (p_prev > aipath.n*3/4 && prel < aipath.n/4) p_lap++;
            p_prev = prel;
        }

        /* race state machine: countdown -> racing -> finished */
        racetimer++;
        if (race_state == 0 && racetimer >= COUNTDOWN) { race_state = 1; racetimer = 0; }
        if (race_state == 1 && p_lap >= LAP_TARGET) {
            int ahead = 0, pp = p_lap*aipath.n + p_prev;
            for (int k = 0; k < nai; k++)
                if (ais[k].lap*aipath.n + ais[k].prevrel > pp) ahead++;
            finish_place = ahead + 1;
            race_state = 2;
        }

        /* car-to-car collision: push overlapping cars apart (circle test).
           player pushed at full weight; AIs share the rest so they don't
           get shoved off their line too hard. */
        {
            const float RAD = 2.6f, MIN = RAD*2.0f;
            for (int k = 0; k < nai; k++) {
                float dx = ais[k].pos[0]-carpos[0], dy = ais[k].pos[1]-carpos[1];
                float d2 = dx*dx+dy*dy;
                if (d2 > 1e-4f && d2 < MIN*MIN) {
                    float d = sqrtf(d2), push = (MIN - d);
                    float ux = dx/d, uy = dy/d;
                    carpos[0]   -= ux*push*0.5f; carpos[1]   -= uy*push*0.5f;
                    ais[k].pos[0]+= ux*push*0.5f; ais[k].pos[1]+= uy*push*0.5f;
                    vel[0]*=0.85f; vel[1]*=0.85f;   /* bump scrubs a little speed */
                    { float s = (speed<0?-speed:speed)/MAXSPD; if(s>g_hit) g_hit = 0.3f+s*0.5f; }
                }
            }
            for (int a = 0; a < nai; a++) for (int b = a+1; b < nai; b++) {
                float dx = ais[b].pos[0]-ais[a].pos[0], dy = ais[b].pos[1]-ais[a].pos[1];
                float d2 = dx*dx+dy*dy;
                if (d2 > 1e-4f && d2 < MIN*MIN) {
                    float d = sqrtf(d2), push = (MIN - d)*0.5f, ux = dx/d, uy = dy/d;
                    ais[a].pos[0]-=ux*push; ais[a].pos[1]-=uy*push;
                    ais[b].pos[0]+=ux*push; ais[b].pos[1]+=uy*push;
                }
            }
        }

        /* camera: menu = slow orbit around the parked car; else chase cam */
        float want[3];
        if (race_state == 3) {              /* orbit the parked car, framing the city around it */
            menuspin += 0.006f;
            want[0] = carpos[0] + cosf(menuspin)*16.0f;
            want[1] = carpos[1] + sinf(menuspin)*16.0f;
            want[2] = carpos[2] + 8.0f;
        } else {
            want[0] = carpos[0]-fwd[0]*10; want[1] = carpos[1]-fwd[1]*10;
            want[2] = carpos[2]+4.5f;
        }
        for (int c=0;c<3;c++) cam[c] += (want[c]-cam[c])*0.22f; /* smooth follow */
        float look[3] = { carpos[0]-cam[0], carpos[1]-cam[1], (carpos[2]+1.5f)-cam[2] };

        int W, H; SDL_GL_GetDrawableSize(win, &W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.06f, 0.07f, 0.11f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float P[16], V[16], MVP[16];
        /* menu orbits ~9m from the car, so use a close near-plane there;
           big regions otherwise push maxr*0.01 past the car and clip it. */
        mat_persp(0.9f, (float)W/H, race_state==3 ? 0.2f : maxr*0.01f, maxr*30, P);
        mat_lookat(cam, look, V);
        mat_mul(P, V, MVP);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        glUniform1f(uUnlit, 0.0f);

        /* track: each mesh wears its own bound texture (grass/road/props/sky);
           terrain without one falls back to the grass tex, everything else to
           flat asphalt (e.g. a region's road texture that didn't decode). */
        for (int i = 0; i < nm; i++) {
            GLuint tex = 0;
            for (int j = 0; j < ntmap; j++) if (tmapkey[j]==gm[i].texkey) { tex = tmaptex[j]; break; }
            if (!tex && gm[i].cat == N2_TERRAIN) tex = texTerr;
            if (tex) { glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, tex); }
            else { glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, 0.28f, 0.29f, 0.31f); }
            draw_gpumesh(&gm[i]);
        }

        /* tyre skid marks: flat dark quads on the ground, alpha-blended */
        if (skidn > 0) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
            glUniform3f(uColor, 0.04f, 0.04f, 0.05f);
            for (int i = 0; i < skidn; i++) {
                float *s = skid[i];                          /* ax,ay,az, bx,by,bz, life */
                if (race_state != 3) s[6] -= 0.0018f;        /* ~9s to fade (frozen on menu) */
                if (s[6] <= 0.0f) continue;
                float dx=s[3]-s[0], dy=s[4]-s[1];            /* segment direction */
                float len=sqrtf(dx*dx+dy*dy); if(len<1e-4f) continue;
                glUniform1f(uAlpha, 0.4f*s[6]);              /* fade with age */
                float ux=dx, uy=dy;                          /* along-travel axis (u) */
                const float W=0.45f; float px=-dy/len*W, py=dx/len*W;  /* width axis (v) */
                /* unit quad (u,v) -> A + u*(B-A) + v*perp*W, centred across width */
                float M[16]={ ux,uy,0,0,  px,py,0,0,  0,0,1,0,
                              s[0]-px*0.5f, s[1]-py*0.5f, s[2], 1 };
                float MV[16]; mat_mul(MVP, M, MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad);
            }
            glUniform1f(uAlpha, 1.0f); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* car: solid-shaded, positioned + rotated to heading */
        if (ncar) {
            float T[16], Rz[16], Model[16], MVPc[16];
            mat_trans(carpos[0], carpos[1], carpos[2] + 0.43f, T);
            mat_rotz(heading, Rz);
            mat_mul(T, Rz, Model);
            mat_mul(MVP, Model, MVPc);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPc);
            /* per-mesh: each part wears its own bound texture (body/wheel/...);
               parts with no in-TPK texture get a sensible flat colour by class
               (body falls back to the atlas so untagged panels stay painted). */
            for (int i = 0; i < ncar; i++) {
                int c = cgm[i].cat;
                GLuint tex = 0;
                for (int j = 0; j < nmap; j++) if (mapkey[j]==cgm[i].texkey) { tex = maptex[j]; break; }
                if (!tex && texCar && (c == N2_CAR_BODY || c == N2_CAR_MISC)) tex = texCar;
                if (tex) {
                    glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, tex);
                } else {
                    glUniform1f(uUseTex, 0.0f);
                    if      (c == N2_CAR_GLASS) glUniform3f(uColor, 0.08f, 0.10f, 0.14f);
                    else if (c == N2_CAR_LIGHT) glUniform3f(uColor, 0.95f, 0.85f, 0.55f);
                    else if (c == N2_CAR_TIRE)  glUniform3f(uColor, 0.06f, 0.06f, 0.07f);
                    else                        glUniform3f(uColor, 0.85f, 0.12f, 0.12f);
                }
                draw_gpumesh(&cgm[i]);
            }
            glUniform1f(uUseTex, 0.0f);   /* AIs stay flat colour */
            /* AI opponents — same body, each in its own colour */
            for (int k = 0; k < nai; k++) {
                mat_trans(ais[k].pos[0], ais[k].pos[1], ais[k].pos[2] + 0.43f, T);
                mat_rotz(ais[k].head, Rz);
                mat_mul(T, Rz, Model);
                mat_mul(MVP, Model, MVPc);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPc);
                glUniform3f(uColor, ais[k].col[0], ais[k].col[1], ais[k].col[2]);
                for (int i = 0; i < ncar; i++) draw_gpumesh(&cgm[i]);
            }
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        }

        /* drift smoke: camera-facing billboards, light + fading, additive-ish */
        if (smoken > 0) {
            float lz = sqrtf(look[0]*look[0]+look[1]*look[1]+look[2]*look[2]); if(lz<1e-4f)lz=1;
            float ld[3]={look[0]/lz,look[1]/lz,look[2]/lz};
            float rt[3]={ld[1]*1-ld[2]*0, ld[2]*0-ld[0]*1, ld[0]*0-ld[1]*0}; /* look x up(0,0,1) */
            rt[0]=ld[1]; rt[1]=-ld[0]; rt[2]=0;
            float rl=sqrtf(rt[0]*rt[0]+rt[1]*rt[1]); if(rl<1e-4f)rl=1; rt[0]/=rl;rt[1]/=rl;
            float up[3]={rt[1]*ld[2]-0, 0-rt[0]*ld[2], rt[0]*ld[1]-rt[1]*ld[0]}; /* right x look */
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f); glUniform1f(uSoft, 1.0f);
            glUniform3f(uColor, 0.78f, 0.78f, 0.80f);
            for (int i = 0; i < smoken; i++) {
                float s = smoke[i].size, *p = smoke[i].p;
                float cxp = p[0]-(rt[0]+up[0])*s*0.5f, cyp = p[1]-(rt[1]+up[1])*s*0.5f,
                      czp = p[2]-(rt[2]+up[2])*s*0.5f;
                float M[16]={ rt[0]*s,rt[1]*s,rt[2]*s,0,  up[0]*s,up[1]*s,up[2]*s,0,
                              0,0,1,0,  cxp,cyp,czp,1 };
                float MV[16]; mat_mul(MVP, M, MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV);
                glUniform1f(uAlpha, smoke[i].life*smoke[i].life*0.22f);
                draw_gpumesh(&quad);
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* HUD: race-position leaderboard — one colour bar per car, ordered by
           progress along the racing line (leader on top); player bar wider. */
        if (nai > 0 && race_state != 3) {
            glDisable(GL_DEPTH_TEST);
            glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
            /* rank by monotonic progress = lap*loop + progress-along-loop */
            int nc = nai + 1, ord[N_AI+1], prog[N_AI+1], pl[N_AI+1];
            float col[N_AI+1][3];
            prog[0] = p_lap*aipath.n + p_prev; pl[0]=1;
            col[0][0]=0.85f; col[0][1]=0.12f; col[0][2]=0.12f;
            for (int k=0;k<nai;k++){
                prog[k+1]=ais[k].lap*aipath.n + ais[k].prevrel;
                memcpy(col[k+1], ais[k].col, sizeof col[0]); pl[k+1]=0;
            }
            for (int i=0;i<nc;i++) ord[i]=i;
            for (int i=0;i<nc;i++) for (int j=i+1;j<nc;j++)
                if (prog[ord[j]] > prog[ord[i]]) { int t=ord[i]; ord[i]=ord[j]; ord[j]=t; }
            for (int i=0;i<nc;i++){
                int c=ord[i];
                float w = pl[c]?0.10f:0.06f, x=-0.97f, y=0.90f-i*0.10f, h=0.075f;
                float M[16]={ w,0,0,0, 0,h,0,0, 0,0,1,0, x,y,0,1 };
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, M);
                glUniform3f(uColor, col[c][0], col[c][1], col[c][2]);
                draw_gpumesh(&quad);
            }
            /* lap counter (green pips, one per completed lap) + lap-progress bar */
            for (int i=0;i<p_lap && i<8;i++){
                float x=-0.97f+i*0.05f, y=-0.93f;
                float M[16]={0.035f,0,0,0, 0,0.05f,0,0, 0,0,1,0, x,y,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                glUniform3f(uColor,0.2f,0.9f,0.3f); draw_gpumesh(&quad);
            }
            {   /* thin bar: fraction of the current lap completed */
                float frac = (float)p_prev / (float)aipath.n;
                float bg[16]={1.5f,0,0,0, 0,0.03f,0,0, 0,0,1,0, -0.75f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,bg);
                glUniform3f(uColor,0.15f,0.15f,0.18f); draw_gpumesh(&quad);
                float fg[16]={1.5f*frac,0,0,0, 0,0.03f,0,0, 0,0,1,0, -0.75f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,fg);
                glUniform3f(uColor,0.9f,0.8f,0.2f); draw_gpumesh(&quad);
            }
            {   /* speedometer: fraction of top speed (bottom-right) */
                float sf = speed / MAXSPD; if (sf < 0) sf = -sf; if (sf > 1) sf = 1;
                float bg[16]={0.35f,0,0,0, 0,0.03f,0,0, 0,0,1,0, 0.58f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,bg);
                glUniform3f(uColor,0.15f,0.15f,0.18f); draw_gpumesh(&quad);
                float fg[16]={0.35f*sf,0,0,0, 0,0.03f,0,0, 0,0,1,0, 0.58f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,fg);
                glUniform3f(uColor,0.3f,0.85f,1.0f); draw_gpumesh(&quad);
            }
            glEnable(GL_DEPTH_TEST);
        }

        /* race banners: 3-2-1 countdown, then a finish banner + place pips */
        glDisable(GL_DEPTH_TEST);
        glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
        if (race_state == 3) {
            /* car selector (Left/Right): a tight row of pips, chosen lit white */
            float cw = 0.02f, cx0 = -((ncars-1)*cw)/2.0f;
            for (int i = 0; i < ncars; i++) {
                int s = (i == selcar);
                float M[16]={0.014f,0,0,0, 0,s?0.05f:0.028f,0,0, 0,0,1,0, cx0+i*cw,0.44f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.95f,0.95f,0.98f);
                else   glUniform3f(uColor,0.30f,0.32f,0.38f);
                draw_gpumesh(&quad);
            }
            /* track selector (Up/Down): one blue pip per STREAM*.BUN found */
            float tx0 = -((ntrack-1)*0.05f)/2.0f;
            for (int i = 0; i < ntrack; i++) {
                int s = (i == seltrack);
                float M[16]={0.035f,0,0,0, 0,s?0.05f:0.03f,0,0, 0,0,1,0, tx0+i*0.05f,0.36f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.3f,0.7f,1.0f);
                else   glUniform3f(uColor,0.25f,0.3f,0.4f);
                draw_gpumesh(&quad);
            }
            /* circuit selector ([ / ]): one yellow pip per circuit, chosen lit + tall */
            float x0 = -((ncirc-1)*0.05f)/2.0f;
            for (int i = 0; i < ncirc; i++) {
                int s = (i == selcirc);
                float h = s?0.05f:0.03f, y = 0.29f - (s?0.01f:0);
                float M[16]={0.035f,0,0,0, 0,h,0,0, 0,0,1,0, x0+i*0.05f,y,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.95f,0.8f,0.2f);
                else   glUniform3f(uColor,0.3f,0.3f,0.35f);
                draw_gpumesh(&quad);
            }
            /* labels: car name (big, white) above its pips; track name below */
            glUniform3f(uColor, 0.95f, 0.95f, 0.98f);
            draw_text(&quad, uMVP, carname, -text_width(carname,0.017f)/2, 0.56f, 0.017f, 0.026f);
            glUniform3f(uColor, 0.45f, 0.7f, 1.0f);
            draw_text(&quad, uMVP, trackname, -text_width(trackname,0.012f)/2, 0.23f, 0.012f, 0.02f);
            /* "press ENTER" prompt: a gently pulsing green bar */
            float pulse = 0.55f + 0.45f*sinf(menuspin*6.0f);
            float M[16]={0.5f,0,0,0, 0,0.06f,0,0, 0,0,1,0, -0.25f,-0.25f,0,1};
            glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
            glUniform3f(uColor,0.15f*pulse,0.85f*pulse,0.3f*pulse);
            draw_gpumesh(&quad);
        } else if (race_state == 0) {
            int n = 3 - racetimer/60;
            for (int i = 0; i < n; i++) {
                float M[16]={0.06f,0,0,0, 0,0.15f,0,0, 0,0,1,0, -0.11f+i*0.09f,0.32f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                glUniform3f(uColor,0.9f,0.2f,0.15f); draw_gpumesh(&quad);
            }
            if (racetimer > COUNTDOWN-18) {   /* GO! */
                float M[16]={0.5f,0,0,0, 0,0.15f,0,0, 0,0,1,0, -0.25f,0.32f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                glUniform3f(uColor,0.2f,0.9f,0.3f); draw_gpumesh(&quad);
            }
        } else if (race_state == 2) {         /* finished */
            float M[16]={1.7f,0,0,0, 0,0.13f,0,0, 0,0,1,0, -0.85f,0.38f,0,1};
            glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
            glUniform3f(uColor,0.95f,0.8f,0.2f); draw_gpumesh(&quad);
            for (int i = 0; i < finish_place; i++) {   /* finishing place */
                float PM[16]={0.05f,0,0,0, 0,0.08f,0,0, 0,0,1,0, -0.08f+i*0.065f,0.4f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,PM);
                glUniform3f(uColor,0.12f,0.12f,0.16f); draw_gpumesh(&quad);
            }
        }
        glEnable(GL_DEPTH_TEST);

        if (shot && ++shotframe >= 40) {
            unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
            for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
            write_png(shot, W, H, fl);
            free(px); free(fl);
            printf("wrote %s (%dx%d) after driving to (%.0f,%.0f)\n", shot, W, H, carpos[0], carpos[1]);
            running = 0;
        }
        SDL_GL_SwapWindow(win);
    }

    n2_free_scene(&scene);
    free(data);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
