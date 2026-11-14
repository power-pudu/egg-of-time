/* Sample mods: an audio file in mods/sounds/<wave archive>/<wave>.<ext> next to the executable replaces one wave of
 * the sound archive's wave archives (the instruments' samples), wherever the game plays it, while the Mods setting
 * is on: mods/sounds/WAVE_SE_01/003.wav replaces wave 3 of WAVE_SE_01 (the names and numbers of the audio dump,
 * sound_dump.c). The game still plays the notes: their pitch (the file at the wave's own key, whatever its sample
 * rate), envelope, volume and pan. The file loops from its LOOPSTART tag to LOOPEND (or LOOPSTART + LOOPLENGTH) as
 * music mods do, an MSU-1 PCM file from its loop point; else as the wave did, from the same time into the file (to
 * its end), or not at all. Formats: those of music mods (music.c), mixed down to mono.
 *
 * The folder is read once, when a wave is first played with mods on; a file is decoded whole the first time its
 * wave plays. The wave archive a note plays from is told by comparing its header and wave table in game memory with
 * the archive's (the driver only has its address). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "display.h"
#include "rt.h"
#include "sdat.h"
#include "sound.h"

typedef struct Mod {
    int arc, wave;        /* the wave archive's index in the archive, the wave's in it */
    char path[512];
    int tried;            /* decoded, or failed to */
    int16_t *data;
    uint32_t len;
    double rate;
    int64_t tag_start, tag_end; /* the file's loop, from its tags or MSU-1 header (start < 0: none) */
    int32_t loop;         /* the loop played (samples; < 0: once), worked out the first time the wave plays */
    int loop_set;
} Mod;

static Mod *mods;
static int mod_count, scanned;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- the folder ---- */

static const char *const extensions[] = {"ogg", "opus", "flac", "mp3", "wav", "m4a", "aac", "pcm"};

static int arc_named(const char *name) {
    int n;
    const SdatFile *arcs = sdat_wave_arcs(&n);
    for (int i = 0; i < n; i++) {
        if (arcs[i].data && !SDL_strcasecmp(arcs[i].name, name)) return i;
    }
    return -1;
}

typedef struct Scan { int arc; const char *dir; } Scan;

static int rank(const char *ext) {
    for (size_t i = 0; i < SDL_arraysize(extensions); i++) {
        if (!SDL_strcasecmp(ext, extensions[i])) return (int)i;
    }
    return (int)SDL_arraysize(extensions);
}

static SDL_EnumerationResult SDLCALL scan_file(void *data, const char *dir, const char *name) {
    const Scan *sc = data;
    (void)dir;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return SDL_ENUM_CONTINUE;
    int known = rank(dot + 1) < (int)SDL_arraysize(extensions);
    char *end;
    long wave = strtol(name, &end, 10);
    if (!known || end != dot || wave < 0 || wave > 0xFFFF) return SDL_ENUM_CONTINUE;
    for (int i = 0; i < mod_count; i++) { /* two files for one wave: the first extension in the list */
        if (mods[i].arc != sc->arc || mods[i].wave != wave) continue;
        if (rank(dot + 1) < rank(strrchr(mods[i].path, '.') + 1)) snprintf(mods[i].path, sizeof mods[i].path, "%s/%s", sc->dir, name);
        return SDL_ENUM_CONTINUE;
    }
    Mod *grown = SDL_realloc(mods, sizeof *mods * (size_t)(mod_count + 1));
    if (!grown) return SDL_ENUM_FAILURE;
    mods = grown;
    Mod *m = &mods[mod_count++];
    memset(m, 0, sizeof *m);
    m->arc = sc->arc;
    m->wave = (int)wave;
    snprintf(m->path, sizeof m->path, "%s/%s", sc->dir, name);
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult SDLCALL scan_dir(void *data, const char *dir, const char *name) {
    (void)data;
    int arc = arc_named(name);
    if (arc < 0) return SDL_ENUM_CONTINUE;
    char path[512];
    snprintf(path, sizeof path, "%s%s", dir, name);
    Scan sc = {arc, path};
    SDL_EnumerateDirectory(path, scan_file, &sc);
    return SDL_ENUM_CONTINUE;
}

static void scan(void) {
    scanned = 1;
    char root[1024];
    snprintf(root, sizeof root, "%smods/sounds", rt_data_dir());
    SDL_EnumerateDirectory(root, scan_dir, NULL);
    if (mod_count) rt_log("sound mod: %d sample files in %s", mod_count, root);
}

/* ---- decoding ---- */

static int64_t tag(const AVFormatContext *fmt, int stream, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, NULL, 0);
    if (!e) e = av_dict_get(fmt->streams[stream]->metadata, key, NULL, 0);
    return e ? strtoll(e->value, NULL, 10) : -1;
}

