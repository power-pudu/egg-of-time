/* Movie mods: an MP4 file (H.264 or H.265, up to 4K) in mods/cutscenes next to the executable replaces one of the
 * game's Mobiclip cutscenes while the Mods setting is on. The file is named after the movie in the ROM:
 * /movie/op_ds.mods -> mods/cutscenes/op_ds.mp4 (the list is in res/mods/cutscenes/readme.txt, copied there by
 * the build).
 *
 * Every cutscene goes through one of two game functions that take the movie's path in r0 and return once it has
 * played (ov017 0x02198d20, and 0x02198da0 for the scenes that also draw on the bottom screen: the intro, the
 * Extras theater, the ending credits). Both are wrapped (tools/recomp/overrides.txt): with a replacement, the
 * game thread plays it instead and returns without running the game's player, so the emulated machine pauses
 * meanwhile (the scheduler and the sound driver skip the missed time, as after any host stall).
 *
 * FFmpeg demuxes and decodes it, on the hardware decoder the codec offers (D3D11VA, VideoToolbox, VAAPI, ...)
 * when one opens, else in software on every core. Frames are paced by the audio actually played (by the wall
 * clock where there is no audio), handed to the host thread as YUV planes and drawn over the whole window, keeping
 * the video's shape. The file is read ahead of the video, which waits in a packet queue, so AUDIO_AHEAD of sound is
 * always queued: a 4K frame's copy back from the GPU alone takes most of a 60 fps frame's time, and with only the
 * audio up to the frame on screen queued, every slow frame let the queue run dry and the sound stuttered. Frames
 * already too late to show are dropped before that copy, so decoding catches up. The audio track (any codec
 * FFmpeg reads, downmixed to stereo) plays through its own SDL audio stream. START or a touch skips, as with the
 * game's player (the intro: any button or a touch); that also sets the game's "skipped" byte. */
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "dispatch.h"
#include "display.h"
#include "movie.h"
#include "rt.h"

#define SKIPPED_FLAG 0x0207b3d0u /* set by the game's per-frame callbacks when the player skips */
#define LATE_DROP 0.1            /* seconds: a frame this late is not shown (decoding is behind) */
#define AUDIO_AHEAD 0.3          /* seconds of audio kept queued ahead of the speakers */
#define VIDEO_AHEAD 240          /* packets: the most video read ahead of decoding to reach that audio */
#define INTRO_FRAME 0x02199930u  /* the intro's per-frame callback (ov017): it skips on any button (0x02199894) */

/* ---- frame hand-over to the host thread ---- */

static SDL_Mutex *lock;
static SDL_AtomicInt playing;
static AVFrame *latest;  /* latest frame to show: YUV420P or NV12 (under lock) */
static uint64_t serial;  /* counts published frames (under lock) */

static void publish(AVFrame *f) {
    SDL_LockMutex(lock);
    av_frame_unref(latest);
    av_frame_move_ref(latest, f);
    serial++;
    SDL_UnlockMutex(lock);
}

static SDL_Colorspace colorspace(const AVFrame *f) {
    int full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    default: /* unspecified: HD sizes are BT.709 */
        if (f->height > 576) return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
        return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    }
}

