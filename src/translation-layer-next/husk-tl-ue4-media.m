/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * com.epicgames.ue4.MediaPlayer14: the Java video player Unreal's AndroidMedia plug-in drives through JNI, over android.media.MediaPlayer.
 * Here it is AVFoundation's: an AVPlayer plays the file (its sound too), and an AVPlayerItemVideoOutput hands the frames over, which are
 * written into the OpenGL ES texture the engine names. A game's videos -- menus, intros, the screens in the world -- are mp4 files in its pak,
 * which the engine hands over as (pak file, offset, size); that range is copied out once to a file AVFoundation can open.
 *
 * The engine calls from its game thread (open, play, seek, release) and its render thread (the frames, with its GL context current), the two at
 * once: what both touch is under the player's lock, and a released player keeps its (emptied) object, which the render thread may still be
 * holding a frame request for -- skipping the intro freed the frame being copied, and the copy went on into the engine's heap.
 */
#import <AVFoundation/AVFoundation.h>
#import <Accelerate/Accelerate.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "husk-tl-jni.h"
#include "husk-tl-egl.h"
#include "husk-tl-bionic.h"

#define CLS "com/epicgames/ue4/MediaPlayer14"

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static jvalue vf(float f) { jvalue v; v.j = 0; v.f = f; return v; }

bool tl_guest_file_range(const char *path, char *host, size_t n, uint64_t *off, uint64_t *size);

@interface HuskUEMedia : NSObject {
@public
    AVPlayer *player;
    AVPlayerItem *item;
    AVPlayerItemVideoOutput *output;
    id endObserver;
    NSString *file;
    int width, height, duration_ms;
    int quarter;                       /* the track's display rotation, in quarter turns clockwise: frames come unrotated */
    uint8_t *turn; size_t turn_size;
    float fps, volume;
    bool prepared, looping, playing, audio_on, video_on, released;
    atomic_bool completed;
    CVPixelBufferRef frame;           /* the latest frame, kept for a texture asked for again with nothing new */
    uint8_t *rgba; size_t rgba_size;
    int uploaded_w, uploaded_h;
}
@end
@implementation HuskUEMedia
- (void)dealloc
{
    if (endObserver) [[NSNotificationCenter defaultCenter] removeObserver:endObserver];
    if (frame) CVPixelBufferRelease(frame);
    free(rgba); free(turn);
}
@end

static HuskUEMedia *M(tl_jcall *c) { return c->self && c->self->native ? (__bridge HuskUEMedia *)c->self->native : nil; }
static bool trace(void) { static int t = -1; if (t < 0) t = getenv("TL_MEDIA_TRACE") != NULL; return t; }

static void MP_init(tl_jcall *c)
{
    if (!c->self) return;
    HuskUEMedia *m = [HuskUEMedia new];
    m->volume = 1; m->audio_on = m->video_on = true;
    c->self->native = (void *)CFBridgingRetain(m);
    if (trace()) tl_log_line("media: new player %p (swizzle %d, vulkan %d, track info %d)", (void *)c->self, c->args[0].z, c->args[1].z, c->args[2].z);
}

static void drop_item(HuskUEMedia *m)
{
    if (m->endObserver) { [[NSNotificationCenter defaultCenter] removeObserver:m->endObserver]; m->endObserver = nil; }
    [m->player pause];
    m->player = nil; m->item = nil; m->output = nil;
    if (m->frame) { CVPixelBufferRelease(m->frame); m->frame = NULL; }
    m->prepared = m->playing = false; atomic_store(&m->completed, false);
}

