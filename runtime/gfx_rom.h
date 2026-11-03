/* The game's graphics in the ROM (gfx_rom.c): its files, their formats decoded, and how their tiles make up the
 * images the graphics dump writes (gfx_dump.c) and graphics mods replace (gfx_mod.c). One thread at a time. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

typedef struct GfxFile { char path[128]; uint32_t id, offset, size; } GfxFile; /* id: its FAT entry */
/* A file's contents, decompressed until a known format shows (data NULL: none); `levels` LZ10 levels were
 * undone. Free with gfx_close. */
typedef struct GfxBlob { const uint8_t *data; uint32_t size; uint8_t *owned; int levels; } GfxBlob;
typedef struct GfxPalette { SDL_Color c[256]; const GfxFile *from; uint32_t colors; } GfxPalette; /* from: NULL = grey */
typedef struct GfxTiles { const uint8_t *data; uint32_t size, offset; int bpp8; } GfxTiles; /* offset: in the blob */
/* A tile placed in an image: its bytes at `off` in the sheet, drawn at (x, y), flipped, in palette row `row`. */
typedef struct GfxPlace { uint32_t off; int x, y, bpp8, hflip, vflip, row; } GfxPlace;
/* An image as its tiles, in drawing order (later ones on top). */
typedef struct GfxLayout { int w, h, bpp8, count; GfxPlace *tiles; } GfxLayout;

uint32_t gfx_le16(const uint8_t *p);
uint32_t gfx_le32(const uint8_t *p);

/* Lists the ROM's files (rt_rom.data, from its FNT and FAT); returns how many. */
int gfx_list_files(void);
const GfxFile *gfx_file(int i);
const GfxFile *gfx_find(const char *path);
int gfx_ends_with(const char *s, const char *end);

GfxBlob gfx_open(const GfxFile *f);
void gfx_close(GfxBlob *b);
uint8_t *gfx_lz10(const uint8_t *src, uint32_t n, uint32_t *out_size);
/* data compressed as LZ10 (malloc'd). */
uint8_t *gfx_lz10_compress(const uint8_t *data, uint32_t n, uint32_t *out_size);

int gfx_read_palette(const GfxFile *f, GfxPalette *p);
/* The palette of the sheet or screen at `path`: the file of the same name, else the only one in its folder,
 * else a grey ramp. */
void gfx_palette_for(const char *path, int bpp8, GfxPalette *p);
/* Where in a palette file (RLCN, NCL; never compressed here) its colours start, and how many; 0 if none. */
int gfx_palette_colors(const GfxFile *f, uint32_t *offset, uint32_t *count);

int gfx_read_tiles(const GfxBlob *b, GfxTiles *t);
int gfx_tile_px(const GfxTiles *t, uint32_t off, int bpp8, int x, int y); /* 0 outside the sheet */

/* Layouts (free l->tiles): a sheet 16 tiles wide; cell `cell` of a .NCER (0 when it is empty or too large);
 * a _nsc.bin screen over tiles of bpp8. */
int gfx_sheet_layout(const GfxTiles *t, GfxLayout *l);
uint32_t gfx_cell_count(const GfxBlob *ncer);
int gfx_cell_layout(const GfxBlob *ncer, uint32_t cell, GfxLayout *l);
int gfx_screen_layout(const GfxBlob *nsc, int bpp8, GfxLayout *l);
/* The layout's pixels as palette indexes (w x h bytes, 0 transparent; row * 16 + index for 4 bits per pixel). */
void gfx_draw(const GfxLayout *l, const GfxTiles *t, uint8_t *px);

/* A message font (.fnt; its format: gfx_rom.c), and its glyphs as a sheet: GFX_FONT_COLUMNS glyphs to a row, in
 * cells of cell_w (the widest glyph's rows) x height pixels. */
typedef struct GfxFont { const uint8_t *data; uint32_t size; int height, bpp, count, cell_w; } GfxFont;
#define GFX_FONT_TABLE 0x10C /* the glyph offsets */
#define GFX_FONT_COLUMNS 16
int gfx_read_font(const GfxBlob *b, GfxFont *f);
/* Glyph g's record: advance width, bytes per row, the rows; NULL when it has none. */
const uint8_t *gfx_font_glyph(const GfxFont *f, int g);
int gfx_font_px(const GfxFont *f, const uint8_t *glyph, int x, int y); /* 0 outside its rows */
/* The font's colours by pixel value: from the only palette in its folder, else in the folder above
 * (msg/font.NCLR for msg/big/), in the order text shows them (1 white letter, 3 black shadow). */
void gfx_font_palette(const char *path, GfxPalette *p);
void gfx_font_size(const GfxFont *f, int *w, int *h);
void gfx_font_draw(const GfxFont *f, uint8_t *px); /* w x h pixel values (0 transparent), px zeroed */