int movie_draw(SDL_Renderer *ren) {
    static SDL_Texture *tex;
    static int tex_w, tex_h, tex_fmt, tex_cs;
    static uint64_t shown;
    static AVFrame *f;
    static float aspect = 1;
    if (!lock || !SDL_GetAtomicInt(&playing)) return 0;
    if (!f) f = av_frame_alloc();
    SDL_LockMutex(lock);
    int fresh = serial != shown && latest->buf[0];
    if (fresh) {
        shown = serial;
        av_frame_ref(f, latest);
    }
    SDL_UnlockMutex(lock);
    if (fresh) {
        int fmt = f->format == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
        int cs = (int)colorspace(f);
        if (!tex || tex_w != f->width || tex_h != f->height || tex_fmt != fmt || tex_cs != cs) {
            if (tex) SDL_DestroyTexture(tex);
            SDL_PropertiesID p = SDL_CreateProperties();
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, fmt);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
            tex = SDL_CreateTextureWithProperties(ren, p);
            SDL_DestroyProperties(p);
            if (!tex) rt_log("movie: cannot create a %dx%d texture: %s", f->width, f->height, SDL_GetError());
            else SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
            tex_w = f->width;
            tex_h = f->height;
            tex_fmt = fmt;
            tex_cs = cs;
        }
        if (tex && fmt == SDL_PIXELFORMAT_NV12) {
            SDL_UpdateNVTexture(tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
        } else if (tex) {
            SDL_UpdateYUVTexture(tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                                 f->linesize[2]);
        }
        AVRational sar = f->sample_aspect_ratio;
        aspect = (float)f->width / f->height * (sar.num > 0 && sar.den > 0 ? (float)av_q2d(sar) : 1.0f);
        av_frame_unref(f);
    }
    if (tex) {
        int ow, oh;
        SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
        float w = (float)ow, h = w / aspect;
        if (h > oh) { h = (float)oh; w = h * aspect; }
        SDL_RenderTexture(ren, tex, NULL, &(SDL_FRect){(ow - w) / 2, (oh - h) / 2, w, h});
    }
    return 1;
}

/* ---- decoding (game thread) ---- */

typedef struct Player {
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vstream, astream;
    enum AVPixelFormat hw_fmt; /* AV_PIX_FMT_NONE: software decoding */
    AVBufferRef *hw_device;
    struct SwsContext *sws;
    SwrContext *swr;
    SDL_AudioStream *audio;
    int audio_rate;            /* Hz, stereo float */
    uint64_t audio_bytes;      /* pushed to the audio stream */
    double audio_start;        /* the first audio sample's time (seconds) */
    double clock_at;           /* clock_now: the time last read from the audio, */
    Uint64 clock_ns;           /* and when */
    uint32_t skip_keys, prev_keys; /* skip_keys: the buttons that skip */
    int prev_touch, skipped;
} Player;

/* Hardware decoder types, in preference order (any other the codec offers comes after). */
static const enum AVHWDeviceType hw_preferred[] = {
    AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA,
#if LIBAVUTIL_VERSION_MAJOR >= 59 /* FFmpeg 7 */
    AV_HWDEVICE_TYPE_D3D12VA,
#endif
    AV_HWDEVICE_TYPE_DXVA2,   AV_HWDEVICE_TYPE_VDPAU,
};

static enum AVPixelFormat get_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
    const Player *p = c->opaque;
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++) {
        if (*f == p->hw_fmt) return *f;
    }
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++) { /* the hardware refused: software */
        if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    }
    return AV_PIX_FMT_NONE;
}

/* Opens a hardware device for the codec: the preferred types first, then any other it supports. */
static void open_hw(Player *p, const AVCodec *codec) {
    p->hw_fmt = AV_PIX_FMT_NONE;
    for (size_t k = 0; k <= SDL_arraysize(hw_preferred); k++) { /* k = the array's size: any other type */
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
            int preferred = 0;
            for (size_t j = 0; j < SDL_arraysize(hw_preferred); j++) preferred |= cfg->device_type == hw_preferred[j];
            if (k < SDL_arraysize(hw_preferred) ? cfg->device_type != hw_preferred[k] : preferred) continue;
            if (av_hwdevice_ctx_create(&p->hw_device, cfg->device_type, NULL, NULL, 0) == 0) {
                p->hw_fmt = cfg->pix_fmt;
                rt_log("movie: %s decoding on %s", codec->name, av_hwdevice_get_type_name(cfg->device_type));
                return;
            }
        }
    }
    rt_log("movie: %s decoding in software", codec->name);
}

static AVCodecContext *open_decoder(Player *p, int stream, int video) {
    AVCodecParameters *par = p->fmt->streams[stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, par) < 0) {
        avcodec_free_context(&c);
        return NULL;
    }
    c->pkt_timebase = p->fmt->streams[stream]->time_base;
    c->opaque = p;
    if (video) {
        open_hw(p, codec);
        if (p->hw_device) {
            c->hw_device_ctx = av_buffer_ref(p->hw_device);
            c->get_format = get_format;
        } else {
            c->thread_count = 0; /* one per core */
        }
    }
    if (avcodec_open2(c, codec, NULL) < 0) avcodec_free_context(&c);
    return c;
}

