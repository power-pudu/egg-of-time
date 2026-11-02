/* Music mods: an audio file in mods/music next to the executable replaces one of the game's music sequences
 * while the Mods setting is on. Files are named after the sequence's symbol in the sound archive
 * (/sound/sound_data.sdat): BGM_00 to BGM_60, e.g. mods/music/BGM_18.ogg (res/mods/music/readme.txt lists them
 * with the titles of the game's Music Box).
 *
 * Formats: anything FFmpeg reads (OGG Vorbis, Opus, FLAC, MP3, WAV, M4A...), looping from the LOOPSTART tag to
 * LOOPEND (or LOOPSTART + LOOPLENGTH), in samples, as vgmstream and RPG Maker use them; without tags the whole
 * file loops. Or MSU-1 PCM (.pcm: "MSU1", a 32-bit loop point in samples, then 44.1 kHz 16-bit stereo), which
 * plays to its end and loops from that point.
 *
 * The driver (snd.c) still runs the game's sequence, muted, so everything the game reads back (player status,
 * sequence variables, the tick count) stays as it was; the replacement plays while that player runs, at the
 * player's volume relative to the sequence's own (so the game's fades and volume changes apply), and stops
 * with it. Which sequence a player loaded is found by comparing its data in RAM with the archive's BGM
 * sequences (the driver only gets the data's address). Decoding happens on the game thread, in step with the
 * mixer (spu.c). A save state restores the players; their replacements restart from the beginning. */
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "display.h"
#include "rt.h"
#include "sound.h"

#define PLAYERS 16
#define MUSIC_GAIN 1.0f /* a replacement at full volume, against the game's mix */

/* ---- the sound archive's music sequences ---- */

typedef struct Bgm {
    char name[16];
    const uint8_t *data; /* the sequence's data (what the driver gets the address of), in the ROM image */
    uint32_t size;
    int volume;          /* the archive's volume for it, which the game sets as the player's volume */
} Bgm;

static Bgm bgms[256];
static int bgm_count, archive_read;
static int is_bgm[PLAYERS]; /* each player's sequence is a BGM (whether or not it is replaced) */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Finds the SDAT in the ROM image and lists its sequences named BGM_*. */
static void read_archive(void) {
    archive_read = 1;
    const uint8_t *rom = rt_rom.data;
    size_t n = rt_rom.size;
    for (size_t at = 0; at + 0x40 <= n; at += 4) {
        const uint8_t *s = rom + at;
        if (memcmp(s, "SDAT", 4) || le16(s + 4) != 0xFEFF || le32(s + 8) > n - at) continue;
        size_t size = le32(s + 8);
        uint32_t symb = le32(s + 0x10), info = le32(s + 0x18), fat = le32(s + 0x20);
        if (!symb || symb + 0x10 > size || info + 0x40 > size || fat + 0x10 > size) continue;
        uint32_t names = symb + le32(s + symb + 8), infos = info + le32(s + info + 8);
        uint32_t count = SDL_min(le32(s + infos), le32(s + names));
        uint32_t files = le32(s + fat + 8);
        for (uint32_t i = 0; i < count && bgm_count < (int)SDL_arraysize(bgms); i++) {
            uint32_t name = le32(s + names + 4 + 4 * i), entry = le32(s + infos + 4 + 4 * i);
            if (!name || !entry || symb + name >= size || info + entry + 8 > size) continue;
            const char *sym = (const char *)s + symb + name;
            if (strncmp(sym, "BGM_", 4)) continue;
            uint32_t file = le16(s + info + entry);
            if (file >= files) continue;
            uint32_t off = le32(s + fat + 12 + 16 * file), len = le32(s + fat + 16 + 16 * file);
            if (off + len > size || len < 0x1C || memcmp(s + off, "SSEQ", 4)) continue;
            uint32_t data = le32(s + off + 0x18);
            if (data >= len) continue;
            Bgm *b = &bgms[bgm_count++];
            snprintf(b->name, sizeof b->name, "%s", sym);
            b->data = s + off + data;
            b->size = len - data;
            b->volume = s[info + entry + 6];
        }
        rt_log("music: %d music sequences in the sound archive", bgm_count);
        return;
    }
    rt_log("music: no sound archive in the ROM");
}

/* The music sequence whose data is at `addr` in game memory, or NULL. */
static const Bgm *identify(uint32_t addr) {
    if (!archive_read) read_archive();
    for (int i = 0; i < bgm_count; i++) {
        const Bgm *b = &bgms[i];
        uint32_t k = 0;
        while (k < b->size && rt_r8(addr + k) == b->data[k]) k++;
        if (k == b->size) return b;
    }
    return NULL;
}

/* ---- decoding ---- */

