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

    ImGui::Text("%.1f FPS   car:%d track:%d", ImGui::GetIO().Framerate,
                g_dbg.car_meshes, g_dbg.track_meshes);
    ImGui::Text("cam  %.1f %.1f %.1f", g_dbg.cam[0], g_dbg.cam[1], g_dbg.cam[2]);
    ImGui::Text("car  %.1f %.1f %.1f  hdg %.2f  %.0f km/h",
                g_dbg.car[0], g_dbg.car[1], g_dbg.car[2], g_dbg.heading, g_dbg.kmh);
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
