/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SharedPreferences that keep what is put in them, as Android's do.
 *
 * Until now every get answered the caller's default and every put was dropped, so whatever a game kept there was gone at the next
 * start -- Unity's PlayerPrefs among it (GoreBox asked for the language and the birth date every time, and kept no settings).
 * Each named file lives where Android keeps it, <data>/shared_prefs/<name>.xml, in Android's own XML: one entry a line, written
 * whole on every apply or commit (the sets are small), read the first time the name is asked for.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void tl_log_line(const char *fmt, ...);

enum { T_STR, T_INT, T_LONG, T_FLOAT, T_BOOL };
typedef struct { char *key; int type; char *s; int64_t i; float f; } entry;
typedef struct store { char name[160]; entry *e; int n, cap; struct store *next; } store;

static store *g_stores;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_dir[600];

void tl_prefs_set_data_dir(const char *data)
{
    snprintf(g_dir, sizeof(g_dir), "%s/shared_prefs", data ? data : ".");
    mkdir(g_dir, 0755);
}

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static jvalue vf(float f) { jvalue v; v.j = 0; v.f = f; return v; }
static jvalue vj(int64_t j) { jvalue v; v.j = j; return v; }
static const char *S(const void *o) { const char *s = o ? tl_jni_string((jobj *)o) : NULL; return s ? s : ""; }

/* ------------------------------------------------------------------ file */

static void path_of(const store *st, char *out, size_t n)
{
    char safe[160]; size_t k = 0;
    for (const char *p = st->name; *p && k + 1 < sizeof(safe); p++) safe[k++] = (*p == '/') ? '_' : *p;
    safe[k] = 0;
    snprintf(out, n, "%s/%s.xml", g_dir, safe);
}

static void xml_put(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': fputs("&amp;", f); break; case '<': fputs("&lt;", f); break; case '>': fputs("&gt;", f); break;
        case '"': fputs("&quot;", f); break; case '\n': fputs("&#10;", f); break; default: fputc(*s, f);
        }
    }
}
static char *xml_get(const char *s, size_t n)
{
    char *o = malloc(n + 1); size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { o[k++] = s[i]; continue; }
        if (!strncmp(s + i, "&amp;", 5)) { o[k++] = '&'; i += 4; }
        else if (!strncmp(s + i, "&lt;", 4)) { o[k++] = '<'; i += 3; }
        else if (!strncmp(s + i, "&gt;", 4)) { o[k++] = '>'; i += 3; }
        else if (!strncmp(s + i, "&quot;", 6)) { o[k++] = '"'; i += 5; }
        else if (!strncmp(s + i, "&apos;", 6)) { o[k++] = '\''; i += 5; }
        else if (!strncmp(s + i, "&#10;", 5)) { o[k++] = '\n'; i += 4; }
        else o[k++] = '&';
    }
    o[k] = 0;
    return o;
}

static entry *find(store *st, const char *key)
{
    for (int i = 0; i < st->n; i++) if (!strcmp(st->e[i].key, key)) return &st->e[i];
    return NULL;
}
static entry *slot(store *st, const char *key)
{
    entry *e = find(st, key);
    if (e) { if (e->type == T_STR) { free(e->s); e->s = NULL; } return e; }
    if (st->n == st->cap) { st->cap = st->cap ? st->cap * 2 : 16; st->e = realloc(st->e, (size_t)st->cap * sizeof(entry)); }
    e = &st->e[st->n++];
    memset(e, 0, sizeof(*e));
    e->key = strdup(key);
    return e;
}
static void drop(store *st, const char *key)
{
    entry *e = find(st, key);
    if (!e) return;
    free(e->key); free(e->s);
    *e = st->e[--st->n];
}

/* An attribute's value from a line: name="..." */
static char *attr(const char *line, const char *name)
{
    char pat[32]; snprintf(pat, sizeof(pat), " %s=\"", name);
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    return q ? xml_get(p, (size_t)(q - p)) : NULL;
}

