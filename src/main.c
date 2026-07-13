/* main.c — OpenUG: an open reimplementation of Need for Speed: Underground 2.
 * Loads the original game data directly (no Wine/box64), assembles a track
 * section, decodes its textures, and drives a textured car with AI opponents
 * around a circuit — SDL2 + OpenGL, portable across x86 and ARM.
 *
 * main.c is the orchestrator: setup, the game loop, input, race flow and HUD.
 * The engine proper lives in the modules it drives:
 *   nfsu2.h    — chunk parser (ground truth — do not "optimize")
 *   render.*   — Renderer: GL objects, shaders, matrices, font, screenshot
 *   physics.*  — car kinematics, wall + car-to-car collision
 *   ai.*       — racing-line opponents, circuit loading
 *   audio.*    — procedural engine/road/skid synth
 *   resource.* — file mapping + track/car/circuit discovery
 *
 * Desktop (x86/ARM, Linux/macOS/Windows): legacy GL 2.1 + GLSL 120.
 * Embedded / mobile (ARM): OpenGL ES 2.0 + GLSL 100 — build with -DN2_GLES
 * (links -lGLESv2). The shaders are written to compile on both.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>   /* execvp: menu track-switch re-launches the process */
#include <SDL.h>

#include "nfsu2.h"
#include "render.h"
#include "physics.h"
#include "ai.h"
#include "audio.h"
#include "resource.h"
#include "world.h"
#include "debug.h"

/* debug tunables — defaults match the previously hard-coded constants, so a
 * normal build behaves exactly as before; `make debug` adds an ImGui panel. */
DbgState g_dbg = {
    .freecam = 0, .speed = 0.6f,
    .wheel_frontf = 0.62f, .wheel_rearf = 0.58f, .wheel_trackf = 0.85f,
    .wheel_z = -0.05f, .wheel_scale = 0.9f,
    .ambient = 0.38f, .diffuse = 0.62f, .body_spec = 0.18f,
    .paint_override = 0, .paint = { 0.68f, 0.09f, 0.08f },
    .show_body = 1, .show_glass = 1, .show_lights = 1, .show_tires = 1,
    .show_misc = 1, .show_track = 1,
};

int main(int argc, char **argv) {
    collide_walls_selftest();
    phys_selftest();
    /* Point at your own NFSU2 install/data directory (contains TRACKS/, CARS/).
       Usage: nfsu2 [DATA_DIR] [options]
         --car NAME       car folder under CARS/ (default HUMMER)
         --track NAME     STREAM .BUN under TRACKS/, or ALL = whole city (default)
         --circuit PATH   circuit Paths .bin under TRACKS/ (default ROUTESL4RF/Paths4602.bin)
         --shot out.png   render one frame and exit */
    const char *selfexe = argv[0];   /* for the menu's track-switch re-exec */
    const char *dataroot = ".", *shot = NULL;
    const char *carname = "HUMMER", *trackname = "ALL";
    const char *circuit = "ROUTESL4RF/Paths4602.bin"; int explicit_circuit = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--shot")    && i+1 < argc) shot      = argv[++i];
        else if (!strcmp(argv[i], "--car")     && i+1 < argc) carname   = argv[++i];
        else if (!strcmp(argv[i], "--track")   && i+1 < argc) trackname = argv[++i];
        else if (!strcmp(argv[i], "--circuit") && i+1 < argc) { circuit = argv[++i]; explicit_circuit = 1; }
        else dataroot = argv[i];
    }
    char carp[1024], cartexp[1024], pathp[1024], troot[1024];
    snprintf(troot,   sizeof troot,   "%s/TRACKS", dataroot);
    snprintf(carp,    sizeof carp,    "%s/CARS/%s/GEOMETRY.BIN", dataroot, carname);
    snprintf(cartexp, sizeof cartexp, "%s/CARS/%s/TEXTURES.BIN", dataroot, carname);
    snprintf(pathp,   sizeof pathp,   "%s/TRACKS/%s", dataroot, circuit);

    static World world;
    int nm = world_load(&world, troot, trackname);
    if (!nm) { fprintf(stderr, "no track data\n  (pass your NFSU2 data dir: nfsu2 /path/to/data)\n"); return 1; }
    N2Scene scene = world.scene;   /* shares world.scene.meshes */
    printf("loaded %d submeshes from %d region(s)\n", nm, world.nreg);
    printf("terrain texture TRN_GRASSC: %s\n", world.have_grass ? "ok" : "-");
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

    /* building collision footprints — the car is kept out of these */
    #define MAXOBST 32768   /* whole city worth of building footprints */
    static float obst[MAXOBST][4];
    int nobst = phys_collect_walls(&scene, obst, MAXOBST);
    printf("collision obstacles: %d buildings\n", nobst);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }

    /* start the procedural engine synth (optional — continue silently on fail) */
    SDL_AudioDeviceID adev = audio_init();
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
    SDL_GL_SetSwapInterval(shot ? 0 : 1);   /* raw frame times in shot mode */
#ifdef DEBUG_UI
    dbgui_init(win, ctx);