/* The bytes [off, off+size) of a file, as a file of their own under the cache (named by where they came from, so a video is copied once). */
static NSString *extract(const char *guest, int64_t off, int64_t size)
{
    char host[1024]; uint64_t base = 0, whole = 0;
    if (!tl_guest_file_range(guest, host, sizeof(host), &base, &whole)) { tl_log_line("media: %s is not there", guest); return nil; }
    struct stat st;
    if (stat(host, &st) != 0) return nil;
    if (size <= 0) size = (int64_t)whole - off;
    off += (int64_t)base;
    if (off == 0 && size == st.st_size) return [NSString stringWithUTF8String:host];
    char key[1200]; snprintf(key, sizeof(key), "%s|%lld|%lld|%lld", host, (long long)off, (long long)size, (long long)st.st_mtime);
    unsigned char d[CC_SHA1_DIGEST_LENGTH]; CC_SHA1(key, (CC_LONG)strlen(key), d);
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"husk-ue4-media"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableString *name = [NSMutableString string];
    for (int i = 0; i < 10; i++) [name appendFormat:@"%02x", d[i]];
    NSString *path = [dir stringByAppendingPathComponent:[name stringByAppendingString:@".mp4"]];
    struct stat have;
    if (stat(path.UTF8String, &have) == 0 && have.st_size == size) return path;
    FILE *in = fopen(host, "rb"), *out = in ? fopen([path stringByAppendingString:@".part"].UTF8String, "wb") : NULL;
    bool ok = in && out && fseeko(in, off, SEEK_SET) == 0;
    static char chunk[1 << 20];
    for (int64_t left = size; ok && left > 0;) {
        size_t n = fread(chunk, 1, left < (int64_t)sizeof(chunk) ? (size_t)left : sizeof(chunk), in);
        if (!n || fwrite(chunk, 1, n, out) != n) ok = false;
        left -= (int64_t)n;
    }
    if (in) fclose(in);
    if (out) fclose(out);
    if (!ok) { tl_log_line("media: could not copy the video out of %s", host); unlink([path stringByAppendingString:@".part"].UTF8String); return nil; }
    rename([path stringByAppendingString:@".part"].UTF8String, path.UTF8String);
    return path;
}

static bool set_source(tl_jcall *c, NSString *path)
{
    HuskUEMedia *m = M(c);
    if (!m) return false;
    if (getenv("TL_MEDIA_OFF")) path = nil;            /* debugging: every video refused, as when there was no player */
    @synchronized (m) { drop_item(m); m->file = path; }
    if (trace()) tl_log_line("media: %p source %s", (void *)c->self, path ? path.UTF8String : "(none)");
    return path != nil;
}
/* setDataSource(String path, long offset, long size): a range of a file (the pak) */
static void MP_setSourceRange(tl_jcall *c) { c->ret = vz(set_source(c, extract(tl_jni_string(c->args[0].l) ?: "", c->args[1].j, c->args[2].j))); }
/* setDataSource(AssetManager, String path, long offset, long size): the same, the path an asset of the APK's (the pak inside it) */
static void MP_setSourceAsset(tl_jcall *c)
{
    char p[1024]; snprintf(p, sizeof(p), "/android_asset/%s", tl_jni_string(c->args[1].l) ?: "");
    c->ret = vz(set_source(c, extract(p, c->args[2].j, c->args[3].j)));
}
/* setDataSourceURL(String): a file:// path or a stream */
static void MP_setSourceURL(tl_jcall *c)
{
    const char *u = tl_jni_string(c->args[0].l) ?: "";
    if (!strncmp(u, "file://", 7)) { c->ret = vz(set_source(c, extract(u + 7, 0, 0))); return; }
    HuskUEMedia *m = M(c);
    if (!m) return;
    @synchronized (m) { drop_item(m); m->file = [NSString stringWithUTF8String:u]; }
    c->ret = vz(1);
}

static void prepare(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m || !m->file) return;
    NSURL *url = [m->file containsString:@"://"] ? [NSURL URLWithString:m->file] : [NSURL fileURLWithPath:m->file];
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
    AVAssetTrack *v = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (v) {
        CGSize sz = CGSizeApplyAffineTransform(v.naturalSize, v.preferredTransform);
        m->width = (int)fabs(sz.width); m->height = (int)fabs(sz.height); m->fps = v.nominalFrameRate;
        CGAffineTransform t = v.preferredTransform;
        m->quarter = ((int)lround(atan2(t.b, t.a) * 2 / M_PI) % 4 + 4) % 4;
    }
    m->duration_ms = (int)(CMTimeGetSeconds(asset.duration) * 1000);
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
    AVPlayerItemVideoOutput *output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) }];
    [item addOutput:output];
    AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
    @synchronized (m) { m->item = item; m->output = output; m->player = player; }
    m->player.actionAtItemEnd = AVPlayerActionAtItemEndNone;
    m->player.volume = m->audio_on ? m->volume : 0;
    if (getenv("TL_AUDIO_MUTE")) m->player.muted = YES;
    __weak HuskUEMedia *weak = m;
    m->endObserver = [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:m->item queue:nil
        usingBlock:^(NSNotification *n) {
            (void)n;
            HuskUEMedia *s = weak;
            if (!s) return;
            if (trace()) tl_log_line("media: %p reached the end (%s)", (__bridge void *)s, s->looping ? "looping" : "done");
            /* As the engine's own Java does: it loops by hand (seekTo(0), start()) and reports the completion either way -- the engine flushes
             * its queue of frames on it, and without that the frames of the next pass, earlier than the ones it had, were never shown. */
            if (s->looping) { [s->player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero]; [s->player play]; }
            else s->playing = false;
            atomic_store(&s->completed, true);
        }];
    m->prepared = true;
    if (trace()) tl_log_line("media: %p prepared %dx%d %.2f fps, %d ms", (void *)c->self, m->width, m->height, m->fps, m->duration_ms);
}
static void MP_prepare(tl_jcall *c) { prepare(c); }

