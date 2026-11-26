/* The room edges of the game's Steam release, read from the player's own copy (steam_edges.c). */
#pragma once
#include <stdint.h>

/* At start-up, before the game thread: finds a Steam copy of the game (EGG_STEAM_DIR, else Steam's libraries) and
 * reads its archive's file list. */
void se_init(void);
/* Any thread: a Steam copy was found. */
int se_available(void);

/* Game thread, each field frame: the room on screen. Its edges are built once the room has held for a few frames
 * (its tiles in VRAM): location (the game's id), the room's bounds left..right (metatile columns, inclusive), the
 * room's metatile table in RAM (layers 0-1, 2x2 tiles of 3 bytes) and BG2's tile data in VRAM (the room's tiles).
 * Returns 1 while edges are ready for it. */
int se_room(int location, int left, int right, uint32_t metatiles, uint32_t chr);

/* The world columns from the edges' first cell to their last, x0..x1-1 (pixels; the room's may lie between them). */
void se_span(int *x0, int *x1);

/* What target BG bg (0-3, as the field lays out the room's layers) shows at world pixel (x, y): SE_OUT where the
 * edges have nothing to say (the room itself, beyond their columns or rows), else 0 (transparent), a map entry
 * (SE_ENTRY | tile, flips, palette as in the BG's map: drawn from the BG's own tiles) or a picture of its own
 * (SE_PICTURE | index for se_picture). */
enum { SE_OUT = -1 };
#define SE_ENTRY 0x10000
#define SE_PICTURE 0x20000
int32_t se_at(int bg, int x, int y);
/* An 8x8 picture's colour indexes, rows of 8 (palette * 16 + colour, 0 transparent; into the BG palette). */
const uint8_t *se_picture(int32_t code);
