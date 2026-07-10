/* render.h — OpenUG Renderer module: GL context-level state (shaders, buffers,
 * textures), the tiny column-major matrix library, the 3x5 bitmap font and the
 * dev PNG screenshot writer. Owns every gl* call that creates GPU objects;
 * main.c only binds/draws through GpuMesh + the RProg uniform handles.
 *
 * Also the single home of the GL headers: desktop legacy GL 2.1 + GLSL 120, or
 * OpenGL ES 2.0 + GLSL 100 with -DN2_GLES — every module includes GL via here.
 */
#ifndef OPENUG_RENDER_H
#define OPENUG_RENDER_H

#include <SDL.h>
#ifdef N2_GLES
#  include <SDL_opengles2.h>
#elif defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/gl.h>
#  include <OpenGL/glext.h>
#else
#  define GL_GLEXT_PROTOTYPES 1
#  include <SDL_opengl.h>
#endif

#include "nfsu2.h"

/* per-mesh GPU buffers + computed normals */
typedef struct { GLuint vbo, nbo, ibo; int nidx, cat; uint32_t texkey; } GpuMesh;

/* the one shader program + its uniform handles */
typedef struct {
    GLuint prog;
    GLint uMVP, uUseTex, uColor, uUnlit, uAlpha, uSoft, uSpec, uAmbient, uDiffuse;
} RProg;

/* ---- tiny 4x4 matrix (column-major) ---- */
void mat_mul(const float *a, const float *b, float *o);
void mat_persp(float fov, float aspect, float znear, float zfar, float *m);
void mat_trans(float x, float y, float z, float *m);
void mat_rotz(float a, float *m);
void mat_lookat(const float *eye, const float *fwd, float *m);   /* up = world +Z */

/* ---- GPU objects ---- */
RProg    render_program(void);          /* compile+link the shader, fetch uniforms */
GpuMesh *upload_scene(N2Scene *s);      /* VBO/NBO/IBO per mesh, normals computed */
GpuMesh  make_wheel(float R, float halfW);  /* procedural tyre (see render.c) */
GpuMesh  make_quad(void);               /* unit quad for HUD / billboards */
void     draw_gpumesh(GpuMesh *g);
GLuint   upload_tex(const N2Tex *t);

/* ---- 3x5 bitmap font (uppercase, digits, _ - /) ---- */
void  draw_text(GpuMesh *quad, GLint uMVP, const char *s,
                float x, float y, float px, float py);
float text_width(const char *s, float px);

/* dev screenshot: rgb is top-left origin, w*h*3 */
void write_png(const char *path, int w, int h, const unsigned char *rgb);

#endif