static void MP_start(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m || !m->player) return;
    if (atomic_load(&m->completed) || CMTimeGetSeconds(m->player.currentTime) * 1000 >= m->duration_ms - 1) [m->player seekToTime:kCMTimeZero];
    atomic_store(&m->completed, false);
    [m->player play]; m->playing = true;
}
static void MP_pause(tl_jcall *c) { HuskUEMedia *m = M(c); if (m && m->player) { [m->player pause]; m->playing = false; } }
static void MP_stop(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m || !m->player) return;
    [m->player pause]; m->playing = false;
    [m->player seekToTime:kCMTimeZero];
}
static void MP_reset(tl_jcall *c) { HuskUEMedia *m = M(c); if (m) @synchronized (m) { drop_item(m); m->file = nil; } }
static void MP_release(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m) return;
    @synchronized (m) {
        drop_item(m);
        m->file = nil; m->released = true;
        free(m->rgba); m->rgba = NULL; m->rgba_size = 0;
        free(m->turn); m->turn = NULL; m->turn_size = 0;
    }
}
static void MP_seekTo(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m || !m->player) return;
    [m->player seekToTime:CMTimeMake(c->args[0].i, 1000) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
    atomic_store(&m->completed, false);
}
static void MP_setLooping(tl_jcall *c) { HuskUEMedia *m = M(c); if (m) m->looping = c->args[0].z; if (trace()) tl_log_line("media: %p looping %d", (void *)c->self, c->args[0].z); }
static void MP_isLooping(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vz(m && m->looping); }
static void MP_isPlaying(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vz(m && m->playing); }
static void MP_isPrepared(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vz(m && m->prepared); }
static void MP_didComplete(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vz(m && atomic_exchange(&m->completed, false)); }
static void MP_didResolutionChange(tl_jcall *c) { (void)c; }
static void MP_getDuration(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vi(m ? m->duration_ms : 0); }
static void MP_getPosition(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m || !m->player) return;
    int ms = (int)(CMTimeGetSeconds(m->player.currentTime) * 1000);
    c->ret = vi(ms < 0 ? 0 : ms);
}
static void MP_getWidth(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vi(m ? m->width : 0); }
static void MP_getHeight(tl_jcall *c) { HuskUEMedia *m = M(c); c->ret = vi(m ? m->height : 0); }
static void MP_setVolume(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m) return;
    m->volume = c->args[0].f;
    if (m->player) m->player.volume = m->audio_on ? m->volume : 0;
}
static void MP_setAudioEnabled(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m) return;
    m->audio_on = c->args[0].z;
    if (m->player) m->player.volume = m->audio_on ? m->volume : 0;
}
static void MP_setVideoEnabled(tl_jcall *c) { HuskUEMedia *m = M(c); if (m) m->video_on = c->args[0].z; }
static void MP_selectTrack(tl_jcall *c) { (void)c; }
static void MP_externalTexture(tl_jcall *c) { (void)c; c->ret = vi(-1); }

