/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GLES 3.1 on a 3.0 driver (included by husk-tl-egl.c, which supplies a_glGetIntegerv / a_eglGetProcAddress).
 *
 * ANGLE over Metal stops at OpenGL ES 3.0, and Minecraft's Android materials hold only "#version 310 es" shaders, chosen by
 * reporting 3.1. For a game that asks for it (tl_egl_es31_shim) the driver is described as 3.1 and each such shader is written
 * down to 300 es: the version line changes, and `layout(binding = N)` -- which 3.0 does not have -- is dropped and applied after
 * the link as glUniform1i / glUniformBlockBinding. What 3.1 really adds (compute, images, buffers) is not there.
 */
static bool g_es31_shim;
static int g_es3_minor = 1;                                 /* the minor version told: 1, or 2 for an app whose manifest needs 3.2 */
void tl_egl_es31_shim(bool on) { g_es31_shim = on; }
void tl_egl_es3_claim(int minor) { if (minor >= 1) { g_es31_shim = true; g_es3_minor = minor > 2 ? 2 : minor; } }

typedef struct { char name[72]; int binding; bool block; } es31_bind;
#define ES31_SHADERS 1024
#define ES31_BINDS 12
#define ES31_ATTACHED 2048
static struct { unsigned shader; int n; es31_bind b[ES31_BINDS]; } g_es31_sh[ES31_SHADERS];
static struct { unsigned prog, shader; } g_es31_att[ES31_ATTACHED];
static int g_es31_natt;
static pthread_mutex_t g_es31_mu = PTHREAD_MUTEX_INITIALIZER;

/* "OpenGL ES 3.0 ..." -> "OpenGL ES 3.1 ...", "OpenGL ES GLSL ES 3.00 ..." -> "... 3.10 ..." (a copy: the driver's string is not ours to edit) */
static const char *es31_fix(const char *s, bool glsl)
{
    static char ring[8][300]; static atomic_uint k;
    char *out = ring[atomic_fetch_add(&k, 1) % 8];
    snprintf(out, 300, "%s", s);
    if (glsl) { char *p = strstr(out, "GLSL ES 3.00"); if (p) { p[10] = (char)('0' + g_es3_minor); p[11] = '0'; } }
    else { char *p = strstr(out, "OpenGL ES 3.0"); if (p) p[12] = (char)('0' + g_es3_minor); }
    return out;
}

/* The driver's own minor version, asked once a context is current: 1 or more and the shim only renumbers shaders. */
static int g_es31_real_minor = -1;
static int es31_real_minor(void)
{
    if (g_es31_real_minor < 0 && a_glGetIntegerv) { int m = 0; a_glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &m); g_es31_real_minor = m; tl_log_line("es31: the driver is OpenGL ES 3.%d", m); }
    return g_es31_real_minor;
}
/* the shim's buffer redirects: only on a 3.0 driver */
static bool es31_buffers(void) { return g_es31_shim && es31_real_minor() < 1; }