typedef struct Source {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVAudioFifo *fifo;   /* decoded output: float stereo at SPU_RATE */
    AVPacket *pkt;
    AVFrame *frame;
    int stream, rate, draining, restarts;
    int64_t loop_start, loop_end; /* samples of the file; loop_end < 0: the end of the file */
    int64_t pos, skip_to;         /* the next decoded sample; samples before skip_to are dropped (after a seek) */
    int ref_db;                   /* the sequence's own volume (0.1 dB): the replacement's full level */
    int running, db;              /* set every driver frame: the player plays, at this volume (0.1 dB) */
} Source;

static Source *sources[PLAYERS];

static void close_source(Source *s) {
    if (!s) return;
    av_audio_fifo_free(s->fifo);
    swr_free(&s->swr);
    av_frame_free(&s->frame);
    av_packet_free(&s->pkt);
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
    av_free(s);
}

static int64_t tag(const AVFormatContext *fmt, int stream, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, NULL, 0);
    if (!e) e = av_dict_get(fmt->streams[stream]->metadata, key, NULL, 0);
    return e ? strtoll(e->value, NULL, 10) : -1;
}

static Source *open_source(const char *path, int msu) {
    Source *s = av_mallocz(sizeof *s);
    if (!s) return NULL;
    AVDictionary *opts = NULL;
    const AVInputFormat *in = NULL;
    s->loop_start = 0;
    s->loop_end = -1;
    if (msu) {
        uint8_t h[8] = {0};
        SDL_IOStream *f = SDL_IOFromFile(path, "rb");
        if (!f || SDL_ReadIO(f, h, 8) != 8 || memcmp(h, "MSU1", 4)) {
            if (f) SDL_CloseIO(f);
            rt_log("music: %s is not an MSU-1 PCM file", path);
            av_free(s);
            return NULL;
        }
        SDL_CloseIO(f);
        s->loop_start = le32(h + 4);
        in = av_find_input_format("s16le");
        av_dict_set(&opts, "sample_rate", "44100", 0);
        av_dict_set(&opts, "ch_layout", "stereo", 0);
        av_dict_set(&opts, "skip_initial_bytes", "8", 0);
    }
    int ok = avformat_open_input(&s->fmt, path, in, &opts) == 0;
    av_dict_free(&opts);
    ok = ok && avformat_find_stream_info(s->fmt, NULL) >= 0;
    if (ok) s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    ok = ok && s->stream >= 0;
    const AVCodec *codec = ok ? avcodec_find_decoder(s->fmt->streams[s->stream]->codecpar->codec_id) : NULL;
    ok = codec && (s->dec = avcodec_alloc_context3(codec)) != NULL &&
         avcodec_parameters_to_context(s->dec, s->fmt->streams[s->stream]->codecpar) >= 0;
    if (ok) s->dec->pkt_timebase = s->fmt->streams[s->stream]->time_base;
    ok = ok && avcodec_open2(s->dec, codec, NULL) >= 0;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    ok = ok && swr_alloc_set_opts2(&s->swr, &stereo, AV_SAMPLE_FMT_FLT, SPU_RATE, &s->dec->ch_layout, s->dec->sample_fmt,
                                   s->dec->sample_rate, 0, NULL) >= 0 && swr_init(s->swr) >= 0;
    ok = ok && (s->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, SPU_RATE / 10)) != NULL &&
         (s->pkt = av_packet_alloc()) != NULL && (s->frame = av_frame_alloc()) != NULL;
    if (!ok) {
        rt_log("music: cannot play %s", path);
        close_source(s);
        return NULL;
    }
    s->rate = s->dec->sample_rate;
    if (!msu) {
        int64_t start = tag(s->fmt, s->stream, "LOOPSTART"), end = tag(s->fmt, s->stream, "LOOPEND");
        int64_t len = tag(s->fmt, s->stream, "LOOPLENGTH");
        if (start >= 0) {
            s->loop_start = start;
            s->loop_end = end > start ? end : len > 0 ? start + len : -1;
        }
    }
    rt_log("music: %s (%s, %d Hz, loop %lld-%lld)", path, codec->name, s->rate, (long long)s->loop_start,
           (long long)s->loop_end);
    return s;
}

