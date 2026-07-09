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
#include <dirent.h>
#include <unistd.h>   /* execvp: menu track-switch re-launches the process */
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
    "  float d=0.55+0.45*abs(dot(normalize(vN),L));\n"
    "  vec3 base = uUseTex>0.5 ? texture2D(uTex,vUV).rgb : uColor;\n"
    "  gl_FragColor=vec4(base*d,1.0);\n"
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
                        float *heading0, int *start_idx) {
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
    *start_idx = 0;
    spawn[0]=aipath->xy[0]; spawn[1]=aipath->xy[1];
    spawn[2]=n2_ground_z(scene, spawn[0], spawn[1], spawn[2]);
    int nx = 1 % aipath->n;
    *heading0 = atan2f(aipath->xy[nx*2+1]-spawn[1], aipath->xy[nx*2]-spawn[0]);
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

int main(int argc, char **argv) {
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

    N2Scene scene;
    int nm = n2_load_scene(data, len, &scene);
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
    char loc4p[1024]; snprintf(loc4p, sizeof loc4p, "%s/TRACKS/LOC4DYNTEX.BIN", dataroot);
    long loc4len; unsigned char *loc4 = n2_read_file(loc4p, &loc4len);
    uint32_t tmapkey[96]; GLuint tmaptex[96]; int ntmap = 0;
    for (int i = 0; i < nm; i++) {
        uint32_t tk = scene.meshes[i].texkey; if (!tk) continue;
        int seen = 0; for (int j = 0; j < ntmap; j++) if (tmapkey[j]==tk) seen = 1;
        if (seen || ntmap >= 96) continue;
        N2Tex tt; int ok = n2_load_stream_tex_by_hash(data, len, tk, &tt);
        if (!ok && loc4) ok = n2_load_car_tex_by_key(loc4, loc4len, tk, &tt);
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
                                   ais, spawn, &heading0, &start_idx) : 0;
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
                    if ((k==SDLK_LEFT || k==SDLK_RIGHT) && ncirc > 1) {
                        selcirc = (selcirc + (k==SDLK_RIGHT?1:ncirc-1)) % ncirc;
                        nai = load_circuit(dataroot, circlist[selcirc], &scene,
                                           &aipath, ais, spawn, &heading0, &start_idx);
                        carpos[0]=spawn[0]; carpos[1]=spawn[1]; carpos[2]=spawn[2];
                        heading=heading0; vel[0]=vel[1]=0; speed=0; p_lap=p_prev=0;
                    } else if ((k==SDLK_UP || k==SDLK_DOWN) && ntrack > 1) {
                        /* switching track means a whole new scene: re-launch the
                           process on it rather than tearing down all GL state. */
                        seltrack = (seltrack + (k==SDLK_DOWN?1:ntrack-1)) % ntrack;
                        char *na[8]; int a=0;
                        na[a++]=(char*)selfexe; na[a++]=(char*)dataroot;
                        na[a++]="--car"; na[a++]=(char*)carname;
                        na[a++]="--track"; na[a++]=tracklist[seltrack]; na[a]=NULL;
                        SDL_Quit();
                        execvp(selfexe, na);
                        return 1;   /* only reached if exec failed */
                    } else if (k==SDLK_RETURN || k==SDLK_SPACE) {
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
            if (ks[SDL_SCANCODE_W] || shot) { vel[0]+=hf[0]*ACCEL; vel[1]+=hf[1]*ACCEL; }
            if (ks[SDL_SCANCODE_S])         { vel[0]-=hf[0]*ACCEL*0.7f; vel[1]-=hf[1]*ACCEL*0.7f; }
        }
        vel[0]*=FRICTION; vel[1]*=FRICTION;
        float spd = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]);
        float dir = (vel[0]*hf[0]+vel[1]*hf[1]) < 0 ? -1.f : 1.f;  /* fwd vs reverse */
        float steer = (ks[SDL_SCANCODE_A]?1.f:0.f) - (ks[SDL_SCANCODE_D]?1.f:0.f);
        heading += steer * TURN * (spd/MAXSPD) * dir;
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
        vl *= GRIP;
        vel[0] = nf[0]*vf + nr[0]*vl; vel[1] = nf[1]*vf + nr[1]*vl;
        speed = vf;                       /* forward speed, for HUD/collision */
        /* engine note follows speed (idle rev during the countdown) */
        { float sp = (speed < 0 ? -speed : speed) / MAXSPD;
          g_eng_freq = 55.0f + sp*185.0f;
          g_eng_vol  = (race_state==0 ? 0.16f : 0.16f + sp*0.5f);
          g_road_vol = sp*sp*0.35f; }   /* tyre/wind roar rises with speed */
        float fwd[3] = { nf[0], nf[1], 0 };
        carpos[0] += vel[0]; carpos[1] += vel[1];
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
        if (race_state == 3) {
            menuspin += 0.008f;
            want[0] = carpos[0] + cosf(menuspin)*9.0f;
            want[1] = carpos[1] + sinf(menuspin)*9.0f;
            want[2] = carpos[2] + 3.5f;
        } else {
            want[0] = carpos[0]-fwd[0]*10; want[1] = carpos[1]-fwd[1]*10;
            want[2] = carpos[2]+4.5f;
        }
        for (int c=0;c<3;c++) cam[c] += (want[c]-cam[c])*0.22f; /* smooth follow */
        float look[3] = { carpos[0]-cam[0], carpos[1]-cam[1], (carpos[2]+1.5f)-cam[2] };

        int W, H; SDL_GL_GetDrawableSize(win, &W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.02f, 0.03f, 0.05f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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
            /* track selector (Up/Down): one blue pip per STREAM*.BUN found */
            float tx0 = -((ntrack-1)*0.05f)/2.0f;
            for (int i = 0; i < ntrack; i++) {
                int s = (i == seltrack);
                float M[16]={0.035f,0,0,0, 0,s?0.05f:0.03f,0,0, 0,0,1,0, tx0+i*0.05f,0.42f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.3f,0.7f,1.0f);
                else   glUniform3f(uColor,0.25f,0.3f,0.4f);
                draw_gpumesh(&quad);
            }
            /* circuit selector (Left/Right): one pip per circuit, chosen lit + tall */
            float x0 = -((ncirc-1)*0.05f)/2.0f;
            for (int i = 0; i < ncirc; i++) {
                int s = (i == selcirc);
                float h = s?0.05f:0.03f, y = 0.30f - (s?0.01f:0);
                float M[16]={0.035f,0,0,0, 0,h,0,0, 0,0,1,0, x0+i*0.05f,y,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.95f,0.8f,0.2f);
                else   glUniform3f(uColor,0.3f,0.3f,0.35f);
                draw_gpumesh(&quad);
            }
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
