/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Java a particular SDL game adds around SDL, implemented in C.
 *
 * SDL games are mostly one native library, but some put an Android interface around it (a start menu, a dialog for a name, a loading overlay) that the library calls
 * into and waits on. Each is a handful of methods; they are kept here, by package, and installed only for that game.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-ld.h"
#include "husk-tl-internal.h"

#include <sys/stat.h>

jobj *tl_assetstream_list(const char *path);
const char *tl_hle_android_id(void);
void tl_sdl_set_default_arguments(const char *args);
void tl_sdl_display_metrics(tl_jcall *c);
jobj *tl_assetstream_file_tree(void);
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static const char *Str(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }
static void H_void(tl_jcall *c) { (void)c; }
static void H_false(tl_jcall *c) { c->ret = vz(0); }

/* A native of the game's own library, by its JNI name. */
static void *game_native(const char *mangled)
{
    static const char *const libs[] = { "libmain.so", NULL };
    (void)libs;
    void *fn = NULL;
    extern void *tl_sdl_game_symbol(const char *name);
    fn = tl_sdl_game_symbol(mangled);
    if (!fn) tl_log_line("sdl: the game has no native %s", mangled);
    return fn;
}

/* The answer Java gives later, from another thread, once its dialog is dismissed: here a moment after the question, from a thread of its own. */
struct answer { void (*fn)(void *env, void *self, int a); void *self; int a; void (*fn_text)(void *env, void *self, uint8_t ok, void *text); char text[200]; };
static void *deliver(void *arg)
{
    struct answer *r = arg;
    usleep(250000);
    void *env = tl_jni_env();
    if (r->fn) r->fn(env, r->self, r->a);
    else if (r->fn_text) r->fn_text(env, r->self, 1, tl_jni_new_string(r->text));
    free(r);
    return NULL;
}
static void answer_later(struct answer *r)
{
    pthread_t t;
    if (pthread_create(&t, NULL, deliver, r) == 0) pthread_detach(t); else free(r);
}

/* ---------------------------------------------------------------- Brogue CE */

