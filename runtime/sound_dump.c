/* Audio dump (System Settings > Dump audio): the game's sound effects and the sound archive's waves as WAV files
 * in mods/sounds/dump, for sound mods. On a thread of its own while the game runs, from the ROM image.
 *
 * Written (names as sound mods take them, music.c and sound_mod.c):
 *   SE_xxx.wav            each sound effect sequence, played through: 48 kHz 16-bit stereo, at the archive's
 *                         volume for it; one that loops runs for EFFECT_MAX_S (the archive's empty slots, silent,
 *                         are only listed)
 *   <archive>/NNN.wav     each wave of each wave archive (the instruments' samples): 16-bit mono at its own rate
 *   effects.txt           each effect's length, whether it loops, and the wave archives its bank plays from
 *   waves.txt             each wave's rate and loop (samples)
 *
 * The effects are played by a copy of the sound driver and mixer of its own (snd.c and spu.c, built again here
 * with game memory replaced by a small one holding the sequence, its bank and wave archives), so the game's sound
 * goes on untouched. Sound mods play no part in it. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "rt.h"
#include "sdat.h"
#include "sound.h"
#include "sound_dump.h"

#define EFFECT_MAX_S 10 /* a looping effect's length */
#define MEM_BASE 0x02000000u

static uint8_t *dump_mem;
static uint32_t dump_mem_size;

static uint32_t dump_r8(uint32_t a) { return a - MEM_BASE < dump_mem_size ? dump_mem[a - MEM_BASE] : 0; }
static uint32_t dump_r16(uint32_t a) { return dump_r8(a) | dump_r8(a + 1) << 8; }
static uint32_t dump_r32(uint32_t a) { return dump_r16(a) | dump_r16(a + 2) << 16; }
static void dump_w16(uint32_t a, uint16_t v) {
    if (a - MEM_BASE + 1 < dump_mem_size) dump_mem[a - MEM_BASE] = (uint8_t)v, dump_mem[a - MEM_BASE + 1] = (uint8_t)(v >> 8);
}
static void dump_w32(uint32_t a, uint32_t v) { dump_w16(a, (uint16_t)v), dump_w16(a + 2, (uint16_t)(v >> 16)); }

static float *dump_pcm; /* the effect's output, float stereo */
static uint32_t dump_frames, dump_cap;

static void dump_capture(const float *buf, uint32_t frames) {
    if (dump_frames + frames > dump_cap) {
        uint32_t want = SDL_max(dump_cap * 2, dump_frames + frames + SPU_RATE);
        float *grown = SDL_realloc(dump_pcm, (size_t)want * 2 * sizeof *grown);
        if (!grown) return;
        dump_pcm = grown, dump_cap = want;
    }
    memcpy(dump_pcm + (size_t)dump_frames * 2, buf, (size_t)frames * 2 * sizeof *buf);
    dump_frames += frames;
}

/* The driver and mixer, again: their names and what they reach outside themselves, renamed to this copy's. */
#define rt_r8 dump_r8
#define rt_r16 dump_r16
#define rt_r32 dump_r32
#define rt_w16 dump_w16
#define rt_w32 dump_w32
#define spu_ch dump_spu_ch
#define spu_master dump_spu_master
#define spu_mod dump_spu_mod
#define spu_open_output dump_spu_open_output
#define spu_reset dump_spu_reset
#define spu_start dump_spu_start
#define spu_render dump_spu_render
#define spu_state dump_spu_state
#define snd_reset dump_snd_reset
#define snd_command_list dump_snd_command_list
#define snd_update dump_snd_update
#define snd_state dump_snd_state
static inline void dump_music_prepare(int pno, uint32_t data) { (void)pno, (void)data; }
static inline void dump_music_pno(int pno) { (void)pno; }
static inline void dump_music_reset(void) {}
static inline int dump_music_query(int pno) { (void)pno; return 0; }
static inline void dump_music_player(int pno, int running, int db) { (void)pno, (void)running, (void)db; }
static inline void dump_music_mix(float *out, uint32_t frames, int speed, int effect_speed, int muted) {
    (void)out, (void)frames, (void)speed, (void)effect_speed, (void)muted;
}
static inline int dump_mod_sample(uint32_t arc, int index, double rate, uint32_t samples, uint32_t loop, int loops, double *mod_rate) {
    (void)arc, (void)index, (void)rate, (void)samples, (void)loop, (void)loops, (void)mod_rate;
    return 0;
}
static inline const int16_t *dump_mod_data(int mod, uint32_t *len, int32_t *loop) {
    (void)mod, *len = 0, *loop = -1;
    return NULL;
}
#define music_prepare dump_music_prepare
#define music_stop dump_music_pno
#define music_end dump_music_pno
#define music_reset dump_music_reset
#define music_replaced dump_music_query
#define music_is_bgm dump_music_query
#define music_player dump_music_player
#define music_mix dump_music_mix
#define sound_mod_sample dump_mod_sample
#define sound_mod_data dump_mod_data
#define display_setting_get(i) 0
#define sched_speed() 1
#define sched_now() 0
#define pxi_arm7_send(v) ((void)0)
#define SPU_CAPTURE dump_capture

