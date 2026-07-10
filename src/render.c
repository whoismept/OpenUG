/* render.c — OpenUG Renderer module implementation. Everything here was
 * extracted verbatim from the original monolithic main.c; the parsing logic
 * (nfsu2.h) is untouched ground truth. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <zlib.h>   /* screenshot PNG (dev only) */

#include "render.h"

#ifdef N2_GLES
#  define GLSL_HEADER "precision mediump float;\n"
#else
#  define GLSL_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#endif

static const char *VS =
    GLSL_HEADER
    "attribute vec3 aPos; attribute vec2 aUV; attribute vec3 aNor;\n"
    "uniform mat4 uMVP; varying vec2 vUV; varying vec3 vN;\n"
    "void main(){ vUV=aUV; vN=aNor; gl_Position=uMVP*vec4(aPos,1.0); }\n";

static const char *FS =
    GLSL_HEADER
    "varying vec2 vUV; varying vec3 vN; uniform sampler2D uTex;\n"
    "uniform float uUseTex; uniform vec3 uColor; uniform float uUnlit; uniform float uAlpha; uniform float uSoft; uniform float uSpec;\n"
    "uniform float uAmbient; uniform float uDiffuse;\n"
    "void main(){\n"
    "  if(uUnlit>0.5){ float a=uAlpha;\n"
    "    if(uSoft>0.5){ float d=length(vUV-vec2(0.5)); a*=clamp(1.0-d*2.0,0.0,1.0); a*=a; }\n"
    "    gl_FragColor=vec4(uColor,a); return; }\n"
    "  vec3 L=normalize(vec3(0.4,0.7,0.6)); vec3 N=normalize(vN);\n"
    "  float d=uAmbient+uDiffuse*max(dot(N,L),0.0);\n"   /* directional; reveals body form */
    "  vec3 base = uUseTex>0.5 ? texture2D(uTex,vUV).rgb : uColor;\n"
    "  float sp = pow(max(dot(N,L),0.0), 20.0)*uSpec;\n"       /* glossy sheen (car paint) */
    "  float rim = pow(1.0-abs(N.z), 3.0)*uSpec*0.4;\n"        /* fresnel-ish edge sheen */
    "  gl_FragColor=vec4(base*d*1.35 + sp + rim, 1.0);\n"      /* lift dark night textures + gloss */
    "}\n";

/* ---- tiny 4x4 matrix (column-major) ---- */
void mat_mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k*4+j]*b[i*4+k]; o[i*4+j] = s;
    }
}
void mat_persp(float f, float ar, float n, float fa, float *m) {
    float t = 1.0f/tanf(f/2);
    memset(m, 0, 16*sizeof(float));
    m[0]=t/ar; m[5]=t; m[10]=(fa+n)/(n-fa); m[11]=-1; m[14]=2*fa*n/(n-fa);
}
static void vnorm(float *v){ float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l<1e-6f)l=1;
    v[0]/=l; v[1]/=l; v[2]/=l; }
void mat_trans(float x,float y,float z,float *m){
    float r[16]={1,0,0,0,0,1,0,0,0,0,1,0,x,y,z,1}; memcpy(m,r,sizeof r); }
void mat_rotz(float a, float *m){ float c=cosf(a),s=sinf(a);
    float r[16]={c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1}; memcpy(m,r,sizeof r); }
