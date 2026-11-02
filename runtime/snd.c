/* ARM7 sound driver (NitroSDK SND), emulated at a high level.
 *
 * The ARM9 SND library sends lists of commands over PXI. This file does what the ARM7 driver does with
 * them: sequence players interpret SSEQ data using SBNK instruments and SWAR waves (all in main RAM, where
 * the ARM9 loaded them) and allocate "ex channels" that add envelopes, LFO and pitch sweep on top of the
 * hardware channels (spu.c); raw channel setup serves streams such as movie audio; alarms pace the ARM9's
 * stream refills; the shared work block reports player/channel status and sequence variables back.
 * A driver frame runs every SND_FRAME_CYCLES, like the ARM7's sound alarm. SNDPlayer and SNDTrack fields
 * that the ARM9 sets by byte offset, and SNDSharedWork, follow the NitroSDK layouts. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>

#include "display.h"
#include "sound.h"

#define SND_FRAME_CYCLES (2728u * 64u) /* about 5.2 ms */
#define OS_TICK_CYCLES 64u
#define PLAYERS 16
#define TRACKS 32
#define CALL_DEPTH 3
#define VOL_MIN (-723) /* 0.1 dB units */
#define ENV_MIN (VOL_MIN * 128)
#define PSG_TIMER 8006 /* PSG/noise timer at an instrument's original key */

enum {
    CMD_START_SEQ, CMD_STOP_SEQ, CMD_PREPARE_SEQ, CMD_START_PREPARED_SEQ, CMD_PAUSE_SEQ, CMD_SKIP_SEQ,
    CMD_PLAYER_PARAM, CMD_TRACK_PARAM, CMD_MUTE_TRACK, CMD_ALLOCATABLE_CHANNEL, CMD_PLAYER_LOCAL_VAR,
    CMD_PLAYER_GLOBAL_VAR, CMD_START_TIMER, CMD_STOP_TIMER, CMD_SETUP_CHANNEL_PCM, CMD_SETUP_CHANNEL_PSG,
    CMD_SETUP_CHANNEL_NOISE, CMD_SETUP_CAPTURE, CMD_SETUP_ALARM, CMD_CHANNEL_TIMER, CMD_CHANNEL_VOLUME,
    CMD_CHANNEL_PAN, CMD_SURROUND_DECAY, CMD_MASTER_VOLUME, CMD_MASTER_PAN, CMD_OUTPUT_SELECTOR,
    CMD_LOCK_CHANNEL, CMD_UNLOCK_CHANNEL, CMD_STOP_UNLOCKED_CHANNEL, CMD_SHARED_WORK, CMD_INVALIDATE_SEQ,
    CMD_INVALIDATE_BANK, CMD_INVALIDATE_WAVE, CMD_READ_DRIVER_INFO, CMD_COUNT
};
enum { LFO_PITCH, LFO_VOLUME, LFO_PAN };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { INST_PCM = 1, INST_PSG, INST_NOISE, INST_DIRECTPCM, INST_NULL, INST_DRUMSET = 16, INST_KEYSPLIT };
enum { ARG_U8, ARG_S16, ARG_VLV, ARG_RANDOM, ARG_VAR };

typedef struct LfoParam { uint8_t target, speed, depth, range; uint16_t delay; } LfoParam;

typedef struct ExChannel {
    int active, start;
    int track, next;               /* owning track and next channel in its list, or -1 */
    uint8_t prio, key, original_key, velocity;
    int init_pan;
    int32_t length;                /* ticks until release; -1 = until the track releases it */
    int env_status;
    int32_t env, attack, decay, sustain, release;
    int user_volume, user_pitch, user_pan; /* from the track, every frame */
    int sweep_pitch, sweep_len, sweep_cnt, auto_sweep;
    LfoParam lfo;
    uint32_t lfo_delay_cnt, lfo_counter;
    int format, repeat, duty;
    uint32_t data, loop_start, loop_len, base_timer;
} ExChannel;

typedef struct Track {
    int used;
    int note_wait, mute, tie, note_finish_wait, porta, cmp, mask_flag;
    uint8_t pan_range;
    uint16_t prg;
    uint8_t volume, volume2;
    int8_t bend;
    uint8_t bend_range;
    int8_t pan, ext_pan;
    int16_t ext_fader, ext_pitch;
    uint8_t attack, decay, sustain, release, prio;
    int8_t transpose;
    uint8_t porta_key, porta_time;
    int16_t sweep_pitch;
    LfoParam mod;
    uint16_t ch_mask;
    int32_t wait;
    uint32_t base, cur, stack[CALL_DEPTH];
    uint8_t loop[CALL_DEPTH];
    int depth, ch_head;
} Track;

typedef struct Player {
    int active, started, paused;
    uint8_t prio, volume;
    int16_t ext_fader;
    int8_t tracks[16];
    uint16_t tempo, tempo_ratio, tempo_counter;
    uint32_t bank;
} Player;

typedef struct Inst {
    int type;
    uint16_t wave[2];
    uint8_t key, attack, decay, sustain, release, pan;
} Inst;

typedef struct SndAlarm { uint32_t tick, period, id; int active; uint64_t next; } SndAlarm;
#define SND_ALARMS 8

static ExChannel ex[SPU_CHANNELS];
static Track tracks[TRACKS];
static Player players[PLAYERS];
static SndAlarm alarms[SND_ALARMS];
static uint32_t shared_work, locked, rand_state;
static uint64_t frame_next;
static int16_t dsq[128];   /* SND_CalcDecibelSquare: 0.1 dB of (x / 127)^2 */
static int8_t lfo_sin[128];
static int log_snd = -1;

/* ---- helpers ---- */

static void init_tables(void) {
    for (int i = 0; i < 128; i++) {
        dsq[i] = i == 0 ? VOL_MIN : (int16_t)SDL_max(VOL_MIN, (int)lround(400.0 * log10(i / 127.0)));
        lfo_sin[i] = (int8_t)lround(127.0 * sin(i * (2.0 * SDL_PI_D / 128.0)));
    }
}

