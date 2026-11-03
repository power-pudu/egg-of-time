/* Graphics mods (gfx_mod.c): PNGs in mods/graphics, at the dump's paths, replacing the game's graphics. */
#pragma once

#include <stdint.h>

/* At start, after the game is built and before it runs (while Mods is on): applies the same-size images and
 * edited palettes to the ROM in memory, and loads the larger (HD) ones. Host thread. */
void gfxmod_load(void);

/* HD art for one 8x8 tile: scale * 8 pixels square, unflipped; indexed (palette indexes as the tile's own: 0-15
 * in its row, or 0-255; 0 transparent) or RGBA (ABGR8888, alpha 0 transparent). */
typedef struct GfxHdTile { int scale, indexed, bpp8; const void *px; } GfxHdTile;

/* The scale HD frames are drawn at: the largest of the HD tiles (0: none loaded). Any thread. */
int gfxmod_hd_scale(void);
/* The HD tile replacing the native tile with these bytes (32, or 64 at 8 bits per pixel); 0 if none. Any thread
 * (the table does not change after gfxmod_load). */
uint32_t gfxmod_find(const uint8_t *tile, int bpp8);
const GfxHdTile *gfxmod_tile(uint32_t n);