static bool es31_ident(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* The 300 es text of a 310 es shader (malloc'd), with the bindings it dropped in rec[0..*nrec). Anything else is returned as it came. */
static char *es31_downgrade(const char *src, size_t n, es31_bind *rec, int *nrec)
{
    char *out = malloc(n + 4096); size_t o = 0;
    *nrec = 0;
    const char *ver = g_es31_shim ? strstr(src, "#version 310 es") : NULL;     /* a real 3.1 driver keeps 310 es */
    if (!ver && g_es31_shim) ver = strstr(src, "#version 320 es");
    /* Any other shader with explicit uniform locations (UE4's GLSL_ES3_1_ANDROID): only those go, also on a real 3.1 driver. */
    bool v300 = !ver && strstr(src, "location") && strstr(src, "uniform");
    if (!ver && !v300) { memcpy(out, src, n); out[n] = 0; return out; }
    for (size_t i = 0; i < n;) {
        if (src + i == ver) { memcpy(out + o, "#version 300 es", 15); o += 15; i += 15; continue; }
        if (i + 7 <= n && !strncmp(src + i, "layout(", 7) && (i == 0 || !es31_ident(src[i - 1]))) {
            size_t j = i + 7; int depth = 1;
            while (j < n && depth) { if (src[j] == '(') depth++; else if (src[j] == ')') depth--; j++; }
            char q[160]; size_t ql = j - 1 - (i + 7); if (ql >= sizeof(q)) ql = sizeof(q) - 1;
            memcpy(q, src + i + 7, ql); q[ql] = 0;
            char *b = v300 ? NULL : strstr(q, "binding");
            size_t d = j; while (d < n && src[d] != ';' && src[d] != '{') d++;
            char decl[200]; size_t dl = d - j; if (dl >= sizeof(decl)) dl = sizeof(decl) - 1;
            memcpy(decl, src + j, dl); decl[dl] = 0;
            bool opaque = strstr(decl, "image") || strstr(decl, "atomic_uint");
            bool ssbo = !v300 && !opaque && strstr(decl, "buffer") && d < n && src[d] == '{' && !strstr(decl, "writeonly") && !strstr(decl, "coherent");
            if (ssbo && b) {
                /* A read-only storage block of structs becomes a uniform block of 1024 of them (std140 and std430 lay these structs out alike
                 * when their members are 4-byte scalars); the game's glBindBufferBase on the storage target is redirected to the uniform one. */
                size_t close = d; int depth2 = 0;
                while (close < n) { if (src[close] == '{') depth2++; else if (src[close] == '}' && --depth2 == 0) break; close++; }
                char *eq = strchr(b, '='); int val = eq ? atoi(eq + 1) : -1;
                size_t e = dl;
                while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\n' || decl[e - 1] == '\t' || decl[e - 1] == '\r')) e--;
                size_t st = e; while (st && es31_ident(decl[st - 1])) st--;
                if (close < n && val >= 0 && e > st && e - st < sizeof(rec[0].name) && *nrec < ES31_BINDS) {
                    memcpy(rec[*nrec].name, decl + st, e - st); rec[*nrec].name[e - st] = 0; rec[*nrec].binding = val; rec[*nrec].block = true; (*nrec)++;
                    o += (size_t)sprintf(out + o, "layout(std140) uniform %.*s\n{", (int)(e - st), decl + st);
                    for (size_t k = d + 1; k < close; k++) {
                        if (src[k] == '[' && k + 1 < close && src[k + 1] == ']') { o += (size_t)sprintf(out + o, "[1024]"); k++; }
                        else out[o++] = src[k];
                    }
                    out[o++] = '}';
                    i = close + 1; continue;
                }
            }
            /* layout(location = N) on a uniform is 3.1 too (explicit uniform locations): dropped, the game finds its uniforms by name. */
            const char *dk = decl; while (*dk == ' ' || *dk == '\n' || *dk == '\t' || *dk == '\r') dk++;
            bool uniloc = strstr(q, "location") && !strncmp(dk, "uniform", 7) && (dk[7] == ' ' || dk[7] == '\t' || dk[7] == '\n');
            if ((b || uniloc) && !opaque && !ssbo) {
                char *eq = b ? strchr(b, '=') : NULL; int val = eq ? atoi(eq + 1) : -1;
                bool block = d < n && src[d] == '{';
                if (*nrec < ES31_BINDS && val >= 0) {
                    size_t e = dl;
                    while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\n' || decl[e - 1] == '\t' || decl[e - 1] == '\r')) e--;
                    while (e && decl[e - 1] == ']') { while (e && decl[e - 1] != '[') e--; if (e) e--; }
                    size_t st = e; while (st && es31_ident(decl[st - 1])) st--;
                    if (e > st && e - st < sizeof(rec[0].name)) {
                        memcpy(rec[*nrec].name, decl + st, e - st); rec[*nrec].name[e - st] = 0;
                        rec[*nrec].binding = val; rec[*nrec].block = block; (*nrec)++;
                    }
                }
                /* the layout again, without the binding qualifier */
                char keep[160]; size_t kl = 0; const char *tok = q;
                while (tok) {
                    const char *comma = strchr(tok, ',');
                    size_t tl = comma ? (size_t)(comma - tok) : strlen(tok);
                    const char *t = tok; size_t tn = tl; while (tn && *t == ' ') { t++; tn--; }
                    bool drop = (tn >= 7 && !strncmp(t, "binding", 7)) || (uniloc && tn >= 8 && !strncmp(t, "location", 8));
                    if (!drop && tn) { if (kl) keep[kl++] = ','; memcpy(keep + kl, t, tn); kl += tn; }
                    tok = comma ? comma + 1 : NULL;
                }
                if (kl) { memcpy(out + o, "layout(", 7); o += 7; memcpy(out + o, keep, kl); o += kl; out[o++] = ')'; }
                i = j; continue;
            }
            memcpy(out + o, src + i, j - i); o += j - i; i = j; continue;
        }
        out[o++] = src[i++];
    }
    out[o] = 0;
    return out;
}

/*
 * ES 3.0 pairs a vertex shader's outputs with a fragment shader's inputs by name; 3.1 also by location, and UE4's GLSL_ES3_1_ANDROID
 * relies on that (var_TEXCOORD0 out, in_TEXCOORD0 in, both location 0). On a 3.0 driver (ANGLE over Metal) each such variable loses
 * its location and is renamed husk_vN after it, so the pairs match by name. Attributes (vertex inputs) and fragment outputs keep
 * theirs: 3.0 has those. Returns a new string, or NULL when there is nothing to do.
 */
#define ES30_MAXV 32
static char *es30_link_by_location(const char *src, bool vertex)
{
    if (!strstr(src, "#version 300 es") || !strstr(src, "location")) return NULL;
    size_t n = strlen(src);
    char *out = malloc(n + 1); size_t o = 0;
    char names[ES30_MAXV][64]; int locs[ES30_MAXV], nv = 0;
    for (size_t i = 0; i < n;) {
        if (i + 7 <= n && !strncmp(src + i, "layout(", 7) && (i == 0 || !es31_ident(src[i - 1]))) {
            size_t j = i + 7; int depth = 1;
            while (j < n && depth) { if (src[j] == '(') depth++; else if (src[j] == ')') depth--; j++; }
            char q[160]; size_t ql = j - 1 - (i + 7); if (ql >= sizeof(q)) ql = sizeof(q) - 1;
            memcpy(q, src + i + 7, ql); q[ql] = 0;
            const char *lp = strstr(q, "location");
            size_t d = j; while (d < n && src[d] != ';' && src[d] != '{') d++;
            /* the words of the declaration: its direction and its name */
            bool in = false, outw = false, uni = false; char name[64] = "";
            for (size_t k = j; k < d;) {
                if (!es31_ident(src[k])) { if (src[k] == '[') break; k++; continue; }
                size_t e = k; while (e < d && es31_ident(src[e])) e++;
                size_t wl = e - k;
                if (wl == 2 && !strncmp(src + k, "in", 2)) in = true;
                else if (wl == 3 && !strncmp(src + k, "out", 3)) outw = true;
                else if (wl == 7 && !strncmp(src + k, "uniform", 7)) uni = true;
                if (wl < sizeof(name)) { memcpy(name, src + k, wl); name[wl] = 0; }
                k = e;
            }
            bool varying = lp && !uni && d < n && src[d] == ';' && ((vertex && outw) || (!vertex && in));
            const char *eq = lp ? strchr(lp, '=') : NULL;
            if (varying && eq && name[0] && nv < ES30_MAXV) {
                snprintf(names[nv], sizeof(names[nv]), "%s", name); locs[nv] = atoi(eq + 1); nv++;
                /* the layout again without location */
                char keep[160]; size_t kl = 0; const char *tok = q;
                while (tok) {
                    const char *comma = strchr(tok, ',');
                    size_t tl = comma ? (size_t)(comma - tok) : strlen(tok);
                    const char *t = tok; size_t tn = tl; while (tn && *t == ' ') { t++; tn--; }
                    if (tn && !(tn >= 8 && !strncmp(t, "location", 8))) { if (kl) keep[kl++] = ','; memcpy(keep + kl, t, tn); kl += tn; }
                    tok = comma ? comma + 1 : NULL;
                }
                if (kl) { memcpy(out + o, "layout(", 7); o += 7; memcpy(out + o, keep, kl); o += kl; out[o++] = ')'; }
                i = j; continue;
            }
            memcpy(out + o, src + i, j - i); o += j - i; i = j; continue;
        }
        out[o++] = src[i++];
    }
    out[o] = 0;
    if (!nv) { free(out); return NULL; }
    /* every use of those names, renamed (a name grows by at most a few characters) */
    char *ren = malloc(o + (size_t)nv * 16 * 64 + 1024); size_t r = 0, cap = o + (size_t)nv * 16 * 64 + 1024;
    for (size_t i = 0; i < o;) {
        if (es31_ident(out[i]) && (i == 0 || !es31_ident(out[i - 1]))) {
            size_t e = i; while (e < o && es31_ident(out[e])) e++;
            int hit = -1;
            for (int v = 0; v < nv && hit < 0; v++) if (strlen(names[v]) == e - i && !strncmp(out + i, names[v], e - i)) hit = v;
            if (hit >= 0 && r + 16 < cap) { r += (size_t)sprintf(ren + r, "husk_v%d", locs[hit]); i = e; continue; }
            if (r + (e - i) + 1 >= cap) { cap = cap * 2 + (e - i); ren = realloc(ren, cap); }
            memcpy(ren + r, out + i, e - i); r += e - i; i = e; continue;
        }
        if (r + 2 >= cap) { cap *= 2; ren = realloc(ren, cap); }
        ren[r++] = out[i++];
    }
    ren[r] = 0;
    free(out);
    return ren;
}

/*
 * Compute shaders on a 3.0 driver: ANGLE over Metal has none, and Unreal's ES 3.1 renderer quits when a compute program does not
 * link (FNAF Security Breach: RHIClearUAVUint -> GetLinkedComputeProgram). Under the shim on 3.0 a compute shader is a stand-in
 * (a real shader name, never compiled), a program it is attached to links without the driver, and dispatching it does nothing. What
 * the game computes that way (clearing buffers, GPU particles, some post-processing) is missing; the rest draws.
 */
#define ES31_FAKE 8192
static unsigned g_fake_sh[ES31_FAKE], g_fake_prog[ES31_FAKE];
static bool fake_has(unsigned *t, unsigned id) { if (!id) return false; for (unsigned k = 0, h = id % ES31_FAKE; k < ES31_FAKE; k++, h = (h + 1) % ES31_FAKE) { if (t[h] == id) return true; if (!t[h]) return false; } return false; }
static void fake_add(unsigned *t, unsigned id) { if (!id || fake_has(t, id)) return; for (unsigned k = 0, h = id % ES31_FAKE; k < ES31_FAKE; k++, h = (h + 1) % ES31_FAKE) if (!t[h]) { t[h] = id; return; } }
static bool es31_buffers(void);
static bool es31_fake_shader(unsigned sh) { return fake_has(g_fake_sh, sh); }
static bool es31_fake_program(unsigned p) { return fake_has(g_fake_prog, p); }

static unsigned (*r_glCreateShader)(unsigned);
static unsigned w_glCreateShader(unsigned type)
{
    if (type == 0x91B9 /* GL_COMPUTE_SHADER */ && es31_buffers()) {
        unsigned sh = r_glCreateShader(0x8B31 /* a vertex shader's name, never compiled */);
        pthread_mutex_lock(&g_es31_mu); fake_add(g_fake_sh, sh); pthread_mutex_unlock(&g_es31_mu);
        static atomic_int said; if (atomic_fetch_add(&said, 1) == 0) tl_log_line("es31: compute shaders are stand-ins on this driver: dispatches do nothing");
        return sh;
    }
    return r_glCreateShader(type);
}
static void (*r_glGetShaderiv)(unsigned, unsigned, int *);
static void w_glGetShaderiv(unsigned sh, unsigned pname, int *v)
{
    if (v && es31_fake_shader(sh)) {
        if (pname == 0x8B4F /* GL_SHADER_TYPE */) { *v = 0x91B9; return; }
        if (pname == 0x8B81 /* GL_COMPILE_STATUS */) { *v = 1; return; }
        if (pname == 0x8B84 /* GL_INFO_LOG_LENGTH */ || pname == 0x8B88 /* GL_SHADER_SOURCE_LENGTH */) { *v = 0; return; }
    }
    r_glGetShaderiv(sh, pname, v);
}
static void (*r_glGetProgramiv)(unsigned, unsigned, int *);
static void w_glGetProgramiv(unsigned p, unsigned pname, int *v)
{
    if (v && es31_fake_program(p)) {
        if (pname == 0x8B82 /* GL_LINK_STATUS */ || pname == 0x8B83 /* GL_VALIDATE_STATUS */) *v = 1;
        else if (pname == 0x8267 /* GL_COMPUTE_WORK_GROUP_SIZE */) { v[0] = v[1] = v[2] = 1; }
        else *v = 0;
        return;
    }
    r_glGetProgramiv(p, pname, v);
}
static void (*r_glUseProgram)(unsigned);
static void w_glUseProgram(unsigned p) { if (es31_fake_program(p)) return; r_glUseProgram(p); }
static void (*r_glDispatchCompute)(unsigned, unsigned, unsigned);
static void w_glDispatchCompute(unsigned x, unsigned y, unsigned z) { if (es31_buffers()) return; if (r_glDispatchCompute) r_glDispatchCompute(x, y, z); }
static void (*r_glDispatchComputeIndirect)(long);
static void w_glDispatchComputeIndirect(long o) { if (es31_buffers()) return; if (r_glDispatchComputeIndirect) r_glDispatchComputeIndirect(o); }
static void (*r_glMemoryBarrier)(unsigned);
static void w_glMemoryBarrier(unsigned b) { if (es31_buffers()) return; if (r_glMemoryBarrier) r_glMemoryBarrier(b); }

/*
 * Buffer textures (samplerBuffer, ES 3.2 / EXT_texture_buffer) on a 3.0 driver. In the shader such a sampler becomes a 2D one 4096 texels
 * wide and texelFetch(s, i) reads texel (i & 4095, i >> 12); on the GL side (below) the buffer's bytes are copied into that texture,
 * on the GPU through the pixel-unpack target, whenever the buffer is given data. Unreal's ES 3.1 vertex factories read their streams
 * this way (FNAF Security Breach: "'samplerBuffer' : Illegal use of reserved word").
 */
#define ES31_TBW 4096
static char *es30_buffer_samplers(const char *src)
{
    if (!strstr(src, "samplerBuffer")) return NULL;
    size_t n = strlen(src);
    char names[16][64]; int nn = 0;
    /* the names of the buffer samplers, and the type renamed: [iu]samplerBuffer -> [iu]sampler2D */
    char *a = malloc(n + 64); size_t o = 0; size_t first_decl = (size_t)-1;
    for (size_t i = 0; i < n;) {
        if (!strncmp(src + i, "samplerBuffer", 13) && !es31_ident(src[i + 13]) && (i == 0 || !es31_ident(src[i - 1]) || src[i - 1] == 'i' || src[i - 1] == 'u')) {
            size_t ls = i; while (ls > 0 && src[ls - 1] != '\n') ls--;
            if (first_decl == (size_t)-1) { first_decl = o - (i - ls); }
            memcpy(a + o, "sampler2D", 9); o += 9; i += 13;
            size_t k = i; while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            size_t e = k; while (e < n && es31_ident(src[e])) e++;
            if (e > k && e - k < 64 && nn < 16) { memcpy(names[nn], src + k, e - k); names[nn][e - k] = 0; nn++; }
            continue;
        }
        a[o++] = src[i++];
    }
    a[o] = 0;
    if (!nn) { free(a); return NULL; }
    /* texelFetch(name, expr) -> texelFetch(name, husk_tb(expr), 0) */
    size_t cap = o * 2 + 512; char *b = malloc(cap); size_t r = 0;
    static const char helper[] = "highp ivec2 husk_tb(highp int i) { return ivec2(i & 4095, i >> 12); }\n";
    for (size_t i = 0; i < o;) {
        if (i == first_decl) { memcpy(b + r, helper, sizeof(helper) - 1); r += sizeof(helper) - 1; }
        if (!strncmp(a + i, "texelFetch", 10) && (i == 0 || !es31_ident(a[i - 1])) && !es31_ident(a[i + 10])) {
            size_t k = i + 10; while (k < o && a[k] == ' ') k++;
            if (k < o && a[k] == '(') {
                size_t ns = k + 1; while (ns < o && a[ns] == ' ') ns++;
                size_t ne = ns; while (ne < o && es31_ident(a[ne])) ne++;
                int hit = 0; for (int v = 0; v < nn; v++) if (strlen(names[v]) == ne - ns && !strncmp(a + ns, names[v], ne - ns)) hit = 1;
                size_t c = ne; while (c < o && a[c] == ' ') c++;
                if (hit && c < o && a[c] == ',') {
                    size_t es = c + 1, j = es; int depth = 0;
                    while (j < o) { if (a[j] == '(') depth++; else if (a[j] == ')') { if (!depth) break; depth--; } else if (a[j] == ',' && !depth) break; j++; }
                    if (j < o && a[j] == ')') {
                        if (r + (j - i) + 64 >= cap) { cap = cap * 2 + (j - i) + 64; b = realloc(b, cap); }
                        r += (size_t)sprintf(b + r, "texelFetch(%.*s, husk_tb(%.*s), 0)", (int)(ne - ns), a + ns, (int)(j - es), a + es);
                        i = j + 1; continue;
                    }
                }
            }
        }
        if (r + 2 >= cap) { cap *= 2; b = realloc(b, cap); }
        b[r++] = a[i++];
    }
    b[r] = 0;
    free(a);
    return b;
}

static void (*r_glShaderSource)(unsigned, int, const char *const *, const int *);
static void w_glShaderSource(unsigned sh, int count, const char *const *str, const int *len)
{
    if (es31_fake_shader(sh)) return;                               /* a compute stand-in: never compiled */
    if (count <= 0 || !str) { r_glShaderSource(sh, count, str, len); return; }
    size_t total = 0;
    for (int i = 0; i < count; i++) total += str[i] ? (len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i])) : 0;
    char *cat = malloc(total + 1); size_t k = 0;
    for (int i = 0; i < count; i++) if (str[i]) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(cat + k, str[i], l); k += l; }
    cat[k] = 0;
    /* A driver that really is 3.1 (ANGLE on the phone; the Mac's stops at 3.0): nothing is written down. A game that asked for 3.1
     * (the shim is on) has its 300 es shaders compiled as 310 es, which gives their in/out variables explicit locations -- UE4's
     * GLSL_ES3_1_ANDROID pairs stages by location, not by name (FNAF Security Breach quit on "'location' : invalid layout
     * qualifier"). All of them, so the stages of a program keep one version. */
    if (es31_real_minor() >= 1) {
        char *v = g_es31_shim ? strstr(cat, "#version 300 es") : NULL;
        if (v) { v[10] = '1'; v[11] = '0'; }                        /* "#version 300 es" -> "#version 310 es" */
        const char *one = cat; int one_len = (int)k;
        r_glShaderSource(sh, 1, &one, &one_len);
        free(cat);
        return;
    }
    es31_bind rec[ES31_BINDS]; int nrec = 0;
    char *down = es31_downgrade(cat, k, rec, &nrec);
    pthread_mutex_lock(&g_es31_mu);
    int slot = (int)(sh % ES31_SHADERS);
    g_es31_sh[slot].shader = sh; g_es31_sh[slot].n = nrec; memcpy(g_es31_sh[slot].b, rec, sizeof(es31_bind) * (size_t)nrec);
    pthread_mutex_unlock(&g_es31_mu);
    /* 3.0: varyings with locations become name-matched (see es30_link_by_location) */
    if (g_es31_shim && a_eglGetProcAddress) {
        static void (*getiv)(unsigned, unsigned, int *);
        if (!getiv) getiv = a_eglGetProcAddress("glGetShaderiv");
        int type = 0; if (getiv) getiv(sh, 0x8B4F /* GL_SHADER_TYPE */, &type);
        char *linked = type ? es30_link_by_location(down, type == 0x8B31 /* GL_VERTEX_SHADER */) : NULL;
        if (linked) { free(down); down = linked; }
        char *tb = es30_buffer_samplers(down);
        if (tb) { free(down); down = tb; }
    }
    const char *one = down; int one_len = (int)strlen(down);
    r_glShaderSource(sh, 1, &one, &one_len);
    free(cat); free(down);
}