static uint32_t snd_rand(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return rand_state >> 16;
}

static int calc_attack(int a) {
    static const uint8_t fast[19] = {0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127, 132, 137, 143};
    return a >= 109 ? fast[127 - a] : 255 - a;
}

static int calc_rate(int r) { /* decay and release, envelope units per frame */
    if (r == 127) return 0xFFFF;
    if (r == 126) return 0x3C00;
    if (r < 50) return r * 2 + 1;
    return 0x1E00 / (126 - r);
}

/* Shared work: localVariable[16] and tickCounter per player, then globalVariable[16]. */
static uint32_t var_addr(int pno, int var) {
    if (!shared_work) return 0;
    if (var < 16) return shared_work + 0x20 + (uint32_t)pno * 0x24 + (uint32_t)var * 2;
    if (var < 32) return shared_work + 0x260 + (uint32_t)(var - 16) * 2;
    return 0;
}

/* ---- ex channels ---- */

static void ex_unlink(int c) {
    ExChannel *e = &ex[c];
    if (e->track >= 0) {
        int *link = &tracks[e->track].ch_head;
        while (*link >= 0 && *link != c) link = &ex[*link].next;
        if (*link == c) *link = e->next;
    }
    e->track = e->next = -1;
}

static void ex_stop(int c) {
    ex_unlink(c);
    ex[c].active = 0;
    ex[c].prio = 0;
    spu_ch[c].active = 0;
}

static void ex_release(int c, int release) {
    ExChannel *e = &ex[c];
    if (release >= 0) e->release = calc_rate(release & 0x7F);
    e->prio = 1;
    e->env_status = ENV_RELEASE;
}

static int ex_volume(const ExChannel *e) { return dsq[e->velocity] + (e->env >> 7); }

/* Picks a channel for a new note: a free one, else the lowest-priority (then quietest) one that the new
 * note's priority may take over. */
static int ex_alloc(uint32_t mask, int prio) {
    static const uint8_t order[16] = {4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13};
    mask &= ~locked;
    int best = -1;
    for (int i = 0; i < 16; i++) {
        int c = order[i];
        if (!(mask & (1u << c))) continue;
        if (best < 0 || (ex[c].prio <= ex[best].prio &&
                         (ex[c].prio != ex[best].prio || ex_volume(&ex[c]) < ex_volume(&ex[best])))) {
            best = c;
        }
    }
    if (best < 0 || prio < ex[best].prio) return -1;
    if (ex[best].active) ex_stop(best);
    return best;
}

/* ---- instruments ---- */

static void read_inst_param(uint32_t p, Inst *inst) {
    inst->wave[0] = (uint16_t)rt_r16(p);
    inst->wave[1] = (uint16_t)rt_r16(p + 2);
    inst->key = rt_r8(p + 4);
    inst->attack = rt_r8(p + 5);
    inst->decay = rt_r8(p + 6);
    inst->sustain = rt_r8(p + 7);
    inst->release = rt_r8(p + 8);
    inst->pan = rt_r8(p + 9);
}

/* SNDBankData: header 0x18, waveArcLink[4] {arc, next}, instCount at 0x38, instOffset[] (type | offset << 8). */
static int read_inst(uint32_t bank, int prg, int key, Inst *inst) {
    if (!bank || (uint32_t)prg >= rt_r32(bank + 0x38)) return 0;
    uint32_t off = rt_r32(bank + 0x3C + (uint32_t)prg * 4);
    uint32_t p = bank + (off >> 8);
    int type = off & 0xFF;
    if (type == INST_DRUMSET) {
        int lo = rt_r8(p), hi = rt_r8(p + 1);
        if (key < lo || key > hi) return 0;
        p += 2 + (uint32_t)(key - lo) * 12;
        type = rt_r8(p);
        p += 2;
    } else if (type == INST_KEYSPLIT) {
        int i = 0;
        while (i < 8 && rt_r8(p + i) && key > (int)rt_r8(p + i)) i++;
        if (i == 8 || !rt_r8(p + i)) return 0;
        p += 8 + (uint32_t)i * 12;
        type = rt_r8(p);
        p += 2;
    }
    if (type < INST_PCM || type > INST_NULL) return 0;
    inst->type = type;
    read_inst_param(p, inst);
    return 1;
}

/* Sets up an ex channel's wave from an instrument; 0 if the wave isn't loaded. */
static int ex_set_wave(ExChannel *e, uint32_t bank, const Inst *inst) {
    uint32_t wave;
    switch (inst->type) {
    case INST_PCM: {
        uint32_t arc = rt_r32(bank + 0x18 + (inst->wave[1] & 3u) * 8);
        if (!arc || (uint32_t)inst->wave[0] >= rt_r32(arc + 0x38)) return 0;
        wave = rt_r32(arc + 0x3C + inst->wave[0] * 4u);
        if (!wave) return 0;
        if (wave < 0x02000000u) wave += arc; /* offset within the archive, else already an address */
        break;
    }
    case INST_DIRECTPCM: wave = inst->wave[0] | (uint32_t)inst->wave[1] << 16; break;
    case INST_PSG:
        e->format = SPU_PSG;
        e->duty = inst->wave[0] & 7;
        e->base_timer = PSG_TIMER;
        return 1;
    case INST_NOISE:
        e->format = SPU_PSG;
        e->base_timer = PSG_TIMER;
        return 1;
    default: return 0;
    }
    /* SNDWaveData: format u8, loop u8, rate u16, timer u16, loopStart u16 (words), loopLen u32 (words), data */
    e->format = rt_r8(wave) & 3;
    e->repeat = rt_r8(wave + 1) ? SPU_LOOP_REPEAT : SPU_LOOP_ONESHOT;
    e->base_timer = rt_r16(wave + 4);
    e->loop_start = rt_r16(wave + 6) * 4u;
    e->loop_len = rt_r32(wave + 8) * 4u;
    e->data = wave + 12;
    return e->format != SPU_PSG;
}