/* Seconds into the movie: the audio played so far, carried on by the wall clock while next to none is queued
 * (before the first audio frame, after the track ends, without an audio track). Not quite none: SDL's resampler
 * holds back a few samples until the stream is flushed, and with the video frame waiting for that audio, the
 * clock would stop. */
static double clock_now(Player *p) {
    Uint64 now = SDL_GetTicksNS();
    int queued = p->audio ? SDL_GetAudioStreamQueued(p->audio) : 0;
    if (queued > p->audio_rate * 8 / 50) { /* 20 ms */
        p->clock_at = p->audio_start + (double)(p->audio_bytes - SDL_min((uint64_t)queued, p->audio_bytes)) /
                                           (p->audio_rate * 8.0);
        p->clock_ns = now;
    }
    return p->clock_at + (double)(now - p->clock_ns) / 1e9;
}

/* A skip button pressed or the screen touched since the last check (holds from before the movie do not count). */
static int skip_pressed(Player *p) {
    uint32_t keys = rt_keys;
    int touch = rt_touch_down;
    int hit = ((keys & ~p->prev_keys) & p->skip_keys) || (touch && !p->prev_touch);
    p->prev_keys = keys;
    p->prev_touch = touch;
    if (hit) p->skipped = 1;
    return p->skipped;
}

/* Converts a decoded frame to one the host can upload (YUV420P or NV12) and shows it at its time. */
static void show_video(Player *p, AVFrame *f) {
    double t = f->best_effort_timestamp == AV_NOPTS_VALUE
                   ? clock_now(p)
                   : f->best_effort_timestamp * av_q2d(p->fmt->streams[p->vstream]->time_base);
    /* Too late to show: dropped here, before the copy and conversion (a 4K copy takes most of a frame's time, and
     * dropping after it kept decoding behind). */
    if (t - clock_now(p) <= -LATE_DROP) return;
    AVFrame *out = av_frame_alloc();
    if (!out) return;
    if (f->format == p->hw_fmt) { /* copy back from the GPU (NV12, or P010 for 10-bit) */
        if (av_hwframe_transfer_data(out, f, 0) < 0 || av_frame_copy_props(out, f) < 0) {
            av_frame_free(&out);
            return;
        }
        av_frame_unref(f);
        av_frame_move_ref(f, out);
    }
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_NV12) {
        av_frame_move_ref(out, f);
    } else { /* 10-bit, 4:2:2, 4:4:4, ...: to NV12 */
        p->sws = sws_getCachedContext(p->sws, f->width, f->height, f->format, f->width, f->height, AV_PIX_FMT_NV12,
                                      SWS_BILINEAR, NULL, NULL, NULL);
        out->format = AV_PIX_FMT_NV12;
        out->width = f->width;
        out->height = f->height;
        if (!p->sws || av_frame_get_buffer(out, 0) < 0 || av_frame_copy_props(out, f) < 0 ||
            sws_scale(p->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, out->data, out->linesize) < 0) {
            av_frame_free(&out);
            return;
        }
    }
    for (;;) {
        if (skip_pressed(p)) break;
        double wait = t - clock_now(p);
        if (wait <= 0) {
            publish(out); /* on time when the copy began (show_video's start) */
            break;
        }
        SDL_DelayPrecise((Uint64)(SDL_min(wait, 0.002) * 1e9));
    }
    av_frame_free(&out);
}

