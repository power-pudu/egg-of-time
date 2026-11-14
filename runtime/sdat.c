/* The sound archive's tables (see sdat.h): SDAT's symbol block (names), info block (each sequence's file, bank and
 * volume; each bank's file and wave archives; each wave archive's file) and file allocation table. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "sdat.h"

enum { REC_SEQ, REC_SEQARC, REC_BANK, REC_WAVEARC };

static SdatSeq *seqs;
static SdatFile *banks, *wave_arcs;
static int seq_count, bank_count, wave_arc_count;
static SDL_InitState init;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct Archive {
    const uint8_t *s;
    uint32_t size, symb, info, fat, files;
} Archive;

/* Record `rec`'s entry count, and its entries' offsets in the symbol and info blocks (0: none). */
static uint32_t entries(const Archive *a, int rec) {
    uint32_t infos = a->info + le32(a->s + a->info + 8 + 4 * rec);
    if (infos + 4 > a->size) return 0;
    return le32(a->s + infos);
}

static uint32_t info_entry(const Archive *a, int rec, uint32_t i) {
    uint32_t infos = a->info + le32(a->s + a->info + 8 + 4 * rec), e = le32(a->s + infos + 4 + 4 * i);
    return e && a->info + e + 16 <= a->size ? a->info + e : 0;
}

static const char *symbol(const Archive *a, int rec, uint32_t i) {
    if (!a->symb) return NULL;
    uint32_t names = a->symb + le32(a->s + a->symb + 8 + 4 * rec);
    if (names + 4 > a->size || i >= le32(a->s + names)) return NULL;
    uint32_t n = le32(a->s + names + 4 + 4 * i);
    return n && a->symb + n < a->size ? (const char *)a->s + a->symb + n : NULL;
}

/* File `id`'s data and size, 0 if it is not in the archive. */
static const uint8_t *file(const Archive *a, uint32_t id, uint32_t *size) {
    if (id >= a->files) return NULL;
    uint32_t off = le32(a->s + a->fat + 12 + 16 * id), len = le32(a->s + a->fat + 16 + 16 * id);
    if (off + len > a->size || len < 0x10) return NULL;
    *size = len;
    return a->s + off;
}

static void read_files(const Archive *a, int rec, SdatFile **out, int *count, const char *magic, const char *prefix) {
    uint32_t n = entries(a, rec);
    *out = SDL_calloc(n ? n : 1, sizeof **out);
    if (!*out) return;
    for (uint32_t i = 0; i < n; i++) {
        SdatFile *f = &(*out)[i];
        for (int k = 0; k < 4; k++) f->wave_arcs[k] = -1;
        const char *name = symbol(a, rec, i);
        if (name) snprintf(f->name, sizeof f->name, "%s", name);
        else snprintf(f->name, sizeof f->name, "%s_%03u", prefix, i);
        uint32_t e = info_entry(a, rec, i), size = 0;
        const uint8_t *d = e ? file(a, le16(a->s + e), &size) : NULL;
        if (!d || memcmp(d, magic, 4)) continue;
        f->data = d;
        f->size = size;
        for (int k = 0; rec == REC_BANK && k < 4; k++) {
            uint16_t w = le16(a->s + e + 4 + 2 * k);
            f->wave_arcs[k] = w == 0xFFFF ? -1 : w;
        }
    }
    *count = (int)n;
}

static void read_archive(void) {
    const uint8_t *rom = rt_rom.data;
    size_t n = rt_rom.size;
    for (size_t at = 0; at + 0x40 <= n; at += 4) {
        const uint8_t *s = rom + at;
        if (memcmp(s, "SDAT", 4) || le16(s + 4) != 0xFEFF || le32(s + 8) > n - at) continue;
        Archive a = {s, le32(s + 8), le32(s + 0x10), le32(s + 0x18), le32(s + 0x20), 0};
        if (a.symb + 0x40 > a.size || a.info + 0x40 > a.size || a.fat + 0x10 > a.size) continue;
        a.files = le32(s + a.fat + 8);
        uint32_t count = entries(&a, REC_SEQ);
        seqs = SDL_calloc(count ? count : 1, sizeof *seqs);
        for (uint32_t i = 0; seqs && i < count; i++) {
            const char *name = symbol(&a, REC_SEQ, i);
            uint32_t e = info_entry(&a, REC_SEQ, i), size = 0;
            const uint8_t *d = e && name ? file(&a, le16(s + e), &size) : NULL;
            if (!d || size < 0x1C || memcmp(d, "SSEQ", 4) || le32(d + 0x18) >= size) continue;
            SdatSeq *q = &seqs[seq_count++];
            snprintf(q->name, sizeof q->name, "%s", name);
            q->data = d + le32(d + 0x18);
            q->size = size - le32(d + 0x18);
            q->bank = le16(s + e + 4);
            q->volume = s[e + 6];
        }
        read_files(&a, REC_BANK, &banks, &bank_count, "SBNK", "BANK");
        read_files(&a, REC_WAVEARC, &wave_arcs, &wave_arc_count, "SWAR", "WAVE");
        rt_log("sdat: %d sequences, %d banks, %d wave archives", seq_count, bank_count, wave_arc_count);
        return;
    }
    rt_log("sdat: no sound archive in the ROM");
}

static void load(void) {
    if (SDL_ShouldInit(&init)) {
        read_archive();
        SDL_SetInitialized(&init, true);
    }
}

const SdatSeq *sdat_seqs(int *count) {
    load();
    *count = seq_count;
    return seqs;
}

const SdatFile *sdat_banks(int *count) {
    load();
    *count = bank_count;
    return banks;
}

const SdatFile *sdat_wave_arcs(int *count) {
    load();
    *count = wave_arc_count;
    return wave_arcs;
}