/* ---- tracks ---- */

static void track_init(Track *t) {
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->note_wait = 1;
    t->pan_range = 127;
    t->volume = t->volume2 = 127;
    t->bend_range = 2;
    t->attack = t->decay = t->sustain = t->release = 0xFF;
    t->prio = 64;
    t->porta_key = 60;
    t->mod = (LfoParam){LFO_PITCH, 16, 0, 1, 0};
    t->ch_mask = 0xFFFF;
    t->ch_head = -1;
}

static int track_alloc(void) {
    for (int i = 0; i < TRACKS; i++) {
        if (!tracks[i].used) {
            track_init(&tracks[i]);
            return i;
        }
    }
    return -1;
}

static void track_release_channels(int ti, int release) {
    for (int c = tracks[ti].ch_head; c >= 0; c = ex[c].next) {
        if (ex[c].active) ex_release(c, release);
    }
}

/* Detaches the track's channels; they finish their release on their own. */
static void track_free_channels(int ti) {
    for (int c = tracks[ti].ch_head, n; c >= 0; c = n) {
        n = ex[c].next;
        ex[c].track = ex[c].next = -1;
    }
    tracks[ti].ch_head = -1;
}

static void track_mute(int ti, int mode) {
    Track *t = &tracks[ti];
    t->mute = mode != 0;
    if (mode >= 2) {
        track_release_channels(ti, mode == 3 ? 127 : -1);
        track_free_channels(ti);
    }
}

static void close_track(Player *p, int i) {
    int ti = p->tracks[i];
    track_release_channels(ti, -1);
    track_free_channels(ti);
    tracks[ti].used = 0;
    p->tracks[i] = -1;
}

static void note_on(int ti, Player *p, int key, int velocity, int32_t length) {
    Track *t = &tracks[ti];
    int c = t->tie ? t->ch_head : -1;
    ExChannel *e;
    if (c >= 0) {
        e = &ex[c];
        e->key = (uint8_t)key;
        e->velocity = (uint8_t)velocity;
    } else {
        Inst inst;
        if (!read_inst(p->bank, t->prg, key, &inst)) return;
        uint32_t mask = inst.type == INST_PSG ? 0x3F00u : inst.type == INST_NOISE ? 0xC000u : 0xFFFFu;
        if (t->mask_flag) mask &= t->ch_mask;
        c = ex_alloc(mask, SDL_min(t->prio + p->prio, 255));
        if (c < 0) return;
        e = &ex[c];
        memset(e, 0, sizeof *e);
        if (!ex_set_wave(e, p->bank, &inst)) {
            e->track = e->next = -1;
            return;
        }
        e->active = e->start = 1;
        e->prio = (uint8_t)SDL_min(t->prio + p->prio, 255);
        e->key = (uint8_t)key;
        e->original_key = inst.key;
        e->velocity = (uint8_t)velocity;
        e->init_pan = inst.pan - 64;
        e->attack = calc_attack(t->attack != 0xFF ? t->attack : inst.attack);
        e->decay = calc_rate(t->decay != 0xFF ? t->decay : inst.decay);
        e->sustain = dsq[(t->sustain != 0xFF ? t->sustain : inst.sustain) & 0x7F] << 7;
        e->release = calc_rate(t->release != 0xFF ? t->release : inst.release);
        e->env = ENV_MIN;
        e->env_status = ENV_ATTACK;
        e->lfo = t->mod;
        e->track = ti;
        e->next = t->ch_head;
        t->ch_head = c;
    }
    e->length = length;
    int sweep = t->sweep_pitch + (t->porta ? (t->porta_key - key) << 6 : 0);
    e->sweep_pitch = sweep;
    e->sweep_cnt = 0;
    if (t->porta_time == 0) {
        e->sweep_len = length;
        e->auto_sweep = 0;
    } else {
        e->sweep_len = (abs(sweep) * t->porta_time * t->porta_time) >> 11;
        e->auto_sweep = 1;
    }
}

static uint8_t rd8(Track *t) { return rt_r8(t->cur++); }
static uint16_t rd16(Track *t) { uint16_t v = rd8(t); return (uint16_t)(v | rd8(t) << 8); }
static uint32_t rd24(Track *t) { uint32_t v = rd16(t); return v | (uint32_t)rd8(t) << 16; }

static int read_arg(Track *t, int pno, int type) {
    switch (type) {
    case ARG_U8: return rd8(t);
    case ARG_S16: return (int16_t)rd16(t);
    case ARG_VLV: {
        uint32_t v = 0;
        uint8_t b;
        do {
            b = rd8(t);
            v = v << 7 | (b & 0x7F);
        } while (b & 0x80);
        return (int)v;
    }
    case ARG_RANDOM: {
        int lo = (int16_t)rd16(t), hi = (int16_t)rd16(t);
        return lo + (int)((snd_rand() * (uint32_t)(hi - lo + 1)) >> 16);
    }
    default: {
        uint32_t a = var_addr(pno, rd8(t));
        return a ? (int16_t)rt_r16(a) : 0;
    }
    }
}

static void var_op(int cmd, uint32_t a, int arg, Track *t) {
    int v = (int16_t)rt_r16(a);
    switch (cmd) {
    case 0xB0: v = arg; break;
    case 0xB1: v += arg; break;
    case 0xB2: v -= arg; break;
    case 0xB3: v *= arg; break;
    case 0xB4: if (arg) v /= arg; break;
    case 0xB5: v = arg >= 0 ? v << arg : v >> -arg; break;
    case 0xB6:
        v = arg < 0 ? -(int)((snd_rand() * (uint32_t)(-arg + 1)) >> 16) : (int)((snd_rand() * (uint32_t)(arg + 1)) >> 16);
        break;
    case 0xB8: t->cmp = v == arg; return;
    case 0xB9: t->cmp = v >= arg; return;
    case 0xBA: t->cmp = v > arg; return;
    case 0xBB: t->cmp = v <= arg; return;
    case 0xBC: t->cmp = v < arg; return;
    case 0xBD: t->cmp = v != arg; return;
    default: return;
    }
    rt_w16(a, (uint16_t)v);
}