/* right-handed lookAt, column-major, up = world +Z */
void mat_lookat(const float *eye, const float *fwd, float *m) {
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
void write_png(const char *path, int w, int h, const unsigned char *rgb) {
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

RProg render_program(void) {
    RProg r;
    r.prog = glCreateProgram();
    glAttachShader(r.prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(r.prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(r.prog, 0, "aPos");
    glBindAttribLocation(r.prog, 1, "aUV");
    glBindAttribLocation(r.prog, 2, "aNor");
    glLinkProgram(r.prog); glUseProgram(r.prog);
    r.uMVP     = glGetUniformLocation(r.prog, "uMVP");
    r.uUseTex  = glGetUniformLocation(r.prog, "uUseTex");
    r.uColor   = glGetUniformLocation(r.prog, "uColor");
    r.uUnlit   = glGetUniformLocation(r.prog, "uUnlit");
    r.uAlpha   = glGetUniformLocation(r.prog, "uAlpha");
    r.uSoft    = glGetUniformLocation(r.prog, "uSoft");
    r.uSpec    = glGetUniformLocation(r.prog, "uSpec");
    r.uAmbient = glGetUniformLocation(r.prog, "uAmbient");
    r.uDiffuse = glGetUniformLocation(r.prog, "uDiffuse");
    glUniform1f(r.uAlpha, 1.0f); glUniform1f(r.uSoft, 0.0f); glUniform1f(r.uSpec, 0.0f);
    return r;
}

/* upload a scene's meshes to GPU buffers, computing per-vertex normals. */
GpuMesh *upload_scene(N2Scene *s) {
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

/* Build a clean procedural wheel (short cylinder, axle along Y, disc in X-Z) —
 * the game's rim meshes are sparse spoke shells that read as spiky urchins when
 * drawn solid, so we substitute a simple tyre. Modelled at the origin like the
 * game wheel, so the same wheelT placement transforms apply. */
GpuMesh make_wheel(float R, float halfW) {
    const int N = 24;
    int nv = N*2 + 2, nt = N*4;         /* 2 rings + 2 hub centres; sides + 2 caps */
    N2Mesh m; memset(&m, 0, sizeof m);
    m.nverts = nv; m.verts = (float *)malloc(nv*5*sizeof(float));
    for (int i = 0; i < N; i++) {
        float a = 2.0f*(float)M_PI*i/N, cx = R*cosf(a), cz = R*sinf(a);
        float u = cosf(a)*0.5f+0.5f, v = sinf(a)*0.5f+0.5f;
        float *p0 = m.verts + i*5;     p0[0]=cx; p0[1]= halfW; p0[2]=cz; p0[3]=u; p0[4]=v;
        float *p1 = m.verts + (N+i)*5; p1[0]=cx; p1[1]=-halfW; p1[2]=cz; p1[3]=u; p1[4]=v;
    }
    int c0 = 2*N, c1 = 2*N+1;
    float *h0=m.verts+c0*5; h0[0]=0;h0[1]= halfW;h0[2]=0;h0[3]=0.5f;h0[4]=0.5f;
    float *h1=m.verts+c1*5; h1[0]=0;h1[1]=-halfW;h1[2]=0;h1[3]=0.5f;h1[4]=0.5f;
    m.idx = (uint16_t *)malloc(nt*3*sizeof(uint16_t)); int k=0;
    for (int i = 0; i < N; i++) {
        int j = (i+1)%N;
        m.idx[k++]=i;    m.idx[k++]=j;    m.idx[k++]=N+j;      /* tread quad */
        m.idx[k++]=i;    m.idx[k++]=N+j;  m.idx[k++]=N+i;
        m.idx[k++]=c0;   m.idx[k++]=i;    m.idx[k++]=j;         /* +Y face */
        m.idx[k++]=c1;   m.idx[k++]=N+j;  m.idx[k++]=N+i;       /* -Y face */
    }
    m.nidx = k; m.cat = N2_CAR_TIRE;
    N2Scene s; s.meshes = &m; s.count = 1; s.cap = 1;
    GpuMesh *g = upload_scene(&s); GpuMesh out = *g;
    free(g); free(m.verts); free(m.idx);
    return out;
}

/* unit-quad buffers for the 2D HUD / billboards (drawn in NDC via uMVP) */
GpuMesh make_quad(void) {
    GpuMesh quad; memset(&quad, 0, sizeof quad);
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
    return quad;
}

void draw_gpumesh(GpuMesh *g) {
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

GLuint upload_tex(const N2Tex *t) {
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,t->w,t->h,0,GL_RGB,GL_UNSIGNED_BYTE,t->rgb);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    return id;
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
    {0,0,0,0,7},{0,0,7,0,0},{1,1,2,4,4},             /* _ - / */
};
static const unsigned char *glyph3x5(char c) {
    if (c >= '0' && c <= '9') return FONT3x5[1 + (c-'0')];
    if (c >= 'A' && c <= 'Z') return FONT3x5[11 + (c-'A')];
    if (c == '_') return FONT3x5[37];
    if (c == '-') return FONT3x5[38];
    if (c == '/') return FONT3x5[39];
    return FONT3x5[0];
}
/* Draw an uppercase string at NDC (x,y = top-left), pixel size (px,py). Colour +
   uUnlit/uUseTex are set by the caller; this only sets uMVP and draws pixels. */
void draw_text(GpuMesh *quad, GLint uMVP, const char *s, float x, float y, float px, float py) {
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
float text_width(const char *s, float px) { return (float)strlen(s) * 4 * px; }