static void (*r_glAttachShader)(unsigned, unsigned);
static void w_glAttachShader(unsigned prog, unsigned sh)
{
    if (es31_fake_shader(sh)) { pthread_mutex_lock(&g_es31_mu); fake_add(g_fake_prog, prog); pthread_mutex_unlock(&g_es31_mu); return; }
    r_glAttachShader(prog, sh);
    if (!g_es31_shim) return;
    pthread_mutex_lock(&g_es31_mu);
    g_es31_att[g_es31_natt % ES31_ATTACHED].prog = prog; g_es31_att[g_es31_natt % ES31_ATTACHED].shader = sh; g_es31_natt++;
    pthread_mutex_unlock(&g_es31_mu);
}

static void (*r_glLinkProgram)(unsigned);
static void w_glLinkProgram(unsigned prog)
{
    if (es31_fake_program(prog)) return;                            /* a compute stand-in links without the driver */
    r_glLinkProgram(prog);
    { static int trace = -1, n; if (trace < 0) trace = getenv("TL_TEX_TRACE") || getenv("TL_LINK_TRACE") ? 1 : 0;
      if (trace && a_eglGetProcAddress) {
          static void (*gpi)(unsigned, unsigned, int *), (*gpl)(unsigned, int, int *, char *);
          if (!gpi) { gpi = a_eglGetProcAddress("glGetProgramiv"); gpl = a_eglGetProcAddress("glGetProgramInfoLog"); }
          int ok = 1; if (gpi) gpi(prog, 0x8B82, &ok);
          if (!ok && n++ < 10) { char log[600] = ""; if (gpl) gpl(prog, sizeof(log), NULL, log); for (char *q = log; *q; q++) if (*q == '\n') *q = '|'; tl_log_line("gl: program %u did not link: %s", prog, log); }
      } }
    if (!g_es31_shim || !a_eglGetProcAddress) return;
    static int (*getloc)(unsigned, const char *); static unsigned (*getblk)(unsigned, const char *);
    static void (*blkbind)(unsigned, unsigned, unsigned), (*uni1i)(int, int), (*useprog)(unsigned), (*getprogiv)(unsigned, unsigned, int *);
    if (!getloc) {
        getloc = a_eglGetProcAddress("glGetUniformLocation"); getblk = a_eglGetProcAddress("glGetUniformBlockIndex");
        blkbind = a_eglGetProcAddress("glUniformBlockBinding"); uni1i = a_eglGetProcAddress("glUniform1i");
        useprog = a_eglGetProcAddress("glUseProgram"); getprogiv = a_eglGetProcAddress("glGetProgramiv");
    }
    if (!getloc || !getblk || !blkbind || !uni1i || !useprog || !getprogiv) return;
    int ok = 0; getprogiv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) return;
    int cur = 0; a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &cur);
    bool used = false;
    pthread_mutex_lock(&g_es31_mu);
    int total = g_es31_natt < ES31_ATTACHED ? g_es31_natt : ES31_ATTACHED;
    for (int a = 0; a < total; a++) {
        if (g_es31_att[a].prog != prog) continue;
        unsigned sh = g_es31_att[a].shader;
        int slot = (int)(sh % ES31_SHADERS);
        if (g_es31_sh[slot].shader != sh) continue;
        for (int b = 0; b < g_es31_sh[slot].n; b++) {
            const es31_bind *e = &g_es31_sh[slot].b[b];
            if (e->block) { unsigned idx = getblk(prog, e->name); if (idx != 0xFFFFFFFFu) blkbind(prog, idx, (unsigned)e->binding); }
            else { int loc = getloc(prog, e->name); if (loc >= 0) { if (!used) { useprog(prog); used = true; } uni1i(loc, e->binding); } }
        }
    }
    pthread_mutex_unlock(&g_es31_mu);
    if (used) useprog((unsigned)cur);
}

