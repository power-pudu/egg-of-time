/* Font mods (gfx_font.c): HD message fonts and TrueType / OpenType fonts, drawn where the game draws its letters. */
#pragma once

#include <stdint.h>

#include "gfx_mod.h"
#include "recomp.h"

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

/* Letters past the fonts' own (the Steam translations, steam_text.c), game thread: glyph numbers from
 * GFXFONT_EXTRA up, which the game passes to its glyph routines like any other, drawn from TrueType. */
#define GFXFONT_EXTRA 0x1000 /* past every version's glyphs (the Japanese big font has 1501) */
/* a .ttf / .otf / .ttc (its first face) to draw them from, after the font mod's own; 0 if it cannot be read. */
int gfxfont_fallback(const char *file);
/* the glyph number for character c in the big font (small: the small one): c itself from 0x1000 up, 0xE000 + c
 * below (at most 0xFFFD: three bytes in the game's text); -1 when no font has it. */
int gfxfont_extra(int small, uint32_t c);
/* forgets every extra letter and fallback font. */
void gfxfont_extra_reset(void);
/* Any thread: extra letters are in use (their HD art is drawn with Mods off too). */
int gfxfont_extras(void);

/* A font file is loaded for the big font (small: the small one; System Settings shows its size). Any thread. */
int gfxfont_has_ttf(int small);
/* The scale HD letters are drawn at (0: none). Any thread. */
int gfxfont_hd_scale(void);
/* The scale the screens are drawn at now: TrueType letters are drawn for it. Host thread. */
void gfxfont_view_scale(int scale);

/* Draws UTF-8 text with the game's glyph routine (main 0x0203d518, through the font mods): font slot as the game
 * passes it (the address holding the font file's), into the 4 bpp buffer buf (wt tiles wide) from (x, y) in colour;
 * wider than the buffer, its spaces narrowed to fit. 0, and nothing drawn, if the font has no glyph for one of its
 * characters or it cannot fit. Game thread. */
int gfxfont_game_text(CPU *cpu, uint32_t slot, uint32_t buf, int wt, const char *text, int x, int y, int colour);

/* HD tiles of letters, numbered from GFXFONT_TILES (gfxmod_find, gfxmod_tile). Game thread, but for gfxfont_tile's
 * pixels, which stay readable. */
#define GFXFONT_TILES 0x400000u
uint32_t gfxfont_find(const uint8_t *tile);
const GfxHdTile *gfxfont_tile(uint32_t n);