static int append(Mod *m, const int16_t *in, int n, uint32_t *cap) {
    if (m->len + (uint32_t)n > *cap) {
        uint32_t want = SDL_max(*cap * 2, m->len + (uint32_t)n + 4096);
        int16_t *grown = SDL_realloc(m->data, want * sizeof *grown);
        if (!grown) return 0;
        m->data = grown, *cap = want;
    }
    memcpy(m->data + m->len, in, (size_t)n * sizeof *in);
    m->len += (uint32_t)n;
    return 1;
}

/* MSU-1 PCM: "MSU1", the loop point, then 44.1 kHz 16-bit stereo, mixed to mono. */
static int decode_msu(Mod *m) {
    size_t size = 0;
    uint8_t *f = SDL_LoadFile(m->path, &size);
    if (!f || size < 8 || memcmp(f, "MSU1", 4)) {
        SDL_free(f);
        return 0;
    }
    uint32_t frames = (uint32_t)((size - 8) / 4);
    m->data = SDL_malloc((frames ? frames : 1) * sizeof *m->data);
    for (uint32_t i = 0; m->data && i < frames; i++) {
        const uint8_t *p = f + 8 + 4 * i;
        m->data[i] = (int16_t)(((int16_t)(p[0] | p[1] << 8) + (int16_t)(p[2] | p[3] << 8)) / 2);
    }
    m->len = m->data ? frames : 0;
    m->rate = 44100;
    m->tag_start = le32(f + 4);
    m->tag_end = -1;
    SDL_free(f);
    return m->data != NULL;
}

static int decode(Mod *m) {
    m->tag_start = m->tag_end = -1;
    const char *dot = strrchr(m->path, '.');
    if (dot && !SDL_strcasecmp(dot + 1, "pcm")) return decode_msu(m);
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int ok = pkt && frame && avformat_open_input(&fmt, m->path, NULL, NULL) == 0 && avformat_find_stream_info(fmt, NULL) >= 0;
    int stream = ok ? av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) : -1;
    const AVCodec *codec = stream >= 0 ? avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id) : NULL;
    ok = codec && (dec = avcodec_alloc_context3(codec)) != NULL &&
         avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) >= 0 && avcodec_open2(dec, codec, NULL) >= 0;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    ok = ok && swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, dec->sample_rate, &dec->ch_layout, dec->sample_fmt,
                                   dec->sample_rate, 0, NULL) >= 0 && swr_init(swr) >= 0;
    uint32_t cap = 0;
    while (ok) {
        int r = avcodec_receive_frame(dec, frame);
        if (r == 0) {
            int max = swr_get_out_samples(swr, frame->nb_samples);
            int16_t *out = max > 0 ? av_malloc((size_t)max * sizeof *out) : NULL;
            int got = out ? swr_convert(swr, (uint8_t **)&out, max, (const uint8_t **)frame->extended_data, frame->nb_samples) : 0;
            if (got > 0) ok = append(m, out, got, &cap);
            av_free(out);
            av_frame_unref(frame);
        } else if (r == AVERROR(EAGAIN)) {
            if (av_read_frame(fmt, pkt) < 0) {
                avcodec_send_packet(dec, NULL);
                continue;
            }
            if (pkt->stream_index == stream) avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        } else {
            break; /* the end */
        }
    }
    if (ok) {
        m->rate = dec->sample_rate;
        int64_t start = tag(fmt, stream, "LOOPSTART"), end = tag(fmt, stream, "LOOPEND"), len = tag(fmt, stream, "LOOPLENGTH");
        if (start >= 0) m->tag_start = start, m->tag_end = end > start ? end : len > 0 ? start + len : -1;
    }
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ok && m->len > 0;
}