static void (*r_glGetIntegerv)(unsigned, int *);
static void w_glGetIntegerv(unsigned pname, int *data)
{
    r_glGetIntegerv(pname, data);
    if (g_es31_shim && pname == 0x821C /* GL_MINOR_VERSION */ && data && *data == 0) *data = g_es3_minor;
    /* the vertex-attribute-binding limits 3.1 has (emulated below) */
    if (data && es31_buffers()) {
        if (pname == 0x82DA /* GL_MAX_VERTEX_ATTRIB_BINDINGS */) *data = 16;
        else if (pname == 0x82D9 /* GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET */) *data = 2047;
        else if (pname == 0x82E5 /* GL_MAX_VERTEX_ATTRIB_STRIDE */) *data = 2048;
    }
}

/*
 * Separate vertex formats and buffer bindings (ES 3.1: glVertexAttribFormat, glVertexAttribBinding, glBindVertexBuffer) on a 3.0
 * driver. Unreal's OpenGL ES 3.1 renderer sets its vertex streams up this way; on 3.0 the calls fail, the attributes stay client arrays
 * with no data and ANGLE crashed in glDrawElements (FNAF Security Breach). Each attribute's format and its binding's buffer, offset,
 * stride and divisor are kept, and an attribute is set again with glVertexAttribPointer (its buffer bound for the call) whenever one
 * of them changes. One set of state for all vertex arrays: Unreal draws with one.
 */
