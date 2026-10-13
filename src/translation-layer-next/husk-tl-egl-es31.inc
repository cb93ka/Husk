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

static void (*r_glShaderSource)(unsigned, int, const char *const *, const int *);
static void w_glShaderSource(unsigned sh, int count, const char *const *str, const int *len)
{
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
    }
    const char *one = down; int one_len = (int)strlen(down);
    r_glShaderSource(sh, 1, &one, &one_len);
    free(cat); free(down);
}

static void (*r_glAttachShader)(unsigned, unsigned);
static void w_glAttachShader(unsigned prog, unsigned sh)
{
    r_glAttachShader(prog, sh);
    if (!g_es31_shim) return;
    pthread_mutex_lock(&g_es31_mu);
    g_es31_att[g_es31_natt % ES31_ATTACHED].prog = prog; g_es31_att[g_es31_natt % ES31_ATTACHED].shader = sh; g_es31_natt++;
    pthread_mutex_unlock(&g_es31_mu);
}

static void (*r_glLinkProgram)(unsigned);
static void w_glLinkProgram(unsigned prog)
{
    r_glLinkProgram(prog);
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
    int prev = 0; r_glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &prev);
    r_glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, g_vab_bind[b].buf);
    const void *at = (const void *)(uintptr_t)(g_vab_bind[b].off + g_vab_attr[a].reloff);
    if (g_vab_attr[a].integer) iptr(a, g_vab_attr[a].size, (unsigned)g_vab_attr[a].type, g_vab_bind[b].stride, at);
    else ptr(a, g_vab_attr[a].size, (unsigned)g_vab_attr[a].type, (unsigned char)g_vab_attr[a].norm, g_vab_bind[b].stride, at);
    if (div) div(a, (unsigned)g_vab_bind[b].divisor);
    r_glBindBuffer(0x8892, (unsigned)prev);
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

/* The other calls that name the storage target: it is the uniform one here. */
#define ES31_STORAGE(t) ((t) == 0x90D2 ? 0x8A11u : (t))
static void (*r_glBindBuffer)(unsigned, unsigned);
static void w_glBindBuffer(unsigned t, unsigned b) { es31_note_storage("glBindBuffer", t); r_glBindBuffer(es31_buffers() ? ES31_STORAGE(t) : t, b); }
static void (*r_glBufferData)(unsigned, long, const void *, unsigned);
static void w_glBufferData(unsigned t, long n, const void *d, unsigned u) { es31_note_storage("glBufferData", t); r_glBufferData(es31_buffers() ? ES31_STORAGE(t) : t, n, d, u); }
static void (*r_glBufferSubData)(unsigned, long, long, const void *);
static void w_glBufferSubData(unsigned t, long o, long n, const void *d) { es31_note_storage("glBufferSubData", t); r_glBufferSubData(es31_buffers() ? ES31_STORAGE(t) : t, o, n, d); }
static void (*r_glBindBufferRange)(unsigned, unsigned, unsigned, long, long);
static void w_glBindBufferRange(unsigned t, unsigned i, unsigned b, long o, long n)
{
    es31_note_storage("glBindBufferRange", t);
    if (es31_buffers() && t == 0x90D2) { w_glBindBufferBase(t, i, b); return; }   /* grows the buffer if need be; the whole buffer is bound */
    r_glBindBufferRange(t, i, b, o, n);
}