/* ---- the driver's side (snd.c, spu.c) ---- */

/* Wave k's offset in the wave archive at `addr`: the loaded table may hold addresses instead (as the driver reads it). */
static uint32_t wave_offset(uint32_t addr, uint32_t k) {
    uint32_t v = rt_r32(addr + 0x3C + 4 * k);
    return v >= 0x02000000u ? v - addr : v;
}

/* Whether the wave archive at `addr` in game memory has file f's size and wave table. */
static int table_match(uint32_t addr, const SdatFile *f) {
    uint32_t count = rt_r32(addr + 0x38);
    if (!f->data || f->size != rt_r32(addr + 8) || le32(f->data + 0x38) != count || f->size < 0x3C + 4 * count) return 0;
    for (uint32_t k = 0; k < count; k++) {
        if (wave_offset(addr, k) != le32(f->data + 0x3C + 4 * k)) return 0;
    }
    return 1;
}

/* The wave archive at `addr` in game memory (its index in the archive, or -1): the archive whose file size and
 * wave table it has (the whole file compared where those do not settle it). The last few found are remembered. */
static int identify_arc(uint32_t addr) {
    static struct { uint32_t addr; int arc; } seen[16];
    static int next;
    int n;
    const SdatFile *arcs = sdat_wave_arcs(&n);
    if (rt_r32(addr) != le32((const uint8_t *)"SWAR") || rt_r32(addr + 0x38) > 0x10000) return -1;
    for (int k = 0; k < 16; k++) {
        if (seen[k].addr == addr && seen[k].arc >= 0 && table_match(addr, &arcs[seen[k].arc])) return seen[k].arc;
    }
    int found = -1, matches = 0;
    for (int i = 0; i < n; i++) {
        if (!table_match(addr, &arcs[i])) continue;
        if (matches++) { /* the same table twice: the data decides */
            uint32_t b = 0x3C + 4 * rt_r32(addr + 0x38); /* the waves, after the table */
            while (b < arcs[i].size && rt_r8(addr + b) == arcs[i].data[b]) b++;
            if (b < arcs[i].size) continue;
        }
        found = i;
    }
    seen[next].addr = addr, seen[next].arc = found;
    next = (next + 1) % 16;
    return found;
}

int sound_mod_sample(uint32_t arc, int index, double rate, uint32_t samples, uint32_t loop_start, int loops, double *mod_rate) {
    if (!display_setting_get(SET_MODS)) return 0;
    if (!scanned) scan();
    if (!mod_count) return 0;
    int a = identify_arc(arc);
    if (a < 0) return 0;
    for (int i = 0; i < mod_count; i++) {
        Mod *m = &mods[i];
        if (m->arc != a || m->wave != index) continue;
        if (!m->tried) {
            m->tried = 1;
            if (!decode(m)) {
                rt_log("sound mod: cannot play %s", m->path);
                SDL_free(m->data);
                m->data = NULL, m->len = 0;
            } else {
                rt_log("sound mod: %s (%.0f Hz, %u samples)", m->path, m->rate, m->len);
            }
        }
        if (!m->data || rate <= 0) return 0;
        if (!m->loop_set) { /* the file's own loop, else the wave's at the same time into the file */
            m->loop_set = 1;
            if (m->tag_start >= 0) {
                if (m->tag_end > m->tag_start && (uint64_t)m->tag_end < m->len) m->len = (uint32_t)m->tag_end;
                m->loop = m->tag_start < m->len ? (int32_t)m->tag_start : -1;
            } else if (loops && samples > loop_start) {
                m->loop = (int32_t)SDL_min((double)loop_start * m->rate / rate, m->len - 1.0);
            } else {
                m->loop = -1;
            }
        }
        *mod_rate = m->rate / rate;
        return i + 1;
    }
    return 0;
}

const int16_t *sound_mod_data(int mod, uint32_t *len, int32_t *loop) {
    if (mod < 0 || mod >= mod_count || !mods[mod].data) return NULL;
    *len = mods[mod].len;
    *loop = mods[mod].loop;
    return mods[mod].data;
}