static void (*r_glBindBuffer)(unsigned, unsigned);
static struct { int size, type, norm, integer, reloff, binding, has; } g_vab_attr[16];
static struct { unsigned buf; long off; int stride, divisor; } g_vab_bind[16];
static void vab_apply(unsigned a)
{
    if (a >= 16 || !g_vab_attr[a].has || !a_eglGetProcAddress) return;
    static void (*ptr)(unsigned, int, unsigned, unsigned char, int, const void *), (*iptr)(unsigned, int, unsigned, int, const void *), (*div)(unsigned, unsigned);
    if (!ptr) { ptr = a_eglGetProcAddress("glVertexAttribPointer"); iptr = a_eglGetProcAddress("glVertexAttribIPointer"); div = a_eglGetProcAddress("glVertexAttribDivisor"); }
    if (!ptr || !iptr) return;
    unsigned b = (unsigned)g_vab_attr[a].binding % 16;
    if (!r_glBindBuffer) r_glBindBuffer = a_eglGetProcAddress("glBindBuffer");
    int prev = 0; a_glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &prev);
    r_glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, g_vab_bind[b].buf);
    const void *at = (const void *)(uintptr_t)(g_vab_bind[b].off + g_vab_attr[a].reloff);
    if (g_vab_attr[a].integer) iptr(a, g_vab_attr[a].size, (unsigned)g_vab_attr[a].type, g_vab_bind[b].stride, at);
    else ptr(a, g_vab_attr[a].size, (unsigned)g_vab_attr[a].type, (unsigned char)g_vab_attr[a].norm, g_vab_bind[b].stride, at);
    if (div) div(a, (unsigned)g_vab_bind[b].divisor);
    r_glBindBuffer(0x8892, (unsigned)prev);
}
static void (*r_glClearBufferfv)(unsigned, int, const float *);
static void w_glClearBufferfv(unsigned buf, int drawbuf, const float *v)
{
    r_glClearBufferfv(buf, drawbuf, v);
    static long probe = -2; if (probe == -2) probe = getenv("TL_DRAW_PROBE") ? atol(getenv("TL_DRAW_PROBE")) : -1;
    if (probe > 0 && (long)atomic_load(&E.presented) == probe - 1) {
        int fb = 0, vp[4] = { 0 }; a_glGetIntegerv(0x8CA9, &fb); a_glGetIntegerv(0x0BA2, vp);
        int sc = 0, sb[4] = { 0 }; static unsigned char (*ie)(unsigned); if (!ie) ie = a_eglGetProcAddress("glIsEnabled"); sc = ie ? ie(0x0C11) : 0; a_glGetIntegerv(0x0C10, sb);
        tl_log_line("probe: clear %#x[%d] fb %d to %g %g %g %g (viewport %dx%d, scissor %d %d,%d %dx%d)", buf, drawbuf, fb, v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v ? v[3] : 0, vp[2], vp[3], sc, sb[0], sb[1], sb[2], sb[3]);
    }
}
/* TL_TEX_TRACE: a draw the driver refuses, with what the emulated vertex streams hold */
static void (*r_glDrawElements)(unsigned, int, unsigned, const void *);
static void w_glDrawElements(unsigned mode, int count, unsigned type, const void *idx)
{
    r_glDrawElements(mode, count, type, idx);
    /* TL_DRAW_PROBE=<swap number>: in that frame, after each draw, the pixel at the viewport's centre and what drew it */
    { static long probe = -2; if (probe == -2) probe = getenv("TL_DRAW_PROBE") ? atol(getenv("TL_DRAW_PROBE")) : -1;
      if (probe > 0 && (long)atomic_load(&E.presented) == probe - 1) {
          int fb = 0, prog = 0, vp[4] = { 0 }, t0 = 0, t1 = 0, act = 0; a_glGetIntegerv(0x8CA6, &fb); a_glGetIntegerv(0x8B8D, &prog); a_glGetIntegerv(0x0BA2, vp);
          a_glGetIntegerv(0x84E0, &act);
          static void (*at)(unsigned), (*rp)(int, int, int, int, unsigned, unsigned, void *), (*rb)(unsigned);
          if (!at) { at = a_eglGetProcAddress("glActiveTexture"); rp = a_eglGetProcAddress("glReadPixels"); rb = a_eglGetProcAddress("glReadBuffer"); }
          at(0x84C0); a_glGetIntegerv(0x8069, &t0); at(0x84C1); a_glGetIntegerv(0x8069, &t1); at((unsigned)act);
          int rfb = 0; a_glGetIntegerv(0x8CAA, &rfb);
          static void (*bfb)(unsigned, unsigned); if (!bfb) bfb = a_eglGetProcAddress("glBindFramebuffer");
          bfb(0x8CA8, (unsigned)fb);
          float pxf[4] = { 0 }; uint8_t px[4] = { 0 };
          rp(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1, 0x1908, 0x1401, px);
          unsigned (*ge)(void) = a_eglGetProcAddress("glGetError"); unsigned e = ge(); if (e) { rp(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1, 0x1908, 0x1406, pxf); e = ge(); }
          bfb(0x8CA8, (unsigned)rfb);
          tl_log_line("probe: draw %d idx fb %d vp %d,%d %dx%d prog %d tex0 %d tex1 %d -> %d %d %d %d | %g %g %g %g (err %#x)", count, fb, vp[0], vp[1], vp[2], vp[3], prog, t0, t1, px[0], px[1], px[2], px[3], pxf[0], pxf[1], pxf[2], pxf[3], e);
      } }
    static int trace = -1, n; if (trace < 0) trace = getenv("TL_TEX_TRACE") ? 1 : 0;
    if (!trace || n >= 12) return;
    unsigned (*ge)(void) = a_eglGetProcAddress("glGetError"); unsigned e = ge ? ge() : 0;
    if (!e) return;
    n++;
    char st[600] = ""; size_t k = 0;
    for (int a = 0; a < 16 && k < 560; a++) if (g_vab_attr[a].has) { unsigned b = (unsigned)g_vab_attr[a].binding % 16; k += (size_t)snprintf(st + k, sizeof(st) - k, " a%d:b%u buf%u off%ld str%d", a, b, g_vab_bind[b].buf, g_vab_bind[b].off + g_vab_attr[a].reloff, g_vab_bind[b].stride); }
    int vao = 0, ebo = 0, prog = 0; a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao); a_glGetIntegerv(0x8895, &ebo); a_glGetIntegerv(0x8B8D, &prog);
    tl_log_line("gl: glDrawElements(%#x, %d) -> error %#x; vao %d ebo %d program %d;%s", mode, count, e, vao, ebo, prog, st);
}
static void (*r_glVertexAttribFormat)(unsigned, int, unsigned, unsigned char, unsigned);
static void w_glVertexAttribFormat(unsigned a, int size, unsigned type, unsigned char norm, unsigned rel)
{
    if (!es31_buffers()) { if (r_glVertexAttribFormat) r_glVertexAttribFormat(a, size, type, norm, rel); return; }
    if (a >= 16) return;
    g_vab_attr[a].size = size; g_vab_attr[a].type = (int)type; g_vab_attr[a].norm = norm; g_vab_attr[a].integer = 0; g_vab_attr[a].reloff = (int)rel; g_vab_attr[a].has = 1;
    vab_apply(a);
}
static void (*r_glVertexAttribIFormat)(unsigned, int, unsigned, unsigned);
static void w_glVertexAttribIFormat(unsigned a, int size, unsigned type, unsigned rel)
{
    if (!es31_buffers()) { if (r_glVertexAttribIFormat) r_glVertexAttribIFormat(a, size, type, rel); return; }
    if (a >= 16) return;
    g_vab_attr[a].size = size; g_vab_attr[a].type = (int)type; g_vab_attr[a].norm = 0; g_vab_attr[a].integer = 1; g_vab_attr[a].reloff = (int)rel; g_vab_attr[a].has = 1;
    vab_apply(a);
}
static void (*r_glVertexAttribBinding)(unsigned, unsigned);
static void w_glVertexAttribBinding(unsigned a, unsigned b)
{
    if (!es31_buffers()) { if (r_glVertexAttribBinding) r_glVertexAttribBinding(a, b); return; }
    if (a >= 16) return;
    g_vab_attr[a].binding = (int)b;
    vab_apply(a);
}
static void (*r_glBindVertexBuffer)(unsigned, unsigned, long, int);
static void w_glBindVertexBuffer(unsigned b, unsigned buf, long off, int stride)
{
    if (!es31_buffers()) { if (r_glBindVertexBuffer) r_glBindVertexBuffer(b, buf, off, stride); return; }
    if (b >= 16) return;
    g_vab_bind[b].buf = buf; g_vab_bind[b].off = off; g_vab_bind[b].stride = stride;
    for (unsigned a = 0; a < 16; a++) if (g_vab_attr[a].has && (unsigned)g_vab_attr[a].binding == b) vab_apply(a);
}
static void (*r_glVertexBindingDivisor)(unsigned, unsigned);
static void w_glVertexBindingDivisor(unsigned b, unsigned d)
{
    if (!es31_buffers()) { if (r_glVertexBindingDivisor) r_glVertexBindingDivisor(b, d); return; }
    if (b >= 16) return;
    g_vab_bind[b].divisor = (int)d;
    for (unsigned a = 0; a < 16; a++) if (g_vab_attr[a].has && (unsigned)g_vab_attr[a].binding == b) vab_apply(a);
}

