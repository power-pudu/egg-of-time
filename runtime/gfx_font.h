/* Font mods (gfx_font.c): HD message fonts and TrueType / OpenType fonts, drawn where the game draws its letters. */
#pragma once

#include <stdint.h>

#include "gfx_mod.h"

/* At start, from gfxmod_load (host thread, before the game runs): */
/* the HD glyphs of the font file at `path` (e.g. "msg/big/msgcmn.fnt"): px holds the dump's glyph sheet at
 * `scale` times its size, pixel values 0-3 (taken over). */
void gfxfont_set_hd(const char *path, int scale, uint8_t *px);
/* a .ttf / .otf file for the font at `path`; 0 if it cannot be read. */
int gfxfont_set_ttf(const char *path, const char *file);
/* a character drawn with glyph g of the font at `path` (the .txt's third column; Unicode code point). */
void gfxfont_set_char(const char *path, int g, uint32_t c);
/* the default height of the font file's letters for the font at `path` (its folder's font.ini), in half pixels. */
void gfxfont_set_size(const char *path, int half_px);

/* A font file is loaded for the big font (small: the small one; System Settings shows its size). Any thread. */
int gfxfont_has_ttf(int small);
/* The scale HD letters are drawn at (0: none). Any thread. */
int gfxfont_hd_scale(void);
/* The scale the screens are drawn at now: TrueType letters are drawn for it. Host thread. */
void gfxfont_view_scale(int scale);

/* HD tiles of letters, numbered from GFXFONT_TILES (gfxmod_find, gfxmod_tile). Game thread, but for gfxfont_tile's
 * pixels, which stay readable. */
#define GFXFONT_TILES 0x400000u
uint32_t gfxfont_find(const uint8_t *tile);
const GfxHdTile *gfxfont_tile(uint32_t n);