static void play_audio(Player *p, AVFrame *f) {
    if (!p->swr) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&p->swr, &stereo, AV_SAMPLE_FMT_FLT, f->sample_rate, &f->ch_layout, f->format,
                                f->sample_rate, 0, NULL) < 0 ||
            swr_init(p->swr) < 0) {
            return;
        }
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, f->sample_rate};
        p->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!p->audio) {
            rt_log("movie: no audio output: %s", SDL_GetError());
            return;
        }
        p->audio_rate = f->sample_rate;
        p->audio_start = f->best_effort_timestamp == AV_NOPTS_VALUE
                             ? 0
                             : f->best_effort_timestamp * av_q2d(p->fmt->streams[p->astream]->time_base);
        SDL_ResumeAudioStreamDevice(p->audio);
    }
    if (!p->audio) return;
    int max = swr_get_out_samples(p->swr, f->nb_samples);
    float *buf = av_malloc((size_t)max * 2 * sizeof(float));
    if (!buf) return;
    uint8_t *outp[1] = {(uint8_t *)buf};
    /* void *: the input is const uint8_t ** before FFmpeg 7, const uint8_t *const * since */
    int n = swr_convert(p->swr, outp, max, (void *)f->extended_data, f->nb_samples);
    if (n > 0 && SDL_PutAudioStreamData(p->audio, buf, n * 2 * (int)sizeof(float))) {
        p->audio_bytes += (uint64_t)n * 2 * sizeof(float);
    }
    av_free(buf);
}

/* Sends a packet (NULL: end of stream) to a decoder and handles the frames it gives back. */
static void decode(Player *p, AVCodecContext *c, const AVPacket *pkt, AVFrame *f) {
    if (!c || avcodec_send_packet(c, pkt) < 0) return;
    while (!p->skipped && avcodec_receive_frame(c, f) == 0) {
        if (c == p->vdec) show_video(p, f);
        else play_audio(p, f);
        av_frame_unref(f);
    }
}

static void close_player(Player *p) {
    if (p->audio) SDL_DestroyAudioStream(p->audio);
    swr_free(&p->swr);
    sws_freeContext(p->sws);
    avcodec_free_context(&p->vdec);
    avcodec_free_context(&p->adec);
    av_buffer_unref(&p->hw_device);
    avformat_close_input(&p->fmt);
}

/* Plays the file to its end or until skipped (skip_keys or a touch); 0 if it cannot be played (the game then plays
 * its own movie). */
