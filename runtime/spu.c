/* Sound hardware: 16 channels (PCM8/PCM16/IMA-ADPCM, PSG square, noise) mixed in software.
 *
 * Mixing runs on the game thread, in step with the SND driver frames, and reads sample data straight from
 * emulated memory. Output is float stereo at SPU_RATE, queued to an SDL3 audio stream; PCM channels use
 * linear interpolation (the hardware uses none). EGG_MUTE=1 skips the audio device, EGG_WAV=<file> records
 * the output. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "sound.h"

#define LATENCY_MIN 0.04 /* seconds queued when output (re)starts, and the most allowed before dropping */
#define LATENCY_MAX 0.15

SpuChannel spu_ch[SPU_CHANNELS];
float spu_master = 1.0f;

static SDL_AudioStream *stream;
static FILE *wav;
static uint32_t wav_bytes;
static double frac_frames;

static const int16_t adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};
static const int8_t adpcm_index_step[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static void wav_header(void) {
    uint8_t h[44] = "RIFF....WAVEfmt ";
    uint32_t v[] = {16, 1 | 2u << 16, SPU_RATE, SPU_RATE * 4, 4 | 16u << 16};
    memcpy(h + 16, v, sizeof v);
    memcpy(h + 36, "data", 4);
    uint32_t riff = 36 + wav_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &wav_bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav);
    fseek(wav, 0, SEEK_END);
}

