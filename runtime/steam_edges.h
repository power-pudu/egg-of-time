/* The room edges of the game's Steam release, read from the player's own copy (steam_edges.c). */
#pragma once
#include <stdint.h>

/* At start-up, before the game thread: finds a Steam copy of the game (dir, the folder set in the menu, else
 * EGG_STEAM_DIR, else Steam's libraries) and reads its archive's file list. */
void se_init(const char *dir);
/* Any thread: a Steam copy was found. */
int se_available(void);
/* Main thread, with the game running: the Steam copy in dir (its folder or its resources.bin), when none was found
 * yet. 1 once there is one (an earlier one kept: another applies at the next start). */
int se_connect(const char *dir);
/* The Steam copy's folder (NULL without one): where its resources.bin is. */
const char *se_dir(void);
/* The translations the copy holds (0 without one), by their folder's code (en, ja, zh-Hans...). */
int se_language_count(void);
const char *se_language(int i);

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
/* Where each of its pixels came from, with HD art loaded (else NULL): the HD tile (gfxmod_tile; 0 none), its texel
 * x, y as the tile holds it, flips (bit 0 horizontal, bit 1 vertical) and palette row (0 for 8 bits a pixel). */
typedef struct SeSource { uint32_t tile; uint8_t x, y, flips, pal; } SeSource;
const SeSource *se_picture_hd(int32_t code);

/* The Steam release's tile sheets (Game/field/map_bin/cg*.bin), for the graphics dump and mods: how many, and
 * the name of sheet i. Any thread once connected. */
int se_sheet_count(void);
const char *se_sheet_name(int i);
/* A sheet's pixels (malloc'd: a colour index a pixel, w x h; NULL if none) as a mod left them; *bpp8: 8 bits a pixel
 * (indexes into the whole BG palette), else 4 (into a palette row the map gives). */
uint8_t *se_sheet(const char *name, int *w, int *h, int *bpp8);
/* A graphics mod's pixels for the sheet (gfx_mod.c, at start before the game runs). */
void se_sheet_set(const char *name, int w, int h, const uint8_t *px);
/* The 8x8 tile at (x, y) of sheet pixels px (w wide) as the DS keeps tiles, into t (32 bytes, or 64 with bpp8). */
uint8_t *se_sheet_tile_bytes(const uint8_t *px, int w, int x, int y, int bpp8, uint8_t *t);