/* Executes one sequence command; -1 at the end of the track. */
static int track_step(int ti, Player *p, int pno, int allow_note) {
    Track *t = &tracks[ti];
    int exec = 1, type = -1;
    int cmd = rd8(t);
    if (cmd == 0xA2) {
        cmd = rd8(t);
        exec = t->cmp;
    }
    if (cmd == 0xA0) {
        cmd = rd8(t);
        type = ARG_RANDOM;
    }
    if (cmd == 0xA1) {
        cmd = rd8(t);
        type = ARG_VAR;
    }
#define ARG(def) read_arg(t, pno, type >= 0 ? type : (def))
    if (cmd < 0x80) {
        int velocity = rd8(t) & 0x7F;
        int length = ARG(ARG_VLV);
        if (!exec) return 0;
        int key = SDL_clamp(cmd + t->transpose, 0, 127);
        if (!t->mute && allow_note) note_on(ti, p, key, velocity, length > 0 ? length : -1);
        t->porta_key = (uint8_t)key;
        if (t->note_wait) {
            t->wait = length;
            if (length == 0) t->note_finish_wait = 1;
        }
        return 0;
    }
    switch (cmd & 0xF0) {
    case 0x80: {
        int a = ARG(ARG_VLV);
        if (!exec) break;
        if (cmd == 0x80) t->wait = a;
        else if (cmd == 0x81 && a < 0x10000) t->prg = (uint16_t)a;
        break;
    }
    case 0x90: {
        int n = cmd == 0x93 ? rd8(t) : 0;
        uint32_t off = rd24(t);
        if (!exec) break;
        if (cmd == 0x93) {
            int nt = p->tracks[n & 15];
            if (nt >= 0 && nt != ti) {
                tracks[nt].base = t->base;
                tracks[nt].cur = t->base + off;
            }
        } else if (cmd == 0x94) {
            t->cur = t->base + off;
        } else if (cmd == 0x95 && t->depth < CALL_DEPTH) {
            t->stack[t->depth++] = t->cur;
            t->cur = t->base + off;
        }
        break;
    }
    case 0xB0: {
        int var = rd8(t);
        int a = ARG(ARG_S16);
        uint32_t va = var_addr(pno, var);
        if (exec && va) var_op(cmd, va, a, t);
        break;
    }
    case 0xC0:
    case 0xD0: {
        int a = ARG(ARG_U8);
        if (!exec) break;
        switch (cmd) {
        case 0xC0: t->pan = (int8_t)(a - 64); break;
        case 0xC1: t->volume = (uint8_t)a; break;
        case 0xC2: p->volume = (uint8_t)a; break;
        case 0xC3: t->transpose = (int8_t)a; break;
        case 0xC4: t->bend = (int8_t)a; break;
        case 0xC5: t->bend_range = (uint8_t)a; break;
        case 0xC6: t->prio = (uint8_t)a; break;
        case 0xC7: t->note_wait = a & 1; break;
        case 0xC8:
            t->tie = a & 1;
            track_release_channels(ti, -1);
            track_free_channels(ti);
            break;
        case 0xC9:
            t->porta_key = (uint8_t)(a + t->transpose);
            t->porta = 1;
            break;
        case 0xCA: t->mod.depth = (uint8_t)a; break;
        case 0xCB: t->mod.speed = (uint8_t)a; break;
        case 0xCC: t->mod.target = (uint8_t)a; break;
        case 0xCD: t->mod.range = (uint8_t)a; break;
        case 0xCE: t->porta = a & 1; break;
        case 0xCF: t->porta_time = (uint8_t)a; break;
        case 0xD0: t->attack = (uint8_t)a; break;
        case 0xD1: t->decay = (uint8_t)a; break;
        case 0xD2: t->sustain = (uint8_t)a; break;
        case 0xD3: t->release = (uint8_t)a; break;
        case 0xD4:
            if (t->depth < CALL_DEPTH) {
                t->stack[t->depth] = t->cur;
                t->loop[t->depth++] = (uint8_t)a;
            }
            break;
        case 0xD5: t->volume2 = (uint8_t)a; break;
        case 0xD7: track_mute(ti, a); break;
        default: break; /* 0xD6: print variable */
        }
        break;
    }
    case 0xE0: {
        int a = ARG(ARG_S16);
        if (!exec) break;
        if (cmd == 0xE0) t->mod.delay = (uint16_t)a;
        else if (cmd == 0xE1) p->tempo = (uint16_t)a;
        else if (cmd == 0xE3) t->sweep_pitch = (int16_t)a;
        break;
    }
    case 0xF0:
        if (cmd == 0xFE) {
            rd16(t);
            break;
        }
        if (!exec) break;
        if (cmd == 0xFF) return -1;
        if (t->depth == 0) break;
        if (cmd == 0xFD) {
            t->cur = t->stack[--t->depth];
        } else if (cmd == 0xFC) {
            uint8_t n = t->loop[t->depth - 1];
            if (n == 1) {
                t->depth--;
                break;
            }
            if (n) t->loop[t->depth - 1] = n - 1; /* 0 loops forever */
            t->cur = t->stack[t->depth - 1];
        }
        break;
    default: break;
    }
#undef ARG
    return 0;
}