void spu_open_output(void) {
    const char *path = getenv("EGG_WAV");
    if (path && (wav = fopen(path, "wb")) != NULL) wav_header();
    if (getenv("EGG_MUTE")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audio: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, SPU_RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        rt_log("audio: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
}

void spu_reset(void) {
    memset(spu_ch, 0, sizeof spu_ch);
    spu_master = 1.0f;
    frac_frames = 0;
}

static uint32_t adpcm_loop_nibble(const SpuChannel *c) { return c->loop_start > 4 ? (c->loop_start - 4) * 2 : 0; }

void spu_start(int ch) {
    SpuChannel *c = &spu_ch[ch];
    c->active = 1;
    c->pos = 0;
    c->lfsr = 0x7FFF;
    if (c->format == SPU_ADPCM) {
        c->adpcm_val = c->adpcm_prev = (int16_t)rt_r16(c->addr);
        c->adpcm_index = rt_r8(c->addr + 2) % 89;
        c->adpcm_n = 0;
        c->loop_val = c->adpcm_val;
        c->loop_index = c->adpcm_index;
    }
}

/* Samples in one pass of the data, and where the loop restarts. */
static void sample_span(const SpuChannel *c, uint32_t *end, uint32_t *loop) {
    uint32_t bytes = c->loop_start + c->loop_len;
    switch (c->format) {
    case SPU_PCM8: *end = bytes; *loop = c->loop_start; break;
    case SPU_PCM16: *end = bytes / 2; *loop = c->loop_start / 2; break;
    default: *end = bytes > 4 ? (bytes - 4) * 2 : 0; *loop = adpcm_loop_nibble(c); break;
    }
}

static int pcm_sample(const SpuChannel *c, uint32_t i) {
    return c->format == SPU_PCM8 ? (int8_t)rt_r8(c->addr + i) * 256 : (int16_t)rt_r16(c->addr + i * 2);
}

/* Decodes nibbles until adpcm_val is sample i (adpcm_prev is sample i - 1). */
static void adpcm_seek(SpuChannel *c, uint32_t i, uint32_t end) {
    uint32_t loop = adpcm_loop_nibble(c);
    while (c->adpcm_n <= i && c->adpcm_n < end) {
        if (c->adpcm_n == loop) {
            c->loop_val = c->adpcm_val;
            c->loop_index = c->adpcm_index;
        }
        uint8_t b = rt_r8(c->addr + 4 + c->adpcm_n / 2);
        int nib = c->adpcm_n & 1 ? b >> 4 : b & 0xF;
        int step = adpcm_step[c->adpcm_index];
        int diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        c->adpcm_prev = c->adpcm_val;
        c->adpcm_val = nib & 8 ? SDL_max(c->adpcm_val - diff, -0x7FFF) : SDL_min(c->adpcm_val + diff, 0x7FFF);
        c->adpcm_index = SDL_clamp(c->adpcm_index + adpcm_index_step[nib & 7], 0, 88);
        c->adpcm_n++;
    }
}

static void mix_pcm(SpuChannel *c, float *out, uint32_t frames, float gl, float gr, double step) {
    uint32_t end, loop;
    sample_span(c, &end, &loop);
    if (end == 0) {
        c->active = 0;
        return;
    }
    for (uint32_t f = 0; f < frames; f++) {
        uint32_t i = (uint32_t)c->pos;
        float frac = (float)(c->pos - i);
        int s0, s1;
        if (c->format == SPU_ADPCM) {
            adpcm_seek(c, i + 1 < end ? i + 1 : i, end);
            s1 = c->adpcm_val;
            s0 = i + 1 < end ? c->adpcm_prev : s1;
        } else {
            s0 = pcm_sample(c, i);
            s1 = i + 1 < end ? pcm_sample(c, i + 1) : c->repeat != SPU_LOOP_ONESHOT ? pcm_sample(c, loop) : s0;
        }
        float s = (s0 + (s1 - s0) * frac) * (1.0f / 32768.0f);
        out[f * 2] += s * gl;
        out[f * 2 + 1] += s * gr;
        c->pos += step;
        if (c->pos >= end) {
            if (c->repeat == SPU_LOOP_ONESHOT || end == loop) {
                c->active = 0;
                return;
            }
            c->pos = loop + SDL_fmod(c->pos - end, (double)(end - loop));
            if (c->format == SPU_ADPCM) {
                c->adpcm_n = loop;
                c->adpcm_val = c->adpcm_prev = c->loop_val;
                c->adpcm_index = c->loop_index;
            }
        }
    }
}

/* Square wave (channels 8-13) or noise (14-15), stepped at the timer rate; c->pos is the step phase. */
static void mix_psg(SpuChannel *c, int ch, float *out, uint32_t frames, float gl, float gr, double step) {
    uint32_t phase = (uint32_t)c->pos & 7;
    double sub = c->pos - (uint32_t)c->pos;
    int noise = ch >= 14;
    float s = 0;
    for (uint32_t f = 0; f < frames; f++) {
        if (noise) s = c->lfsr & 1 ? -1.0f : 1.0f;
        else if (ch >= 8) s = c->duty < 7 && phase <= (uint32_t)c->duty ? 1.0f : -1.0f;
        out[f * 2] += s * gl;
        out[f * 2 + 1] += s * gr;
        sub += step;
        while (sub >= 1.0) {
            sub -= 1.0;
            phase = (phase + 1) & 7;
            if (noise) c->lfsr = c->lfsr & 1 ? (c->lfsr >> 1) ^ 0x6000 : c->lfsr >> 1;
        }
    }
    c->pos = phase + sub;
}

static void output(const float *buf, uint32_t frames) {
    int bytes = (int)(frames * 2 * sizeof(float));
    if (stream) {
        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued == 0) { /* (re)start: queue some silence so host scheduling jitter does not underrun */
            static float silence[SPU_RATE / 25 * 2]; /* LATENCY_MIN */
            SDL_PutAudioStreamData(stream, silence, sizeof silence);
        }
        if (queued < (int)(SPU_RATE * LATENCY_MAX) * 2 * (int)sizeof(float)) SDL_PutAudioStreamData(stream, buf, bytes);
    }
    if (wav) {
        int16_t pcm[2048];
        for (uint32_t i = 0; i < frames * 2; i += 2048) {
            uint32_t n = SDL_min(2048u, frames * 2 - i);
            for (uint32_t k = 0; k < n; k++) pcm[k] = (int16_t)(buf[i + k] * 32767.0f);
            fwrite(pcm, 2, n, wav);
            wav_bytes += n * 2;
        }
        wav_header();
    }
}

void spu_render(double seconds, int speed, uint16_t normal, uint16_t muted, int music_speed) {
    static float buf[SPU_RATE / 10 * 2];
    frac_frames += seconds * SPU_RATE;
    uint32_t frames = (uint32_t)frac_frames;
    if (frames > SPU_RATE / 10) frames = SPU_RATE / 10; /* host stall: drop the rest */
    frac_frames -= (uint32_t)frac_frames;
    if (frames == 0) return;
    memset(buf, 0, frames * 2 * sizeof(float));
    for (int ch = 0; ch < SPU_CHANNELS; ch++) {
        SpuChannel *c = &spu_ch[ch];
        if (!c->active) continue;
        uint32_t timer = SDL_clamp(c->timer, 0x10u, 0xFFFFu);
        double step = SPU_CLOCK / timer / SPU_RATE * (normal >> ch & 1 ? 1 : speed);
        float vol = muted >> ch & 1 ? 0 : c->volume; /* muted: still played, so it ends on time */
        float gl = vol * (127 - c->pan) / 127.0f, gr = vol * c->pan / 127.0f;
        if (c->format == SPU_PSG) mix_psg(c, ch, buf, frames, gl, gr, step);
        else mix_pcm(c, buf, frames, gl, gr, step);
    }
    music_mix(buf, frames, music_speed);
    for (uint32_t i = 0; i < frames * 2; i++) buf[i] = SDL_clamp(buf[i] * spu_master, -1.0f, 1.0f);
    output(buf, frames);
}

void spu_state(RtStateIO *s) {
    RT_STATE(s, spu_ch);
    RT_STATE(s, spu_master);
    RT_STATE(s, frac_frames);
}
