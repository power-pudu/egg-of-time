/* Override music: the Steam release's music, read from the player's own copy and kept in memory only.
 *
 * Each track is Sound/BGM/BGM_xx.sab in the copy's resources.bin (se_read), xx the DS sequence it replaces. A .sab
 * is a SEAD sound bank (little-endian): "sabf", a chunk table at 0x20 (16 bytes an entry: a tag, two u16, the
 * chunk's u32 offset, a u32), with the chunks "snd ", "seq ", "trk " and "mtrl". The mtrl chunk has its material
 * count (u16) at +0x04 and their u32 offsets (from the chunk) from +0x10. A material: channels at +0x04, codec at
 * +0x05 (7: HCA), sample rate +0x08, loop start and end +0x0c and +0x10 (samples; both 0: no loop), extradata
 * size +0x14, stream size +0x18, extradata id +0x1c (u16); the extradata from +0x20 (HCA header size at +0x02,
 * masked at +0x0d) and the HCA file from +0x30. A masked file has every byte after its header XORed with the SEAD
 * banks' 256-byte key, from key[extradata id & 255] on. Only the first material is used (BGM_12, an ambience, has
 * two variants).
 *
 * FFmpeg's HCA decoder counts samples from before the encoder delay (the HCA fmt chunk), so the delay is added to
 * the material's loop points; the end is inclusive there and exclusive here.
 *
 * Unpacking a large track takes tens of milliseconds, too long for the game thread: a worker thread unpacks what
 * the game asks for, and the last 4 tracks stay in memory, so going back and forth (field and battle music) is
 * immediate. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "sound.h"
#include "steam_edges.h"
#include "steam_music.h"

static const uint8_t key[256] = {
    0x3A, 0x32, 0x32, 0x32, 0x03, 0x7E, 0x12, 0xF7, 0xB2, 0xE2, 0xA2, 0x67, 0x32, 0x32, 0x22, 0x32,
    0x32, 0x52, 0x16, 0x1B, 0x3C, 0xA1, 0x54, 0x7B, 0x1B, 0x97, 0xA6, 0x93, 0x1A, 0x4B, 0xAA, 0xA6,
    0x7A, 0x7B, 0x1B, 0x97, 0xA6, 0xF7, 0x02, 0xBB, 0xAA, 0xA6, 0xBB, 0xF7, 0x2A, 0x51, 0xBE, 0x03,
    0xF4, 0x2A, 0x51, 0xBE, 0x03, 0xF4, 0x2A, 0x51, 0xBE, 0x12, 0x06, 0x56, 0x27, 0x32, 0x32, 0x36,
    0x32, 0xB2, 0x1A, 0x3B, 0xBC, 0x91, 0xD4, 0x7B, 0x58, 0xFC, 0x0B, 0x55, 0x2A, 0x15, 0xBC, 0x40,
    0x92, 0x0B, 0x5B, 0x7C, 0x0A, 0x95, 0x12, 0x35, 0xB8, 0x63, 0xD2, 0x0B, 0x3B, 0xF0, 0xC7, 0x14,
    0x51, 0x5C, 0x94, 0x86, 0x94, 0x59, 0x5C, 0xFC, 0x1B, 0x17, 0x3A, 0x3F, 0x6B, 0x37, 0x32, 0x32,
    0x30, 0x32, 0x72, 0x7A, 0x13, 0xB7, 0x26, 0x60, 0x7A, 0x13, 0xB7, 0x26, 0x50, 0xBA, 0x13, 0xB4,
    0x2A, 0x50, 0xBA, 0x13, 0xB5, 0x2E, 0x40, 0xFA, 0x13, 0x95, 0xAE, 0x40, 0x38, 0x18, 0x9A, 0x92,
    0xB0, 0x38, 0x00, 0xFA, 0x12, 0xB1, 0x7E, 0x00, 0xDB, 0x96, 0xA1, 0x7C, 0x08, 0xDB, 0x9A, 0x91,
    0xBC, 0x08, 0xD8, 0x1A, 0x86, 0xE2, 0x70, 0x39, 0x1F, 0x86, 0xE0, 0x78, 0x7E, 0x03, 0xE7, 0x64,
    0x51, 0x9C, 0x8F, 0x34, 0x6F, 0x4E, 0x41, 0xFC, 0x0B, 0xD5, 0xAE, 0x41, 0xFC, 0x0B, 0xD5, 0xAE,
    0x41, 0xFC, 0x3B, 0x70, 0x71, 0x64, 0x33, 0x32, 0x12, 0x32, 0x32, 0x36, 0x70, 0x34, 0x2B, 0x56,
    0x22, 0x70, 0x3A, 0x13, 0xB7, 0x26, 0x60, 0xBA, 0x1B, 0x94, 0xAA, 0x40, 0x38, 0x00, 0xFA, 0xB2,
    0xE2, 0xA2, 0x67, 0x32, 0x32, 0x12, 0x32, 0xB2, 0x32, 0x32, 0x32, 0x32, 0x75, 0xA3, 0x26, 0x7B,
    0x83, 0x26, 0xF9, 0x83, 0x2E, 0xFF, 0xE3, 0x16, 0x7D, 0xC0, 0x1E, 0x63, 0x21, 0x07, 0xE3, 0x01,
};

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void path_of(const char *seq, char *out, size_t n) { snprintf(out, n, "Sound/BGM/%s.sab", seq); }

/* The first material of bank d (n bytes) into t, unmasked in place; 0 when it is not an HCA track. */
static int parse(uint8_t *d, size_t n, SteamTrack *t) {
    if (n < 0x60 || memcmp(d, "sabf", 4)) return 0;
    size_t mtrl = 0;
    for (size_t e = 0x20; e + 16 <= 0x60; e += 16) {
        if (!memcmp(d + e, "mtrl", 4)) mtrl = rd32(d + e + 8);
    }
    if (!mtrl || mtrl + 0x14 > n || rd16(d + mtrl + 4) < 1) return 0;
    size_t m = mtrl + rd32(d + mtrl + 0x10);
    if (m < mtrl || m + 0x30 > n || d[m + 5] != 7) return 0;
    size_t extra = rd32(d + m + 0x14), stream = rd32(d + m + 0x18);
    size_t room = n - (m + 0x30);
    if (extra < 0x10 || stream > room || extra - 0x10 > room - stream) return 0;
    uint8_t *h = d + m + 0x30;
    size_t len = extra + stream - 0x10, header = rd16(d + m + 0x22);
    if (len < 0x18 || memcmp(h, "HCA", 3) || header < 0x18 || header > len) return 0;
    if ((rd32(h + 8) & 0x7F7F7F7Fu) != rd32((const uint8_t *)"fmt\0")) return 0;
    if (d[m + 0x2d]) {
        uint32_t k = rd16(d + m + 0x1c);
        for (size_t i = header; i < len; i++) h[i] ^= key[(k + i - header) & 255];
    }
    int64_t delay = (int64_t)h[0x14] << 8 | h[0x15];
    int64_t start = rd32(d + m + 0x0c), end = rd32(d + m + 0x10);
    t->hca = h;
    t->len = len;
    t->loop_start = start || end ? start + delay : 0;
    t->loop_end = start || end ? end + 1 + delay : -1;
    return 1;
}