/* One sequence tick for a track; 0 when it reaches its end. */
static int track_tick(int ti, Player *p, int pno, int allow_note) {
    Track *t = &tracks[ti];
    for (int c = t->ch_head; c >= 0; c = ex[c].next) {
        if (ex[c].length > 0) ex[c].length--;
        if (!ex[c].auto_sweep && ex[c].sweep_cnt < ex[c].sweep_len) ex[c].sweep_cnt++;
    }
    if (t->note_finish_wait) {
        if (t->ch_head >= 0) return 1;
        t->note_finish_wait = 0;
    }
    if (t->wait > 0 && --t->wait > 0) return 1;
    for (int guard = 0; t->wait == 0 && !t->note_finish_wait; guard++) {
        if (guard == 100000) {
            rt_log("snd: player %d track stuck at 0x%08x", pno, t->cur);
            return 0;
        }
        if (track_step(ti, p, pno, allow_note) < 0) return 0;
    }
    return 1;
}

static void track_update_channels(int ti, const Player *p) {
    Track *t = &tracks[ti];
    int vol = dsq[t->volume & 0x7F] + dsq[t->volume2 & 0x7F] + dsq[p->volume & 0x7F] + t->ext_fader + p->ext_fader;
    if (music_replaced((int)(p - players))) vol = VOL_MIN; /* a music mod plays instead (music.c) */
    int pitch = ((t->bend * t->bend_range) << 6 >> 7) + t->ext_pitch;
    int pan = t->pan;
    if (t->pan_range != 127) pan = (pan * t->pan_range + 64) >> 7;
    pan += t->ext_pan;
    for (int c = t->ch_head; c >= 0; c = ex[c].next) {
        ExChannel *e = &ex[c];
        e->user_volume = vol;
        if (e->env_status == ENV_RELEASE) continue;
        e->user_pitch = pitch;
        e->user_pan = pan;
        e->lfo = t->mod;
        if (e->length == 0) {
            e->prio = 0;
            ex_release(c, -1);
        }
    }
}

/* ---- players ---- */

static void player_stop(int pno) {
    Player *p = &players[pno];
    if (!p->active) return;
    for (int i = 0; i < 16; i++) {
        if (p->tracks[i] >= 0) close_track(p, i);
    }
    p->active = p->started = p->paused = 0;
    music_stop(pno);
}

static void player_prepare(int pno, uint32_t base, uint32_t offset, uint32_t bank) {
    if (pno >= PLAYERS) return;
    player_stop(pno);
    Player *p = &players[pno];
    *p = (Player){.prio = 64, .volume = 127, .tempo = 120, .tempo_ratio = 256, .tempo_counter = 240, .bank = bank};
    memset(p->tracks, -1, sizeof p->tracks);
    for (int v = 0; v < 16; v++) {
        if (var_addr(pno, v)) rt_w16(var_addr(pno, v), 0xFFFF);
    }
    if (shared_work) rt_w32(shared_work + 0x20 + (uint32_t)pno * 0x24 + 0x20, 0);
    int ti = track_alloc();
    if (ti < 0) return;
    tracks[ti].base = base;
    tracks[ti].cur = base + offset;
    p->tracks[0] = (int8_t)ti;
    if (rt_r8(tracks[ti].cur) == 0xFE) { /* allocate the tracks the sequence opens */
        tracks[ti].cur++;
        uint16_t mask = rd16(&tracks[ti]);
        for (int i = 1; i < 16; i++) {
            if (!(mask & (1u << i))) continue;
            int n = track_alloc();
            if (n < 0) break;
            p->tracks[i] = (int8_t)n;
        }
    }
    p->active = 1;
    music_prepare(pno, base);
}

/* One tick of every track; 0 when all have ended. */
static int player_tick(int pno, int allow_note) {
    Player *p = &players[pno];
    int playing = 0;
    for (int i = 0; i < 16; i++) {
        int ti = p->tracks[i];
        if (ti < 0 || !tracks[ti].cur) continue;
        if (track_tick(ti, p, pno, allow_note)) playing = 1;
        else close_track(p, i);
    }
    return playing;
}

static void player_main(int pno) {
    Player *p = &players[pno];
    if (!p->active || !p->started) return;
    if (!p->paused) {
        while (p->tempo_counter >= 240) {
            p->tempo_counter -= 240;
            if (!player_tick(pno, 1)) {
                player_stop(pno);
                return;
            }
            if (shared_work) {
                uint32_t a = shared_work + 0x20 + (uint32_t)pno * 0x24 + 0x20;
                rt_w32(a, rt_r32(a) + 1);
            }
        }
        p->tempo_counter = (uint16_t)(p->tempo_counter + ((p->tempo * p->tempo_ratio) >> 8));
    }
    for (int i = 0; i < 16; i++) {
        if (p->tracks[i] >= 0) track_update_channels(p->tracks[i], p);
    }
}

/* SNDPlayer fields by byte offset (SND_COMMAND_PLAYER_PARAM). */
static void player_set_byte(Player *p, uint32_t off, uint8_t b) {
#define SET16(f) f = (uint16_t)((off & 1) ? ((f & 0xFF) | b << 8) : ((f & 0xFF00) | b))
    switch (off) {
    case 4: p->prio = b; break;
    case 5: p->volume = b; break;
    case 6: case 7: SET16(p->ext_fader); break;
    case 0x18: case 0x19: SET16(p->tempo); break;
    case 0x1A: case 0x1B: SET16(p->tempo_ratio); break;
    case 0x1C: case 0x1D: SET16(p->tempo_counter); break;
    default: break;
    }
}

