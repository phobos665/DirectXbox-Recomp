/*
 * remote_stream.c -- the host's picture, to remote play's joiners
 * (remote_stream.h): H.264 through FFmpeg loaded at run time, the host's
 * encoding thread, and the joiner's reassembly and decoding.
 */
#include "remote_stream.h"
#include "remote_pads.h"
#include "recomp_config.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef REMOTE_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>

/* ---- FFmpeg, loaded at run time (as src/video/xmv_decode.c does) ---------- */

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE lib_t;
static lib_t lib_open(const char *dir, const char *name)
{
    char path[MAX_PATH * 2];
    if (!dir)
        return LoadLibraryA(name);
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *lib_sym(lib_t l, const char *s) { return (void *)GetProcAddress(l, s); }
#define STR2(x) #x
#define STR(x) STR2(x)
#define AVCODEC_NAME "avcodec-" STR(LIBAVCODEC_VERSION_MAJOR) ".dll"
#define AVUTIL_NAME  "avutil-" STR(LIBAVUTIL_VERSION_MAJOR) ".dll"
#else
#include <dlfcn.h>
typedef void *lib_t;
static lib_t lib_open(const char *dir, const char *name)
{
    char path[4096];
    if (!dir)
        return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}
static void *lib_sym(lib_t l, const char *s) { return dlsym(l, s); }
#define STR2(x) #x
#define STR(x) STR2(x)
#if defined(__APPLE__)
#define AVCODEC_NAME "libavcodec." STR(LIBAVCODEC_VERSION_MAJOR) ".dylib"
#define AVUTIL_NAME  "libavutil." STR(LIBAVUTIL_VERSION_MAJOR) ".dylib"
#else
#define AVCODEC_NAME "libavcodec.so." STR(LIBAVCODEC_VERSION_MAJOR)
#define AVUTIL_NAME  "libavutil.so." STR(LIBAVUTIL_VERSION_MAJOR)
#endif
#endif

static struct {
    int tried, ok;
    const AVCodec *(*find_encoder_by_name)(const char *);
    const AVCodec *(*find_decoder_by_name)(const char *);
    AVCodecContext *(*alloc_context3)(const AVCodec *);
    int  (*open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
    void (*free_context)(AVCodecContext **);
    int  (*send_frame)(AVCodecContext *, const AVFrame *);
    int  (*receive_packet)(AVCodecContext *, AVPacket *);
    int  (*send_packet)(AVCodecContext *, const AVPacket *);
    int  (*receive_frame)(AVCodecContext *, AVFrame *);
    AVPacket *(*packet_alloc)(void);
    void (*packet_free)(AVPacket **);
    void (*packet_unref)(AVPacket *);
    AVFrame *(*frame_alloc)(void);
    void (*frame_free)(AVFrame **);
    int  (*frame_get_buffer)(AVFrame *, int);
    int  (*frame_make_writable)(AVFrame *);
    int  (*opt_set)(void *, const char *, const char *, int);
    void (*log_set_level)(int);
} ff;

static int ff_load(void)
{
    const char *dirs[4];
    lib_t codec = NULL, util = NULL;
    int i;

    if (ff.tried)
        return ff.ok;
    ff.tried = 1;
    dirs[0] = NULL;
    dirs[1] = getenv("RECOMP_FFMPEG_DIR");
#ifdef REMOTE_FFMPEG_BIN
    dirs[2] = REMOTE_FFMPEG_BIN;
#else
    dirs[2] = NULL;
#endif
#if defined(__APPLE__)
    dirs[3] = "@executable_path";
#else
    dirs[3] = NULL;
#endif
    for (i = 0; i < 4 && !codec; i++) {
        if (i > 0 && !dirs[i])
            continue;
        if ((util = lib_open(dirs[i], AVUTIL_NAME)) != NULL)
            codec = lib_open(dirs[i], AVCODEC_NAME);
    }
    if (!codec || !util) {
        fprintf(stderr, "[REMOTE] no FFmpeg (%s, %s) beside the executable or in "
                "RECOMP_FFMPEG_DIR: remote play has no picture\n", AVCODEC_NAME, AVUTIL_NAME);
        return 0;
    }
#define LOAD(field, lib, name) \
    if (!(*(void **)&ff.field = lib_sym(lib, name))) { \
        fprintf(stderr, "[REMOTE] FFmpeg is missing %s: remote play has no picture\n", name); \
        return 0; }
    LOAD(find_encoder_by_name, codec, "avcodec_find_encoder_by_name");
    LOAD(find_decoder_by_name, codec, "avcodec_find_decoder_by_name");
    LOAD(alloc_context3, codec, "avcodec_alloc_context3");
    LOAD(open2, codec, "avcodec_open2");
    LOAD(free_context, codec, "avcodec_free_context");
    LOAD(send_frame, codec, "avcodec_send_frame");
    LOAD(receive_packet, codec, "avcodec_receive_packet");
    LOAD(send_packet, codec, "avcodec_send_packet");
    LOAD(receive_frame, codec, "avcodec_receive_frame");
    LOAD(packet_alloc, codec, "av_packet_alloc");
    LOAD(packet_free, codec, "av_packet_free");
    LOAD(packet_unref, codec, "av_packet_unref");
    LOAD(frame_alloc, util, "av_frame_alloc");
    LOAD(frame_free, util, "av_frame_free");
    LOAD(frame_get_buffer, util, "av_frame_get_buffer");
    LOAD(frame_make_writable, util, "av_frame_make_writable");
    LOAD(opt_set, util, "av_opt_set");
    LOAD(log_set_level, util, "av_log_set_level");
#undef LOAD
    ff.log_set_level(AV_LOG_ERROR);        /* an encoder that will not open says so loudly */
    ff.ok = 1;
    return 1;
}

/* ---- colour ------------------------------------------------------------ */

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* BT.601, studio range: what every decoder assumes of an H.264 stream that
 * says nothing about its colour. */
static void rgb_to_yuv(int r, int g, int b, int *y, int *u, int *v)
{
    *y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    *u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    *v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

/* Planar 4:2:0 (or NV12) from RGBA at the same size. w and h even. */
static void rgba_to_frame(const uint8_t *rgba, unsigned pitch, unsigned w, unsigned h, AVFrame *f)
{
    unsigned x, y;
    int nv12 = f->format == AV_PIX_FMT_NV12;

    for (y = 0; y < h; y += 2) {
        const uint8_t *r0 = rgba + (size_t)y * pitch, *r1 = r0 + pitch;
        uint8_t *y0 = f->data[0] + (size_t)y * f->linesize[0], *y1 = y0 + f->linesize[0];
        uint8_t *u = f->data[1] + (size_t)(y / 2) * f->linesize[1];
        uint8_t *v = nv12 ? NULL : f->data[2] + (size_t)(y / 2) * f->linesize[2];

        for (x = 0; x < w; x += 2) {
            int yy, uu, vv, su = 0, sv = 0, k;
            const uint8_t *p[4] = { r0 + x * 4, r0 + x * 4 + 4, r1 + x * 4, r1 + x * 4 + 4 };
            uint8_t *dy[4] = { y0 + x, y0 + x + 1, y1 + x, y1 + x + 1 };
            for (k = 0; k < 4; k++) {
                rgb_to_yuv(p[k][0], p[k][1], p[k][2], &yy, &uu, &vv);
                *dy[k] = clamp8(yy);
                su += uu;
                sv += vv;
            }
            if (nv12) {
                u[x] = clamp8(su / 4);
                u[x + 1] = clamp8(sv / 4);
            } else {
                u[x / 2] = clamp8(su / 4);
                v[x / 2] = clamp8(sv / 4);
            }
        }
    }
}

/* RECOMP_REMOTE_PLAY_TRACE=1: a line per encoded packet. */
static int stream_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_config_int("RECOMP_REMOTE_PLAY_TRACE", NULL, 0);
    return on;
}

/* ---- the encoder ------------------------------------------------------- */

struct RemoteEncoder {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;
    int64_t         pts;
    char            name[32];
};

/* In order of preference. On a Mac libx264 comes first: VideoToolbox holds
 * every frame back two frames (33 ms at 60 fps) whatever it is asked, and
 * this FFmpeg has no low-latency mode for it, while x264's ultrafast,
 * zero-latency mode hands each frame back as it goes in, for a few percent
 * of one core at 640x480. A release's LGPL FFmpeg has no x264, and then it
 * is VideoToolbox. */
static const char *const ENCODERS[] = {
#if defined(__APPLE__)
    "libx264", "h264_videotoolbox",
#elif defined(_WIN32)
    "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf",
#else
    "h264_nvenc",
#endif
#if !defined(__APPLE__)
    "libx264",
#endif
    "libopenh264", NULL
};

static void encoder_options(AVCodecContext *c, const char *name)
{
    /* Low delay above all: one frame in, one frame out. */
    if (!strcmp(name, "libx264")) {
        ff.opt_set(c->priv_data, "preset", "ultrafast", 0);
        ff.opt_set(c->priv_data, "tune", "zerolatency", 0);
    } else if (!strcmp(name, "h264_videotoolbox")) {
        ff.opt_set(c->priv_data, "realtime", "1", 0);
        ff.opt_set(c->priv_data, "allow_sw", "1", 0);
        ff.opt_set(c->priv_data, "prio_speed", "1", 0);
        ff.opt_set(c->priv_data, "profile", "constrained_baseline", 0);
        ff.opt_set(c->priv_data, "max_ref_frames", "1", 0);
    } else if (!strcmp(name, "h264_nvenc")) {
        ff.opt_set(c->priv_data, "preset", "p1", 0);
        ff.opt_set(c->priv_data, "tune", "ull", 0);
        ff.opt_set(c->priv_data, "zerolatency", "1", 0);
    } else if (!strcmp(name, "h264_amf")) {
        ff.opt_set(c->priv_data, "usage", "ultralowlatency", 0);
    } else if (!strcmp(name, "h264_qsv")) {
        ff.opt_set(c->priv_data, "preset", "veryfast", 0);
    }
}

static RemoteEncoder *open_encoder(const char *name, unsigned w, unsigned h, int kbps)
{
    static const enum AVPixelFormat fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12 };
    const AVCodec *codec = ff.find_encoder_by_name(name);
    size_t f;

    if (!codec)
        return NULL;
    for (f = 0; f < sizeof fmts / sizeof fmts[0]; f++) {
        RemoteEncoder *e = (RemoteEncoder *)calloc(1, sizeof *e);
        AVCodecContext *c;

        if (!e || !(c = ff.alloc_context3(codec))) {
            free(e);
            return NULL;
        }
        c->width = (int)w;
        c->height = (int)h;
        c->pix_fmt = fmts[f];
        c->time_base = (AVRational){ 1, 60 };
        c->framerate = (AVRational){ 60, 1 };
        c->gop_size = 600;                  /* keyframes on request, and every 10 s */
        c->max_b_frames = 0;
        c->bit_rate = (int64_t)kbps * 1000;
        c->rc_max_rate = c->bit_rate;
        c->rc_buffer_size = (int)(c->bit_rate / 15);   /* about four frames */
        c->flags |= AV_CODEC_FLAG_LOW_DELAY;
        encoder_options(c, name);
        if (ff.open2(c, codec, NULL) < 0) {
            ff.free_context(&c);
            free(e);
            continue;
        }
        e->ctx = c;
        e->frame = ff.frame_alloc();
        e->pkt = ff.packet_alloc();
        if (!e->frame || !e->pkt) {
            remote_encoder_destroy(e);
            return NULL;
        }
        e->frame->format = c->pix_fmt;
        e->frame->width = c->width;
        e->frame->height = c->height;
        if (ff.frame_get_buffer(e->frame, 0) < 0) {
            remote_encoder_destroy(e);
            return NULL;
        }
        snprintf(e->name, sizeof e->name, "%s", name);
        return e;
    }
    return NULL;
}

RemoteEncoder *remote_encoder_create(const char *name, unsigned w, unsigned h, int kbps)
{
    int i;

    if (!ff_load() || !w || !h || (w | h) & 1)
        return NULL;
    if (name && *name)
        return open_encoder(name, w, h, kbps);
    for (i = 0; ENCODERS[i]; i++) {
        RemoteEncoder *e = open_encoder(ENCODERS[i], w, h, kbps);
        if (e)
            return e;
    }
    return NULL;
}

const char *remote_encoder_name(const RemoteEncoder *e) { return e ? e->name : ""; }

int remote_encoder_frame(RemoteEncoder *e, const uint8_t *rgba, unsigned pitch, int key,
                         void (*out)(const uint8_t *, size_t, int, void *), void *user)
{
    int n = 0;

    if (ff.frame_make_writable(e->frame) < 0)
        return -1;
    rgba_to_frame(rgba, pitch, (unsigned)e->ctx->width, (unsigned)e->ctx->height, e->frame);
    e->frame->pts = e->pts++;
    e->frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
#ifdef AV_FRAME_FLAG_KEY
    if (key)
        e->frame->flags |= AV_FRAME_FLAG_KEY;
    else
        e->frame->flags &= ~AV_FRAME_FLAG_KEY;
#endif
    if (ff.send_frame(e->ctx, e->frame) < 0)
        return -1;
    {
        /* A hardware encoder answers later, on a thread of its own:
         * VideoToolbox's packet for a frame is ready a few milliseconds
         * after it went in, and FFmpeg hands it over only when asked again.
         * Asked once, every frame went out a frame or more late and a
         * forced keyframe came out after the frames before it. So wait a
         * little, until the packet for this frame (by its pts) is out;
         * any older one found on the way goes too. */
        int64_t mine = e->pts - 1;
        uint64_t until = SDL_GetTicks() + 12;
        for (;;) {
            int r = ff.receive_packet(e->ctx, e->pkt);
            if (r == 0) {
                int64_t pts = e->pkt->pts;
                if (stream_trace())
                    fprintf(stderr, "[REMOTE] encoded frame %lld (sent %lld%s): pts %lld, %d bytes%s\n",
                            (long long)mine, (long long)mine, key ? ", key asked" : "",
                            (long long)pts, e->pkt->size,
                            (e->pkt->flags & AV_PKT_FLAG_KEY) ? ", keyframe" : "");
                out(e->pkt->data, (size_t)e->pkt->size,
                    (e->pkt->flags & AV_PKT_FLAG_KEY) != 0, user);
                ff.packet_unref(e->pkt);
                n++;
                if (pts == AV_NOPTS_VALUE || pts >= mine)
                    break;
                continue;
            }
            if (r != AVERROR(EAGAIN) || SDL_GetTicks() >= until)
                break;
            SDL_Delay(1);
        }
    }
    return n;
}

void remote_encoder_destroy(RemoteEncoder *e)
{
    if (!e)
        return;
    if (e->ctx)
        ff.free_context(&e->ctx);
    if (e->frame)
        ff.frame_free(&e->frame);
    if (e->pkt)
        ff.packet_free(&e->pkt);
    free(e);
}

/* ---- the decoder ------------------------------------------------------- */

struct RemoteDecoder {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;
    uint8_t        *in;          /* the frame, with FFmpeg's padding */
    size_t          in_cap;
    uint8_t        *bgra;
    size_t          bgra_cap;
};

RemoteDecoder *remote_decoder_create(void)
{
    const AVCodec *codec;
    RemoteDecoder *d;

    if (!ff_load() || !(codec = ff.find_decoder_by_name("h264")))
        return NULL;
    if (!(d = (RemoteDecoder *)calloc(1, sizeof *d)))
        return NULL;
    d->ctx = ff.alloc_context3(codec);
    d->frame = ff.frame_alloc();
    d->pkt = ff.packet_alloc();
    if (!d->ctx || !d->frame || !d->pkt) {
        remote_decoder_destroy(d);
        return NULL;
    }
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->thread_count = 1;           /* frame threads would hold frames back */
    if (ff.open2(d->ctx, codec, NULL) < 0) {
        remote_decoder_destroy(d);
        return NULL;
    }
    return d;
}

void remote_decoder_destroy(RemoteDecoder *d)
{
    if (!d)
        return;
    if (d->ctx)
        ff.free_context(&d->ctx);
    if (d->frame)
        ff.frame_free(&d->frame);
    if (d->pkt)
        ff.packet_free(&d->pkt);
    free(d->in);
    free(d->bgra);
    free(d);
}

int remote_decoder_frame(RemoteDecoder *d, const uint8_t *data, size_t len,
                         const uint8_t **bgra, unsigned *w, unsigned *h)
{
    int got = 0;

    if (len + AV_INPUT_BUFFER_PADDING_SIZE > d->in_cap) {
        uint8_t *n = (uint8_t *)realloc(d->in, len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!n)
            return -1;
        d->in = n;
        d->in_cap = len + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memcpy(d->in, data, len);
    memset(d->in + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    d->pkt->data = d->in;
    d->pkt->size = (int)len;
    if (ff.send_packet(d->ctx, d->pkt) < 0)
        return -1;
    while (ff.receive_frame(d->ctx, d->frame) == 0) {
        AVFrame *f = d->frame;
        unsigned fw = (unsigned)f->width, fh = (unsigned)f->height, x, y;
        int nv12 = f->format == AV_PIX_FMT_NV12;
        size_t need = (size_t)fw * fh * 4;

        if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && !nv12)
            continue;
        if (need > d->bgra_cap) {
            uint8_t *n = (uint8_t *)realloc(d->bgra, need);
            if (!n)
                return -1;
            d->bgra = n;
            d->bgra_cap = need;
        }
        for (y = 0; y < fh; y++) {
            const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
            const uint8_t *pu = f->data[1] + (size_t)(y / 2) * f->linesize[1];
            const uint8_t *pv = nv12 ? pu + 1 : f->data[2] + (size_t)(y / 2) * f->linesize[2];
            uint8_t *o = d->bgra + (size_t)y * fw * 4;
            for (x = 0; x < fw; x++) {
                int c = py[x] - 16;
                int du = (nv12 ? pu[(x / 2) * 2] : pu[x / 2]) - 128;
                int dv = (nv12 ? pv[(x / 2) * 2] : pv[x / 2]) - 128;
                o[x * 4 + 0] = clamp8((298 * c + 516 * du + 128) >> 8);
                o[x * 4 + 1] = clamp8((298 * c - 100 * du - 208 * dv + 128) >> 8);
                o[x * 4 + 2] = clamp8((298 * c + 409 * dv + 128) >> 8);
                o[x * 4 + 3] = 0xFF;
            }
        }
        *bgra = d->bgra;
        *w = fw;
        *h = fh;
        got = 1;
    }
    return got;
}

#else  /* !REMOTE_HAVE_FFMPEG */

RemoteEncoder *remote_encoder_create(const char *n, unsigned w, unsigned h, int k)
{ (void)n; (void)w; (void)h; (void)k; return NULL; }
const char *remote_encoder_name(const RemoteEncoder *e) { (void)e; return ""; }
int remote_encoder_frame(RemoteEncoder *e, const uint8_t *p, unsigned pitch, int key,
                         void (*out)(const uint8_t *, size_t, int, void *), void *user)
{ (void)e; (void)p; (void)pitch; (void)key; (void)out; (void)user; return -1; }
void remote_encoder_destroy(RemoteEncoder *e) { (void)e; }
RemoteDecoder *remote_decoder_create(void) { return NULL; }
void remote_decoder_destroy(RemoteDecoder *d) { (void)d; }
int remote_decoder_frame(RemoteDecoder *d, const uint8_t *p, size_t n, const uint8_t **o,
                         unsigned *w, unsigned *h)
{ (void)d; (void)p; (void)n; (void)o; (void)w; (void)h; return -1; }

#endif /* REMOTE_HAVE_FFMPEG */

/* ---- the host: a thread that encodes the newest frame ------------------ */

static struct {
    SDL_Mutex     *lock;
    SDL_Condition *ready;
    SDL_Thread    *thread;
    int            started, off, failed;
    uint8_t       *pending;          /* RGBA at pw x ph, the newest frame  */
    unsigned       pw, ph;
    int            have;
    unsigned long  sent, dropped, handed, replaced;
} S;

static void send_packet(const uint8_t *data, size_t len, int key, void *user)
{
    uint32_t *frame = (uint32_t *)user;
    recomp_remote_send_video((*frame)++, data, len, key);
}

static int SDLCALL encode_thread(void *arg)
{
    RemoteEncoder *enc = NULL;
    uint8_t *mine = NULL;
    unsigned w = 0, h = 0;
    uint32_t frame = 0;
    int kbps = recomp_config_int("RECOMP_REMOTE_PLAY_BITRATE", "remote_play_bitrate", 6000);
    const char *want = recomp_config_lookup("RECOMP_REMOTE_PLAY_ENCODER", "remote_play_encoder");
    (void)arg;

    for (;;) {
        int key;

        SDL_LockMutex(S.lock);
        while (!S.have)
            SDL_WaitCondition(S.ready, S.lock);
        if (!mine || S.pw != w || S.ph != h) {
            free(mine);
            w = S.pw;
            h = S.ph;
            mine = (uint8_t *)malloc((size_t)w * h * 4);
            remote_encoder_destroy(enc);
            enc = NULL;
        }
        if (mine)
            memcpy(mine, S.pending, (size_t)w * h * 4);
        S.have = 0;
        SDL_UnlockMutex(S.lock);
        if (!mine)
            continue;
        if (!enc) {
            enc = remote_encoder_create(want, w, h, kbps);
            if (!enc) {
                fprintf(stderr, "[REMOTE] no H.264 encoder would open for %ux%u: remote "
                        "play has no picture\n", w, h);
                fflush(stderr);
                S.failed = 1;
                free(mine);
                return 0;
            }
            fprintf(stderr, "[REMOTE] picture: %ux%u H.264 through %s, %d kbit/s\n", w, h,
                    remote_encoder_name(enc), kbps);
            fflush(stderr);
            recomp_remote_keyframe_wanted();     /* the first frame is one anyway */
            key = 1;
        } else {
            key = recomp_remote_keyframe_wanted();
        }
        if (remote_encoder_frame(enc, mine, w * 4, key, send_packet, &frame) < 0)
            S.dropped++;
        else
            S.sent++;
        if (stream_trace()) {
            static uint64_t next;
            uint64_t now = SDL_GetTicks();
            if (now >= next) {
                fprintf(stderr, "[REMOTE] frames: %lu handed over, %lu replaced before "
                        "encoding, %lu encoded, %lu failed\n", S.handed, S.replaced, S.sent,
                        S.dropped);
                next = now + 1000;
            }
        }
    }
}

static void stream_start(void)
{
    if (S.started)
        return;
    S.started = 1;
    S.off = !recomp_config_int("RECOMP_REMOTE_PLAY_VIDEO", "remote_play_video", 1);
    if (S.off)
        return;
    S.lock = SDL_CreateMutex();
    S.ready = SDL_CreateCondition();
    if (!S.lock || !S.ready ||
        !(S.thread = SDL_CreateThread(encode_thread, "remote picture", NULL)))
        S.off = 1;
}

int remote_stream_wanted(void)
{
    if (!recomp_remote_mask())
        return 0;
    stream_start();
    return !S.off && !S.failed;
}

/* Box-filters src (R8G8B8A8) down to out_w x out_h, or samples it when the
 * two are not a whole multiple apart. */
static void scale_into(uint8_t *dst, unsigned ow, unsigned oh, const uint8_t *src,
                       unsigned sw, unsigned sh, unsigned pitch)
{
    unsigned x, y;

    if (sw % ow == 0 && sh % oh == 0) {
        unsigned fx = sw / ow, fy = sh / oh, n = fx * fy;
        for (y = 0; y < oh; y++)
            for (x = 0; x < ow; x++) {
                unsigned s[3] = { 0, 0, 0 }, i, j;
                for (j = 0; j < fy; j++) {
                    const uint8_t *p = src + (size_t)(y * fy + j) * pitch + (size_t)x * fx * 4;
                    for (i = 0; i < fx; i++, p += 4) {
                        s[0] += p[0];
                        s[1] += p[1];
                        s[2] += p[2];
                    }
                }
                dst[0] = (uint8_t)(s[0] / n);
                dst[1] = (uint8_t)(s[1] / n);
                dst[2] = (uint8_t)(s[2] / n);
                dst[3] = 0xFF;
                dst += 4;
            }
        return;
    }
    for (y = 0; y < oh; y++) {
        const uint8_t *row = src + (size_t)(y * sh / oh) * pitch;
        for (x = 0; x < ow; x++) {
            memcpy(dst, row + (size_t)(x * sw / ow) * 4, 4);
            dst[3] = 0xFF;
            dst += 4;
        }
    }
}

void remote_stream_frame(const uint8_t *rgba, unsigned src_w, unsigned src_h, unsigned pitch,
                         unsigned out_w, unsigned out_h)
{
    out_w &= ~1u;
    out_h &= ~1u;
    if (!rgba || !out_w || !out_h || !remote_stream_wanted())
        return;
    SDL_LockMutex(S.lock);
    if (!S.pending || S.pw != out_w || S.ph != out_h) {
        free(S.pending);
        S.pending = (uint8_t *)malloc((size_t)out_w * out_h * 4);
        S.pw = out_w;
        S.ph = out_h;
    }
    if (S.pending) {
        scale_into(S.pending, out_w, out_h, rgba, src_w, src_h, pitch);
        S.handed++;
        S.replaced += S.have;
        S.have = 1;
        SDL_SignalCondition(S.ready);
    }
    SDL_UnlockMutex(S.lock);
}

/* ---- the joiner: frames back together --------------------------------- */

#define MAX_PIECES 1024

struct RemoteAssembler {
    uint32_t frame;              /* the one being collected            */
    int      collecting, key;
    uint16_t count, got;
    uint8_t  have[MAX_PIECES];
    uint16_t lens[MAX_PIECES];
    uint8_t *buf;                /* MAX_PIECES * REMOTE_VIDEO_CHUNK    */
    uint8_t *whole;
    int      done_any;
    uint32_t last_done;
    int      need_key;           /* after a loss, wait for a keyframe  */
};

RemoteAssembler *remote_assembler_create(void)
{
    RemoteAssembler *a = (RemoteAssembler *)calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->buf = (uint8_t *)malloc((size_t)MAX_PIECES * REMOTE_VIDEO_CHUNK);
    a->whole = (uint8_t *)malloc((size_t)MAX_PIECES * REMOTE_VIDEO_CHUNK);
    if (!a->buf || !a->whole) {
        remote_assembler_destroy(a);
        return NULL;
    }
    a->need_key = 1;             /* the picture starts at a keyframe */
    return a;
}

void remote_assembler_destroy(RemoteAssembler *a)
{
    if (!a)
        return;
    free(a->buf);
    free(a->whole);
    free(a);
}

int remote_assembler_piece(RemoteAssembler *a, uint32_t frame, uint16_t index, uint16_t count,
                           int key, const uint8_t *data, size_t len,
                           const uint8_t **out, size_t *out_len, int *lost)
{
    *lost = 0;
    if (count > MAX_PIECES || index >= count || len > REMOTE_VIDEO_CHUNK)
        return 0;
    if (a->done_any && (int32_t)(frame - a->last_done) <= 0)
        return 0;                                   /* late, or already shown */
    if (a->collecting && frame != a->frame) {
        if ((int32_t)(frame - a->frame) < 0)
            return 0;                               /* older than the one collecting */
        *lost = 1;                                  /* that one never completed */
        a->need_key = 1;
        a->collecting = 0;
    }
    if (!a->collecting) {
        if (a->done_any && frame != a->last_done + 1 && !*lost) {
            *lost = 1;                              /* whole frames went missing */
            a->need_key = 1;
        }
        a->collecting = 1;
        a->frame = frame;
        a->count = count;
        a->got = 0;
        a->key = key;
        memset(a->have, 0, count);
    }
    if (count != a->count || a->have[index])
        return 0;
    memcpy(a->buf + (size_t)index * REMOTE_VIDEO_CHUNK, data, len);
    a->lens[index] = (uint16_t)len;
    a->have[index] = 1;
    if (++a->got < a->count)
        return 0;
    a->collecting = 0;
    a->done_any = 1;
    a->last_done = frame;
    if (a->need_key && !a->key)
        return 0;                                   /* undecodable until a keyframe */
    a->need_key = 0;
    {
        size_t n = 0;
        uint16_t i;
        for (i = 0; i < a->count; i++) {
            memcpy(a->whole + n, a->buf + (size_t)i * REMOTE_VIDEO_CHUNK, a->lens[i]);
            n += a->lens[i];
        }
        *out = a->whole;
        *out_len = n;
    }
    return 1;
}