/* ---- the cache and its worker ---- */

enum { EMPTY, QUEUED, LOADING, READY, FAILED };
typedef struct Slot {
    char seq[16];
    int state, refs;
    uint64_t used; /* when last asked for: the least recent unused one is reused first */
    uint8_t *data; /* the unpacked bank (the track points into it) */
    SteamTrack track;
} Slot;

static Slot slots[4];
static SDL_Mutex *lock;
static SDL_Condition *wake;
static uint64_t asked;

static int SDLCALL worker(void *unused) {
    (void)unused;
    SDL_LockMutex(lock);
    for (;;) {
        Slot *s = NULL;
        for (int i = 0; i < 4 && !s; i++) {
            if (slots[i].state == QUEUED) s = &slots[i];
        }
        if (!s) {
            SDL_WaitCondition(wake, lock);
            continue;
        }
        char path[64];
        path_of(s->seq, path, sizeof path);
        s->state = LOADING; /* a loading slot is never reused, so s stays this track's */
        SDL_UnlockMutex(lock);
        size_t n = 0;
        uint8_t *d = se_read(path, &n);
        SteamTrack t = {0};
        if (d && !parse(d, n, &t)) free(d), d = NULL;
        if (!d) rt_log("steam music: %s cannot be read", path);
        SDL_LockMutex(lock);
        s->data = d;
        s->track = t;
        s->state = d ? READY : FAILED;
    }
    return 0;
}

