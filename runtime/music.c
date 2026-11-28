/* Music mods: an audio file in mods/music next to the executable replaces one of the game's music sequences
 * while the Mods setting is on. Files are named after the sequence's symbol in the sound archive
 * (/sound/sound_data.sdat): BGM_00 to BGM_60, e.g. mods/music/BGM_18.ogg (res/mods/music/readme.txt lists them
 * with the titles of the game's Music Box). Sound effect mods the same way, in mods/sounds (SE_000 to SE_1FF): an
 * effect's file plays once, to its own end even past the sequence's (music_end), unless it has loop tags.
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
 * mixer (spu.c). A save state restores the players; their replacements restart from the beginning.
 *
 * Override music (steammusic): without a mod file (or with Mods off), a BGM the player's Steam copy has plays the
 * Steam release's version (steam_music.c), from memory through an AVIOContext (FFmpeg's HCA demuxer), with the
 * track's own loop points; a track without them plays once. Its data is unpacked on a worker thread: until it is
 * ready the player's source waits (pending), silent, with the sequence muted. */
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "display.h"
#include "rt.h"
#include "sdat.h"
#include "sound.h"
#include "steam_music.h"

#define PLAYERS 16
#define MUSIC_GAIN 1.0f /* a replacement at full volume, against the game's mix */
#define STEAM_MUSIC_GAIN 1.5f /* +3.5 dB: the Steam release's music is mastered quieter than the DS's mix */

/* ---- the sound archive's music and effect sequences ---- */

static int is_bgm[PLAYERS]; /* each player's sequence is a BGM (whether or not it is replaced) */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int is_effect(const SdatSeq *q) { return !strncmp(q->name, "SE_", 3); }

/* The music or effect sequences (by name: BGM_*, SE_*) whose data is at `addr` in game memory, longest first (one
 * sequence's data can begin another's): up to `max`, their number returned. */
static int identify(uint32_t addr, const SdatSeq **found, int max) {
    int count, n = 0;
    const SdatSeq *seqs = sdat_seqs(&count);
    for (int i = 0; i < count; i++) {
        const SdatSeq *q = &seqs[i];
        if (strncmp(q->name, "BGM_", 4) && !is_effect(q)) continue;
        uint32_t k = 0;
        while (k < q->size && rt_r8(addr + k) == q->data[k]) k++;
        if (k < q->size) continue;
        int at;
        if (n < max) at = n++;
        else if (found[max - 1]->size < q->size) at = max - 1;
        else continue;
        while (at > 0 && found[at - 1]->size < q->size) found[at] = found[at - 1], at--;
        found[at] = q;
    }
    return n;
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
    int once, effect, ended;      /* plays to its end (no loop tags); a sound effect's; it has ended */
    float gain;                   /* at full volume */
    SteamTrack *track;            /* a Steam track's data (a reference), read through avio */
    AVIOContext *avio;
    int64_t mem_pos;
    int pending;                  /* waiting for the Steam track named seq to be unpacked: nothing else is open */
    char seq[16];
} Source;

static Source *sources[PLAYERS];
static Source *tails[PLAYERS]; /* effects playing on after their sequence ended */

static void close_source(Source *s) {
    if (!s) return;
    av_audio_fifo_free(s->fifo);
    swr_free(&s->swr);
    av_frame_free(&s->frame);
    av_packet_free(&s->pkt);
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
    if (s->avio) av_freep(&s->avio->buffer);
    avio_context_free(&s->avio);
    steam_music_release(s->track);
    av_free(s);
}

/* A Steam track read from memory. */
static int read_mem(void *opaque, uint8_t *buf, int n) {
    Source *s = opaque;
    int64_t left = (int64_t)s->track->len - s->mem_pos;
    if (left <= 0) return AVERROR_EOF;
    if (n > left) n = (int)left;
    memcpy(buf, s->track->hca + s->mem_pos, (size_t)n);
    s->mem_pos += n;
    return n;
}

static int64_t seek_mem(void *opaque, int64_t offset, int whence) {
    Source *s = opaque;
    int64_t len = (int64_t)s->track->len, pos;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return len;
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = s->mem_pos + offset; break;
    case SEEK_END: pos = len + offset; break;
    default: return -1;
    }
    if (pos < 0 || pos > len) return -1;
    return s->mem_pos = pos;
}

static int64_t tag(const AVFormatContext *fmt, int stream, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, NULL, 0);
    if (!e) e = av_dict_get(fmt->streams[stream]->metadata, key, NULL, 0);
    return e ? strtoll(e->value, NULL, 10) : -1;
}