/* The storage-buffer target (a block of the game's data) is a uniform buffer here; a buffer the game made for it is grown to what the
 * 1024-element block above needs, so the binding is never smaller than the block. */
static void es31_note_storage(const char *call, unsigned target)
{
    static atomic_int said;
    if (es31_buffers() && target == 0x90D2 && atomic_fetch_add(&said, 1) < 12) tl_log_line("es31: %s on the storage-buffer target", call);
}
#define ES31_UBO_MIN (1024 * 32)
static void (*r_glBindBufferBase)(unsigned, unsigned, unsigned);
static void w_glBindBufferBase(unsigned target, unsigned index, unsigned buffer)
{
    es31_note_storage("glBindBufferBase", target);
    if (es31_buffers() && target == 0x90D2 /* GL_SHADER_STORAGE_BUFFER */) {
        static void (*getparam)(unsigned, unsigned, int *), (*bindbuf)(unsigned, unsigned), (*bufdata)(unsigned, long, const void *, unsigned),
                    (*copysub)(unsigned, unsigned, long, long, long), (*getsub)(unsigned, long, long, void *), (*subdata)(unsigned, long, long, const void *);
        if (!getparam && a_eglGetProcAddress) {
            getparam = a_eglGetProcAddress("glGetBufferParameteriv"); bindbuf = a_eglGetProcAddress("glBindBuffer");
            bufdata = a_eglGetProcAddress("glBufferData"); getsub = a_eglGetProcAddress("glGetBufferSubData"); subdata = a_eglGetProcAddress("glBufferSubData");
            copysub = a_eglGetProcAddress("glCopyBufferSubData");
        }
        if (getparam && bindbuf && bufdata && subdata && copysub && buffer) {
            int size = 0, usage = 0x88E4; /* GL_STATIC_DRAW */
            bindbuf(0x8F36 /* GL_COPY_WRITE_BUFFER */, buffer);
            getparam(0x8F36, 0x8764 /* GL_BUFFER_SIZE */, &size);
            getparam(0x8F36, 0x8765 /* GL_BUFFER_USAGE */, &usage);
            if (size > 0 && size < ES31_UBO_MIN) {
                unsigned tmp = 0; void (*gen)(int, unsigned *) = a_eglGetProcAddress("glGenBuffers"); void (*del)(int, const unsigned *) = a_eglGetProcAddress("glDeleteBuffers");
                if (gen && del) {
                    gen(1, &tmp);
                    bindbuf(0x8F37 /* GL_COPY_READ_BUFFER */, tmp);
                    bufdata(0x8F37, size, NULL, 0x88E4);
                    copysub(0x8F36, 0x8F37, 0, 0, size);
                    bufdata(0x8F36, ES31_UBO_MIN, NULL, (unsigned)usage);
                    copysub(0x8F37, 0x8F36, 0, 0, size);
                    del(1, &tmp);
                }
            }
            bindbuf(0x8F36, 0);
        }
        target = 0x8A11; /* GL_UNIFORM_BUFFER */
    }
    r_glBindBufferBase(target, index, buffer);
}