static void load(store *st)
{
    char path[800]; path_of(st, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        const char *t = line; while (*t == ' ' || *t == '\t') t++;
        int type = !strncmp(t, "<string ", 8) ? T_STR : !strncmp(t, "<int ", 5) ? T_INT : !strncmp(t, "<long ", 6) ? T_LONG
                 : !strncmp(t, "<float ", 7) ? T_FLOAT : !strncmp(t, "<boolean ", 9) ? T_BOOL : -1;
        if (type < 0) continue;
        char *key = attr(t, "name");
        if (!key) continue;
        entry *e = slot(st, key);
        e->type = type;
        if (type == T_STR) {
            const char *a = strchr(t, '>'), *b = strstr(t, "</string>");
            if (a && b && b > a) e->s = xml_get(a + 1, (size_t)(b - a - 1));
            else if (a && a[-1] == '/') e->s = strdup("");
            else e->s = strdup(a ? a + 1 : "");
        } else {
            char *v = attr(t, "value");
            if (type == T_INT || type == T_LONG) e->i = v ? strtoll(v, NULL, 10) : 0;
            else if (type == T_FLOAT) e->f = v ? strtof(v, NULL) : 0;
            else e->i = v && !strcmp(v, "true");
            free(v);
        }
        free(key);
    }
    free(line);
    fclose(f);
}

static void save(const store *st)
{
    char path[800], tmp[820]; path_of(st, path, sizeof(path)); snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { tl_log_line("prefs: cannot write %s", tmp); return; }
    fputs("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n", f);
    for (int i = 0; i < st->n; i++) {
        const entry *e = &st->e[i];
        static const char *const tag[] = { "string", "int", "long", "float", "boolean" };
        fprintf(f, "    <%s name=\"", tag[e->type]); xml_put(f, e->key);
        if (e->type == T_STR) { fputs("\">", f); xml_put(f, e->s ? e->s : ""); fputs("</string>\n", f); }
        else if (e->type == T_FLOAT) fprintf(f, "\" value=\"%.9g\" />\n", e->f);
        else if (e->type == T_BOOL) fprintf(f, "\" value=\"%s\" />\n", e->i ? "true" : "false");
        else fprintf(f, "\" value=\"%lld\" />\n", (long long)e->i);
    }
    fputs("</map>\n", f);
    fclose(f);
    rename(tmp, path);                                      /* whole or not at all: a crash mid-write keeps the last copy */
}