#define BROGUE "org/broguece/game/BrogueActivity"
static void BR_showStartMenu(tl_jcall *c)                              /* (hasSavedGame, ...): New Game */
{
    struct answer *r = calloc(1, sizeof(*r));
    r->fn = game_native("Java_org_broguece_game_BrogueActivity_nativeStartMenuResult");
    r->self = tl_jni_ref(c->self); r->a = 0;
    if (!r->fn) { free(r); return; }
    tl_log_line("sdl: Brogue start menu: new game");
    answer_later(r);
}
static void BR_showTextInput(tl_jcall *c)                              /* (title, default, maxLength, ...): accept the default */
{
    struct answer *r = calloc(1, sizeof(*r));
    r->fn_text = game_native("Java_org_broguece_game_BrogueActivity_nativeTextInputResult");
    r->self = tl_jni_ref(c->self);
    snprintf(r->text, sizeof(r->text), "%s", c->args[1].l ? Str(c->args[1].l) : "");
    if (!r->fn_text) { free(r); return; }
    answer_later(r);
}
static void BR_getInt(tl_jcall *c) { c->ret = vi(c->args[1].i); }       /* a setting not stored: its default */
static const tl_jhle k_brogue[] = {
    { BROGUE, "showStartMenu", "(ZZ)V", BR_showStartMenu },
    { BROGUE, "showTextInputDialog", "(Ljava/lang/String;Ljava/lang/String;IZ)V", BR_showTextInput },
    { BROGUE, "setOverlayVisible", "(Z)V", H_void },
    { BROGUE, "setLoadingVisible", "(Z)V", H_void },
    { BROGUE, "setLoadingProgress", "(I)V", H_void },
    { BROGUE, "setRestoringVisible", "(Z)V", H_void },
    { BROGUE, "getSettingBool", "(Ljava/lang/String;)Z", H_false },
    { BROGUE, "getSettingInt", "(Ljava/lang/String;I)I", BR_getInt },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------- Pekka Kana 2 */

#define PK2 "org/pgnapps/pk2/PK2Activity"
static void PK_listDir(tl_jcall *c)                                    /* (path): the entries of an asset folder */
{
    jvalue r; r.j = 0; r.l = tl_assetstream_list(Str(c->args[0].l));
    c->ret = r;
}
static const tl_jhle k_pk2[] = {
    { PK2, "externalPermitted", "()Z", H_false },                      /* no shared storage: levels come from the app */
    { PK2, "listDir", "(Ljava/lang/String;)[Ljava/lang/String;", PK_listDir },
    { "org/libsdl/app/SDLActivity", "setSurfaceViewFormat", "(I)V", H_void },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------------- LOVE */

/* LOVE (love2d.org) on Android: its GameActivity adds a few screen settings and the message box LOVE shows when the game's Lua fails. */
#define LOVE "org/love2d/android/GameActivity"
static void LV_dpi(tl_jcall *c) { jvalue v; v.j = 0; v.f = 1.0f; c->ret = v; }                 /* the surface is in real pixels */
static void LV_messagebox(tl_jcall *c)                                                          /* (flags, title, message, ...): said in the log, no button pressed */
{
    tl_log_line("love: message box \"%s\": %s", c->args[1].l ? Str(c->args[1].l) : "", c->args[2].l ? Str(c->args[2].l) : "");
    c->ret = vi(0);
}
static void LV_buildFileTree(tl_jcall *c) { jvalue v; v.j = 0; v.l = tl_assetstream_file_tree(); c->ret = v; }
static const tl_jhle k_love[] = {
    { LOVE, "buildFileTree", "()[Ljava/lang/String;", LV_buildFileTree },
    { LOVE, "getMetrics", "()Landroid/util/DisplayMetrics;", tl_sdl_display_metrics },
    { LOVE, "getImmersiveMode", "()Z", H_false },
    { LOVE, "setImmersiveMode", "(Z)V", H_void },
    { LOVE, "getDPIScale", "()F", LV_dpi },
    { LOVE, "messageboxShowMessageBox", "(ILjava/lang/String;Ljava/lang/String;[I[I[Ljava/lang/String;[I)I", LV_messagebox },
    { NULL, NULL, NULL, NULL }
};

/*
 * LuaJIT. Its compiler writes machine code into memory it makes executable, which the phone does not give a guest (only the loader gets executable memory); with the JIT
 * off, LuaJIT runs its interpreter, and that is what a game gets. Each new Lua state is switched off as the standard libraries are opened.
 */
static void (*g_real_openlibs)(void *L);
static int (*g_jit_setmode)(void *L, int idx, int mode);
static void luajit_openlibs(void *L)
{
    if (!g_real_openlibs) g_real_openlibs = tl_ld_sym(NULL, "luaL_openlibs");
    if (!g_jit_setmode) g_jit_setmode = tl_ld_sym(NULL, "luaJIT_setmode");
    if (g_real_openlibs) g_real_openlibs(L);
    if (g_jit_setmode) g_jit_setmode(L, 0, 0 /* LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF */);
}

/* ------------------------------------------------------------- Xash3D FWGS */

/*
 * Xash3D FWGS (Half-Life and Counter-Strike 1.6 ports). The engine finds its game folders under XASH3D_BASEDIR, which its activity sets
 * along with the game (XASH3D_GAME) before SDL starts; asset files the APK carries (touch layouts, fonts) it reads through the activity's
 * getAssets/getAssetsList.
 *
 * Mobsik's Counter-Strike 1.6 (store.androidapplications.cs16.mobsik) has a launcher that unpacks the game -- assets/cs16-data.zip (valve,
 * cstrike: 750 MB) and assets/bundled-maps -- into files/game-data-v1 on first start, then opens the game with "-log -port 27522 -port6 27522".
 * That launcher (and its own room-based matchmaking) is Java that does not run here, so the unpacking is done here, once; the engine's own
 * server browser is the way online.
 */
static bool g_xash;

static void mkdirs_to(char *path)
{
    for (char *p = path + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}

/* Every entry of `z` under `prefix` written below `out` (the prefix dropped); returns the files written, -1 on a write failure. */
static long unpack(const tl_zip *z, const char *prefix, const char *out)
{
    size_t pl = strlen(prefix); long n = 0;
    for (size_t i = 0; i < z->count; i++) {
        const tl_zip_entry *e = &z->entries[i];
        if (strncmp(e->name, prefix, pl) || !e->name[pl] || strstr(e->name, "..") || e->name[strlen(e->name) - 1] == '/') continue;
        char path[1400]; snprintf(path, sizeof(path), "%s/%s", out, e->name + pl);
        mkdirs_to(path);
        const uint8_t *d; size_t len; bool owned; char err[160];
        if (!tl_zip_data(z, e, (size_t)1 << 30, &d, &len, &owned, err, sizeof(err))) { tl_log_line("xash: %s: %s", e->name, err); continue; }
        char tmp[1450]; snprintf(tmp, sizeof(tmp), "%s.part", path);
        FILE *f = fopen(tmp, "wb");
        bool ok = f && fwrite(d, 1, len, f) == len;
        if (f && fclose(f)) ok = false;
        if (owned) free((void *)d);
        if (!ok || rename(tmp, path)) { tl_log_line("xash: cannot write %s", path); unlink(tmp); return -1; }
        n++;
    }
    return n;
}

static bool mobsik_unpack(const char *apk, const char *base)
{
    char stamp[800]; snprintf(stamp, sizeof(stamp), "%s/.husk-unpacked", base);
    struct stat st, ap;
    if (stat(stamp, &st) == 0 && stat(apk, &ap) == 0 && st.st_mtime >= ap.st_mtime) return true;
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) { tl_log_line("xash: %s", err); return false; }
    const tl_zip_entry *data = tl_zip_find(&z, "assets/cs16-data.zip");
    bool ok = false;
    if (data) {
        tl_log_line("xash: unpacking the game (%.0f MB) into %s -- the first start takes a while", data->usize / 1048576.0, base);
        char inner[800]; snprintf(inner, sizeof(inner), "%s.zip.part", base);
        const uint8_t *d; size_t len; bool owned;
        mkdirs_to(inner);
        FILE *f = NULL;
        if (tl_zip_data(&z, data, (size_t)2 << 30, &d, &len, &owned, err, sizeof(err))) {
            f = fopen(inner, "wb");
            ok = f && fwrite(d, 1, len, f) == len;
            if (f && fclose(f)) ok = false;
            if (owned) free((void *)d);
        }
        tl_zip iz;
        if (ok && tl_zip_open(&iz, inner, err, sizeof(err))) {
            long a = unpack(&iz, "", base);
            tl_zip_close(&iz);
            long b = a >= 0 ? unpack(&z, "assets/bundled-maps/", base) : -1;
            ok = a > 0 && b >= 0;
            tl_log_line("xash: unpacked %ld game files and %ld bundled map files", a, b);
        } else ok = false;
        unlink(inner);
    }
    tl_zip_close(&z);
    if (ok) { FILE *f = fopen(stamp, "w"); if (f) fclose(f); }
    else tl_log_line("xash: unpacking the game failed (is there room on the device?)");
    return ok;
}

void tl_xash_prepare(const char *apk, const char *data_dir, const char *package)
{
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return;
    g_xash = tl_zip_find(&z, "lib/arm64-v8a/libxash.so") != NULL;
    bool mobsik = tl_zip_find(&z, "assets/cs16-data.zip") != NULL;
    tl_zip_close(&z);
    if (!g_xash) return;
    char base[700];
    if (mobsik) {
        snprintf(base, sizeof(base), "%s/files/game-data-v1", data_dir);
        mkdir(base, 0755);
        mobsik_unpack(apk, base);
        tl_sdl_set_default_arguments("-log -port 27522 -port6 27522");
    } else {
        /* a plain Xash3D port: the game folders where the player puts them, in the app's files */
        snprintf(base, sizeof(base), "%s/files", data_dir);
    }
    setenv("XASH3D_BASEDIR", base, 1);
    setenv("XASH3D_GAME", (mobsik || strstr(package, "cs16")) ? "cstrike" : "valve", 0);
    setenv("XASH3D_GAMELIBDIR", "/data/app/lib/arm64", 1);
    setenv("SDL_ANDROID_BLOCK_ON_PAUSE", "1", 1);
    tl_log_line("xash: XASH3D_BASEDIR=%s XASH3D_GAME=%s", base, getenv("XASH3D_GAME"));
}

static void XA_androidID(tl_jcall *c) { jvalue v; v.j = 0; v.l = tl_jni_new_string(tl_hle_android_id()); c->ret = v; }
static void XA_null(tl_jcall *c) { jvalue v; v.j = 0; v.l = NULL; c->ret = v; }
static void XA_getAssets(tl_jcall *c) { c->ret = tl_jni_call(c->self, "getAssets", "()Landroid/content/res/AssetManager;", NULL); }
static void XA_getAssetsList(tl_jcall *c) { jvalue v; v.j = 0; v.l = tl_assetstream_list(c->args[1].l ? Str(c->args[1].l) : ""); c->ret = v; }
static void XA_messagebox(tl_jcall *c)
{
    tl_log_line("xash: message box \"%s\": %s", c->args[1].l ? Str(c->args[1].l) : "", c->args[2].l ? Str(c->args[2].l) : "");
    c->ret = vi(0);
}
#define SDLA_X "org/libsdl/app/SDLActivity"
static const tl_jhle k_xash[] = {
    { SDLA_X, "getAndroidID", "()Ljava/lang/String;", XA_androidID },
    { SDLA_X, "loadAndroidID", "()Ljava/lang/String;", XA_androidID },
    { SDLA_X, "saveAndroidID", "(Ljava/lang/String;)V", H_void },
    { SDLA_X, "getCallingPackage", "()Ljava/lang/String;", XA_null },
    { SDLA_X, "getAssets", "(Z)Landroid/content/res/AssetManager;", XA_getAssets },
    { SDLA_X, "getAssetsList", "(ZLjava/lang/String;)[Ljava/lang/String;", XA_getAssetsList },
    { SDLA_X, "messageboxShowMessageBox", "(ILjava/lang/String;Ljava/lang/String;[I[I[Ljava/lang/String;[I)I", XA_messagebox },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------------ install */

/* python-for-android (Kivy): the statics PythonActivity.onCreate and Hardware's initialiser fill, which Python reads through pyjnius
 * (PythonActivity.mActivity, Hardware.metrics). */
jobj *tl_hle_activity(void);
static void kivy_statics(void)
{
    jvalue act; act.j = 0; act.l = tl_hle_activity();
    tl_jni_set_static("org/kivy/android/PythonActivity", "mActivity", "Lorg/kivy/android/PythonActivity;", act);
    jvalue res = tl_jni_call(act.l, "getResources", "()Landroid/content/res/Resources;", NULL);
    jvalue metrics = res.l ? tl_jni_call(res.l, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;", NULL) : (jvalue){ .j = 0 };
    if (metrics.l) tl_jni_set_static("org/renpy/android/Hardware", "metrics", "Landroid/util/DisplayMetrics;", metrics);
}

void tl_sdl_apps_install(const char *package)
{
    if (tl_dexidx_has_class("org/kivy/android/PythonActivity")) kivy_statics();
    if (!strcmp(package, "org.broguece.game")) tl_jni_register_hle(k_brogue);
    if (tl_dexidx_has_class(LOVE)) { tl_jni_register_hle(k_love); tl_ld_interpose("luaL_openlibs", (void *)luajit_openlibs); }
    if (!strcmp(package, "org.pgnapps.pk2")) tl_jni_register_hle(k_pk2);
    if (g_xash) tl_jni_register_hle(k_xash);
}