/* Track lists: one video track and the sound as one audio track, as the file has them. */
static void MP_videoTracks(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    bool has = m && m->prepared && m->width > 0;
    tl_jclass *k = tl_jni_class(CLS "$VideoTrackInfo");
    jobj *a = tl_jni_new_obj_array(k, has ? 1 : 0);
    if (has) {
        jobj *t = tl_jni_new_object(k);
        tl_jni_set_field(t, "Index", "I", vi(0));
        tl_jni_set_field(t, "MimeType", "Ljava/lang/String;", vl(tl_jni_new_string("video/avc")));
        tl_jni_set_field(t, "DisplayName", "Ljava/lang/String;", vl(tl_jni_new_string("Video Track 0")));
        tl_jni_set_field(t, "Language", "Ljava/lang/String;", vl(tl_jni_new_string("und")));
        tl_jni_set_field(t, "BitRate", "I", vi(0));
        tl_jni_set_field(t, "Width", "I", vi(m->width));
        tl_jni_set_field(t, "Height", "I", vi(m->height));
        tl_jni_set_field(t, "FrameRate", "F", vf(m->fps > 0 ? m->fps : 30));
        a->oarr.v[0] = t;
    }
    c->ret = vl(a);
}
static void MP_audioTracks(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    bool has = m && m->prepared && [m->item.asset tracksWithMediaType:AVMediaTypeAudio].count > 0;
    tl_jclass *k = tl_jni_class(CLS "$AudioTrackInfo");
    jobj *a = tl_jni_new_obj_array(k, has ? 1 : 0);
    if (has) {
        jobj *t = tl_jni_new_object(k);
        tl_jni_set_field(t, "Index", "I", vi(0));
        tl_jni_set_field(t, "MimeType", "Ljava/lang/String;", vl(tl_jni_new_string("audio/mp4a-latm")));
        tl_jni_set_field(t, "DisplayName", "Ljava/lang/String;", vl(tl_jni_new_string("Audio Track 0")));
        tl_jni_set_field(t, "Language", "Ljava/lang/String;", vl(tl_jni_new_string("und")));
        tl_jni_set_field(t, "Channels", "I", vi(2));
        tl_jni_set_field(t, "SampleRate", "I", vi(44100));
        a->oarr.v[0] = t;
    }
    c->ret = vl(a);
}
static void MP_captionTracks(tl_jcall *c) { (void)c; c->ret = vl(tl_jni_new_obj_array(tl_jni_class(CLS "$CaptionTrackInfo"), 0)); }

/* ------------------------------------------------------------------------------------------------ frames */

typedef void (*gl_get_t)(unsigned, int *);
typedef void (*gl_bind_t)(unsigned, unsigned);
typedef void (*gl_pixi_t)(unsigned, int);
typedef void (*gl_tex_t)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void (*gl_sub_t)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void (*gl_parami_t)(unsigned, unsigned, int);
typedef unsigned (*gl_err_t)(void);
static struct { gl_get_t get; gl_bind_t bind, bindbuf; gl_pixi_t store; gl_tex_t tex; gl_sub_t sub; gl_parami_t param; gl_err_t err; } G;
static bool gl(void)
{
    if (G.tex) return true;
    G.get = (gl_get_t)tl_egl_resolve("glGetIntegerv"); G.bind = (gl_bind_t)tl_egl_resolve("glBindTexture");
    G.bindbuf = (gl_bind_t)tl_egl_resolve("glBindBuffer"); G.store = (gl_pixi_t)tl_egl_resolve("glPixelStorei");
    G.sub = (gl_sub_t)tl_egl_resolve("glTexSubImage2D"); G.param = (gl_parami_t)tl_egl_resolve("glTexParameteri");
    G.err = (gl_err_t)tl_egl_resolve("glGetError");
    G.tex = (gl_tex_t)tl_egl_resolve("glTexImage2D");
    return G.get && G.bind && G.bindbuf && G.store && G.sub && G.param && G.err && G.tex;
}

/* The frame for now from the player's output, if a new one has come; the last one stays in m->frame. */
static bool next_frame(HuskUEMedia *m)
{
    if (!m->output) return false;
    CMTime t = [m->output itemTimeForHostTime:CACurrentMediaTime()];
    if (![m->output hasNewPixelBufferForItemTime:t]) return false;
    CVPixelBufferRef pb = [m->output copyPixelBufferForItemTime:t itemTimeForDisplay:NULL];
    if (!pb) return false;
    if (m->frame) CVPixelBufferRelease(m->frame);
    m->frame = pb;
    return true;
}

