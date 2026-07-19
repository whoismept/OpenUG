/* OpenUG debug tunables — shared between the C engine (main.c) and the optional
 * Dear ImGui panel (debugui.cpp, built only with `make debug` / -DDEBUG_UI).
 * g_dbg is defined once in main.c and exists in every build; without the UI it
 * just holds its defaults, so the engine reads it the same either way. */
#ifndef OPENUG_DEBUG_H
#define OPENUG_DEBUG_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* --- freecam (works in every build; toggle with F) --- */
    int   freecam;
    float speed;                 /* freecam move units/frame */

    /* --- wheel placement (fractions of the car AABB) --- */
    float wheel_frontf, wheel_rearf, wheel_trackf, wheel_z, wheel_scale;

    /* --- lighting (fed to the shader as uniforms) --- */
    float ambient, diffuse, body_spec;
    float fog_density;          /* exp^2 fog: f = exp(-(depth*density)^2) */
    float fog_r, fog_g, fog_b;  /* fog + sky-clear colour (kept identical) */

    /* --- car appearance --- */
    int   paint_override;        /* 1 = use paint[] below instead of the per-car hash */
    float paint[3];
    int   show_body, show_glass, show_lights, show_tires, show_misc, show_track;

    /* --- readouts (engine writes, panel displays) --- */
    float cam[3], car[3], heading, kmh;
    int   car_meshes, track_meshes, fps;
    int   drawn;

    /* --- session info + menu HUD (moved off the 3D viewport in debug builds:
       consulted only under DEBUG_UI, so plain builds are unaffected either
       way) --- */
    int  hud_hide_menu;              /* 1 = ALL retro pixel-font viewport text
                                         (menu car/track/pips/prompt AND the
                                         in-race position/lap/speed HUD) is
                                         suppressed; read it in ImGui instead.
                                         Plain builds have no ImGui, so they
                                         never consult this — always drawn. */
    char car_name[32], track_name[64];
    int  sel_car, n_cars, sel_track, n_tracks, sel_circuit, n_circuits;
    int  race_pos, race_cars, race_lap, race_laps;   /* mirrors the in-race HUD */

    /* Session combo boxes: main.c points these at its own fixed-size car/
     * track name arrays (read-only from here) once at startup; they don't
     * move afterward, so the raw pointer is safe for the process lifetime.
     * The panel writes want_car/want_track (else leaves them -1) when the
     * user picks a different entry; main.c polls them once per frame and
     * performs the SAME relaunch() the arrow-key menu already uses. */
    const char (*car_list)[64];
    const char (*track_list)[64];
    int  want_car, want_track;

    /* --- car modification (wheel library); panel writes, main.c acts --- */
    const char (*wheel_brands)[24];  /* brand name table owned by main.c */
    int  wheel_brand_n;              /* entries in it */
    int  wheel_brand;                /* selected index */
    int  wheel_style;                /* STYLEnn within that brand */
    int  wheel_reload;               /* panel sets 1 => main.c re-streams rims */

    /* --- Mesh Inspector (passive: observes/overlays, never alters assets) --- */
    int  insp_count;        /* how many car meshes are inspectable */
    const int *insp_cat;    /* N2_CAR_* per mesh (main.c owns the array) */
    const int *insp_verts;  /* vertex count per mesh */
    int  insp_sel;          /* selected mesh index, -1 = none */
    int  insp_highlight;    /* 1 = force the selection to a neon unlit overlay */
    int  insp_wire;         /* 1 = draw the selection as wireframe */
    int  insp_dump;         /* panel raises, main.c dumps telemetry and clears */

    /* --- diagnostics --- */
    int  show_uv_checker;   /* fed to the shader's uUVCheck uniform each frame */
} DbgState;

extern DbgState g_dbg;

/* Dear ImGui bridge — no-ops unless DEBUG_UI is compiled in. Declared always so
 * main.c can call them unconditionally behind a single #ifdef. */
#ifdef DEBUG_UI
struct SDL_Window; union SDL_Event;
void dbgui_init(struct SDL_Window *win, void *glctx);
void dbgui_event(const union SDL_Event *e);
int  dbgui_want_mouse(void);          /* 1 = panel is grabbing the mouse */
int  dbgui_want_keyboard(void);
void dbgui_frame(void);               /* build the panel from g_dbg */
void dbgui_render(void);              /* draw it (call last, before SwapWindow) */
void dbgui_shutdown(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