/* A file (path; msu: MSU-1 PCM) or a Steam track (track: its reference passes to the source, path names it). */
static Source *open_source(const char *path, int msu, SteamTrack *track) {
    Source *s = av_mallocz(sizeof *s);
    if (!s) {
        steam_music_release(track);
        return NULL;
    }
    AVDictionary *opts = NULL;
    const AVInputFormat *in = NULL;
    s->loop_start = 0;
    s->loop_end = -1;
    s->gain = MUSIC_GAIN;
    if (track) {
        uint8_t *buf = av_malloc(1 << 16);
        s->track = track;
        s->gain = STEAM_MUSIC_GAIN;
        if (buf && !(s->avio = avio_alloc_context(buf, 1 << 16, 0, s, read_mem, NULL, seek_mem))) av_free(buf);
        if (!s->avio || !(s->fmt = avformat_alloc_context())) {
            rt_log("music: cannot play %s", path);
            close_source(s);
            return NULL;
        }
        s->fmt->pb = s->avio;
        s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        in = av_find_input_format("hca");
    } else if (msu) {
        uint8_t h[8] = {0};
        SDL_IOStream *f = SDL_IOFromFile(path, "rb");
        if (!f || SDL_ReadIO(f, h, 8) != 8 || memcmp(h, "MSU1", 4)) {
            if (f) SDL_CloseIO(f);
            rt_log("sound mod: %s is not an MSU-1 PCM file", path);
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
    int ok = avformat_open_input(&s->fmt, track ? "" : path, in, &opts) == 0;
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
        rt_log("sound mod: cannot play %s", path);
        close_source(s);
        return NULL;
    }
    s->rate = s->dec->sample_rate;
    if (track) {
        s->loop_start = track->loop_start;
        s->loop_end = track->loop_end;
        s->once = track->loop_end < 0;
    } else if (!msu) {
        int64_t start = tag(s->fmt, s->stream, "LOOPSTART"), end = tag(s->fmt, s->stream, "LOOPEND");
        int64_t len = tag(s->fmt, s->stream, "LOOPLENGTH");
        if (start >= 0) {
            s->loop_start = start;
            s->loop_end = end > start ? end : len > 0 ? start + len : -1;
        }
    }
    rt_log("sound mod: %s (%s, %d Hz, loop %lld-%lld)", path, codec->name, s->rate, (long long)s->loop_start,
           (long long)s->loop_end);
    return s;
}

/* Goes back to the loop start; 0 if looping has stopped producing anything (the loop lies past the end). */
static int restart(Source *s) {
    if (s->once || ++s->restarts > 2) return 0;
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

int music_loop_check(const char *seq, SteamTrack *t, int64_t *landed) {
    Source *s = open_source(seq, 0, t);
    if (!s) return 0;
    int ok = fill(s, 1) && restart(s) && av_read_frame(s->fmt, s->pkt) >= 0 && s->pkt->pts != AV_NOPTS_VALUE;
    if (ok) {
        *landed = av_rescale_q(s->pkt->pts, s->fmt->streams[s->stream]->time_base, (AVRational){1, s->rate});
        ok = *landed <= s->loop_start && s->loop_start - *landed < 1024;
    }
    close_source(s);
    return ok;
}

/* ---- the driver's side (snd.c) ---- */

static const char *const extensions[] = {"ogg", "opus", "flac", "mp3", "wav", "m4a", "aac", "pcm"};

/* The replacement for sequence q (mods/music/BGM_*.*, mods/sounds/SE_*.*), or NULL. */
static Source *open_replacement(const SdatSeq *q) {
    const char *base = rt_data_dir();
    for (size_t i = 0; i < SDL_arraysize(extensions); i++) {
        char path[1024];
        snprintf(path, sizeof path, "%smods/%s/%s.%s", base, is_effect(q) ? "sounds" : "music", q->name, extensions[i]);
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) continue;
        int msu = !strcmp(extensions[i], "pcm");
        Source *s = open_source(path, msu, NULL);
        if (!s) return NULL;
        s->ref_db = q->volume ? (int)SDL_lround(400.0 * SDL_log10(q->volume / 127.0)) : 0;
        s->effect = is_effect(q);
        s->once = s->effect && !msu && s->loop_end < 0 && s->loop_start == 0; /* without loop tags: once */
        return s;
    }
    return NULL;
}

void music_prepare(int pno, uint32_t seq_data) {
    music_stop(pno);
    if (pno < 0 || pno >= PLAYERS) return;
    const SdatSeq *found[4];
    int n = identify(seq_data, found, 4);
    is_bgm[pno] = n && !is_effect(found[0]);
    if (!n) return;
    if (display_setting_get(SET_MODS)) {
        for (int i = 0; i < n && !sources[pno]; i++) sources[pno] = open_replacement(found[i]);
    }
    for (int i = 0; i < n && !sources[pno] && is_bgm[pno] && display_setting_get(SET_STEAM_MUSIC); i++) {
        if (is_effect(found[i]) || !steam_music_has(found[i]->name) || !(sources[pno] = av_mallocz(sizeof(Source)))) continue;
        Source *s = sources[pno]; /* opened by music_mix once unpacked */
        s->pending = 1;
        SDL_strlcpy(s->seq, found[i]->name, sizeof s->seq);
        s->ref_db = found[i]->volume ? (int)SDL_lround(400.0 * SDL_log10(found[i]->volume / 127.0)) : 0;
    }
    if (!sources[pno] && is_bgm[pno]) rt_log("music: %s (no replacement)", found[0]->name);
}

void music_stop(int pno) {
    if (pno < 0 || pno >= PLAYERS) return;
    close_source(sources[pno]);
    sources[pno] = NULL;
}

void music_reset(void) {
    for (int i = 0; i < PLAYERS; i++) {
        music_stop(i);
        close_source(tails[i]);
        tails[i] = NULL;
    }
}

/* The player's sequence ended by itself: an effect's replacement plays on to its own end (in a free tail; with
 * none, it stops with the sequence). */
void music_end(int pno) {
    if (pno < 0 || pno >= PLAYERS || !sources[pno] || !sources[pno]->effect || sources[pno]->ended) return;
    for (int i = 0; i < PLAYERS; i++) {
        if (tails[i]) continue;
        tails[i] = sources[pno];
        sources[pno] = NULL;
        return;
    }
}

int music_replaced(int pno) { return pno >= 0 && pno < PLAYERS && sources[pno]; }
int music_is_bgm(int pno) { return pno >= 0 && pno < PLAYERS && is_bgm[pno]; }

void music_player(int pno, int running, int db) {
    if (!music_replaced(pno)) return;
    sources[pno]->running = running && !sources[pno]->ended;
    sources[pno]->db = db;
}

/* Adds a source's next frames to out, `speed` (1 or 2) times faster; 0 once it has nothing more. */
static int mix_source(Source *s, float *out, uint32_t frames, int speed) {
    static float buf[SPU_RATE / 10 * 2 * 2];
    if (!fill(s, (int)frames * speed) && !s->once) {
        rt_log("sound mod: playback stopped (decoding failed)");
        return 0;
    }
    float *p = buf;
    int got = av_audio_fifo_read(s->fifo, (void **)&p, (int)frames * speed) / speed;
    int db = SDL_min(s->db - s->ref_db, 60); /* up to 6 dB above the sequence's own volume */
    float gain = db <= -723 ? 0.0f : s->gain * (float)SDL_pow(10.0, db / 200.0) / speed;
    for (int k = 0; k < got; k++) { /* fast forward: each output frame averages `speed` of the file's */
        for (int j = 0; j < speed; j++) {
            out[k * 2] += buf[(k * speed + j) * 2] * gain;
            out[k * 2 + 1] += buf[(k * speed + j) * 2 + 1] * gain;
        }
    }
    return got > 0;
}

void music_mix(float *out, uint32_t frames, int speed, int effect_speed, int effects_muted) {
    static float silent[SPU_RATE / 10 * 2]; /* muted effects: played, so they end on time, then dropped */
    frames = SDL_min(frames, (uint32_t)SPU_RATE / 10);
    for (int i = 0; i < PLAYERS * 2; i++) {
        Source **slot = i < PLAYERS ? &sources[i] : &tails[i - PLAYERS];
        Source *s = *slot;
        if (s && s->pending) { /* a Steam track: opened once unpacked, dropped if it cannot be (the sequence plays) */
            SteamTrack *t = NULL;
            int r = steam_music_get(s->seq, &t);
            if (r == 0) continue;
            Source *o = r > 0 ? open_source(s->seq, 0, t) : NULL;
            if (o) o->ref_db = s->ref_db, o->running = s->running, o->db = s->db;
            close_source(s);
            *slot = s = o;
        }
        if (!s || !s->running) continue;
        int sp = SDL_clamp(s->effect ? effect_speed : speed, 1, 2);
        if (mix_source(s, s->effect && effects_muted ? silent : out, frames, sp)) continue;
        if (i < PLAYERS && s->once) { /* ended before its sequence: silent while the sequence runs on */
            s->running = 0, s->ended = 1;
            continue;
        }
        close_source(s);
        *slot = NULL;
    }
    memset(silent, 0, sizeof silent);
}