static int play_file(const char *path, uint32_t skip_keys) {
    Player p = {0};
    p.skip_keys = skip_keys;
    p.vstream = p.astream = -1;
    if (avformat_open_input(&p.fmt, path, NULL, NULL) < 0) {
        rt_log("movie: cannot open %s", path);
        return 0;
    }
    if (avformat_find_stream_info(p.fmt, NULL) < 0) {
        rt_log("movie: cannot read %s", path);
        close_player(&p);
        return 0;
    }
    p.vstream = av_find_best_stream(p.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    p.astream = av_find_best_stream(p.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (p.vstream < 0 || !(p.vdec = open_decoder(&p, p.vstream, 1))) {
        rt_log("movie: no playable video in %s", path);
        close_player(&p);
        return 0;
    }
    if (p.astream >= 0) p.adec = open_decoder(&p, p.astream, 0);
    rt_log("movie: playing %s (%dx%d %s)", path, p.vdec->width, p.vdec->height, p.vdec->codec->name);

    if (!lock) {
        lock = SDL_CreateMutex();
        latest = av_frame_alloc();
    }
    p.prev_keys = rt_keys;
    p.prev_touch = rt_touch_down;
    p.clock_ns = SDL_GetTicksNS();
    SDL_SetAtomicInt(&playing, 1);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    /* Video packets read ahead of decoding, so the audio, decoded as it is read, stays AUDIO_AHEAD queued. */
    AVFifo *ahead = av_fifo_alloc2(VIDEO_AHEAD, sizeof(AVPacket *), 0);
    int eof = !ahead;
    while (!skip_pressed(&p)) {
        size_t queued = ahead ? av_fifo_can_read(ahead) : 0;
        int audio_low = p.adec && (!p.audio || SDL_GetAudioStreamQueued(p.audio) < p.audio_rate * 8 * AUDIO_AHEAD);
        if (!eof && (!queued || (audio_low && av_fifo_can_write(ahead)))) {
            if (av_read_frame(p.fmt, pkt) < 0) {
                eof = 1;
            } else if (pkt->stream_index == p.vstream) {
                AVPacket *v = av_packet_clone(pkt);
                if (v) av_fifo_write(ahead, &v, 1);
            } else if (pkt->stream_index == p.astream) {
                decode(&p, p.adec, pkt, f);
            }
            av_packet_unref(pkt);
        } else if (queued) {
            AVPacket *v;
            av_fifo_read(ahead, &v, 1);
            decode(&p, p.vdec, v, f);
            av_packet_free(&v);
        } else {
            break; /* read and decoded to the end */
        }
    }
    for (AVPacket *v; ahead && av_fifo_read(ahead, &v, 1) >= 0;) av_packet_free(&v); /* skipped */
    av_fifo_freep2(&ahead);
    decode(&p, p.vdec, NULL, f);
    decode(&p, p.adec, NULL, f);
    if (p.audio) SDL_FlushAudioStream(p.audio); /* the resampler's last samples too */
    while (p.audio && !skip_pressed(&p) && SDL_GetAudioStreamQueued(p.audio) > 0) SDL_Delay(5);
    SDL_SetAtomicInt(&playing, 0);
    SDL_LockMutex(lock);
    av_frame_unref(latest);
    SDL_UnlockMutex(lock);
    av_frame_free(&f);
    av_packet_free(&pkt);
    int skipped = p.skipped;
    close_player(&p);
    if (skipped) rt_w8(SKIPPED_FLAG, 1);
    return 1;
}

/* The replacement for the game movie at `path` (game memory, e.g. "/movie/op_ds.mods") when Mods is on and the
 * file exists: mods/cutscenes/op_ds.mp4. */
static int replacement(uint32_t path, char *out, size_t n) {
    if (!display_setting_get(SET_MODS)) return 0;
    char name[64];
    size_t len = 0;
    for (uint8_t c; len < sizeof name - 1 && (c = rt_r8(path + (uint32_t)len)) != 0; len++) name[len] = (char)c;
    name[len] = 0;
    const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    char *dot = strrchr(name, '.');
    if (dot && dot > base) *dot = 0;
    snprintf(out, n, "%smods/cutscenes/%s.mp4", rt_data_dir(), base);
    SDL_PathInfo info;
    return SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* The game's movie players (overrides.txt), r0 = the movie's path: play the replacement if there is one. */
static void game_movie_play(CPU *cpu) { rt_call_wrapped(cpu, "ov017", 0x02198d20u); } /* the originals */
static void game_movie_play_scene(CPU *cpu) { rt_call_wrapped(cpu, "ov017", 0x02198da0u); }
void rt_movie_play(CPU *cpu) {
    char path[1024];
    if (cpu->resume_pc || !replacement(R[0], path, sizeof path) || !play_file(path, KEY_START)) game_movie_play(cpu);
}
/* This one also takes callbacks the game's player calls with the scene's data ([sp]) in r1: r1 when it starts (it
 * sets up the bottom screen and the scene's result, which the front end reads next), r2 every frame (left out:
 * the bottom screen's animation), r3 when it ends (frees what r1 set up). */
void rt_movie_play_scene(CPU *cpu) {
    char path[1024];
    if (cpu->resume_pc || !replacement(R[0], path, sizeof path)) {
        game_movie_play_scene(cpu);
        return;
    }
    uint32_t args[4] = {R[0], R[1], R[2], R[3]}, data = rt_r32(R[13]);
    if (args[1]) rt_call_game(cpu, args[1], 0, data, 0);
    uint32_t skip_keys = args[2] == INTRO_FRAME ? 0xFFFu : KEY_START; /* 0xFFF: every button, A to Y */
    int played = play_file(path, skip_keys);
    if (args[3]) rt_call_game(cpu, args[3], 0, data, 0);
    if (!played) { /* the game's own movie, from the start */
        for (int i = 0; i < 4; i++) R[i] = args[i];
        game_movie_play_scene(cpu);
    }
}