/* The frame into a GL_TEXTURE_2D, as RGBA, the top row first in memory as the engine's own copy (an FBO blit of the video) leaves it. */
static bool upload(HuskUEMedia *m, unsigned tex)
{
    if (!m->frame || !gl()) return false;
    CVPixelBufferRef pb = m->frame;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    const uint8_t *src = CVPixelBufferGetBaseAddress(pb);
    size_t need = (size_t)w * h * 4;
    if (m->rgba_size < need) { free(m->rgba); m->rgba = malloc(need); m->rgba_size = m->rgba ? need : 0; }
    if (m->quarter && m->turn_size < need) { free(m->turn); m->turn = malloc(need); m->turn_size = m->turn ? need : 0; }
    if (!m->rgba || (m->quarter && !m->turn)) { CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); return false; }
    vImage_Buffer from = { (void *)src, (vImagePixelCount)h, (vImagePixelCount)w, stride }, to = { m->rgba, (vImagePixelCount)h, (vImagePixelCount)w, (size_t)w * 4 };
    static const uint8_t bgra_to_rgba[4] = { 2, 1, 0, 3 };
    vImagePermuteChannels_ARGB8888(&from, &to, bgra_to_rgba, kvImageNoFlags);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *pixels = m->rgba;
    if (m->quarter) {                  /* a phone's portrait video: turned as it is shown, and as big as the engine was told */
        int tw = m->quarter == 2 ? w : h, th = m->quarter == 2 ? h : w;
        vImage_Buffer turned = { m->turn, (vImagePixelCount)th, (vImagePixelCount)tw, (size_t)tw * 4 };
        static const uint8_t black[4] = { 0, 0, 0, 255 };
        static const uint8_t rot[4] = { kRotate0DegreesClockwise, kRotate90DegreesClockwise, kRotate180DegreesClockwise, kRotate90DegreesCounterClockwise };
        vImageRotate90_ARGB8888(&to, &turned, rot[m->quarter], black, kvImageNoFlags);
        pixels = m->turn; w = tw; h = th;
    }

    int old_tex = 0, old_pbo = 0, old_align = 4, old_row = 0;
    G.get(0x8069 /* TEXTURE_BINDING_2D */, &old_tex); G.get(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &old_pbo);
    G.get(0x0CF5 /* UNPACK_ALIGNMENT */, &old_align); G.get(0x0CF2 /* UNPACK_ROW_LENGTH */, &old_row);
    while (G.err()) {}
    G.bind(0x0DE1, tex);
    if (old_pbo) G.bindbuf(0x88EC, 0);
    G.store(0x0CF5, 4); G.store(0x0CF2, 0);
    if (m->uploaded_w == w && m->uploaded_h == h) G.sub(0x0DE1, 0, 0, 0, w, h, 0x1908, 0x1401, pixels);
    if (m->uploaded_w != w || m->uploaded_h != h || G.err()) {
        G.tex(0x0DE1, 0, 0x1908 /* RGBA */, w, h, 0, 0x1908, 0x1401, pixels);
        G.param(0x0DE1, 0x2801, 0x2601); G.param(0x0DE1, 0x2800, 0x2601);      /* linear, no mipmaps: else the texture is incomplete */
        G.param(0x0DE1, 0x2802, 0x812F); G.param(0x0DE1, 0x2803, 0x812F);
        m->uploaded_w = w; m->uploaded_h = h;
    }
    G.store(0x0CF5, old_align); G.store(0x0CF2, old_row);
    if (old_pbo) G.bindbuf(0x88EC, (unsigned)old_pbo);
    G.bind(0x0DE1, (unsigned)old_tex);
    return true;
}

/* getVideoLastFrame(int destTexture): the latest frame into the engine's texture */
static void MP_lastFrame(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    if (!m) return;
    bool fresh = false;
    @synchronized (m) {
        if (m->released) return;
        fresh = next_frame(m);
        if (fresh || (m->frame && m->uploaded_w == 0)) c->ret = vz(upload(m, (unsigned)c->args[0].i));
    }
    static bool said; if (trace() && !said && fresh) {
        said = true; char tn[64] = ""; pthread_getname_np(pthread_self(), tn, sizeof(tn));
        void *(*cur)(void) = (void *(*)(void))tl_egl_resolve("eglGetCurrentContext");
        tl_log_line("media: frames through getVideoLastFrame(%d) on thread '%s', context %p", c->args[0].i, tn, cur ? cur() : NULL);
    }
}

