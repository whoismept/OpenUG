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
typedef struct { GLuint vbo, nbo, ibo; int nidx, cat, trim, roof; uint32_t texkey; } GpuMesh;

/* ---- static-world batching: meshes merged per (256m grid cell, texture) ----
 * One interleaved VBO per batch kills the per-mesh bind/attrib overhead;
 * batches stay small enough for the XY distance cull to keep working. */
typedef struct {
    float pos[3];
    float uv[2];
    float normal[3];
} BatchedVertex;              /* 32 B, interleaved */

typedef struct {
    GLuint vbo;               /* unified interleaved VBO */
    GLuint ibo;               /* consolidated u16 IBO (<= 65535 verts/batch) */
    int index_count;
    uint32_t texkey;          /* first member mesh's TPK key (debugging) */
    GLuint tex;               /* resolved GL texture (0 = untextured fallback) */
    int nmesh;                /* source meshes merged in (drawn-mesh metric) */
    float bbox_min[3];        /* culling bounds */
    float bbox_max[3];
} N2Batch;

/* the one shader program + its uniform handles */
typedef struct {
    GLuint prog;
    GLint uMVP, uUseTex, uColor, uUnlit, uAlpha, uSoft, uSpec, uAmbient, uDiffuse,
          uLight,   /* sun direction in the CURRENT object's model space */
          uDecal,   /* 1 = texture is an alpha-masked decal over uColor paint */
          uFogColor, uFogDensity,   /* exp^2 distance fog (matches the sky) */
          uCamPos,  /* camera in the current object's model space */
          uEnv,     /* environment-reflection amount (cars only) */
          uUVCheck, /* 1 = show the diagnostic UV-coordinate visualization
                       instead of lighting/texture (toggle lives in the
                       ImGui Session panel, make debug only) */
          uGloss;   /* specular pow() exponent: high = tight metallic-paint
                       highlight, low = broad plastic/trim sheen (cars only) */
} RProg;

/* world-space sun direction (night scene key light) */
#define N2_SUN_X 0.4f
#define N2_SUN_Y 0.7f
#define N2_SUN_Z 0.6f

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
GLuint   make_wheel_tex(void);          /* radial alloy-rim texture for it */
GpuMesh  make_quad(void);               /* unit quad for HUD / billboards */
void     draw_gpumesh(GpuMesh *g);

/* Merge the static world into per-(cell,texture) batches and upload them.
 * mtex = per-mesh resolved GL texture, texTerr = grass fallback for terrain
 * meshes without one. Sorted by texture so binds are rare. The CPU-side scene
 * is left untouched (physics reads it). Returns the batch count. */
int  upload_world_batches(const N2Scene *s, const float (*mbb)[4],
                          const GLuint *mtex, GLuint texTerr, N2Batch **out);
/* Same merge, but for one category (N2_SKY / N2_GLOW) pulled out of the main
 * batching pass above — grouped by texture only, no spatial cell/cull grid,
 * since there are only ever a handful of skybox/neon meshes per city. */
int  upload_cat_batches(const N2Scene *s, int cat, const GLuint *mtex, N2Batch **out);
void draw_batch(const N2Batch *b);
GLuint   upload_tex(const N2Tex *t);

/* ---- 3x5 bitmap font (uppercase, digits, _ - /) ---- */
void  draw_text(GpuMesh *quad, GLint uMVP, const char *s,
                float x, float y, float px, float py);
float text_width(const char *s, float px);

/* dev screenshot: rgb is top-left origin, w*h*3 */
void write_png(const char *path, int w, int h, const unsigned char *rgb);

#endif
