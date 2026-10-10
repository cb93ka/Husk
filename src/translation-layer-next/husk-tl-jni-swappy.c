/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Swappy's Java half: com.google.androidgamesdk.SwappyDisplayManager.
 *
 * Swappy (Android Frame Pacing, linked into Unity's libswappywrapper and into Godot) carries this class as dex bytes in its
 * own library and loads it with an InMemoryDexClassLoader. The class's constructor tells the native side the display's
 * refresh periods (nSetSupportedRefreshPeriods), and Swappy is only usable once it has heard them: without the class it
 * stayed half set up, and Godot, which presents through SwappyVk_queuePresent without checking, got
 * VK_ERROR_INITIALIZATION_FAILED for every frame (nothing shown, then a GPU timeout).
 *
 * The loader hands out the class, and the constructor reports one 60 Hz display mode -- for engines that turn this on
 * (tl_swappy_enable: Godot). Unity's Swappy is left without it: Unity feeds Swappy its frames from its own Java
 * Choreographer, which Husk does not run, so a working Swappy stalled Unity's rendering while a failed one is skipped.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void tl_log_line(const char *fmt, ...);

#define SDM "com/google/androidgamesdk/SwappyDisplayManager"
#define REFRESH_NS 16666666ll

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static bool g_enabled;
void tl_swappy_enable(bool on) { g_enabled = on; }
static void Noop(tl_jcall *c) { (void)c; }

/* new InMemoryDexClassLoader(dexBytes, parent): the one class it is asked for is Swappy's. */
static void IMDCL_loadClass(tl_jcall *c)
{
    const char *n = tl_jni_string(c->args[0].l);
    if (g_enabled && n && (!strcmp(n, "com.google.androidgamesdk.SwappyDisplayManager") || !strcmp(n, SDM))) { c->ret = vl(tl_jni_class_object(SDM)); return; }
    tl_jni_throw("java/lang/ClassNotFoundException", n ? n : "");
    c->ret = vl(NULL);
}

/* SwappyDisplayManager(long cookie, Activity activity): what the real constructor learns from the Display, told to the native side. */
static void SDM_init(tl_jcall *c)
{
    typedef void (*periods_fn)(void *env, void *self, int64_t cookie, void *periods, void *modes);
    periods_fn fn = (periods_fn)tl_jni_native(SDM, "nSetSupportedRefreshPeriods", "(J[J[I)V");
    if (!fn) { tl_log_line("swappy: nSetSupportedRefreshPeriods was not registered"); return; }
    jobj *periods = tl_jni_new_prim_array('J', 1), *modes = tl_jni_new_prim_array('I', 1);
    ((int64_t *)periods->arr.data)[0] = REFRESH_NS;
    ((int32_t *)modes->arr.data)[0] = 0;
    fn(tl_jni_env(), c->self, c->args[0].j, periods, modes);
    if (tl_jni_pending()) tl_jni_clear();
    tl_log_line("swappy: display manager ready (60 Hz)");
}

#define M(c, n, s, f) { c, n, s, f }
static const tl_jhle k_swappy_hle[] = {
    M("dalvik/system/InMemoryDexClassLoader", "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V", Noop),
    M("dalvik/system/InMemoryDexClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", IMDCL_loadClass),
    M("dalvik/system/InMemoryDexClassLoader", "findClass", "(Ljava/lang/String;)Ljava/lang/Class;", IMDCL_loadClass),
    M(SDM, "<init>", "(JLandroid/app/Activity;)V", SDM_init),
    M(SDM, "setPreferredDisplayModeId", "(I)V", Noop),
    M(SDM, "terminate", "()V", Noop),
    { NULL, NULL, NULL, NULL }
};

void tl_swappy_install(void)
{
    tl_jni_declare("dalvik/system/InMemoryDexClassLoader", "java/lang/ClassLoader");
    tl_jni_declare(SDM, "java/lang/Object");
    tl_jni_register_hle(k_swappy_hle);
}
