/* Sound: the ARM7's 16 hardware channels (spu.c) and the SND driver that runs on them (snd.c). */
#pragma once

#include "rt.h"

#define SPU_CHANNELS 16
#define SPU_RATE 48000 /* output sample rate */
#define SPU_CLOCK 16756991.0 /* channel timers count at half the bus clock */

enum { SPU_PCM8, SPU_PCM16, SPU_ADPCM, SPU_PSG }; /* SPU_PSG: square wave on 8-13, noise on 14-15 */
enum { SPU_LOOP_MANUAL, SPU_LOOP_REPEAT, SPU_LOOP_ONESHOT };

typedef struct SpuChannel {
    int active;
    int format, repeat;
    uint32_t addr;                /* sample data */
    uint32_t loop_start, loop_len; /* bytes; playback covers loop_start + loop_len, then repeats loop_len */
    uint32_t timer;               /* clock cycles per sample (hardware register value is 0x10000 - timer) */
    float volume;                 /* linear, 0-1 */
    int pan;                      /* 0 (left) - 127 (right) */
    int duty;                     /* PSG: high for duty + 1 of 8 steps */
    /* playback state (spu.c) */
    double pos;
    uint32_t adpcm_n;             /* nibbles decoded */
    int adpcm_val, adpcm_prev, adpcm_index, loop_val, loop_index;
    uint32_t lfsr;
} SpuChannel;

extern SpuChannel spu_ch[SPU_CHANNELS];
extern float spu_master; /* master volume, 0-1 */

void spu_open_output(void);    /* host thread, once at startup */
void spu_reset(void);
void spu_start(int ch);        /* key on: restart playback with the channel's current settings */
/* Game thread: mix this much output (host time) and queue it. Fast forward: channels play `speed` times faster
 * (higher), except those in `normal`; the music mods play music_speed times faster. Channels in `muted` are
 * silent. */
void spu_render(double seconds, int speed, uint16_t normal, uint16_t muted, int music_speed);

void snd_reset(void);
void snd_command_list(uint32_t list); /* ARM9 -> ARM7 SOUND message: linked list of SNDCommand */
void snd_update(uint64_t now);        /* driver frames, alarms and audio output up to now */

/* Music mods (music.c): replacement files for the game's music sequences. Game thread. */
void music_prepare(int pno, uint32_t seq_data); /* a player loaded the sequence whose data is at seq_data */
void music_stop(int pno);                       /* the player stopped */
int music_replaced(int pno);                    /* the player's sequence is replaced: mute it */
int music_is_bgm(int pno);                      /* the player's sequence is one of the archive's BGM_* */
void music_player(int pno, int running, int db); /* every driver frame: playing (not paused), volume in 0.1 dB */
void music_mix(float *out, uint32_t frames, int speed); /* adds the replacements to float stereo output at SPU_RATE,
                                                       * `speed` (1 or 2) times faster */