/* The store a SharedPreferences object names, loaded the first time. Called with g_mu held. */
static store *store_named(const char *name)
{
    for (store *s = g_stores; s; s = s->next) if (!strcmp(s->name, name)) return s;
    store *s = calloc(1, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    if (!g_dir[0]) tl_prefs_set_data_dir(".");
    load(s);
    s->next = g_stores; g_stores = s;
    return s;
}
static const char *name_of(const jobj *self)
{
    return self ? S(tl_jni_get_field((jobj *)self, "name", "Ljava/lang/String;").l) : "";
}

/* ------------------------------------------------------- SharedPreferences */

static jobj *prefs_object(const char *name)
{
    jobj *p = tl_jni_new_object(tl_jni_class("android/content/SharedPreferences"));
    tl_jni_set_field(p, "name", "Ljava/lang/String;", vl(tl_jni_new_string(name)));
    return p;
}
static void Context_getSharedPreferences(tl_jcall *c) { c->ret = vl(prefs_object(S(c->args[0].l))); }
/* PreferenceManager.getDefaultSharedPreferences(context): <package>_preferences, as Android names it. */
static void PM_getDefault(tl_jcall *c)
{
    const char *pkg = c->args[0].l ? S(tl_jni_call((jobj *)c->args[0].l, "getPackageName", "()Ljava/lang/String;", NULL).l) : "";
    char n[200]; snprintf(n, sizeof(n), "%s_preferences", pkg[0] ? pkg : "app");
    c->ret = vl(prefs_object(n));
}
/* Activity.getPreferences(mode): a file named after the activity's class. */
static void Activity_getPreferences(tl_jcall *c) { c->ret = vl(prefs_object(c->self ? tl_jni_class_name(c->self) : "activity")); }

#define GET(fn, cond, val, deflt) \
    static void fn(tl_jcall *c) { \
        pthread_mutex_lock(&g_mu); \
        store *st = store_named(name_of(c->self)); entry *e = find(st, S(c->args[0].l)); \
        jvalue r = (e && (cond)) ? (val) : (deflt); \
        pthread_mutex_unlock(&g_mu); \
        c->ret = r; }
GET(SP_getInt, e->type == T_INT || e->type == T_LONG, vi((int)e->i), vi(c->args[1].i))
GET(SP_getLong, e->type == T_INT || e->type == T_LONG, vj(e->i), vj(c->args[1].j))
GET(SP_getFloat, e->type == T_FLOAT, vf(e->f), vf(c->args[1].f))
GET(SP_getBoolean, e->type == T_BOOL, vz((int)e->i), vz(c->args[1].z))
static void SP_getString(tl_jcall *c)
{
    pthread_mutex_lock(&g_mu);
    store *st = store_named(name_of(c->self)); entry *e = find(st, S(c->args[0].l));
    char *v = e && e->type == T_STR && e->s ? strdup(e->s) : NULL;
    pthread_mutex_unlock(&g_mu);
    if (v) { c->ret = vl(tl_jni_new_string(v)); free(v); }
    else c->ret = vl(c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL);
}
static void SP_contains(tl_jcall *c)
{
    pthread_mutex_lock(&g_mu);
    bool has = find(store_named(name_of(c->self)), S(c->args[0].l)) != NULL;
    pthread_mutex_unlock(&g_mu);
    c->ret = vz(has);
}
static void SP_getStringSet(tl_jcall *c) { c->ret = vl(c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL); }
static void Noop(tl_jcall *c) { (void)c; }

/* ------------------------------------------------------------------ Editor */

typedef struct { int op; int type; char *key; char *s; int64_t i; float f; } change;     /* op 0 put, 1 remove */
typedef struct { bool clear; change *ch; int n, cap; } pending;

static pending *pend(jobj *ed)
{
    if (!ed->native) ed->native = calloc(1, sizeof(pending));
    return ed->native;
}
static change *add(tl_jcall *c, int op, int type)
{
    pending *p = pend(c->self);
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 8; p->ch = realloc(p->ch, (size_t)p->cap * sizeof(change)); }
    change *ch = &p->ch[p->n++];
    memset(ch, 0, sizeof(*ch));
    ch->op = op; ch->type = type; ch->key = strdup(S(c->args[0].l));
    c->ret = vl(tl_jni_ref(c->self));
    return ch;
}
static void SP_edit(tl_jcall *c)
{
    jobj *ed = tl_jni_new_object(tl_jni_class("android/content/SharedPreferences$Editor"));
    tl_jni_set_field(ed, "name", "Ljava/lang/String;", vl(tl_jni_new_string(name_of(c->self))));
    c->ret = vl(ed);
}
static void Ed_putString(tl_jcall *c)
{
    if (!c->args[1].l) { add(c, 1, 0); return; }                 /* putString(key, null) removes it, as on Android */
    change *ch = add(c, 0, T_STR); ch->s = strdup(S(c->args[1].l));
}
static void Ed_putInt(tl_jcall *c) { change *ch = add(c, 0, T_INT); ch->i = c->args[1].i; }
static void Ed_putLong(tl_jcall *c) { change *ch = add(c, 0, T_LONG); ch->i = c->args[1].j; }
static void Ed_putFloat(tl_jcall *c) { change *ch = add(c, 0, T_FLOAT); ch->f = c->args[1].f; }
static void Ed_putBoolean(tl_jcall *c) { change *ch = add(c, 0, T_BOOL); ch->i = c->args[1].z != 0; }
static void Ed_putStringSet(tl_jcall *c) { c->ret = vl(tl_jni_ref(c->self)); }
static void Ed_remove(tl_jcall *c) { add(c, 1, 0); }
static void Ed_clear(tl_jcall *c) { pend(c->self)->clear = true; c->ret = vl(tl_jni_ref(c->self)); }