/* SNDTrack fields by byte offset (SND_COMMAND_TRACK_PARAM). */
static void track_set_byte(Track *t, uint32_t off, uint8_t b) {
    switch (off) {
    case 1: t->pan_range = b; break;
    case 2: case 3: SET16(t->prg); break;
    case 4: t->volume = b; break;
    case 5: t->volume2 = b; break;
    case 6: t->bend = (int8_t)b; break;
    case 7: t->bend_range = b; break;
    case 8: t->pan = (int8_t)b; break;
    case 9: t->ext_pan = (int8_t)b; break;
    case 0xA: case 0xB: SET16(t->ext_fader); break;
    case 0xC: case 0xD: SET16(t->ext_pitch); break;
    case 0xE: t->attack = b; break;
    case 0xF: t->decay = b; break;
    case 0x10: t->sustain = b; break;
    case 0x11: t->release = b; break;
    case 0x12: t->prio = b; break;
    case 0x13: t->transpose = (int8_t)b; break;
    case 0x14: t->porta_key = b; break;
    case 0x15: t->porta_time = b; break;
    case 0x16: case 0x17: SET16(t->sweep_pitch); break;
    case 0x18: t->mod.target = b; break;
    case 0x19: t->mod.speed = b; break;
    case 0x1A: t->mod.depth = b; break;
    case 0x1B: t->mod.range = b; break;
    case 0x1C: case 0x1D: SET16(t->mod.delay); break;
    case 0x1E: case 0x1F: SET16(t->ch_mask); break;
    default: break;
    }
#undef SET16
}

/* ---- per-frame channel processing ---- */

/* The channels in `mask`. */
static void ex_main(uint16_t mask) {
    for (int c = 0; c < SPU_CHANNELS; c++) {
        ExChannel *e = &ex[c];
        if (!e->active || !(mask >> c & 1)) continue;
        SpuChannel *s = &spu_ch[c];
        if (e->start) {
            s->format = e->format;
            s->repeat = e->repeat;
            s->addr = e->data;
            s->loop_start = e->loop_start;
            s->loop_len = e->loop_len;
            s->duty = e->duty;
            spu_start(c);
            e->start = 0;
        } else if (!s->active) {
            ex_stop(c);
            continue;
        }
        switch (e->env_status) {
        case ENV_ATTACK:
            e->env = -((-e->env * e->attack) >> 8);
            if (e->env == 0) e->env_status = ENV_DECAY;
            break;
        case ENV_DECAY:
            e->env -= e->decay;
            if (e->env <= e->sustain) {
                e->env = e->sustain;
                e->env_status = ENV_SUSTAIN;
            }
            break;
        case ENV_RELEASE: e->env -= e->release; break;
        default: break;
        }
        if (e->env_status == ENV_RELEASE && e->env <= ENV_MIN) {
            ex_stop(c);
            continue;
        }
        int vol = dsq[e->velocity & 0x7F] + e->user_volume + (e->env >> 7);
        int pitch = ((e->key - e->original_key) << 6) + e->user_pitch;
        int pan = e->init_pan + e->user_pan;
        if (e->sweep_pitch && e->sweep_cnt < e->sweep_len) {
            pitch += (int)((int64_t)e->sweep_pitch * (e->sweep_len - e->sweep_cnt) / e->sweep_len);
            if (e->auto_sweep) e->sweep_cnt++;
        }
        if (e->lfo.depth) {
            if (e->lfo_delay_cnt < e->lfo.delay) {
                e->lfo_delay_cnt++;
            } else {
                int64_t v = (int64_t)lfo_sin[(e->lfo_counter >> 8) & 127] * e->lfo.depth * e->lfo.range;
                if (e->lfo.target == LFO_VOLUME) vol += (int)((v * 60) >> 14);
                else if (e->lfo.target == LFO_PAN) pan += (int)((v * 64) >> 14);
                else pitch += (int)((v * 64) >> 14);
                e->lfo_counter += (uint32_t)e->lfo.speed << 6;
                e->lfo_counter %= 128u << 8;
            }
        }
        vol = SDL_clamp(vol, VOL_MIN, 0);
        s->volume = vol <= VOL_MIN ? 0.0f : (float)pow(10.0, vol / 200.0);
        s->timer = (uint32_t)SDL_clamp(e->base_timer * exp2(-pitch / 768.0), 16.0, 65535.0);
        s->pan = SDL_clamp(pan, -64, 63) + 64;
    }
}

static void update_shared_work(void) {
    if (!shared_work) return;
    uint32_t pst = 0, cst = 0;
    for (int i = 0; i < PLAYERS; i++) pst |= (uint32_t)players[i].active << i;
    for (int c = 0; c < SPU_CHANNELS; c++) cst |= (uint32_t)spu_ch[c].active << c;
    rt_w32(shared_work + 4, pst);
    rt_w16(shared_work + 8, (uint16_t)cst);
    rt_w16(shared_work + 10, 0);
}

/* The players whose sequence is a BGM (bgm 1), the others (0) or all (-1). */
static void players_main(int bgm) {
    for (int i = 0; i < PLAYERS; i++) {
        if (bgm >= 0 && music_is_bgm(i) != bgm) continue;
        player_main(i);
        const Player *p = &players[i];
        music_player(i, p->active && p->started && !p->paused, dsq[p->volume & 0x7F] + p->ext_fader);
    }
}

/* The channels held by the tracks of players whose sequence is a BGM. */
static uint16_t bgm_channels(void) {
    uint8_t bgm_track[TRACKS] = {0};
    for (int i = 0; i < PLAYERS; i++) {
        if (!players[i].active || !music_is_bgm(i)) continue;
        for (int t = 0; t < 16; t++) {
            if (players[i].tracks[t] >= 0) bgm_track[players[i].tracks[t]] = 1;
        }
    }
    uint16_t mask = 0;
    for (int c = 0; c < SPU_CHANNELS; c++) {
        if (ex[c].active && ex[c].track >= 0 && bgm_track[ex[c].track]) mask |= (uint16_t)(1u << c);
    }
    return mask;
}

/* One driver frame of emulated time, and its output. Fast forward (sched.c) shortens it in host time: the mixer
 * plays the channels that much faster. With the Fast forward music setting at Normal speed, the BGM players and
 * their channels instead get a frame per SND_FRAME_CYCLES of host time and play at their own pitch; the game
 * then sees the music advance slower than itself. */