/* Buffer textures, the GL side (see es30_buffer_samplers). The texture keeps its name and is a 2D one here. */
#define ES31_TB 1024
static struct { unsigned tex, buf, ifmt; long off, size; int upload_float; } g_tb[ES31_TB];
static unsigned g_tb_bound, g_buf_bound_tb;                       /* the last buffer texture bound; the buffer bound to GL_TEXTURE_BUFFER */
static void (*r_glBindTexture)(unsigned, unsigned);
static bool tb_format(unsigned ifmt, unsigned *fmt, unsigned *type, int *bpp)
{
    switch (ifmt) {
    case 0x822E: *fmt = 0x1903; *type = 0x1406; *bpp = 4; return true;   /* R32F */
    case 0x8230: *fmt = 0x8227; *type = 0x1406; *bpp = 8; return true;   /* RG32F */
    case 0x8814: *fmt = 0x1908; *type = 0x1406; *bpp = 16; return true;  /* RGBA32F */
    case 0x8815: *fmt = 0x1907; *type = 0x1406; *bpp = 12; return true;  /* RGB32F */
    case 0x881A: *fmt = 0x1908; *type = 0x140B; *bpp = 8; return true;   /* RGBA16F */
    case 0x881B: *fmt = 0x1907; *type = 0x140B; *bpp = 6; return true;   /* RGB16F */
    case 0x8D7E: *fmt = 0x8D98; *type = 0x1405; *bpp = 12; return true;  /* RGB32UI */
    case 0x8D83: *fmt = 0x8D98; *type = 0x1404; *bpp = 12; return true;  /* RGB32I */
    case 0x8F97: *fmt = 0x1908; *type = 0x1400; *bpp = 4; return true;   /* RGBA8_SNORM */
    case 0x8D8E: *fmt = 0x8D99; *type = 0x1400; *bpp = 4; return true;   /* RGBA8I */
    case 0x8D88: *fmt = 0x8D99; *type = 0x1402; *bpp = 8; return true;   /* RGBA16I */
    case 0x822D: *fmt = 0x1903; *type = 0x140B; *bpp = 2; return true;   /* R16F */
    case 0x822F: *fmt = 0x8227; *type = 0x140B; *bpp = 4; return true;   /* RG16F */
    case 0x8058: *fmt = 0x1908; *type = 0x1401; *bpp = 4; return true;   /* RGBA8 */
    case 0x8229: *fmt = 0x1903; *type = 0x1401; *bpp = 1; return true;   /* R8 */
    case 0x822B: *fmt = 0x8227; *type = 0x1401; *bpp = 2; return true;   /* RG8 */
    case 0x8236: *fmt = 0x8D94; *type = 0x1405; *bpp = 4; return true;   /* R32UI */
    case 0x823C: *fmt = 0x8228; *type = 0x1405; *bpp = 8; return true;   /* RG32UI */
    case 0x8D70: *fmt = 0x8D99; *type = 0x1405; *bpp = 16; return true;  /* RGBA32UI */
    case 0x8235: *fmt = 0x8D94; *type = 0x1404; *bpp = 4; return true;   /* R32I */
    case 0x823B: *fmt = 0x8228; *type = 0x1404; *bpp = 8; return true;   /* RG32I */
    case 0x8D82: *fmt = 0x8D99; *type = 0x1404; *bpp = 16; return true;  /* RGBA32I */
    case 0x8234: *fmt = 0x8D94; *type = 0x1403; *bpp = 2; return true;   /* R16UI */
    case 0x8D76: *fmt = 0x8D99; *type = 0x1403; *bpp = 8; return true;   /* RGBA16UI */
    case 0x8232: *fmt = 0x8D94; *type = 0x1401; *bpp = 1; return true;   /* R8UI */
    case 0x8D7C: *fmt = 0x8D99; *type = 0x1401; *bpp = 4; return true;   /* RGBA8UI */
    }
    return false;
}
/* the buffer's bytes into slot k's texture, on the GPU; storage (re)made when the size changes */
static void tb_upload(int k, bool realloc_storage)
{
    static void (*getiv)(unsigned, int *), (*getbp)(unsigned, unsigned, int *), (*teximg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *),
                (*texsub)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*texpar)(unsigned, unsigned, int), (*pstore)(unsigned, int);
    if (!getiv && a_eglGetProcAddress) {
        getiv = a_eglGetProcAddress("glGetIntegerv"); getbp = a_eglGetProcAddress("glGetBufferParameteriv"); teximg = a_eglGetProcAddress("glTexImage2D");
        texsub = a_eglGetProcAddress("glTexSubImage2D"); texpar = a_eglGetProcAddress("glTexParameteri"); pstore = a_eglGetProcAddress("glPixelStorei");
    }
    if (!r_glBindTexture) r_glBindTexture = a_eglGetProcAddress("glBindTexture");
    if (!r_glBindBuffer) r_glBindBuffer = a_eglGetProcAddress("glBindBuffer");
    if (!getiv || !getbp || !teximg || !texsub || !texpar || !pstore || !r_glBindTexture || !r_glBindBuffer) return;
    unsigned fmt, type; int bpp;
    if (!tb_format(g_tb[k].ifmt, &fmt, &type, &bpp)) { static int said; if (!said++) tl_log_line("es31: buffer texture format %#x not handled", g_tb[k].ifmt); return; }
    if (g_tb[k].upload_float) { type = 0x1406; bpp = 4 * g_tb[k].upload_float; fmt = g_tb[k].upload_float == 3 ? 0x1907 : 0x1908; }   /* 32-bit float data into the RGBA16F stand-in */
    int old_tex = 0, old_unpack = 0, old_align = 4, old_row = 0;
    getiv(0x8069 /* GL_TEXTURE_BINDING_2D */, &old_tex); getiv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &old_unpack);
    getiv(0x0CF5 /* GL_UNPACK_ALIGNMENT */, &old_align); getiv(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, &old_row);
    r_glBindBuffer(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, g_tb[k].buf);
    long size = g_tb[k].size;
    if (size <= 0) { int bs = 0; getbp(0x88EC, 0x8764 /* GL_BUFFER_SIZE */, &bs); size = bs - g_tb[k].off; }
    long texels = size > 0 ? size / bpp : 0;
    int rows = (int)((texels + ES31_TBW - 1) / ES31_TBW); if (rows < 1) rows = 1;
    static unsigned (*geterr)(void); if (!geterr) geterr = a_eglGetProcAddress("glGetError");
    static int dbg = -1; if (dbg < 0) dbg = getenv("TL_TB_TRACE") ? 1 : 0;
#define TBE(tag) if (dbg && geterr) { unsigned e_ = geterr(); if (e_) tl_log_line("es31: buffer texture: GL error %#x at %s", e_, tag); }
    TBE("start")
    r_glBindTexture(0x0DE1, g_tb[k].tex); TBE("bind texture")
    if (realloc_storage) {
        r_glBindBuffer(0x88EC, 0);
        if (geterr) while (geterr()) {}
        teximg(0x0DE1, 0, (int)g_tb[k].ifmt, ES31_TBW, rows, 0, fmt, type, NULL);
        /* storage a driver refuses for 32-bit float texels: the 16-bit float format with as many channels, converted on upload (half precision) */
        if ((g_tb[k].ifmt == 0x8814 || g_tb[k].ifmt == 0x8815) && geterr && geterr()) { g_tb[k].upload_float = g_tb[k].ifmt == 0x8815 ? 3 : 4; g_tb[k].ifmt = g_tb[k].upload_float == 3 ? 0x881B : 0x881A; teximg(0x0DE1, 0, (int)g_tb[k].ifmt, ES31_TBW, rows, 0, g_tb[k].upload_float == 3 ? 0x1907 : 0x1908, 0x1406, NULL); }
        TBE("storage")
        texpar(0x0DE1, 0x2801, 0x2600); texpar(0x0DE1, 0x2800, 0x2600);   /* NEAREST, no mipmaps: complete for texelFetch */
        r_glBindBuffer(0x88EC, g_tb[k].buf);
    }
    pstore(0x0CF5, 1); pstore(0x0CF2, 0);
    long full = texels / ES31_TBW, rest = texels % ES31_TBW;
    if (full) texsub(0x0DE1, 0, 0, 0, ES31_TBW, (int)full, fmt, type, (const void *)(uintptr_t)g_tb[k].off); TBE("rows")
    if (rest) texsub(0x0DE1, 0, 0, (int)full, (int)rest, 1, fmt, type, (const void *)(uintptr_t)(g_tb[k].off + full * ES31_TBW * bpp)); TBE("last row")
    pstore(0x0CF5, old_align); pstore(0x0CF2, old_row);
    r_glBindBuffer(0x88EC, (unsigned)old_unpack);
    r_glBindTexture(0x0DE1, (unsigned)old_tex);
}
static void tb_refresh_buffer(unsigned buf)
{
    if (!buf) return;
    for (int k = 0; k < ES31_TB; k++) if (g_tb[k].tex && g_tb[k].buf == buf) tb_upload(k, false);
}
/* A buffer texture's slot is freed with the texture (or the name bound as an ordinary texture, which is a new one by that name): the names
 * come back, and an entry left behind went on filling an engine's ordinary texture of that name from a buffer, until the table ran out and the
 * buffer textures after that were never filled (FNAF Security Breach, a minute into the game, then a crash). */
