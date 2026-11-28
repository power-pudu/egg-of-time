/* The Steam release's music, read from the player's own copy (steam_music.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct SteamTrack {
    const uint8_t *hca;           /* an HCA file, unmasked, in memory */
    size_t len;
    int64_t loop_start, loop_end; /* in samples as FFmpeg decodes the file, end exclusive; loop_end < 0: plays once */
} SteamTrack;

/* Any thread: the copy has a version of the sequence named seq ("BGM_30": Sound/BGM/BGM_30.sab). */
int steam_music_has(const char *seq);
/* Game thread, never waits: 1 and *out (a reference, for steam_music_release) once the track is unpacked; 0 while
 * it is being unpacked (asking starts that); -1 when the copy has none or it is damaged. */
int steam_music_get(const char *seq, SteamTrack **out);
void steam_music_release(SteamTrack *t);

/* EGG_STEAMMUSIC_CHECK=1: unpacks every track the copy has, logs each and exits (main thread, at start). */
void steam_music_check(void);