static void snd_frame(void) {
    static double bgm_frames; /* BGM frames owed (Normal speed) */
    int speed = sched_speed();
    double seconds = (double)SND_FRAME_CYCLES / RT_CLOCK_HZ / speed;
    if (speed == 1 || display_setting_get(SET_FF_MUSIC) != FF_MUSIC_NORMAL) {
        bgm_frames = 0;
        players_main(-1);
        ex_main(0xFFFF);
        update_shared_work();
        spu_render(seconds, speed, 0, speed);
        return;
    }
    players_main(0);
    ex_main((uint16_t)~bgm_channels());
    for (bgm_frames += 1.0 / speed; bgm_frames >= 1; bgm_frames -= 1) {
        players_main(1);
        ex_main(bgm_channels());
    }
    update_shared_work();
    spu_render(seconds, speed, bgm_channels(), 1);
}

/* ---- commands ---- */

static void alarm_timers(uint32_t mask, int start) {
    uint64_t now = sched_now();
    for (int i = 0; i < SND_ALARMS; i++) {
        if (!(mask & (1u << i))) continue;
        SndAlarm *a = &alarms[i];
        a->active = start;
        a->next = now + (uint64_t)(a->tick ? a->tick : a->period) * OS_TICK_CYCLES;
    }
}

static void setup_channel_pcm(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    static const float shift_div[4] = {1, 2, 4, 16};
    SpuChannel *s = &spu_ch[a0 & 15];
    s->active = 0;
    s->timer = a0 >> 16;
    s->addr = a1;
    s->volume = (a2 >> 24 & 0x7F) / 127.0f / shift_div[a2 >> 22 & 3];
    s->loop_len = (a2 & 0x3FFFFF) * 4;
    s->loop_start = (a3 & 0xFFFF) * 4;
    s->pan = a3 >> 16 & 0x7F;
    s->format = a3 >> 24 & 3;
    s->repeat = a3 >> 26 & 3;
}

static void for_channels(uint32_t mask, void (*fn)(int c, uint32_t v), uint32_t v) {
    for (int c = 0; c < SPU_CHANNELS; c++) {
        if (mask & (1u << c)) fn(c, v);
    }
}
static void ch_start(int c, uint32_t v) { (void)v; spu_start(c); }
static void ch_stop(int c, uint32_t v) { (void)v; spu_ch[c].active = 0; }
static void ch_timer(int c, uint32_t v) { spu_ch[c].timer = v; }
static void ch_pan(int c, uint32_t v) { spu_ch[c].pan = v & 0x7F; }
static void ch_lock(int c, uint32_t v) { (void)v; if (ex[c].active) ex_stop(c); }
static void ch_stop_unlocked(int c, uint32_t v) {
    (void)v;
    if (locked & (1u << c)) return;
    if (ex[c].active) ex_stop(c);
    spu_ch[c].active = 0;
}

static void invalidate(int what, uint32_t start, uint32_t end) {
#define IN(a) ((a) >= start && (a) < end)
    if (what == CMD_INVALIDATE_WAVE) {
        for (int c = 0; c < SPU_CHANNELS; c++) {
            if (ex[c].active && IN(ex[c].data)) ex_stop(c);
        }
        return;
    }
    for (int i = 0; i < PLAYERS; i++) {
        Player *p = &players[i];
        if (!p->active) continue;
        int hit = what == CMD_INVALIDATE_BANK && IN(p->bank);
        for (int k = 0; k < 16 && !hit; k++) hit = what == CMD_INVALIDATE_SEQ && p->tracks[k] >= 0 && IN(tracks[p->tracks[k]].base);
        if (hit) player_stop(i);
    }
#undef IN
}