/* updateVideoFrame(int externalTexture): the frame for the engine's external (OES) texture -- the path of engines that sample one; written as a 2D one */
static void MP_updateFrame(tl_jcall *c)
{
    HuskUEMedia *m = M(c);
    jobj *info = tl_jni_new_object(tl_jni_class(CLS "$FrameUpdateInfo"));
    bool ready = false;
    int pos = 0;
    if (m) @synchronized (m) {
        if (!m->released && next_frame(m)) ready = upload(m, (unsigned)c->args[0].i);
        if (m->player) pos = (int)(CMTimeGetSeconds(m->player.currentTime) * 1000);
    }
    tl_jni_set_field(info, "CurrentPosition", "I", vi(pos < 0 ? 0 : pos));
    tl_jni_set_field(info, "FrameReady", "Z", vz(ready));
    tl_jni_set_field(info, "RegionChanged", "Z", vz(0));
    tl_jni_set_field(info, "UScale", "F", vf(1)); tl_jni_set_field(info, "UOffset", "F", vf(0));
    tl_jni_set_field(info, "VScale", "F", vf(1)); tl_jni_set_field(info, "VOffset", "F", vf(0));
    static bool said; if (trace() && !said && ready) { said = true; tl_log_line("media: frames through updateVideoFrame(%d)", c->args[0].i); }
    c->ret = vl(info);
}

/* getVideoLastFrameData(): the frame as a direct buffer (the Vulkan path); not done yet, the engine then has no frame */
static void MP_lastFrameData(tl_jcall *c)
{
    static bool said; if (!said) { said = true; tl_log_line("media: getVideoLastFrameData is not implemented (no video frames on Vulkan)"); }
    c->ret = vl(NULL);
}

#define E(n, s, f) { CLS, n, s, f }
static const tl_jhle k_media[] = {
    E("<init>", "(ZZZ)V", MP_init),
    E("setDataSource", "(Ljava/lang/String;JJ)Z", MP_setSourceRange),
    E("setDataSource", "(Landroid/content/res/AssetManager;Ljava/lang/String;JJ)Z", MP_setSourceAsset),
    E("setDataSourceURL", "(Ljava/lang/String;)Z", MP_setSourceURL),
    E("prepare", "()V", MP_prepare), E("prepareAsync", "()V", MP_prepare),
    E("start", "()V", MP_start), E("pause", "()V", MP_pause), E("stop", "()V", MP_stop),
    E("reset", "()V", MP_reset), E("release", "()V", MP_release),
    E("seekTo", "(I)V", MP_seekTo), E("setLooping", "(Z)V", MP_setLooping), E("isLooping", "()Z", MP_isLooping),
    E("isPlaying", "()Z", MP_isPlaying), E("isPrepared", "()Z", MP_isPrepared), E("didComplete", "()Z", MP_didComplete),
    E("didResolutionChange", "()Z", MP_didResolutionChange),
    E("getDuration", "()I", MP_getDuration), E("getCurrentPosition", "()I", MP_getPosition),
    E("getVideoWidth", "()I", MP_getWidth), E("getVideoHeight", "()I", MP_getHeight),
    E("setAudioVolume", "(F)V", MP_setVolume), E("setAudioEnabled", "(Z)V", MP_setAudioEnabled), E("setVideoEnabled", "(Z)V", MP_setVideoEnabled),
    E("selectTrack", "(I)V", MP_selectTrack), E("getExternalTextureId", "()I", MP_externalTexture),
    E("GetVideoTracks", "()[L" CLS "$VideoTrackInfo;", MP_videoTracks),
    E("GetAudioTracks", "()[L" CLS "$AudioTrackInfo;", MP_audioTracks),
    E("GetCaptionTracks", "()[L" CLS "$CaptionTrackInfo;", MP_captionTracks),
    E("getVideoLastFrame", "(I)Z", MP_lastFrame),
    E("updateVideoFrame", "(I)L" CLS "$FrameUpdateInfo;", MP_updateFrame),
    E("getVideoLastFrameData", "()Ljava/nio/Buffer;", MP_lastFrameData),
    { NULL, NULL, NULL, NULL }
};

void tl_ue4_media_install(void)
{
    tl_jni_declare(CLS, "android/media/MediaPlayer");
    tl_jni_register_hle(k_media);
}
