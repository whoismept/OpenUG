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
    "uniform float uUseTex; uniform vec3 uColor; uniform float uUnlit;\n"
    "void main(){\n"
    "  if(uUnlit>0.5){ gl_FragColor=vec4(uColor,1.0); return; }\n"
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
typedef struct { GLuint vbo, nbo, ibo; int nidx, cat; } GpuMesh;

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
        gm[i].nidx = m->nidx; gm[i].cat = m->cat;
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

int main(int argc, char **argv) {
    /* Point at your own NFSU2 install/data directory (contains TRACKS/, CARS/).
       Usage: nfsu2 [DATA_DIR] [--shot out.png]   (DATA_DIR defaults to ".") */
    const char *dataroot = ".";
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i+1 < argc) shot = argv[++i];
        else dataroot = argv[i];
    }
    char trackp[1024], carp[1024], cartexp[1024], pathp[1024];
    snprintf(trackp,  sizeof trackp,  "%s/TRACKS/STREAML4RH.BUN", dataroot);
    snprintf(carp,    sizeof carp,    "%s/CARS/HUMMER/GEOMETRY.BIN", dataroot);
    snprintf(cartexp, sizeof cartexp, "%s/CARS/HUMMER/TEXTURES.BIN", dataroot);
    snprintf(pathp,   sizeof pathp,   "%s/TRACKS/ROUTESL4RF/Paths4602.bin", dataroot);

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

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
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

    GpuMesh *gm = upload_scene(&scene);

    /* load a car and drop it on the track (36-byte vertex format) */
    long clen; unsigned char *cdata = n2_read_file(carp, &clen);
    long ctlen; unsigned char *ctdata = n2_read_file(cartexp, &ctlen);
    N2Tex cartex; int have_cartex = ctdata && n2_load_car_texture(ctdata, ctlen, &cartex);
    N2Scene car; int ncar = 0; GpuMesh *cgm = NULL;
    float spawn[3] = { cx, cy, cz }, heading0 = 0.0f;
    if (cdata) {
        ncar = n2_load_car(cdata, clen, &car);
        cgm = upload_scene(&car);
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

    /* Racing line: a closed-loop CIRCUIT within the rendered region, so laps
       are real (Paths4006 etc. are open sprints; Paths4602 is a lap loop). */
    long plen; unsigned char *pdata = n2_read_file(pathp, &plen);
    N2Path aipath = {0};
    AiCar ais[N_AI]; int nai = 0, start_idx = 0;
    static const float AICOL[N_AI][3] = {
        {0.15f,0.4f,0.95f}, {0.2f,0.8f,0.35f}, {0.95f,0.8f,0.15f}, {0.85f,0.2f,0.8f} };
    if (pdata && n2_load_path(pdata, plen, &aipath) > 4) {
        /* start/finish = waypoint 0; grid the cars around the loop from there */
        start_idx = 0;
        spawn[0]=aipath.xy[0]; spawn[1]=aipath.xy[1];
        spawn[2]=n2_ground_z(&scene, spawn[0], spawn[1], spawn[2]);
        int nx = 1 % aipath.n;
        heading0 = atan2f(aipath.xy[nx*2+1]-spawn[1], aipath.xy[nx*2]-spawn[0]);
        nai = N_AI;
        for (int k = 0; k < N_AI; k++) {
            int t = (start_idx + 2 + k*2) % aipath.n;
            ais[k].t = t; ais[k].lap = 0; ais[k].prevrel = t;
            ais[k].pos[0]=aipath.xy[t*2]; ais[k].pos[1]=aipath.xy[t*2+1];
            ais[k].pos[2]=spawn[2]; ais[k].head=heading0;
            ais[k].spd = 3.0f + k*0.12f;
            memcpy(ais[k].col, AICOL[k], sizeof AICOL[k]);
        }
        printf("circuit: %d-waypoint loop; %d AI racers, lap system on\n",
               aipath.n, nai);
    }

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
    float heading = heading0, speed = 0.0f, cam[3] = { spawn[0], spawn[1], spawn[2]+5 };
    int p_lap = 0, p_prev = 0;   /* player lap + previous loop-progress */
    const float ACCEL=0.3f, MAXSPD=4.5f, FRICTION=0.95f, TURN=0.04f;
    /* race flow: 0 = countdown, 1 = racing, 2 = finished */
    const int COUNTDOWN = 180, LAP_TARGET = 2;
    int race_state = shot ? 1 : 0, racetimer = 0, finish_place = 0;
    int running = 1, shotframe = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }
        /* W/S throttle, A/D steer (only once racing) */
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (race_state == 1) {
            if (ks[SDL_SCANCODE_W] || shot) speed += ACCEL;   /* shot mode auto-drives */
            if (ks[SDL_SCANCODE_S]) speed -= ACCEL;
        }
        speed *= FRICTION;
        if (speed >  MAXSPD) speed =  MAXSPD;
        if (speed < -MAXSPD*0.4f) speed = -MAXSPD*0.4f;
        float steer = (ks[SDL_SCANCODE_A]?1.f:0.f) - (ks[SDL_SCANCODE_D]?1.f:0.f);
        heading += steer * TURN * (speed / MAXSPD);
        if (shot && nai > 0) {   /* screenshot autopilot: chase the nearest AI */
            float *ap = ais[0].pos;
            float da = atan2f(ap[1]-carpos[1], ap[0]-carpos[0]) - heading;
            while (da> 3.14159f) da-=6.28318f;
            while (da<-3.14159f) da+=6.28318f;
            if (da> 0.05f) da= 0.05f; if (da<-0.05f) da=-0.05f;
            heading += da;
        }
        float fwd[3] = { cosf(heading), sinf(heading), 0 };
        carpos[0] += fwd[0]*speed; carpos[1] += fwd[1]*speed;
        /* sit on the road/terrain surface (smoothed to avoid jitter) */
        float gz = n2_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
        carpos[2] += (gz - carpos[2]) * 0.35f;

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

        /* chase camera: trail behind + above, look at the car */
        float want[3] = { carpos[0]-fwd[0]*10, carpos[1]-fwd[1]*10, carpos[2]+4.5f };
        for (int c=0;c<3;c++) cam[c] += (want[c]-cam[c])*0.22f; /* smooth follow */
        float look[3] = { carpos[0]-cam[0], carpos[1]-cam[1], (carpos[2]+1.5f)-cam[2] };

        int W, H; SDL_GL_GetDrawableSize(win, &W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.02f, 0.03f, 0.05f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float P[16], V[16], MVP[16];
        mat_persp(0.9f, (float)W/H, maxr*0.01f, maxr*30, P);
        mat_lookat(cam, look, V);
        mat_mul(P, V, MVP);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        glUniform1f(uUnlit, 0.0f);

        /* track: grass is textured; roads are flat asphalt (real per-tile
           road textures live in an as-yet-undecoded global pack). */
        for (int i = 0; i < nm; i++) {
            if (gm[i].cat == N2_TERRAIN && texTerr) {
                glUniform1f(uUseTex, 1.0f);
                glBindTexture(GL_TEXTURE_2D, texTerr);
            } else {
                glUniform1f(uUseTex, 0.0f);
                glUniform3f(uColor, 0.28f, 0.29f, 0.31f); /* asphalt */
            }
            draw_gpumesh(&gm[i]);
        }
        /* car: solid-shaded, positioned + rotated to heading */
        if (ncar) {
            float T[16], Rz[16], Model[16], MVPc[16];
            mat_trans(carpos[0], carpos[1], carpos[2] + 0.43f, T);
            mat_rotz(heading, Rz);
            mat_mul(T, Rz, Model);
            mat_mul(MVP, Model, MVPc);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPc);
            if (texCar) {          /* player car: real body texture */
                glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, texCar);
            } else {
                glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, 0.85f, 0.12f, 0.12f);
            }
            for (int i = 0; i < ncar; i++) draw_gpumesh(&cgm[i]);
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

        /* HUD: race-position leaderboard — one colour bar per car, ordered by
           progress along the racing line (leader on top); player bar wider. */
        if (nai > 0) {
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
            glEnable(GL_DEPTH_TEST);
        }

        /* race banners: 3-2-1 countdown, then a finish banner + place pips */
        glDisable(GL_DEPTH_TEST);
        glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
        if (race_state == 0) {
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
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
