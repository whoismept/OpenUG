/* OpenUG debug overlay — Dear ImGui panel, compiled only into `make debug`.
 * Thin C-callable wrapper (declared in debug.h) so main.c stays plain C.
 * Backends: SDL2 + legacy OpenGL2 (matches the app's 2.1/compat GL context). */
#include <cstdio>
#include <SDL.h>
#ifdef __APPLE__
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/gl.h>
#else
#  include <SDL_opengl.h>
#endif
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl2.h"
#include "debug.h"

extern "C" void dbgui_init(struct SDL_Window *win, void *glctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;          /* don't litter imgui.ini */
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL((SDL_Window *)win, glctx);
    ImGui_ImplOpenGL2_Init();
}

extern "C" void dbgui_event(const union SDL_Event *e) {
    ImGui_ImplSDL2_ProcessEvent((const SDL_Event *)e);
}
extern "C" int dbgui_want_mouse(void)    { return ImGui::GetIO().WantCaptureMouse; }
extern "C" int dbgui_want_keyboard(void) { return ImGui::GetIO().WantCaptureKeyboard; }

extern "C" void dbgui_frame(void) {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("OpenUG Debug");
    g_dbg.fps = (int)ImGui::GetIO().Framerate;

    ImGui::Text("%.1f FPS   total=%d   cars=%d  track=%d", 
                ImGui::GetIO().Framerate,
                g_dbg.drawn, g_dbg.car_meshes, g_dbg.track_meshes);
    ImGui::Text("cam  %.1f %.1f %.1f", g_dbg.cam[0], g_dbg.cam[1], g_dbg.cam[2]);
    ImGui::Text("car  %.1f %.1f %.1f  hdg %.2f  %.0f km/h",
                g_dbg.car[0], g_dbg.car[1], g_dbg.car[2], g_dbg.heading, g_dbg.kmh);
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Session", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool show_menu_hud = !g_dbg.hud_hide_menu;
        ImGui::Checkbox("show 3D HUD (menu + race)", &show_menu_hud);
        g_dbg.hud_hide_menu = !show_menu_hud;
        ImGui::SameLine();
        ImGui::TextDisabled("(off = viewport is scene-only; this panel still shows selection/telemetry)");
        if (g_dbg.race_cars > 0)
            ImGui::Text("race: P%d/%d   lap %d/%d", g_dbg.race_pos, g_dbg.race_cars,
                        g_dbg.race_lap, g_dbg.race_laps);
        /* car/track combos: picking a different entry sets want_car/
           want_track for main.c to notice and relaunch with (same clean
           re-exec path as the arrow keys — see relaunch() in main.c) */
        g_dbg.want_car = -1; g_dbg.want_track = -1;
        if (g_dbg.car_list && g_dbg.n_cars > 0) {
            static const char *items[64];
            int n = g_dbg.n_cars < 64 ? g_dbg.n_cars : 64;
            for (int i = 0; i < n; i++) items[i] = g_dbg.car_list[i];
            int cur = g_dbg.sel_car;
            if (ImGui::Combo("car", &cur, items, n) && cur != g_dbg.sel_car)
                g_dbg.want_car = cur;
        } else ImGui::Text("car:     %-20s (%d/%d)", g_dbg.car_name, g_dbg.sel_car+1, g_dbg.n_cars);
        if (g_dbg.track_list && g_dbg.n_tracks > 0) {
            static const char *items[64];
            int n = g_dbg.n_tracks < 64 ? g_dbg.n_tracks : 64;
            for (int i = 0; i < n; i++) items[i] = g_dbg.track_list[i];
            int cur = g_dbg.sel_track;
            if (ImGui::Combo("track", &cur, items, n) && cur != g_dbg.sel_track)
                g_dbg.want_track = cur;
        } else ImGui::Text("track:   %-20s (%d/%d)", g_dbg.track_name, g_dbg.sel_track+1, g_dbg.n_tracks);
        ImGui::Text("circuit: %d/%d", g_dbg.sel_circuit+1, g_dbg.n_circuits);
        ImGui::TextDisabled("Left/Right car, Up/Down track, [ / ] circuit, Enter race");
        ImGui::Checkbox("Show UV Checker", (bool *)&g_dbg.show_uv_checker);
        ImGui::SameLine();
        ImGui::TextDisabled("(R=u, G=v, grid=10x10 cells; flat colour = UV outside [0,1])");
    }
    ImGui::Separator();

    ImGui::Checkbox("Freecam (F)", (bool *)&g_dbg.freecam);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("speed", &g_dbg.speed, 0.05f, 3.0f);

    if (ImGui::CollapsingHeader("Wheels", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("front frac", &g_dbg.wheel_frontf, 0.0f, 1.0f);
        ImGui::SliderFloat("rear frac",  &g_dbg.wheel_rearf,  0.0f, 1.0f);
        ImGui::SliderFloat("track frac", &g_dbg.wheel_trackf, 0.0f, 1.2f);
        ImGui::SliderFloat("z offset",   &g_dbg.wheel_z,    -1.0f, 1.0f);
        ImGui::SliderFloat("scale",      &g_dbg.wheel_scale, 0.3f, 2.0f);
    }
    if (ImGui::CollapsingHeader("Lighting")) {
        ImGui::SliderFloat("ambient",   &g_dbg.ambient,   0.0f, 1.0f);
        ImGui::SliderFloat("diffuse",   &g_dbg.diffuse,   0.0f, 1.5f);
        ImGui::SliderFloat("body spec", &g_dbg.body_spec, 0.0f, 1.0f);
        ImGui::SliderFloat("fog density", &g_dbg.fog_density, 0.0f, 0.01f, "%.4f");
        ImGui::ColorEdit3("fog / sky", &g_dbg.fog_r);
    }
    if (ImGui::CollapsingHeader("Car parts", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("body",   (bool *)&g_dbg.show_body);   ImGui::SameLine();
        ImGui::Checkbox("glass",  (bool *)&g_dbg.show_glass);  ImGui::SameLine();
        ImGui::Checkbox("lights", (bool *)&g_dbg.show_lights);
        ImGui::Checkbox("tires",  (bool *)&g_dbg.show_tires);  ImGui::SameLine();
        ImGui::Checkbox("misc",   (bool *)&g_dbg.show_misc);   ImGui::SameLine();
        ImGui::Checkbox("track",  (bool *)&g_dbg.show_track);
        ImGui::Checkbox("paint override", (bool *)&g_dbg.paint_override);
        ImGui::ColorEdit3("paint", g_dbg.paint);
    }
    ImGui::End();

    /* ---- Car Modification Setup: live wheel brand/style swapping ---- */
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Car Modification Setup");
    if (g_dbg.wheel_brands && g_dbg.wheel_brand_n > 0) {
        static const char *brands[32];
        int nb = g_dbg.wheel_brand_n < 32 ? g_dbg.wheel_brand_n : 32;
        for (int i = 0; i < nb; i++) brands[i] = g_dbg.wheel_brands[i];
        if (ImGui::Combo("wheel brand", &g_dbg.wheel_brand, brands, nb))
            g_dbg.wheel_reload = 1;
        if (ImGui::SliderInt("wheel style", &g_dbg.wheel_style, 1, 8))
            g_dbg.wheel_reload = 1;
        ImGui::TextDisabled("re-streams GEOMETRY_<BRAND>.BIN and rebuilds the rim VBO/IBO");
    } else ImGui::TextDisabled("wheel library not loaded");
    ImGui::Separator();
    ImGui::TextDisabled("body kit: K cycles KIT00/01/02 (see console)");

    if (ImGui::CollapsingHeader("Mesh Inspector", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char *catn[] = {"ROAD","TERRAIN","OTHER","SKY","GLOW","?","?","?","?","?",
                                     "BODY","GLASS","LIGHT","TIRE","MISC","BRAKELIGHT","MECH"};
        ImGui::Checkbox("highlight", &(bool &)*(bool *)&g_dbg.insp_highlight);
        ImGui::SameLine();
        ImGui::Checkbox("wireframe", &(bool &)*(bool *)&g_dbg.insp_wire);
        if (ImGui::Button("Dump Selected Mesh Telemetry")) g_dbg.insp_dump = 1;
        ImGui::BeginChild("meshlist", ImVec2(0, 180), true);
        for (int i = 0; i < g_dbg.insp_count; i++) {
            int c = (g_dbg.insp_cat && i < g_dbg.insp_count) ? g_dbg.insp_cat[i] : 0;
            const char *cn = (c >= 0 && c <= 16) ? catn[c] : "?";
            char lbl[96];
            snprintf(lbl, sizeof lbl, "%3d  %-10s %5d v", i, cn,
                     g_dbg.insp_verts ? g_dbg.insp_verts[i] : 0);
            if (ImGui::Selectable(lbl, g_dbg.insp_sel == i)) g_dbg.insp_sel = i;
        }
        ImGui::EndChild();
        if (ImGui::Button("clear selection")) g_dbg.insp_sel = -1;
    }
    ImGui::End();
}

extern "C" void dbgui_render(void) {
    ImGui::Render();
    /* the app binds its shader program once and never rebinds; the GL2 backend
       is fixed-function, so unbind the program around it and restore after. */
    GLint prev = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
    glUseProgram(0);
    /* the GL2 backend draws with client-side arrays: any VBO the app left bound
       would make glVertexPointer read garbage, and attrib 0 aliases gl_Vertex. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    glUseProgram(prev);
}

extern "C" void dbgui_shutdown(void) {
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}