void dump_spu_open_output(void);
void dump_spu_reset(void);
void dump_spu_start(int ch);
void dump_spu_render(double seconds, int speed, uint16_t normal, uint16_t muted, int music_speed);
void dump_spu_state(RtStateIO *s);
void dump_snd_reset(void);
void dump_snd_command_list(uint32_t list);
void dump_snd_update(uint64_t now);
void dump_snd_state(RtStateIO *s);
extern SpuChannel dump_spu_ch[SPU_CHANNELS];
extern float dump_spu_master;
extern SpuMod dump_spu_mod[SPU_CHANNELS];

#include "spu.c"
#include "snd.c"

/* ---- the dump ---- */

static SDL_AtomicInt running, finished, done_count, total_count, file_count;
static char out_root[1024];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

/* 16-bit PCM as a WAV file: path under out_root, its folder made. */
static void write_wav(const char *name, const int16_t *pcm, uint32_t frames, int channels, uint32_t rate) {
    char path[1200], dir[1200];
    snprintf(path, sizeof path, "%s%s.wav", out_root, name);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    SDL_CreateDirectory(dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        rt_log("sounddump: cannot write %s", path);
        return;
    }
    uint32_t bytes = frames * (uint32_t)channels * 2;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    put32(h + 4, 36 + bytes);
    put32(h + 16, 16);
    put32(h + 20, 1 | (uint32_t)channels << 16);
    put32(h + 24, rate);
    put32(h + 28, rate * (uint32_t)channels * 2);
    put32(h + 32, (uint32_t)channels * 2 | 16u << 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
    fwrite(h, 1, sizeof h, f);
    fwrite(pcm, 2, (size_t)frames * (size_t)channels, f);
    fclose(f);
    SDL_AddAtomicInt(&file_count, 1);
}

/* Copies a file into the dump's memory at `at` (aligned), returning its address. */
static uint32_t place(uint32_t *at, const uint8_t *data, uint32_t size) {
    uint32_t a = (*at + 31) & ~31u;
    if (a - MEM_BASE + size > dump_mem_size) return 0;
    memcpy(dump_mem + (a - MEM_BASE), data, size);
    *at = a + size;
    return a;
}

/* Plays effect q through (to EFFECT_MAX_S if it loops), into a WAV file and its line in effects.txt. */
static void render_effect(const SdatSeq *q, const SdatFile *banks, int bank_count, const SdatFile *arcs, int arc_count,
                          FILE *list) {
    uint32_t need = q->size + 64;
    const SdatFile *bank = q->bank >= 0 && q->bank < bank_count && banks[q->bank].data ? &banks[q->bank] : NULL;
    if (bank) {
        need += bank->size + 64;
        for (int k = 0; k < 4; k++) {
            int w = bank->wave_arcs[k];
            if (w >= 0 && w < arc_count && arcs[w].data) need += arcs[w].size + 64;
        }
    }
    if (need > dump_mem_size) {
        uint8_t *grown = SDL_realloc(dump_mem, need);
        if (!grown) return;
        dump_mem = grown, dump_mem_size = need;
    }
    memset(dump_mem, 0, dump_mem_size);
    uint32_t at = MEM_BASE, seq = place(&at, q->data, q->size), bank_at = 0;
    char arcs_used[160] = "";
    if (bank && (bank_at = place(&at, bank->data, bank->size)) != 0) {
        for (int k = 0; k < 4; k++) { /* the bank's wave archive links: where they are loaded */
            int w = bank->wave_arcs[k];
            uint32_t arc = w >= 0 && w < arc_count && arcs[w].data ? place(&at, arcs[w].data, arcs[w].size) : 0;
            dump_w32(bank_at + 0x18 + 8 * k, arc);
            if (arc) {
                size_t len = strlen(arcs_used);
                snprintf(arcs_used + len, sizeof arcs_used - len, "%s%s", len ? " " : "", arcs[w].name);
            }
        }
    }
    dump_frames = 0;
    dump_snd_reset();
    player_prepare(0, seq, 0, bank_at);
    players[0].started = 1;
    players[0].volume = (uint8_t)q->volume;
    double frame = (double)SND_FRAME_CYCLES / RT_CLOCK_HZ;
    int frames_max = (int)(EFFECT_MAX_S / frame), f = 0, loops = 1;
    for (; f < frames_max; f++) {
        player_main(0);
        ex_main(0xFFFF);
        dump_spu_render(frame, 1, 0, 0, 1);
        int sounding = players[0].active;
        for (int c = 0; c < SPU_CHANNELS; c++) sounding |= ex[c].active || dump_spu_ch[c].active;
        if (!sounding) {
            loops = 0;
            break;
        }
    }
    int16_t *pcm = SDL_malloc(((size_t)dump_frames * 2 + 2) * sizeof *pcm);
    if (!pcm) return;
    int heard = 0;
    for (uint32_t i = 0; i < dump_frames * 2; i++) {
        pcm[i] = (int16_t)lrintf(SDL_clamp(dump_pcm[i], -1.0f, 1.0f) * 32767.0f);
        heard |= pcm[i];
    }
    if (heard) write_wav(q->name, pcm, dump_frames, 2, SPU_RATE); /* else one of the archive's empty slots */
    SDL_free(pcm);
    if (list && heard) {
        fprintf(list, "%s\t%.2f s%s\t%s\t%s\n", q->name, (double)dump_frames / SPU_RATE, loops ? " (loops)" : "",
                bank ? bank->name : "-", arcs_used[0] ? arcs_used : "-");
    } else if (list) {
        fprintf(list, "%s\tsilent (an empty slot: no file)\t%s\t-\n", q->name, bank ? bank->name : "-");
    }
}

/* IMA-ADPCM as the hardware decodes it (spu.c): a header word (first sample, step index), then nibbles. */
static void decode_adpcm(const uint8_t *d, uint32_t samples, int16_t *out) {
    int val = (int16_t)le16(d), index = d[2] % 89;
    for (uint32_t n = 0; n < samples; n++) {
        uint8_t b = d[4 + n / 2];
        int nib = n & 1 ? b >> 4 : b & 0xF, step = adpcm_step[index], diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        val = nib & 8 ? SDL_max(val - diff, -0x7FFF) : SDL_min(val + diff, 0x7FFF);
        index = SDL_clamp(index + adpcm_index_step[nib & 7], 0, 88);
        out[n] = (int16_t)val;
    }
}

/* Every wave of wave archive a, as <archive>/NNN.wav. */
static void dump_waves(const SdatFile *a, FILE *list) {
    if (!a->data || a->size < 0x3C) return;
    uint32_t count = le32(a->data + 0x38);
    for (uint32_t k = 0; k < count && 0x3C + 4 * k + 4 <= a->size; k++) {
        uint32_t off = le32(a->data + 0x3C + 4 * k);
        if (off + 12 > a->size) continue;
        const uint8_t *w = a->data + off;
        int format = w[0] & 3, loops = w[1] != 0;
        uint32_t timer = le16(w + 4), loop_bytes = le16(w + 6) * 4u, bytes = loop_bytes + le32(w + 8) * 4u;
        if (format == SPU_PSG || !timer || off + 12 + bytes > a->size) continue;
        uint32_t samples = format == SPU_PCM8 ? bytes : format == SPU_PCM16 ? bytes / 2 : bytes > 4 ? (bytes - 4) * 2 : 0;
        uint32_t loop = format == SPU_PCM8 ? loop_bytes : format == SPU_PCM16 ? loop_bytes / 2 : loop_bytes > 4 ? (loop_bytes - 4) * 2 : 0;
        if (!samples) continue;
        int16_t *pcm = SDL_malloc(samples * sizeof *pcm);
        if (!pcm) continue;
        const uint8_t *d = w + 12;
        for (uint32_t i = 0; format != SPU_ADPCM && i < samples; i++) pcm[i] = format == SPU_PCM8 ? (int16_t)((int8_t)d[i] * 256) : (int16_t)le16(d + 2 * i);
        if (format == SPU_ADPCM) decode_adpcm(d, samples, pcm);
        uint32_t rate = (uint32_t)lround(SPU_CLOCK / timer);
        char name[64];
        snprintf(name, sizeof name, "%s/%03u", a->name, k);
        write_wav(name, pcm, samples, 1, rate);
        SDL_free(pcm);
        if (!list) continue;
        if (loops) fprintf(list, "%s.wav\t%u Hz\t%u samples\tloops from %u\n", name, rate, samples, loop);
        else fprintf(list, "%s.wav\t%u Hz\t%u samples\tonce\n", name, rate, samples);
    }
}

static FILE *open_list(const char *name, const char *header) {
    char path[1200];
    snprintf(path, sizeof path, "%s%s", out_root, name);
    FILE *f = fopen(path, "w");
    if (f) fputs(header, f);
    return f;
}

static int worker(void *arg) {
    (void)arg;
    Uint64 t0 = SDL_GetTicks();
    snprintf(out_root, sizeof out_root, "%smods/sounds/dump/", rt_data_dir());
    char dir[1100];
    snprintf(dir, sizeof dir, "%smods/sounds", rt_data_dir());
    SDL_CreateDirectory(dir);
    SDL_CreateDirectory(out_root);
    int seq_count, bank_count, arc_count, effects = 0;
    const SdatSeq *seqs = sdat_seqs(&seq_count);
    const SdatFile *banks = sdat_banks(&bank_count), *arcs = sdat_wave_arcs(&arc_count);
    for (int i = 0; i < seq_count; i++) effects += !strncmp(seqs[i].name, "SE_", 3);
    SDL_SetAtomicInt(&total_count, effects + arc_count);
    FILE *list = open_list("effects.txt", "# effect\tlength\tbank\twave archives\n");
    for (int i = 0; i < seq_count; i++) {
        if (strncmp(seqs[i].name, "SE_", 3)) continue;
        render_effect(&seqs[i], banks, bank_count, arcs, arc_count, list);
        SDL_AddAtomicInt(&done_count, 1);
    }
    if (list) fclose(list);
    list = open_list("waves.txt", "# wave\trate\tlength\tloop\n");
    for (int i = 0; i < arc_count; i++) {
        dump_waves(&arcs[i], list);
        SDL_AddAtomicInt(&done_count, 1);
    }
    if (list) fclose(list);
    SDL_free(dump_pcm), dump_pcm = NULL, dump_frames = dump_cap = 0;
    SDL_free(dump_mem), dump_mem = NULL, dump_mem_size = 0;
    rt_log("sounddump: %d files in %.1f s, in %s", SDL_GetAtomicInt(&file_count), (double)(SDL_GetTicks() - t0) / 1000, out_root);
    SDL_SetAtomicInt(&finished, 1);
    SDL_SetAtomicInt(&running, 0);
    return 0;
}

void sounddump_start(void) {
    if (!SDL_CompareAndSwapAtomicInt(&running, 0, 1)) return;
    SDL_SetAtomicInt(&done_count, 0);
    SDL_SetAtomicInt(&total_count, 0);
    SDL_SetAtomicInt(&file_count, 0);
    SDL_Thread *t = SDL_CreateThread(worker, "sounddump", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&running, 0);
}

int sounddump_running(void) { return SDL_GetAtomicInt(&running); }

void sounddump_progress(int *done, int *total, float *share) {
    *done = SDL_GetAtomicInt(&done_count);
    *total = SDL_GetAtomicInt(&total_count);
    *share = *total ? (float)*done / (float)*total : 0;
}

int sounddump_finished(int *files) {
    if (!SDL_CompareAndSwapAtomicInt(&finished, 1, 0)) return 0;
    *files = SDL_GetAtomicInt(&file_count);
    return 1;
}
