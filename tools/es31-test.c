/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The ES 3.1 shim on a 3.0 driver (ANGLE over Metal), through the entry points a game gets: a triangle whose vertex streams are set
 * with glVertexAttribFormat/glBindVertexBuffer, whose vertex shader reads its colour from a buffer texture (samplerBuffer), and whose
 * varyings pair by location (UE4's GLSL_ES3_1_ANDROID); a compute shader is created, linked and dispatched (a stand-in). The centre
 * pixel must come out the buffer's colour.
 *
 *   es31-test <libEGL.dylib> <libGLESv2.dylib>      exit status 0 if it did
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "husk-tl-egl.h"

void tl_log_line(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }

typedef void *P;
#define F(ret, name, args) static ret (*name) args
F(P, eglGetDisplay, (P)); F(unsigned, eglInitialize, (P, int *, int *)); F(unsigned, eglChooseConfig, (P, const int *, P *, int, int *));
F(P, eglCreateContext, (P, P, P, const int *)); F(P, eglCreatePbufferSurface, (P, P, const int *)); F(unsigned, eglMakeCurrent, (P, P, P, P));
F(unsigned, glCreateShader, (unsigned)); F(void, glShaderSource, (unsigned, int, const char *const *, const int *)); F(void, glCompileShader, (unsigned));
F(void, glGetShaderiv, (unsigned, unsigned, int *)); F(unsigned, glCreateProgram, (void)); F(void, glAttachShader, (unsigned, unsigned));
F(void, glLinkProgram, (unsigned)); F(void, glGetProgramiv, (unsigned, unsigned, int *)); F(void, glGetProgramInfoLog, (unsigned, int, int *, char *));
F(void, glUseProgram, (unsigned)); F(void, glGenBuffers, (int, unsigned *)); F(void, glBindBuffer, (unsigned, unsigned));
F(void, glBufferData, (unsigned, long, const void *, unsigned)); F(void, glGenTextures, (int, unsigned *)); F(void, glBindTexture, (unsigned, unsigned));
F(void, glTexBuffer, (unsigned, unsigned, unsigned)); F(int, glGetUniformLocation, (unsigned, const char *)); F(void, glUniform1i, (int, int));
F(void, glVertexAttribFormat, (unsigned, int, unsigned, unsigned char, unsigned)); F(void, glVertexAttribBinding, (unsigned, unsigned));
F(void, glBindVertexBuffer, (unsigned, unsigned, long, int)); F(void, glEnableVertexAttribArray, (unsigned)); F(void, glGenVertexArrays, (int, unsigned *));
F(void, glBindVertexArray, (unsigned)); F(void, glViewport, (int, int, int, int)); F(void, glClearColor, (float, float, float, float)); F(void, glClear, (unsigned));
F(void, glDrawElements, (unsigned, int, unsigned, const void *)); F(void, glReadPixels, (int, int, int, int, unsigned, unsigned, void *));
F(void, glDispatchCompute, (unsigned, unsigned, unsigned)); F(unsigned, glGetError, (void)); F(void, glActiveTexture, (unsigned));
#define E(tag) { unsigned e_ = glGetError(); if (e_) printf("GL error %#x after %s\n", e_, tag); }
#define R(name) name = tl_egl_resolve(#name); if (!name) { printf("no %s\n", #name); return 1; }

static const char *vs =
    "#version 300 es\n"
    "layout(location=0) in highp vec2 in_ATTRIBUTE0;\n"
    "uniform highp samplerBuffer vs0;\n"
    "layout(location=0) out highp vec4 var_COLOR0;\n"
    "void main() { var_COLOR0 = texelFetch(vs0, 4097); gl_Position = vec4(in_ATTRIBUTE0, 0.0, 1.0); }\n";
static const char *fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "layout(location=0) in highp vec4 in_COLOR0;\n"
    "layout(location=0) out vec4 out_Target0;\n"
    "void main() { out_Target0 = in_COLOR0; }\n";
static const char *cs = "#version 310 es\nlayout(local_size_x=8) in;\nvoid main() {}\n";

static unsigned shader(unsigned type, const char *src)
{
    unsigned s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    int ok = 0; glGetShaderiv(s, 0x8B81, &ok); printf("shader %#x compiled: %d\n", type, ok); return s;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s libEGL libGLESv2\n", argv[0]); return 2; }
    tl_egl_es31_shim(true);
    if (!tl_egl_init(argv[1], argv[2], NULL, 0)) { puts("egl init failed"); return 1; }
    R(eglGetDisplay) R(eglInitialize) R(eglChooseConfig) R(eglCreateContext) R(eglCreatePbufferSurface) R(eglMakeCurrent)
    P d = eglGetDisplay(NULL); eglInitialize(d, NULL, NULL);
    int ca[] = { 0x3040, 0x40, 0x3033, 1, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038 }; P cfg; int nc = 0;
    eglChooseConfig(d, ca, &cfg, 1, &nc);
    int xa[] = { 0x3098, 3, 0x30fb, 1, 0x3038 };
    P ctx = eglCreateContext(d, cfg, NULL, xa); int pa[] = { 0x3057, 16, 0x3056, 16, 0x3038 };
    P surf = eglCreatePbufferSurface(d, cfg, pa); unsigned mc = eglMakeCurrent(d, surf, surf, ctx); printf("ctx %p surf %p current %u configs %d\n", ctx, surf, mc, nc);
    R(glCreateShader) R(glShaderSource) R(glCompileShader) R(glGetShaderiv) R(glCreateProgram) R(glAttachShader) R(glLinkProgram) R(glGetProgramiv)
    R(glGetProgramInfoLog) R(glUseProgram) R(glGenBuffers) R(glBindBuffer) R(glBufferData) R(glGenTextures) R(glBindTexture) R(glTexBuffer)
    R(glGetUniformLocation) R(glUniform1i) R(glVertexAttribFormat) R(glVertexAttribBinding) R(glBindVertexBuffer) R(glEnableVertexAttribArray)
    R(glGenVertexArrays) R(glBindVertexArray) R(glViewport) R(glClearColor) R(glClear) R(glDrawElements) R(glReadPixels) R(glDispatchCompute) R(glGetError) R(glActiveTexture)

    unsigned p = glCreateProgram(); glAttachShader(p, shader(0x8B31, vs)); glAttachShader(p, shader(0x8B30, fs)); glLinkProgram(p);
    int ok = 0; char log[1000] = ""; glGetProgramiv(p, 0x8B82, &ok); glGetProgramInfoLog(p, 1000, NULL, log); printf("linked: %d %s\n", ok, log);
    unsigned cp = glCreateProgram(); glAttachShader(cp, shader(0x91B9, cs)); glLinkProgram(cp); glGetProgramiv(cp, 0x8B82, &ok); printf("compute linked: %d\n", ok);
    glUseProgram(cp); glDispatchCompute(1, 1, 1); E("compute")

    /* the colour at texel 4097 of an RGBA32F buffer texture: the second row */
    float *texels = calloc(4100 * 4, sizeof(float)); texels[4097 * 4 + 0] = 0.0f; texels[4097 * 4 + 1] = 1.0f; texels[4097 * 4 + 2] = 0.0f; texels[4097 * 4 + 3] = 1.0f;
    unsigned tb, tex; glGenBuffers(1, &tb); glBindBuffer(0x8C2A, tb); glBufferData(0x8C2A, 4100 * 16, texels, 0x88E4);
    glGenTextures(1, &tex); glActiveTexture(0x84C0); glBindTexture(0x8C2A, tex); glTexBuffer(0x8C2A, 0x8815, tb); E("texbuffer")

    float verts[] = { -1, -1, 3, -1, -1, 3 }; unsigned short idx[] = { 0, 1, 2 };
    unsigned vao, vb, ib; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vb); glBindBuffer(0x8892, vb); glBufferData(0x8892, sizeof(verts), verts, 0x88E4);
    glGenBuffers(1, &ib); glBindBuffer(0x8893, ib); glBufferData(0x8893, sizeof(idx), idx, 0x88E4);
    glBindBuffer(0x8892, 0); E("buffers")
    glVertexAttribFormat(0, 2, 0x1406, 0, 0); glVertexAttribBinding(0, 3); glBindVertexBuffer(3, vb, 0, 8); glEnableVertexAttribArray(0); E("vab")

    glUseProgram(p); glUniform1i(glGetUniformLocation(p, "vs0"), 0); E("uniform")
    glViewport(0, 0, 16, 16); glClearColor(1, 0, 0, 1); glClear(0x4000); E("clear")
    glDrawElements(4, 3, 0x1403, 0); E("draw")
    uint8_t px[4] = { 0 }; glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, px);
    printf("pixel %d %d %d %d, GL error %#x\n", px[0], px[1], px[2], px[3], glGetError());
    return px[0] < 30 && px[1] > 220 && px[2] < 30 ? 0 : 1;
}