static void tb_forget_texture(unsigned tex)
{
    for (int k = 0; k < ES31_TB && tex; k++) if (g_tb[k].tex == tex) memset(&g_tb[k], 0, sizeof(g_tb[k]));
    if (g_tb_bound == tex) g_tb_bound = 0;
}
static void w_glBindTexture(unsigned target, unsigned tex)
{
    if (target == 0x8C2A /* GL_TEXTURE_BUFFER */ && es31_buffers()) { g_tb_bound = tex; r_glBindTexture(0x0DE1, tex); return; }
    if (tex && es31_buffers() && (target == 0x0DE1 || target == 0x8513 || target == 0x806F || target == 0x8C1A)) tb_forget_texture(tex);
    r_glBindTexture(target, tex);
}
static void (*r_glDeleteTextures)(int, const unsigned *);
static void w_glDeleteTextures(int n, const unsigned *names)
{
    if (es31_buffers() && names) for (int i = 0; i < n; i++) tb_forget_texture(names[i]);
    r_glDeleteTextures(n, names);
}
static void (*r_glDeleteBuffers)(int, const unsigned *);
static void w_glDeleteBuffers(int n, const unsigned *names)
{
    if (es31_buffers() && names) for (int i = 0; i < n; i++) for (int k = 0; k < ES31_TB && names[i]; k++) if (g_tb[k].buf == names[i]) g_tb[k].buf = 0;
    r_glDeleteBuffers(n, names);
}
static void tb_attach(unsigned ifmt, unsigned buf, long off, long size)
{
    if (!g_tb_bound) return;
    int k = -1;
    for (int i = 0; i < ES31_TB && k < 0; i++) if (g_tb[i].tex == g_tb_bound) k = i;
    for (int i = 0; i < ES31_TB && k < 0; i++) if (!g_tb[i].tex) k = i;
    if (k < 0) { static int said; if (!said++) tl_log_line("es31: more than %d buffer textures", ES31_TB); return; }
    g_tb[k].tex = g_tb_bound; g_tb[k].buf = buf; g_tb[k].ifmt = ifmt; g_tb[k].off = off; g_tb[k].size = size; g_tb[k].upload_float = 0;
    static atomic_int said; if (atomic_fetch_add(&said, 1) == 0) tl_log_line("es31: buffer textures are 2D textures here (first: format %#x)", ifmt);
    tb_upload(k, true);
}
static void (*r_glTexBuffer)(unsigned, unsigned, unsigned);
static void w_glTexBuffer(unsigned target, unsigned ifmt, unsigned buf) { if (es31_buffers()) { tb_attach(ifmt, buf, 0, 0); return; } if (r_glTexBuffer) r_glTexBuffer(target, ifmt, buf); }
static void (*r_glTexBufferEXT)(unsigned, unsigned, unsigned);
static void w_glTexBufferEXT(unsigned target, unsigned ifmt, unsigned buf) { if (es31_buffers()) { tb_attach(ifmt, buf, 0, 0); return; } if (r_glTexBufferEXT) r_glTexBufferEXT(target, ifmt, buf); }
static void (*r_glTexBufferOES)(unsigned, unsigned, unsigned);
static void w_glTexBufferOES(unsigned target, unsigned ifmt, unsigned buf) { if (es31_buffers()) { tb_attach(ifmt, buf, 0, 0); return; } if (r_glTexBufferOES) r_glTexBufferOES(target, ifmt, buf); }
static void (*r_glTexBufferRange)(unsigned, unsigned, unsigned, long, long);
static void w_glTexBufferRange(unsigned target, unsigned ifmt, unsigned buf, long off, long size) { if (es31_buffers()) { tb_attach(ifmt, buf, off, size); return; } if (r_glTexBufferRange) r_glTexBufferRange(target, ifmt, buf, off, size); }
static void (*r_glTexBufferRangeEXT)(unsigned, unsigned, unsigned, long, long);
static void w_glTexBufferRangeEXT(unsigned target, unsigned ifmt, unsigned buf, long off, long size) { if (es31_buffers()) { tb_attach(ifmt, buf, off, size); return; } if (r_glTexBufferRangeEXT) r_glTexBufferRangeEXT(target, ifmt, buf, off, size); }

/* which buffer each target holds, for the data calls below to find it */
static unsigned g_buf_on[8];
static int buf_slot(unsigned t)
{
    switch (t) { case 0x8892: return 0; case 0x8893: return 1; case 0x8F36: return 2; case 0x8F37: return 3; case 0x8A11: return 4; case 0x8C2A: return 5; case 0x88EC: return 6; case 0x90D2: return 7; }
    return -1;
}
static unsigned buf_on(unsigned t) { int s = buf_slot(t); return s >= 0 ? g_buf_on[s] : 0; }
/* GL_TEXTURE_BUFFER as a buffer target does not exist on 3.0: the copy-write target stands in for it */
#define ES31_TBTARGET(t) ((t) == 0x8C2A ? 0x8F37u : (t))

/* The other calls that name the storage target: it is the uniform one here. */
#define ES31_STORAGE(t) ((t) == 0x90D2 ? 0x8A11u : (t))
static void w_glBindBuffer(unsigned t, unsigned b)
{
    es31_note_storage("glBindBuffer", t);
    if (!es31_buffers()) { r_glBindBuffer(t, b); return; }
    int sl = buf_slot(t); if (sl >= 0) g_buf_on[sl] = b;
    r_glBindBuffer(ES31_TBTARGET(ES31_STORAGE(t)), b);
}
static void (*r_glBufferData)(unsigned, long, const void *, unsigned);
static void w_glBufferData(unsigned t, long n, const void *d, unsigned u)
{
    es31_note_storage("glBufferData", t);
    if (!es31_buffers()) { r_glBufferData(t, n, d, u); return; }
    r_glBufferData(ES31_TBTARGET(ES31_STORAGE(t)), n, d, u);
    unsigned b = buf_on(t);
    for (int k = 0; k < ES31_TB; k++) if (g_tb[k].tex && g_tb[k].buf == b && b) tb_upload(k, true);
}
static void (*r_glBufferSubData)(unsigned, long, long, const void *);
static void w_glBufferSubData(unsigned t, long o, long n, const void *d)
{
    es31_note_storage("glBufferSubData", t);
    if (!es31_buffers()) { r_glBufferSubData(t, o, n, d); return; }
    r_glBufferSubData(ES31_TBTARGET(ES31_STORAGE(t)), o, n, d);
    tb_refresh_buffer(buf_on(t));
}
static void *(*r_glMapBufferRange)(unsigned, long, long, unsigned);
static void *w_glMapBufferRange(unsigned t, long o, long n, unsigned access) { return r_glMapBufferRange(es31_buffers() ? ES31_TBTARGET(ES31_STORAGE(t)) : t, o, n, access); }
static unsigned char (*r_glUnmapBuffer)(unsigned);
static unsigned char w_glUnmapBuffer(unsigned t)
{
    if (!es31_buffers()) return r_glUnmapBuffer(t);
    unsigned char ok = r_glUnmapBuffer(ES31_TBTARGET(ES31_STORAGE(t)));
    tb_refresh_buffer(buf_on(t));
    return ok;
}
static void (*r_glCopyBufferSubData)(unsigned, unsigned, long, long, long);
static void w_glCopyBufferSubData(unsigned rt, unsigned wt, long ro, long wo, long n)
{
    if (!es31_buffers()) { r_glCopyBufferSubData(rt, wt, ro, wo, n); return; }
    r_glCopyBufferSubData(ES31_TBTARGET(ES31_STORAGE(rt)), ES31_TBTARGET(ES31_STORAGE(wt)), ro, wo, n);
    tb_refresh_buffer(buf_on(wt));
}
static void (*r_glBindBufferRange)(unsigned, unsigned, unsigned, long, long);
static void w_glBindBufferRange(unsigned t, unsigned i, unsigned b, long o, long n)
{
    es31_note_storage("glBindBufferRange", t);
    if (es31_buffers() && t == 0x90D2) { w_glBindBufferBase(t, i, b); return; }   /* grows the buffer if need be; the whole buffer is bound */
    r_glBindBufferRange(t, i, b, o, n);
}