#endif

    RProg rp = render_program();
    GLint uMVP = rp.uMVP, uUseTex = rp.uUseTex, uColor = rp.uColor,
          uUnlit = rp.uUnlit, uAlpha = rp.uAlpha, uSoft = rp.uSoft,
          uSpec = rp.uSpec, uAmbient = rp.uAmbient, uDiffuse = rp.uDiffuse,
          uLight = rp.uLight;

    GpuMesh *gm = upload_scene(&scene);

    /* Per-mesh track textures: each mesh's diffuse key (0x134012) resolves to a
       texture in the region's own STREAM TPK (grass/road/props) or the shared
       LOC4DYNTEX pack (sky/facades). Decode each distinct key once, reject any
       that decode to noise (wrong format / swizzled surface), map key->GL tex.
       Generalises the old hard-coded TRN_GRASSC/RDP_PARKING lookup across
       regions (L4RR uses ORG_GRASS_001 etc.). */
    static uint32_t tmapkey[2048]; static GLuint tmaptex[2048];
    int ntmap = world_bind_textures(&world, tmapkey, tmaptex, 2048);
    printf("track textures bound: %d distinct\n", ntmap);
    /* resolve each mesh's texture once — the per-frame key scan was fine for
       one region, not for a whole city of meshes */
    GLuint *mtex = (GLuint *)calloc(nm, sizeof *mtex);
    for (int i = 0; i < nm; i++)
        for (int j = 0; j < ntmap; j++)
            if (tmapkey[j] == scene.meshes[i].texkey) { mtex[i] = tmaptex[j]; break; }
    /* draw order grouped by texture: one GL bind per texture run, not per mesh */
    int *order = (int *)malloc(nm * sizeof *order);
    { int k = 0;
      for (int j = 0; j <= ntmap; j++) {              /* j==ntmap: untextured */
        GLuint want = j < ntmap ? tmaptex[j] : 0;
        for (int i = 0; i < nm; i++) if (mtex[i] == want) order[k++] = i;
      } }

    /* load a car and drop it on the track (36-byte vertex format) */
    long clen; unsigned char *cdata = n2_read_file(carp, &clen);
    long ctlen; unsigned char *ctdata = n2_read_file(cartexp, &ctlen);
    /* per-mesh car textures: get the TPK keys, then decode each key referenced
       by a mesh into its own GL texture (body, wheel, brake, ... bound by UVs). */
    uint32_t ckeys[64]; int nck = ctdata ? n2_car_tex_keys(ctdata, ctlen, ckeys, 64) : 0;
    uint32_t mapkey[32]; GLuint maptex[32]; int nmap = 0;
    N2Scene car; int ncar = 0; GpuMesh *cgm = NULL;
    float wheelT[4][16];                         /* 4 wheel placements (car-local) */
    GpuMesh wheelmesh; int have_wheel = 0;       /* procedural tyre, built after GL init */
    for (int k=0;k<4;k++){ float I[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; memcpy(wheelT[k],I,sizeof(I)); }
    float carbb[6] = {0,0,0,0,0,0};              /* body AABB min/max, for wheel placement */
    float spawn[3] = { cx, cy, cz }, heading0 = 0.0f;
    if (cdata) {
        ncar = n2_load_car(cdata, clen, &car, ckeys, nck);
        /* Wheels are modelled once at the origin (the SolidObject transform is
           identity for every part — verified), so a lone wheel mesh renders
           buried in the body. Place it at 4 arch positions derived from the body
           AABB. ponytail: fractions, since no explicit wheel data exists in
           GEOMETRY.BIN / PARTS_ANIMATIONS.bin — tune here if a car sits wrong. */
        float bb0[3]={1e30f,1e30f,1e30f}, bb1[3]={-1e30f,-1e30f,-1e30f};
        float tb0[3]={1e30f,1e30f,1e30f}, tb1[3]={-1e30f,-1e30f,-1e30f}; int ntv=0;
        for (int i=0;i<ncar;i++) {
            int tire = car.meshes[i].cat==N2_CAR_TIRE;
            for (int v=0; v<car.meshes[i].nverts; v++) {
                float *p=car.meshes[i].verts+v*5;
                if (tire) { ntv++; for(int a=0;a<3;a++){ if(p[a]<tb0[a])tb0[a]=p[a]; if(p[a]>tb1[a])tb1[a]=p[a]; } }
                else for (int a=0;a<3;a++){ if(p[a]<bb0[a])bb0[a]=p[a]; if(p[a]>bb1[a])bb1[a]=p[a]; }
            }
        }
        carbb[0]=bb0[0];carbb[1]=bb0[1];carbb[2]=bb0[2];
        carbb[3]=bb1[0];carbb[4]=bb1[1];carbb[5]=bb1[2];  /* wheelT built per-frame from g_dbg */
        cgm = upload_scene(&car);
        /* size the procedural tyre from the car's own wheel mesh (radius in X-Z,
           width in Y) so a Hummer gets big tyres and a compact gets small ones. */
        float wR = 0.33f, wHW = 0.11f;
        if (ntv) { wR = 0.25f*((tb1[0]-tb0[0])+(tb1[2]-tb0[2]));   /* mean disc radius */
                   wHW = 0.5f*(tb1[1]-tb0[1]); if (wHW<0.04f) wHW=0.04f; }
        wheelmesh = make_wheel(wR, wHW); have_wheel = 1;
        /* decode + upload each distinct texture actually bound by a mesh */
        for (int i = 0; i < ncar; i++) {
            uint32_t tk = car.meshes[i].texkey; if (!tk) continue;
            int seen = 0; for (int j = 0; j < nmap; j++) if (mapkey[j]==tk) seen = 1;
            if (seen || nmap >= 32) continue;
            N2Tex ct;
            if (n2_load_car_tex_by_key(ctdata, ctlen, tk, &ct)) {
                mapkey[nmap] = tk; maptex[nmap] = upload_tex(&ct); nmap++;
                /* car textures are atlases (UVs in [0,1]): clamp so REPEAT
                   wrap + mip filtering can't bleed the opposite border in. */
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

    /* Enumerate the selectable assets for the pre-race menu. Switching car or
       track means a whole new load, so those re-launch the process (below);
       circuit is a cheap in-place reload. */
    #define MAXTRACK WORLD_MAXREG
    #define MAXCARS  64
    #define MAXCIRC  24
    char tracklist[MAXTRACK][64]; int seltrack = 0;
    int ntrack = res_list_tracks(troot, tracklist, MAXTRACK, trackname, &seltrack);
    if (ntrack < MAXTRACK) {   /* city mode is selectable from the menu too */
        snprintf(tracklist[ntrack], sizeof tracklist[0], "ALL");
        if (!strcmp(trackname, "ALL")) seltrack = ntrack;
        ntrack++;
    }
    char carlist[MAXCARS][64]; int selcar = 0;
    int ncars = res_list_cars(dataroot, carlist, MAXCARS, carname, &selcar);
    char circlist[MAXCIRC][256]; int selcirc = 0;
    int ncirc = res_list_circuits(troot, circlist, MAXCIRC, mn, mx);
    if (explicit_circuit)  /* honor an explicit --circuit if it's valid on this track */
        for (int i=0;i<ncirc;i++) if(!strcmp(circlist[i], circuit)) selcirc=i;
    printf("circuits available: %d (menu: Left/Right)\n", ncirc);

    N2Path aipath = {0};
    AiCar ais[N_AI]; int start_idx = 0;
    int nai = ncirc ? load_circuit(dataroot, circlist[selcirc], &scene, &aipath,
                                   ais, spawn, &heading0, &start_idx, densx, densy) : 0;
    if (nai) printf("circuit: %d-waypoint loop; %d AI racers, lap system on\n",
                    aipath.n, nai);

    GLuint texTerr = world.have_grass ? upload_tex(&world.grass) : 0;

    /* unit-quad for the 2D HUD (drawn in NDC via uMVP) */
    GpuMesh quad = make_quad();

    glEnable(GL_DEPTH_TEST);
    /* Per-car paint colour. NFSU2 paints the body a solid colour and overlays
       badges as decals — the car's body "texture" is just that badge atlas, so
       painting the panels with it looks wrong. The files carry no chosen paint,
       so derive a stable, vivid hue from the car name (FNV hash -> HSV). */
    float paint[3];
    {
        uint32_t ph = 0x811c9dc5u;
        for (const char *s = carname; *s; s++) ph = (ph ^ (unsigned char)*s) * 16777619u;
        float h = (ph % 360) / 60.0f, x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f), r=0,g=0,b=0;
        if      (h < 1) { r=1; g=x; } else if (h < 2) { r=x; g=1; }
        else if (h < 3) { g=1; b=x; } else if (h < 4) { g=x; b=1; }
        else if (h < 5) { r=x; b=1; } else            { r=1; b=x; }
        paint[0]=0.12f+r*0.65f; paint[1]=0.12f+g*0.65f; paint[2]=0.12f+b*0.65f;
    }
    float carpos[3] = { spawn[0], spawn[1], spawn[2] };
    float heading = heading0, speed = 0.0f, vel[2] = {0,0};
    float cam[3] = { spawn[0], spawn[1], spawn[2]+5 };
    float fc[3] = { spawn[0]-6, spawn[1], spawn[2]+3 };   /* freecam eye */
    float fyaw = 0.0f, fpitch = -0.25f;                   /* freecam look angles */
    int   mlook = 0;                                      /* right-drag mouse look active */
    int p_lap = 0, p_prev = 0;   /* player lap + previous loop-progress */
    /* race flow: 3 = pre-race menu, 0 = countdown, 1 = racing, 2 = finished */
    const int COUNTDOWN = 180, LAP_TARGET = 2;
    int race_state = shot ? 1 : 3, racetimer = 0, finish_place = 0;
    float menuspin = 0.0f;   /* orbit-camera angle on the menu screen */
    int running = 1, shotframe = 0;
    uint32_t t0 = SDL_GetTicks();   /* --shot prints avg ms/frame at exit */
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
#ifdef DEBUG_UI
            dbgui_event(&e);
            if ((e.type==SDL_KEYDOWN||e.type==SDL_KEYUP) && dbgui_want_keyboard()) continue;
#endif
            if (e.type == SDL_QUIT) running = 0;
            /* freecam mouse-look: hold right button to rotate (keeps the cursor
               free for the ImGui panel the rest of the time). */
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && g_dbg.freecam) {
                SDL_SetRelativeMouseMode(SDL_TRUE); mlook = 1;
            }
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                SDL_SetRelativeMouseMode(SDL_FALSE); mlook = 0;
            }
            else if (e.type == SDL_MOUSEMOTION && mlook) {
                fyaw   -= e.motion.xrel * 0.005f;
                fpitch -= e.motion.yrel * 0.005f;
                if (fpitch> 1.5f) fpitch= 1.5f; if (fpitch<-1.5f) fpitch=-1.5f;
            }
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_f) {       /* toggle freecam; seed it from the current view */
                    g_dbg.freecam ^= 1;
                    if (g_dbg.freecam) {
                        fc[0]=cam[0]; fc[1]=cam[1]; fc[2]=cam[2];
                        fyaw = atan2f(carpos[1]-cam[1], carpos[0]-cam[0]);
                        fpitch = -0.2f;
                    }
                }
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
                            carpos[2]=world_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
                            int nx=(start_idx+1)%aipath.n;
                            heading=atan2f(aipath.xy[nx*2+1]-carpos[1], aipath.xy[nx*2]-carpos[0]);
                            vel[0]=vel[1]=0; speed=0;
                        }
                        race_state = 0; racetimer = 0;   /* -> 3-2-1 countdown */
                    }
                }
            }
        }
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (g_dbg.freecam) {                 /* fly the camera; WASD move, arrows look */
            float sp = g_dbg.speed * (ks[SDL_SCANCODE_LSHIFT]?4.0f:1.0f);
            float cp=cosf(fpitch);
            float dir[3]={cp*cosf(fyaw), cp*sinf(fyaw), sinf(fpitch)};
            float rt[3]={sinf(fyaw), -cosf(fyaw), 0};   /* strafe axis */
            if (ks[SDL_SCANCODE_W]){ fc[0]+=dir[0]*sp; fc[1]+=dir[1]*sp; fc[2]+=dir[2]*sp; }
            if (ks[SDL_SCANCODE_S]){ fc[0]-=dir[0]*sp; fc[1]-=dir[1]*sp; fc[2]-=dir[2]*sp; }
            if (ks[SDL_SCANCODE_D]){ fc[0]+=rt[0]*sp;  fc[1]+=rt[1]*sp; }
            if (ks[SDL_SCANCODE_A]){ fc[0]-=rt[0]*sp;  fc[1]-=rt[1]*sp; }
            if (ks[SDL_SCANCODE_E]||ks[SDL_SCANCODE_SPACE]) fc[2]+=sp;
            if (ks[SDL_SCANCODE_Q]||ks[SDL_SCANCODE_LCTRL]) fc[2]-=sp;
            if (ks[SDL_SCANCODE_LEFT])  fyaw   += 0.03f;
            if (ks[SDL_SCANCODE_RIGHT]) fyaw   -= 0.03f;
            if (ks[SDL_SCANCODE_UP])    fpitch += 0.02f;
            if (ks[SDL_SCANCODE_DOWN])  fpitch -= 0.02f;
            if (fpitch> 1.5f) fpitch= 1.5f; if (fpitch<-1.5f) fpitch=-1.5f;
        }
        /* driving inputs -> Physics module (velocity-vector arcade kinematics:
           throttle along the heading, steering rotates it, tyres scrub the
           sideways component so hard cornering at speed slides/drifts). */
        float throttle = 0.0f;
        if (!g_dbg.freecam && race_state == 1) {
            if      (ks[SDL_SCANCODE_W] || shot) throttle =  1.0f;
            else if (ks[SDL_SCANCODE_S])         throttle = -1.0f;
        }
        float steer = g_dbg.freecam ? 0.f
                    : (ks[SDL_SCANCODE_A]?1.f:0.f) - (ks[SDL_SCANCODE_D]?1.f:0.f);
        int handbrake = (race_state==1 && ks[SDL_SCANCODE_SPACE]);
        if (shot && nai > 0) {   /* screenshot autopilot: steer toward nearest AI */
            float da = atan2f(ais[0].pos[1]-carpos[1], ais[0].pos[0]-carpos[0]) - heading;
            while (da> 3.14159f) da-=6.28318f;
            while (da<-3.14159f) da+=6.28318f;
            if (da> 0.06f) da= 0.06f; if (da<-0.06f) da=-0.06f;
            heading += da;
        }
        float dmag = phys_car_step(carpos, vel, &heading, &speed,
                                   throttle, steer, handbrake);
        float nf[2] = { cosf(heading), sinf(heading) }, nr[2] = { nf[1], -nf[0] };
        /* engine note follows speed (idle rev during the countdown) */
        { float sp = (speed < 0 ? -speed : speed) / PHYS_MAXSPD;
          g_eng_freq = 55.0f + sp*185.0f;
          g_eng_vol  = (race_state==0 ? 0.16f : 0.16f + sp*0.5f);
          g_road_vol = sp*sp*0.35f; }   /* tyre/wind roar rises with speed */
        float fwd[3] = { nf[0], nf[1], 0 };
        /* building collision: push the car out of any wall footprint it entered.*/
        if (race_state == 1 && collide_walls(carpos, vel, obst, nobst, 1.3f)) g_hit = 0.5f;
        /* sit on the road/terrain surface (smoothed to avoid jitter) */
        float gz = world_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
        carpos[2] += (gz - carpos[2]) * 0.35f;

        /* drifting? extend the tyre ribbons + puff smoke at the rear wheels */
        int drifting = (dmag > PHYS_MAXSPD*0.2f && speed > PHYS_MAXSPD*0.22f);
        /* tyre screech scales with how hard the back is sliding */
        g_skid = (race_state==1 && drifting)
               ? (0.12f + 0.45f*(dmag - PHYS_MAXSPD*0.2f)/PHYS_MAXSPD) : 0.0f;
        if (g_skid > 0.35f) g_skid = 0.35f;
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
        for (int k = 0; race_state == 1 && k < nai; k++)
            ai_step(&ais[k], k, &aipath, &scene, start_idx,
                    p_lap*aipath.n + p_prev);
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

        /* car-to-car collision: push overlapping cars apart (circle test) */
        { float thud = phys_car_contacts(carpos, vel, speed, ais, nai);
          if (thud > g_hit) g_hit = thud; }

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
        if (g_dbg.freecam) {                 /* fly-through overrides the chase/orbit cam */
            cam[0]=fc[0]; cam[1]=fc[1]; cam[2]=fc[2];
            float cp=cosf(fpitch);
            look[0]=cp*cosf(fyaw); look[1]=cp*sinf(fyaw); look[2]=sinf(fpitch);
        }

        int W, H; SDL_GL_GetDrawableSize(win, &W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.06f, 0.07f, 0.11f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float P[16], V[16], MVP[16];
        /* menu orbits ~9m from the car, so use a close near-plane there; the
           driving near scales with region size but is clamped so a whole-city
           maxr can't push it past the car and clip it. */
        float znear = race_state==3 ? 0.2f : (maxr*0.01f < 1.0f ? maxr*0.01f : 1.0f);
        mat_persp(0.9f, (float)W/H, znear, maxr*30, P);
        mat_lookat(cam, look, V);
        mat_mul(P, V, MVP);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        glUniform1f(uUnlit, 0.0f); glUniform1f(uSpec, 0.0f);   /* world is matte */
        glUniform1f(uAmbient, g_dbg.ambient); glUniform1f(uDiffuse, g_dbg.diffuse);
        glUniform3f(uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);   /* track = world space */

        /* track: each mesh wears its own bound texture (grass/road/props/sky);
           terrain without one falls back to the grass tex, everything else to
           flat asphalt (e.g. a region's road texture that didn't decode). */
        /* cull: skip meshes beyond the view range or too small on screen.
           ponytail: XY distance + size only, no frustum test — add one if the
           draw count is still the bottleneck. */
        #define VIEW_DIST 700.0f
        int ndrawn = 0;
        GLuint lasttex = (GLuint)-1;
        for (int k = 0; g_dbg.show_track && k < nm; k++) {
            int i = order[k];
            float *b = world.mbb[i];
            float dx = cam[0] < b[0] ? b[0]-cam[0] : (cam[0] > b[2] ? cam[0]-b[2] : 0);
            float dy = cam[1] < b[1] ? b[1]-cam[1] : (cam[1] > b[3] ? cam[1]-b[3] : 0);
            float d2 = dx*dx + dy*dy;
            if (d2 > VIEW_DIST*VIEW_DIST) continue;
            float rw = b[2]-b[0], rh = b[3]-b[1], r2 = 0.25f*(rw*rw + rh*rh);
            if (r2 < d2 * 6.0e-5f) continue;   /* under ~0.8% of view: invisible */
            ndrawn++;
            GLuint tex = mtex[i];
            if (!tex && gm[i].cat == N2_TERRAIN) tex = texTerr;
            if (tex != lasttex) {   /* texture-sorted order: binds are rare */
                if (tex) { glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, tex); }
                else { glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, 0.28f, 0.29f, 0.31f); }
                lasttex = tex;
            }
            draw_gpumesh(&gm[i]);
        }

        /* car shadows: a soft dark blob on the ground under each car, so they
           sit on the road instead of floating (darkens, so it reads on any
           surface unlike the additive glows). */
        if (ncar) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit,1.0f); glUniform1f(uUseTex,0.0f); glUniform1f(uSoft,1.0f);
            glUniform3f(uColor, 0.0f, 0.0f, 0.0f); glUniform1f(uAlpha, 0.5f);
            for (int c=0; c<=nai; c++) {
                float *cp = c==0 ? carpos : ais[c-1].pos;
                float gz2 = world_ground_z(&scene, cp[0], cp[1], cp[2]) + 0.03f, sz=3.2f;
                float M[16]={sz,0,0,0, 0,sz,0,0, 0,0,1,0, cp[0]-sz*0.5f, cp[1]-sz*0.5f, gz2, 1};
                float MV[16]; mat_mul(MVP,M,MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad);
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f);
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* headlights: warm soft light-pools cast on the road ahead (it's night),
           three overlapping ground glows that widen + fade with distance to read
           as a headlight cone. Additive blend. */
        if (race_state != 3) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit,1.0f); glUniform1f(uUseTex,0.0f); glUniform1f(uSoft,1.0f);
            glUniform3f(uColor, 0.60f, 0.56f, 0.42f);
            float fx=cosf(heading), fy=sinf(heading);
            for (int s=0;s<3;s++){
                float dist=3.0f+s*3.5f, size=3.0f+s*2.4f;
                float px=carpos[0]+fx*dist, py=carpos[1]+fy*dist;
                float pz=world_ground_z(&scene,px,py,carpos[2])+0.06f;
                float M[16]={size,0,0,0, 0,size,0,0, 0,0,1,0, px-size*0.5f, py-size*0.5f, pz, 1};
                float MV[16]; mat_mul(MVP, M, MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV);
                glUniform1f(uAlpha, 0.5f - s*0.13f);
                draw_gpumesh(&quad);
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f);
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);
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
                float len2=sqrtf(dx*dx+dy*dy); if(len2<1e-4f) continue;
                glUniform1f(uAlpha, 0.4f*s[6]);              /* fade with age */
                float ux=dx, uy=dy;                          /* along-travel axis (u) */
                const float SW=0.45f; float px=-dy/len2*SW, py=dx/len2*SW;  /* width axis (v) */
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
            /* normals stay model-space, so counter-rotate the sun into car
               space — otherwise the lit side turns with the car's heading. */
            { float ch=cosf(heading), sh=sinf(heading);
              glUniform3f(uLight, ch*N2_SUN_X + sh*N2_SUN_Y,
                                 -sh*N2_SUN_X + ch*N2_SUN_Y, N2_SUN_Z); }
            /* rebuild the 4 wheel placements from the (live-tunable) fractions */
            { float fr=carbb[3]*g_dbg.wheel_frontf, rr=carbb[0]*g_dbg.wheel_rearf,
                    tr=carbb[4]*g_dbg.wheel_trackf, s=g_dbg.wheel_scale, wz=g_dbg.wheel_z;
              float wp[4][2]={{fr,tr},{fr,-tr},{rr,tr},{rr,-tr}};
              for(int k=0;k<4;k++){ float sy=(k&1)?-s:s;
                  float M[16]={s,0,0,0, 0,sy,0,0, 0,0,s,0, wp[k][0],wp[k][1],wz,1};
                  memcpy(wheelT[k],M,sizeof(M)); } }
            float *pnt = g_dbg.paint_override ? g_dbg.paint : paint;
            /* per-mesh: each part wears its own bound texture (body/wheel/...);
               parts with no in-TPK texture get a sensible flat colour by class. */
            for (int i = 0; i < ncar; i++) {
                int c = cgm[i].cat;
                if ((c==N2_CAR_BODY  && !g_dbg.show_body)  || (c==N2_CAR_GLASS && !g_dbg.show_glass) ||
                    (c==N2_CAR_LIGHT && !g_dbg.show_lights)|| (c==N2_CAR_TIRE  && !g_dbg.show_tires) ||
                    (c==N2_CAR_MISC  && !g_dbg.show_misc)) continue;
                glUniform1f(uSpec, c==N2_CAR_GLASS?0.6f : (c==N2_CAR_BODY||c==N2_CAR_MISC)?g_dbg.body_spec
                                 : c==N2_CAR_LIGHT?0.3f : 0.0f);
                GLuint tex = 0;
                for (int j = 0; j < nmap; j++) if (mapkey[j]==cgm[i].texkey) { tex = maptex[j]; break; }
                if (c == N2_CAR_BODY || c == N2_CAR_MISC) {
                    /* solid paint (the body's own "texture" is only a badge sheet) */
                    glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, pnt[0], pnt[1], pnt[2]);
                } else if (tex) {
                    glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, tex);  /* wheel/brake */
                } else {
                    glUniform1f(uUseTex, 0.0f);
                    if      (c == N2_CAR_GLASS) glUniform3f(uColor, 0.08f, 0.10f, 0.14f);
                    else if (c == N2_CAR_LIGHT) glUniform3f(uColor, 0.95f, 0.85f, 0.55f);
                    else if (c == N2_CAR_TIRE)  glUniform3f(uColor, 0.05f, 0.05f, 0.06f);
                    else                        glUniform3f(uColor, pnt[0], pnt[1], pnt[2]);
                }
                if (c != N2_CAR_TIRE) draw_gpumesh(&cgm[i]);   /* tyres = procedural, below */
            }
            /* procedural tyres at the 4 arches (the game rims render as urchins) */
            if (have_wheel) {
                glUniform1f(uUseTex, 0.0f); glUniform1f(uSpec, 0.05f);
                glUniform3f(uColor, 0.05f, 0.05f, 0.06f);
                for (int k=0;k<4;k++){ float MVPw[16]; mat_mul(MVPc, wheelT[k], MVPw);
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPw); draw_gpumesh(&wheelmesh); }
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPc);
            }
            glUniform1f(uUseTex, 0.0f);
            glUniform1f(uSpec, 0.3f);     /* AIs: flat colour but glossy paint */
            /* AI opponents — same body, each in its own colour */
            for (int k = 0; k < nai; k++) {
                mat_trans(ais[k].pos[0], ais[k].pos[1], ais[k].pos[2] + 0.43f, T);
                mat_rotz(ais[k].head, Rz);
                mat_mul(T, Rz, Model);
                mat_mul(MVP, Model, MVPc);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPc);
                { float ch=cosf(ais[k].head), sh=sinf(ais[k].head);
                  glUniform3f(uLight, ch*N2_SUN_X + sh*N2_SUN_Y,
                                     -sh*N2_SUN_X + ch*N2_SUN_Y, N2_SUN_Z); }
                glUniform3f(uColor, ais[k].col[0], ais[k].col[1], ais[k].col[2]);
                for (int i = 0; i < ncar; i++)
                    if (cgm[i].cat != N2_CAR_TIRE) draw_gpumesh(&cgm[i]);
                if (have_wheel) {                         /* procedural tyres */
                    glUniform3f(uColor, 0.05f, 0.05f, 0.06f);
                    for (int w=0;w<4;w++){ float MVPw[16]; mat_mul(MVPc, wheelT[w], MVPw);
                        glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPw); draw_gpumesh(&wheelmesh); }
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPc);
                    glUniform3f(uColor, ais[k].col[0], ais[k].col[1], ais[k].col[2]);
                }
            }
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
            glUniform3f(uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);   /* back to world */
        }

        /* tail lights: red camera-facing glows at each car's rear (night) */
        if (race_state != 3 && ncar) {
            float lz=sqrtf(look[0]*look[0]+look[1]*look[1]+look[2]*look[2]); if(lz<1e-4f)lz=1;
            float ld[3]={look[0]/lz,look[1]/lz,look[2]/lz};
            float rt[3]={ld[1],-ld[0],0}; float rl=sqrtf(rt[0]*rt[0]+rt[1]*rt[1]); if(rl<1e-4f)rl=1;
            rt[0]/=rl; rt[1]/=rl;
            float up[3]={rt[1]*ld[2], -rt[0]*ld[2], rt[0]*ld[1]-rt[1]*ld[0]};
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE); glDisable(GL_DEPTH_TEST);   /* glow overlay on the rear */
            glUniform1f(uUnlit,1.0f); glUniform1f(uUseTex,0.0f); glUniform1f(uSoft,1.0f);
            glUniform3f(uColor, 1.0f, 0.12f, 0.06f); glUniform1f(uAlpha, 0.9f);
            for (int c=0; c<=nai; c++) {
                float *cp = c==0 ? carpos : ais[c-1].pos;
                float hd = c==0 ? heading : ais[c-1].head;
                float fx=cosf(hd), fy=sinf(hd), rx=-fy, ry=fx;
                for (int side=-1; side<=1; side+=2) {
                    float bx=cp[0]-fx*1.9f+rx*0.6f*side, by=cp[1]-fy*1.9f+ry*0.6f*side, bz=cp[2]+0.5f;
                    float s=1.1f;
                    float cxp=bx-(rt[0]+up[0])*s*0.5f, cyp=by-(rt[1]+up[1])*s*0.5f, czp=bz-(rt[2]+up[2])*s*0.5f;
                    float M[16]={rt[0]*s,rt[1]*s,rt[2]*s,0, up[0]*s,up[1]*s,up[2]*s,0, 0,0,1,0, cxp,cyp,czp,1};
                    float MV[16]; mat_mul(MVP,M,MV);
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad);
                }
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f);
            glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        }

        /* drift smoke: camera-facing billboards, light + fading, additive-ish */
        if (smoken > 0) {
            float lz = sqrtf(look[0]*look[0]+look[1]*look[1]+look[2]*look[2]); if(lz<1e-4f)lz=1;
            float ld[3]={look[0]/lz,look[1]/lz,look[2]/lz};
            float rt[3];                                   /* look x up(0,0,1) */
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
                float sf = speed / PHYS_MAXSPD; if (sf < 0) sf = -sf; if (sf > 1) sf = 1;
                float bg[16]={0.35f,0,0,0, 0,0.03f,0,0, 0,0,1,0, 0.58f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,bg);
                glUniform3f(uColor,0.15f,0.15f,0.18f); draw_gpumesh(&quad);
                float fg[16]={0.35f*sf,0,0,0, 0,0.03f,0,0, 0,0,1,0, 0.58f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,fg);
                glUniform3f(uColor,0.3f,0.85f,1.0f); draw_gpumesh(&quad);
            }
            {   /* text labels: position (top-right), lap (bottom-left), speed (kph) */
                char buf[32];
                int ppos = 1; for (int i=0;i<nc;i++) if (ord[i]==0) { ppos=i+1; break; }
                snprintf(buf,sizeof buf,"P%d/%d", ppos, nc);
                glUniform3f(uColor,1,1,1);
                draw_text(&quad, uMVP, buf, -text_width(buf,0.024f)/2, 0.96f, 0.024f, 0.036f);
                snprintf(buf,sizeof buf,"LAP %d/%d", p_lap<LAP_TARGET?p_lap+1:LAP_TARGET, LAP_TARGET);
                draw_text(&quad, uMVP, buf, -0.97f, -0.80f, 0.018f, 0.028f);
                int kph = (int)PHYS_KMH(speed<0?-speed:speed);
                snprintf(buf,sizeof buf,"%d", kph);
                glUniform3f(uColor,0.3f,0.85f,1.0f);
                draw_text(&quad, uMVP, buf, 0.58f, -0.80f, 0.02f, 0.03f);
            }
            if (aipath.n > 1) {   /* minimap (top-right): racing line + car dots */
                float bx0=0.66f, by0=0.52f, bw=0.30f, bh=0.36f;
                float px0=1e30f,py0=1e30f,px1=-1e30f,py1=-1e30f;
                for (int i=0;i<aipath.n;i++){ float x=aipath.xy[i*2],y=aipath.xy[i*2+1];
                    if(x<px0)px0=x; if(x>px1)px1=x; if(y<py0)py0=y; if(y>py1)py1=y; }
                float asp=(float)W/H, span=(px1-px0)>(py1-py0)?(px1-px0):(py1-py0);
                if (span < 1e-3f) span = 1e-3f;
                float scy = bh*0.9f/span, scx = scy/asp;   /* uniform on screen */
                float ccx=(px0+px1)*0.5f, cyw=(py0+py1)*0.5f;
                float mmx0 = bx0+bw*0.5f, mmy0 = by0+bh*0.5f;
                /* dark panel */
                float PM[16]={bw,0,0,0, 0,bh,0,0, 0,0,1,0, bx0,by0,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,PM); glUniform3f(uColor,0.04f,0.05f,0.08f); draw_gpumesh(&quad);
                /* draw a centred dot of half-size hs at world (wx,wy) */
                #define MMDOT(wx,wy,hs,r,g,b) do{ \
                    float mmx=mmx0+((wx)-ccx)*scx, mmy=mmy0+((wy)-cyw)*scy; \
                    float M[16]={(hs)*2/asp,0,0,0, 0,(hs)*2,0,0, 0,0,1,0, mmx-(hs)/asp,mmy-(hs),0,1}; \
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,M); glUniform3f(uColor,r,g,b); draw_gpumesh(&quad); }while(0)
                for (int i=0;i<aipath.n;i++) MMDOT(aipath.xy[i*2],aipath.xy[i*2+1], 0.004f, 0.4f,0.4f,0.46f);
                for (int k=0;k<nai;k++) MMDOT(ais[k].pos[0],ais[k].pos[1], 0.010f, ais[k].col[0],ais[k].col[1],ais[k].col[2]);
                MMDOT(carpos[0],carpos[1], 0.013f, 0.95f,0.15f,0.15f);
                #undef MMDOT
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
        } else if (race_state == 0) {         /* 3-2-1 / GO, big and centred */
            if (racetimer >= COUNTDOWN-24) {
                glUniform3f(uColor,0.2f,0.95f,0.3f);
                draw_text(&quad, uMVP, "GO", -text_width("GO",0.13f)/2, 0.44f, 0.13f, 0.17f);
            } else {
                char b[4]; snprintf(b,sizeof b,"%d", 3 - racetimer/60);
                glUniform3f(uColor,0.95f,0.25f,0.15f);
                draw_text(&quad, uMVP, b, -text_width(b,0.13f)/2, 0.44f, 0.13f, 0.17f);
            }
        } else if (race_state == 2) {         /* finished: FINISH + place */
            glUniform3f(uColor,0.95f,0.8f,0.2f);
            draw_text(&quad, uMVP, "FINISH", -text_width("FINISH",0.07f)/2, 0.36f, 0.07f, 0.10f);
            char b[16]; snprintf(b,sizeof b,"P%d/%d", finish_place, nai+1);
            glUniform3f(uColor,1,1,1);
            draw_text(&quad, uMVP, b, -text_width(b,0.05f)/2, 0.14f, 0.05f, 0.075f);
        }
        glEnable(GL_DEPTH_TEST);