/* The first call (the game thread, or the check before it runs) makes the lock and the worker. */
static int start(void) {
    if (lock) return 1;
    SDL_Mutex *l = SDL_CreateMutex();
    wake = SDL_CreateCondition();
    if (!l || !wake) return 0;
    lock = l;
    SDL_Thread *t = SDL_CreateThread(worker, "steam music", NULL);
    if (!t) return 0;
    SDL_DetachThread(t); /* waits for work until the program ends */
    return 1;
}

int steam_music_has(const char *seq) {
    char path[64];
    path_of(seq, path, sizeof path);
    return se_has(path);
}

int steam_music_get(const char *seq, SteamTrack **out) {
    if (!steam_music_has(seq) || !start()) return -1;
    int r = 0;
    SDL_LockMutex(lock);
    Slot *s = NULL, *reuse = NULL;
    for (int i = 0; i < 4; i++) {
        Slot *c = &slots[i];
        if (c->state != EMPTY && !strcmp(c->seq, seq)) s = c;
        int idle = c->state == EMPTY || ((c->state == READY || c->state == FAILED) && !c->refs);
        if (idle && (!reuse || (reuse->state != EMPTY && (c->state == EMPTY || c->used < reuse->used)))) reuse = c;
    }
    if (s) {
        s->used = ++asked;
        if (s->state == READY) {
            s->refs++;
            *out = &s->track;
            r = 1;
        } else if (s->state == FAILED) {
            r = -1;
        }
    } else if (reuse) { /* else every slot is in use: it is asked for again later */
        free(reuse->data);
        memset(reuse, 0, sizeof *reuse);
        SDL_strlcpy(reuse->seq, seq, sizeof reuse->seq);
        reuse->state = QUEUED;
        reuse->used = ++asked;
        SDL_SignalCondition(wake);
    }
    SDL_UnlockMutex(lock);
    return r;
}

void steam_music_release(SteamTrack *t) {
    if (!t || !lock) return;
    SDL_LockMutex(lock);
    for (int i = 0; i < 4; i++) {
        if (&slots[i].track == t && slots[i].refs > 0) slots[i].refs--;
    }
    SDL_UnlockMutex(lock);
}

void steam_music_check(void) {
    if (!getenv("EGG_STEAMMUSIC_CHECK")) return;
    int tracks = 0, bad = 0;
    for (int i = 0; i <= 0x62; i++) {
        char seq[16];
        snprintf(seq, sizeof seq, "BGM_%02X", i);
        if (!steam_music_has(seq)) continue;
        SteamTrack *t = NULL;
        int r;
        Uint64 t0 = SDL_GetTicksNS();
        while ((r = steam_music_get(seq, &t)) == 0) SDL_Delay(1);
        tracks++;
        if (r < 0) {
            printf("steam music: %s FAILED\n", seq);
            bad++;
            continue;
        }
        size_t header = (size_t)t->hca[6] << 8 | t->hca[7];
        int sync = header + 2 <= t->len && t->hca[header] == 0xFF && t->hca[header + 1] == 0xFF;
        printf("steam music: %s %zu bytes, loop %lld-%lld, %.1f ms%s", seq, t->len, (long long)t->loop_start,
               (long long)t->loop_end, (double)(SDL_GetTicksNS() - t0) / 1e6, sync ? "" : " NO SYNC");
        bad += !sync;
        if (t->loop_end >= 0) { /* the loop: a seek back to its start lands in its block */
            int64_t landed = -1, start = t->loop_start;
            int ok = music_loop_check(seq, t, &landed); /* gives the reference up */
            printf(", seek to %lld%s%s", (long long)landed, ok ? "" : " SEEK FAILED",
                   start % 1024 >= 128 ? "" : " (seam: within a block's overlap)");
            bad += !ok;
        } else {
            steam_music_release(t);
        }
        printf("\n");
    }
    printf("steam music: %d tracks, %d bad\n", tracks, bad);
    exit(0);
}
