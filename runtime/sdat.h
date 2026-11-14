/* The game's sound archive (/sound/sound_data.sdat) in the ROM image: its sequences, instrument banks and wave
 * archives, by the names its symbol block gives them (sdat.c). Read once, from any thread. */
#pragma once

#include <stdint.h>

typedef struct SdatSeq {
    char name[16];        /* BGM_18, SE_0A3... */
    const uint8_t *data;  /* the sequence's data (what the driver gets the address of), in the ROM image */
    uint32_t size;
    int bank;             /* its instrument bank, or -1 */
    int volume;           /* the archive's volume for it, which the game sets as the player's volume */
} SdatSeq;

typedef struct SdatFile {
    char name[32];        /* BANK_SE_01, WAVE_SE_01... */
    const uint8_t *data;  /* the whole file (SBNK, SWAR), in the ROM image */
    uint32_t size;
    int wave_arcs[4];     /* banks: the wave archives the bank's instruments use (-1: none) */
} SdatFile;

/* The archive's sequences, banks and wave archives (counts 0 without an archive). */
const SdatSeq *sdat_seqs(int *count);
const SdatFile *sdat_banks(int *count);
const SdatFile *sdat_wave_arcs(int *count);