/* apply and commit: Android clears first, then runs the edits in order; the file is written once. */
static void flush(jobj *ed)
{
    pending *p = ed->native;
    if (!p) return;
    pthread_mutex_lock(&g_mu);
    store *st = store_named(name_of(ed));
    if (p->clear) while (st->n) drop(st, st->e[0].key);
    for (int k = 0; k < p->n; k++) {
        change *ch = &p->ch[k];
        if (ch->op == 1) drop(st, ch->key);
        else { entry *e = slot(st, ch->key); e->type = ch->type; e->i = ch->i; e->f = ch->f; if (ch->s) e->s = strdup(ch->s); }
        free(ch->key); free(ch->s);
    }
    p->n = 0; p->clear = false;
    save(st);
    pthread_mutex_unlock(&g_mu);
}
static void Ed_apply(tl_jcall *c) { flush(c->self); }
static void Ed_commit(tl_jcall *c) { flush(c->self); c->ret = vz(1); }

#define M(cl, n, s, f) { cl, n, s, f }
#define SP "android/content/SharedPreferences"
#define ED "android/content/SharedPreferences$Editor"
static const tl_jhle k_prefs_hle[] = {
    M("android/content/Context", "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", Context_getSharedPreferences),
    M("android/preference/PreferenceManager", "getDefaultSharedPreferences", "(Landroid/content/Context;)Landroid/content/SharedPreferences;", PM_getDefault),
    M("android/app/Activity", "getPreferences", "(I)Landroid/content/SharedPreferences;", Activity_getPreferences),
    M(SP, "getInt", "(Ljava/lang/String;I)I", SP_getInt), M(SP, "getLong", "(Ljava/lang/String;J)J", SP_getLong),
    M(SP, "getFloat", "(Ljava/lang/String;F)F", SP_getFloat), M(SP, "getBoolean", "(Ljava/lang/String;Z)Z", SP_getBoolean),
    M(SP, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", SP_getString),
    M(SP, "getStringSet", "(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;", SP_getStringSet),
    M(SP, "contains", "(Ljava/lang/String;)Z", SP_contains), M(SP, "edit", "()Landroid/content/SharedPreferences$Editor;", SP_edit),
    M(SP, "registerOnSharedPreferenceChangeListener", "(Landroid/content/SharedPreferences$OnSharedPreferenceChangeListener;)V", Noop),
    M(SP, "unregisterOnSharedPreferenceChangeListener", "(Landroid/content/SharedPreferences$OnSharedPreferenceChangeListener;)V", Noop),
    M(ED, "putString", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", Ed_putString),
    M(ED, "putInt", "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", Ed_putInt),
    M(ED, "putLong", "(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;", Ed_putLong),
    M(ED, "putFloat", "(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", Ed_putFloat),
    M(ED, "putBoolean", "(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;", Ed_putBoolean),
    M(ED, "putStringSet", "(Ljava/lang/String;Ljava/util/Set;)Landroid/content/SharedPreferences$Editor;", Ed_putStringSet),
    M(ED, "remove", "(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", Ed_remove),
    M(ED, "clear", "()Landroid/content/SharedPreferences$Editor;", Ed_clear),
    M(ED, "apply", "()V", Ed_apply), M(ED, "commit", "()Z", Ed_commit),
    { NULL, NULL, NULL, NULL }
};

void tl_prefs_install(void)
{
    tl_jni_declare("android/preference/PreferenceManager", "java/lang/Object");
    tl_jni_register_hle(k_prefs_hle);
}