#ifdef DEBUG_UI     /* debug readouts + ImGui overlay, drawn on top of everything */
        g_dbg.cam[0]=cam[0]; g_dbg.cam[1]=cam[1]; g_dbg.cam[2]=cam[2];
        g_dbg.car[0]=carpos[0]; g_dbg.car[1]=carpos[1]; g_dbg.car[2]=carpos[2];
        g_dbg.heading=heading; g_dbg.kmh=PHYS_KMH(speed);
        g_dbg.car_meshes=ncar; g_dbg.track_meshes=nm;
        dbgui_frame();
        dbgui_render();
#endif
        if (shot && ++shotframe >= 40) {
            unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
            for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
            write_png(shot, W, H, fl);
            free(px); free(fl);
            printf("frame avg: %.1f ms (vsync on), %d/%d meshes drawn\n",
                   (SDL_GetTicks()-t0)/(float)shotframe, ndrawn, nm);
            printf("wrote %s (%dx%d) after driving to (%.0f,%.0f)\n", shot, W, H, carpos[0], carpos[1]);
            running = 0;
        }
        SDL_GL_SwapWindow(win);
    }

#ifdef DEBUG_UI
    dbgui_shutdown();
#endif
    n2_free_scene(&scene);   /* region buffers already freed after texture upload */
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