static void command(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    switch (id) {
    case CMD_START_SEQ:
        player_prepare((int)a0, a1, a2, a3);
        if (a0 < PLAYERS) players[a0].started = 1;
        break;
    case CMD_STOP_SEQ: if (a0 < PLAYERS) player_stop((int)a0); break;
    case CMD_PREPARE_SEQ: player_prepare((int)a0, a1, a2, a3); break;
    case CMD_START_PREPARED_SEQ: if (a0 < PLAYERS) players[a0].started = 1; break;
    case CMD_PAUSE_SEQ:
        if (a0 < PLAYERS && players[a0].active) {
            Player *p = &players[a0];
            if (a1 && !p->paused) {
                for (int i = 0; i < 16; i++) {
                    if (p->tracks[i] >= 0) track_release_channels(p->tracks[i], 127);
                }
            }
            p->paused = a1 != 0;
        }
        break;
    case CMD_SKIP_SEQ:
        if (a0 < PLAYERS && players[a0].active) {
            for (uint32_t n = 0; n < a1; n++) {
                if (!player_tick((int)a0, 0)) {
                    player_stop((int)a0);
                    break;
                }
            }
        }
        break;
    case CMD_PLAYER_PARAM:
        if (a0 < PLAYERS) {
            for (uint32_t k = 0; k < a3 && k < 4; k++) player_set_byte(&players[a0], a1 + k, (uint8_t)(a2 >> (8 * k)));
        }
        break;
    case CMD_TRACK_PARAM:
    case CMD_MUTE_TRACK:
    case CMD_ALLOCATABLE_CHANNEL: {
        uint32_t pno = id == CMD_TRACK_PARAM ? a0 & 0xFFFFFF : a0;
        if (pno >= PLAYERS || !players[pno].active) break;
        for (int i = 0; i < 16; i++) {
            int ti = players[pno].tracks[i];
            if (ti < 0 || !(a1 & (1u << i))) continue;
            if (id == CMD_MUTE_TRACK) {
                track_mute(ti, (int)a2);
            } else if (id == CMD_ALLOCATABLE_CHANNEL) {
                tracks[ti].ch_mask = (uint16_t)a2;
                tracks[ti].mask_flag = 1;
            } else {
                for (uint32_t k = 0; k < (a0 >> 24) && k < 4; k++) track_set_byte(&tracks[ti], a2 + k, (uint8_t)(a3 >> (8 * k)));
            }
        }
        break;
    }
    case CMD_PLAYER_LOCAL_VAR: if (a0 < PLAYERS && var_addr((int)a0, (int)(a1 & 15))) rt_w16(var_addr((int)a0, (int)(a1 & 15)), (uint16_t)a2); break;
    case CMD_PLAYER_GLOBAL_VAR: if (var_addr(0, (int)(a0 & 15) + 16)) rt_w16(var_addr(0, (int)(a0 & 15) + 16), (uint16_t)a1); break;
    case CMD_START_TIMER:
        for_channels(a0, ch_start, 0);
        alarm_timers(a2, 1);
        break;
    case CMD_STOP_TIMER:
        for_channels(a0, ch_stop, 0);
        alarm_timers(a2, 0);
        break;
    case CMD_SETUP_CHANNEL_PCM: setup_channel_pcm(a0, a1, a2, a3); break;
    case CMD_SETUP_ALARM:
        if (a0 < SND_ALARMS) alarms[a0] = (SndAlarm){a1, a2, a3 & 0xFF, 0, 0};
        break;
    case CMD_CHANNEL_TIMER: for_channels(a0, ch_timer, a1); break;
    case CMD_CHANNEL_PAN: for_channels(a0, ch_pan, a1); break;
    case CMD_CHANNEL_VOLUME: {
        static const float shift_div[4] = {1, 2, 4, 16};
        for (int c = 0; c < SPU_CHANNELS; c++) {
            if (a0 & (1u << c)) spu_ch[c].volume = (a1 & 0x7F) / 127.0f / shift_div[a2 & 3];
        }
        break;
    }
    case CMD_MASTER_VOLUME: spu_master = (a0 & 0x7F) / 127.0f; break;
    case CMD_LOCK_CHANNEL:
        for_channels(a0, ch_lock, 0);
        locked |= a0 & 0xFFFF;
        break;
    case CMD_UNLOCK_CHANNEL: locked &= ~a0; break;
    case CMD_STOP_UNLOCKED_CHANNEL: for_channels(a0, ch_stop_unlocked, 0); break;
    case CMD_SHARED_WORK: shared_work = a0; break;
    case CMD_INVALIDATE_SEQ:
    case CMD_INVALIDATE_BANK:
    case CMD_INVALIDATE_WAVE: invalidate((int)id, a0, a1); break;
    case CMD_SURROUND_DECAY:
    case CMD_MASTER_PAN:
    case CMD_OUTPUT_SELECTOR: break; /* no effect on the mixed output we produce */
    default: {
        static uint64_t warned;
        if (id < 64 && !(warned & (1ull << id))) {
            warned |= 1ull << id;
            rt_log("snd: command %u not implemented (args %08x %08x %08x %08x)", id, a0, a1, a2, a3);
        }
        break;
    }
    }
}

void snd_command_list(uint32_t list) {
    if (log_snd < 0) log_snd = getenv("EGG_LOG_PXI") != NULL;
    for (uint32_t c = list; c; c = rt_r32(c)) {
        uint32_t id = rt_r32(c + 4), a0 = rt_r32(c + 8), a1 = rt_r32(c + 12), a2 = rt_r32(c + 16), a3 = rt_r32(c + 20);
        if (log_snd) rt_log("snd: command %u args %08x %08x %08x %08x", id, a0, a1, a2, a3);
        command(id, a0, a1, a2, a3);
    }
    /* The ARM9 waits for each list by its tag: finishCommandTag, the first word of the shared work. Player and
     * channel status must already reflect the list: NNS checks them once the tag finishes, and a player it
     * started that is not yet marked active counts as ended (its handle is released, volume changes are lost). */
    update_shared_work();
    if (shared_work) rt_w32(shared_work, rt_r32(shared_work) + 1);
}

void snd_update(uint64_t now) {
    for (int i = 0; i < SND_ALARMS; i++) {
        SndAlarm *a = &alarms[i];
        if (!a->active || now < a->next) continue;
        pxi_arm7_send(7 | ((uint32_t)i | a->id << 8) << 6); /* PXI tag SOUND */
        if (!a->period) {
            a->active = 0;
            continue;
        }
        a->next += (uint64_t)a->period * OS_TICK_CYCLES;
        if (a->next < now) a->next = now + (uint64_t)a->period * OS_TICK_CYCLES; /* host stall: skip */
    }
    if (frame_next == 0) frame_next = now;
    if (now > frame_next + 32ull * SND_FRAME_CYCLES) frame_next = now; /* host stall: skip the missed time */
    while (now >= frame_next) {
        snd_frame();
        frame_next += SND_FRAME_CYCLES;
    }
}

void snd_reset(void) {
    if (!dsq[0]) init_tables();
    memset(ex, 0, sizeof ex);
    for (int c = 0; c < SPU_CHANNELS; c++) ex[c].track = ex[c].next = -1;
    memset(tracks, 0, sizeof tracks);
    memset(players, 0, sizeof players);
    for (int i = 0; i < PLAYERS; i++) music_stop(i);
    memset(alarms, 0, sizeof alarms);
    shared_work = locked = 0;
    rand_state = 0x12345678u;
    frame_next = 0;
    spu_reset();
}

void snd_state(RtStateIO *s) {
    RT_STATE(s, ex);
    RT_STATE(s, tracks);
    RT_STATE(s, players);
    RT_STATE(s, alarms);
    RT_STATE(s, shared_work);
    RT_STATE(s, locked);
    RT_STATE(s, rand_state);
    RT_STATE(s, frame_next);
    if (rt_state_loading(s)) { /* music mods: the loaded players' replacements, from the start */
        for (int i = 0; i < PLAYERS; i++) {
            music_stop(i);
            if (players[i].active && players[i].tracks[0] >= 0) music_prepare(i, tracks[players[i].tracks[0]].base);
        }
    }
}