/* Goes back to the loop start; 0 if looping has stopped producing anything (the loop lies past the end). */
static int restart(Source *s) {
    if (++s->restarts > 2) return 0;
    AVRational tb = s->fmt->streams[s->stream]->time_base;
    if (av_seek_frame(s->fmt, s->stream, av_rescale_q(s->loop_start, (AVRational){1, s->rate}, tb), AVSEEK_FLAG_BACKWARD) < 0 &&
        av_seek_frame(s->fmt, s->stream, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        return 0;
    }
    avcodec_flush_buffers(s->dec);
    s->draining = 0;
    s->pos = 0;
    s->skip_to = s->loop_start;
    return 1;
}

/* Converts the part of a decoded frame inside the loop into the FIFO; 0 once the loop end is reached. */
static int push_frame(Source *s, const AVFrame *f) {
    if (f->pts != AV_NOPTS_VALUE) {
        s->pos = av_rescale_q(f->pts, s->fmt->streams[s->stream]->time_base, (AVRational){1, s->rate});
    }
    int64_t pos = s->pos;
    int n = f->nb_samples, start = 0, end = n;
    s->pos += n;
    if (pos < s->skip_to) start = (int)SDL_min((int64_t)n, s->skip_to - pos);
    int at_end = s->loop_end >= 0 && pos + n >= s->loop_end;
    if (at_end) end = (int)SDL_max((int64_t)start, s->loop_end - pos);
    if (end > start) {
        int planar = av_sample_fmt_is_planar(f->format), bps = av_get_bytes_per_sample(f->format);
        int ch = f->ch_layout.nb_channels;
        const uint8_t *in[64];
        for (int c = 0; c < (planar ? SDL_min(ch, 64) : 1); c++) in[c] = f->extended_data[c] + (size_t)start * bps * (planar ? 1 : ch);
        int max = swr_get_out_samples(s->swr, end - start);
        float *out = NULL;
        if (max > 0 && (out = av_malloc((size_t)max * 2 * sizeof(float))) != NULL) {
            int got = swr_convert(s->swr, (uint8_t **)&out, max, in, end - start);
            if (got > 0) {
                av_audio_fifo_write(s->fifo, (void **)&out, got);
                s->restarts = 0;
            }
            av_free(out);
        }
    }
    return !at_end;
}

/* Decodes until the FIFO holds `need` frames; 0 if the file cannot go on. */
static int fill(Source *s, int need) {
    while (av_audio_fifo_size(s->fifo) < need) {
        int r = avcodec_receive_frame(s->dec, s->frame);
        if (r == 0) {
            int go_on = push_frame(s, s->frame);
            av_frame_unref(s->frame);
            if (!go_on && !restart(s)) return 0;
        } else if (r == AVERROR(EAGAIN)) {
            if (av_read_frame(s->fmt, s->pkt) < 0) {
                avcodec_send_packet(s->dec, NULL); /* the end: decode what is left, then loop */
                s->draining = 1;
                continue;
            }
            if (s->pkt->stream_index == s->stream) avcodec_send_packet(s->dec, s->pkt);
            av_packet_unref(s->pkt);
        } else if (!restart(s)) { /* the end of the file (or an error) */
            return 0;
        }
    }
    return 1;
}

/* ---- the driver's side (snd.c) ---- */

static const char *const extensions[] = {"ogg", "opus", "flac", "mp3", "wav", "m4a", "aac", "pcm"};

void music_prepare(int pno, uint32_t seq_data) {
    music_stop(pno);
    if (pno < 0 || pno >= PLAYERS) return;
    const Bgm *b = identify(seq_data);
    is_bgm[pno] = b != NULL;
    if (!b || !display_setting_get(SET_MODS)) return;
    const char *base = rt_data_dir();
    for (size_t i = 0; i < SDL_arraysize(extensions); i++) {
        char path[1024];
        snprintf(path, sizeof path, "%smods/music/%s.%s", base, b->name, extensions[i]);
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) continue;
        Source *s = open_source(path, !strcmp(extensions[i], "pcm"));
        if (!s) return;
        s->ref_db = b->volume ? (int)SDL_lround(400.0 * SDL_log10(b->volume / 127.0)) : 0;
        sources[pno] = s;
        return;
    }
    rt_log("music: %s (no replacement)", b->name);
}

void music_stop(int pno) {
    if (pno < 0 || pno >= PLAYERS) return;
    close_source(sources[pno]);
    sources[pno] = NULL;
}

int music_replaced(int pno) { return pno >= 0 && pno < PLAYERS && sources[pno]; }
int music_is_bgm(int pno) { return pno >= 0 && pno < PLAYERS && is_bgm[pno]; }

void music_player(int pno, int running, int db) {
    if (!music_replaced(pno)) return;
    sources[pno]->running = running;
    sources[pno]->db = db;
}

void music_mix(float *out, uint32_t frames, int speed) {
    static float buf[SPU_RATE / 10 * 2 * 2];
    speed = SDL_clamp(speed, 1, 2);
    frames = SDL_min(frames, (uint32_t)SPU_RATE / 10);
    for (int i = 0; i < PLAYERS; i++) {
        Source *s = sources[i];
        if (!s || !s->running) continue;
        if (!fill(s, (int)frames * speed)) {
            rt_log("music: playback stopped (decoding failed)");
            music_stop(i);
            continue;
        }
        float *p = buf;
        int got = av_audio_fifo_read(s->fifo, (void **)&p, (int)frames * speed) / speed;
        int db = SDL_min(s->db - s->ref_db, 60); /* up to 6 dB above the sequence's own volume */
        float gain = db <= -723 ? 0.0f : MUSIC_GAIN * (float)SDL_pow(10.0, db / 200.0) / speed;
        for (int k = 0; k < got; k++) { /* fast forward: each output frame averages `speed` of the file's */
            for (int j = 0; j < speed; j++) {
                out[k * 2] += buf[(k * speed + j) * 2] * gain;
                out[k * 2 + 1] += buf[(k * speed + j) * 2 + 1] * gain;
            }
        }
    }
}
