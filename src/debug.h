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

    /* --- car appearance --- */
    int   paint_override;        /* 1 = use paint[] below instead of the per-car hash */
    float paint[3];
    int   show_body, show_glass, show_lights, show_tires, show_misc, show_track;

    /* --- readouts (engine writes, panel displays) --- */
    float cam[3], car[3], heading, kmh;
    int   car_meshes, track_meshes, fps;
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
