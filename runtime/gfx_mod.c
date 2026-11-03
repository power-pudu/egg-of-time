/* Graphics mods: PNG files in mods/graphics/sheets, frames and screens, at the paths the graphics dump writes
 * (gfx_dump.c), replace the game's graphics. Read once at start while Mods is on (a change needs a restart).
 *
 * An image matches what it replaces in the dump at 1 to 8 times its size (both sides by the same whole number):
 *   - Same size: its pixels go into the game's own tiles in the ROM in memory (rt_rom.data), so it shows
 *     everywhere, the one-screen menus included. The changed files are compressed again (LZ10, as many times as
 *     they were) and written back in place, or after the ROM's end when they grew, with their FAT entries moved.
 *   - Larger (HD): each 8x8 tile of the image becomes HD art for the native tile it was cut from, found again by
 *     that tile's bytes wherever the game shows it (gfxmod_find, video.c), drawn at the HD scale (display.c).
 * Each image pixel belongs to the top-most tile covering it (the first OAM entry of a frame); pixels a tile has
 * under another keep that tile's own. An indexed image keeps palette indexes, its HD art then takes the game's
 * live palette (palette animations and fades go on); an RGBA image's colours are matched to the nearest of the
 * tile's palette row (same size) or kept as they are (HD). Fully transparent native tiles cannot be told apart,
 * so they are never replaced. An indexed image whose palette differs from the game's (a sheet with a palette file
 * of its own: see palettes.txt in the dump) changes that palette file's colours. Where images overlap, the one
 * read later (in path order) wins.
 *
 * Fonts (fonts/<file>.png and .txt, the dump's) change the message font files: glyph pixels at the same size only
 * (text draws glyphs into tiles at run time, so there are no font tiles to give HD art), advance widths, and new
 * glyphs where the font has none. The font file is rebuilt.
 *
 * Images are read in two passes: palettes and same-size images first, then the HD ones, keyed by the tiles as the
 * first pass left them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx_mod.h"
#include "gfx_rom.h"
#include "rt.h"

#define SCALE_MAX 8

/* ---- the HD tile table ---- */

typedef struct Entry { GfxHdTile hd; uint8_t bytes[64]; int bpp8; uint64_t hash; } Entry;
static Entry *entries; /* [0] unused: tile number 0 is none */
static uint32_t entry_count, entry_cap, *slots, slot_mask;
static int hd_scale;

static uint64_t tile_hash(const uint8_t *t, int bpp8) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)bpp8;
    for (int i = 0; i < (bpp8 ? 64 : 32); i++) h = (h ^ t[i]) * 1099511628211ull;
    return h;
}

static uint32_t lookup(const uint8_t *t, int bpp8, uint64_t h) {
    if (!slots) return 0;
    for (uint32_t s = (uint32_t)h & slot_mask;; s = (s + 1) & slot_mask) {
        uint32_t n = slots[s];
        if (!n) return 0;
        const Entry *e = &entries[n];
        if (e->hash == h && e->bpp8 == bpp8 && !memcmp(e->bytes, t, bpp8 ? 64 : 32)) return n;
    }
}

static void rebuild_slots(void) {
    uint32_t size = 64;
    while (size < entry_count * 2) size *= 2;
    free(slots);
    slots = calloc(size, sizeof *slots);
    slot_mask = size - 1;
    for (uint32_t n = 1; n < entry_count; n++) {
        uint32_t s = (uint32_t)entries[n].hash & slot_mask;
        while (slots[s]) s = (s + 1) & slot_mask;
        slots[s] = n;
    }
}

/* Sets the HD art of a native tile, replacing earlier art for it; px is taken over. */
static int add_hd(const uint8_t *t, int bpp8, int scale, int indexed, void *px) {
    uint64_t h = tile_hash(t, bpp8);
    uint32_t n = lookup(t, bpp8, h);
    if (n) {
        free((void *)entries[n].hd.px);
        entries[n].hd = (GfxHdTile){scale, indexed, bpp8, px};
        return 0;
    }
    if (entry_count == entry_cap) {
        entry_cap = entry_cap ? entry_cap * 2 : 1024;
        entries = realloc(entries, entry_cap * sizeof *entries);
    }
    if (!entry_count) entry_count = 1; /* number 0: none */
    Entry *e = &entries[entry_count++];
    *e = (Entry){{scale, indexed, bpp8, px}, {0}, bpp8, h};
    memcpy(e->bytes, t, bpp8 ? 64 : 32);
    if (entry_count * 2 > slot_mask) rebuild_slots();
    else {
        uint32_t s = (uint32_t)h & slot_mask;
        while (slots[s]) s = (s + 1) & slot_mask;
        slots[s] = entry_count - 1;
    }
    return 1;
}

int gfxmod_hd_scale(void) { return hd_scale; }

uint32_t gfxmod_find(const uint8_t *t, int bpp8) { return entry_count > 1 ? lookup(t, bpp8, tile_hash(t, bpp8)) : 0; }

const GfxHdTile *gfxmod_tile(uint32_t n) { return n && n < entry_count ? &entries[n].hd : NULL; }

/* ---- the images ---- */

typedef struct Item {
    char rel[512];          /* sheets/..., frames/..., screens/... */
    const GfxFile *sheet;   /* the tile file */
    GfxBlob sheet_blob, aux_blob; /* aux: the .NCER or _nsc.bin */
    GfxTiles tiles;
    GfxLayout layout;
    GfxPalette pal;
    int *owner;             /* by native pixel: the top-most tile covering it (-1: none) */
} Item;

static void item_free(Item *it) {
    gfx_close(&it->sheet_blob);
    gfx_close(&it->aux_blob);
    free(it->layout.tiles);
    free(it->owner);
    memset(it, 0, sizeof *it);
}

/* The image at `rel` (under mods/graphics/) as the dump drew it. */
static int resolve(const char *rel, Item *it) {
    memset(it, 0, sizeof *it);
    snprintf(it->rel, sizeof it->rel, "%s", rel);
    char path[512];
    size_t n = strlen(rel);
    if (n < 4 || strcmp(rel + n - 4, ".png")) return 0;
    if (!strncmp(rel, "sheets/", 7)) {
        snprintf(path, sizeof path, "%.*s", (int)(n - 4 - 7), rel + 7);
        if (!(it->sheet = gfx_find(path))) return 0;
        it->sheet_blob = gfx_open(it->sheet);
        if (!gfx_read_tiles(&it->sheet_blob, &it->tiles) || !gfx_sheet_layout(&it->tiles, &it->layout)) return 0;
        gfx_palette_for(it->sheet->path, it->tiles.bpp8, &it->pal);
    } else if (!strncmp(rel, "frames/", 7)) {
        const char *slash = strrchr(rel, '/');
        char *end;
        unsigned long cell = strtoul(slash + 1, &end, 10);
        if (slash == rel + 6 || end != rel + n - 4) return 0;
        snprintf(path, sizeof path, "%.*s.NCER", (int)(slash - rel - 7), rel + 7);
        const GfxFile *cf = gfx_find(path);
        snprintf(path, sizeof path, "%.*s.NCGR", (int)(slash - rel - 7), rel + 7);
        if (!cf || !(it->sheet = gfx_find(path))) return 0;
        it->sheet_blob = gfx_open(it->sheet);
        it->aux_blob = gfx_open(cf);
        if (!gfx_read_tiles(&it->sheet_blob, &it->tiles) || !gfx_cell_layout(&it->aux_blob, (uint32_t)cell, &it->layout)) return 0;
        gfx_palette_for(it->sheet->path, it->tiles.bpp8, &it->pal);
    } else if (!strncmp(rel, "screens/", 8)) {
        snprintf(path, sizeof path, "%.*s", (int)(n - 4 - 8), rel + 8);
        const GfxFile *sf = gfx_find(path);
        if (!sf || !gfx_ends_with(path, "_nsc.bin")) return 0;
        snprintf(path, sizeof path, "%.*s_ncg.bin", (int)(strlen(sf->path) - 8), sf->path);
        if (!(it->sheet = gfx_find(path))) return 0;
        it->sheet_blob = gfx_open(it->sheet);
        it->aux_blob = gfx_open(sf);
        if (!gfx_read_tiles(&it->sheet_blob, &it->tiles) || !gfx_screen_layout(&it->aux_blob, it->tiles.bpp8, &it->layout)) return 0;
        gfx_palette_for(sf->path, it->tiles.bpp8, &it->pal);
    } else {
        return 0;
    }
    const GfxLayout *l = &it->layout;
    it->owner = malloc((size_t)l->w * l->h * sizeof *it->owner);
    for (int i = 0; i < l->w * l->h; i++) it->owner[i] = -1;
    for (int i = 0; i < l->count; i++) {
        const GfxPlace *p = &l->tiles[i];
        for (int y = SDL_max(p->y, 0); y < SDL_min(p->y + 8, l->h); y++) {
            for (int x = SDL_max(p->x, 0); x < SDL_min(p->x + 8, l->w); x++) it->owner[y * l->w + x] = i;
        }
    }
    return 1;
}

/* A loaded PNG: its palette indexes (indexed images) or ABGR8888 colours. */
typedef struct Png { int w, h, indexed, colors; uint8_t *idx; uint32_t *rgba; SDL_Color pal[256]; } Png;

static void png_free(Png *p) {
    free(p->idx);
    free(p->rgba);
    memset(p, 0, sizeof *p);
}

static int png_load(const char *path, Png *png) {
    memset(png, 0, sizeof *png);
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) return 0;
    png->w = s->w, png->h = s->h;
    SDL_Palette *sp = SDL_GetSurfacePalette(s);
    if (SDL_ISPIXELFORMAT_INDEXED(s->format) && sp) {
        int bits = SDL_BITSPERPIXEL(s->format), lsb = SDL_PIXELORDER(s->format) == SDL_BITMAPORDER_4321;
        png->indexed = 1;
        png->colors = SDL_min(sp->ncolors, 256);
        memcpy(png->pal, sp->colors, (size_t)png->colors * sizeof *png->pal);
        png->idx = malloc((size_t)s->w * s->h);
        for (int y = 0; y < s->h; y++) {
            const uint8_t *row = (const uint8_t *)s->pixels + y * s->pitch;
            for (int x = 0; x < s->w; x++) {
                int bit = x * bits, shift = lsb ? bit % 8 : 8 - bits - bit % 8;
                png->idx[y * s->w + x] = bits == 8 ? row[x] : (uint8_t)((row[bit / 8] >> shift) & ((1 << bits) - 1));
            }
        }
    } else {
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
        if (!c) {
            SDL_DestroySurface(s);
            return 0;
        }
        png->rgba = malloc((size_t)s->w * s->h * 4);
        for (int y = 0; y < s->h; y++) memcpy(png->rgba + y * s->w, (const uint8_t *)c->pixels + y * c->pitch, (size_t)s->w * 4);
        SDL_DestroySurface(c);
    }
    SDL_DestroySurface(s);
    return 1;
}

/* The colour nearest to rgb among palette entries from..to-1. */
static int nearest(const GfxPalette *p, int from, int to, int r, int g, int b) {
    int best = from, best_d = INT32_MAX;
    for (int i = from; i < to; i++) {
        int dr = p->c[i].r - r, dg = p->c[i].g - g, db = p->c[i].b - b, d = dr * dr + dg * dg + db * db;
        if (d < best_d) best = i, best_d = d;
    }
    return best;
}

/* A pixel of the image as a value of tile `p` (0-15 in its row, or 0-255; 0 transparent). */
static int tile_value(const Png *png, int i, const GfxPlace *p, const GfxPalette *pal) {
    int row = p->bpp8 ? 0 : p->row * 16, end = p->bpp8 ? 256 : row + 16;
    if (png->indexed) {
        int v = png->idx[i];
        if (p->bpp8 || !(v & 15) || v >> 4 == p->row) return p->bpp8 ? v : v & 15;
        SDL_Color c = v < png->colors ? png->pal[v] : (SDL_Color){0, 0, 0, 255};
        return nearest(pal, row + 1, end, c.r, c.g, c.b) - row;
    }
    uint32_t c = png->rgba[i];
    if ((c >> 24) < 128) return 0;
    return nearest(pal, row + 1, end, c & 0xFF, c >> 8 & 0xFF, c >> 16 & 0xFF) - row;
}

/* ---- ROM edits ---- */

typedef struct Edit { uint32_t id; uint8_t *data; uint32_t size; int levels, dirty; } Edit; /* a file decompressed */
static Edit *edits;
static int edit_count;

static Edit *edit_for(const GfxFile *f) {
    for (int i = 0; i < edit_count; i++) {
        if (edits[i].id == f->id) return &edits[i];
    }
    GfxBlob b = gfx_open(f);
    if (!b.data) return NULL;
    edits = realloc(edits, (size_t)(edit_count + 1) * sizeof *edits);
    Edit *e = &edits[edit_count++];
    *e = (Edit){f->id, malloc(b.size), b.size, b.levels, 0};
    memcpy(e->data, b.data, b.size);
    gfx_close(&b);
    return e;
}

static void set_tile_px(uint8_t *t, int bpp8, int x, int y, int v) {
    if (bpp8) t[y * 8 + x] = (uint8_t)v;
    else {
        uint8_t *b = &t[y * 4 + x / 2];
        *b = x & 1 ? (uint8_t)((*b & 0x0F) | v << 4) : (uint8_t)((*b & 0xF0) | v);
    }
}

/* Writes the changed files back into the ROM, moving those that grew after its end. */
static int commit_edits(void) {
    int written = 0;
    uint32_t fat = gfx_le32(rt_rom.data + 0x48);
    for (int i = 0; i < edit_count; i++) {
        Edit *e = &edits[i];
        if (!e->dirty) continue;
        uint8_t *data = e->data;
        uint32_t size = e->size;
        for (int k = 0; k < e->levels; k++) {
            uint32_t n;
            uint8_t *c = gfx_lz10_compress(data, size, &n);
            if (data != e->data) free(data);
            data = c, size = n;
        }
        uint8_t *at = rt_rom.data + fat + e->id * 8;
        uint32_t start = gfx_le32(at), end = gfx_le32(at + 4);
        if (size > end - start) {
            start = (uint32_t)((rt_rom.size + 0x1FF) & ~(size_t)0x1FF);
            uint8_t *rom = realloc(rt_rom.data, (size_t)start + size);
            if (!rom) {
                rt_log("gfxmod: out of memory for a grown file");
                if (data != e->data) free(data);
                continue;
            }
            memset(rom + rt_rom.size, 0xFF, start - rt_rom.size);
            rt_rom.data = rom, rt_rom.size = (size_t)start + size;
            at = rt_rom.data + fat + e->id * 8;
        }
        memcpy(rt_rom.data + start, data, size);
        uint32_t new_end = start + size;
        memcpy(at, &start, 4);
        memcpy(at + 4, &new_end, 4);
        if (data != e->data) free(data);
        written++;
    }
    for (int i = 0; i < edit_count; i++) free(edits[i].data);
    free(edits);
    edits = NULL;
    edit_count = 0;
    return written;
}

/* An indexed image's palette over the game's: the palette file's colours where they differ. */
static int apply_palette(const Png *png, const Item *it) {
    uint32_t at, count;
    if (!png->indexed || !it->pal.from || !gfx_palette_colors(it->pal.from, &at, &count)) return 0;
    int changed = 0;
    for (int i = 0; i < png->colors && (uint32_t)i < count; i++) {
        SDL_Color c = png->pal[i];
        if ((it->layout.bpp8 ? i == 0 : (i & 15) == 0) || (c.r == it->pal.c[i].r && c.g == it->pal.c[i].g && c.b == it->pal.c[i].b)) continue;
        uint16_t v = (uint16_t)((c.r >> 3) | (c.g >> 3) << 5 | (c.b >> 3) << 10);
        memcpy(rt_rom.data + at + i * 2, &v, 2);
        changed++;
    }
    return changed;
}

static int same_size(const Png *png, Item *it) {
    Edit *e = edit_for(it->sheet);
    if (!e) return 0;
    uint8_t *tiles = e->data + it->tiles.offset;
    const GfxLayout *l = &it->layout;
    int changed = 0;
    for (int i = 0; i < l->count; i++) {
        const GfxPlace *p = &l->tiles[i];
        if (p->off + (p->bpp8 ? 64u : 32u) > it->tiles.size) continue;
        uint8_t *t = tiles + p->off;
        int was = 0;
        for (int ty = 0; ty < 8; ty++) {
            for (int tx = 0; tx < 8; tx++) {
                int x = p->x + tx, y = p->y + ty;
                if (x < 0 || y < 0 || x >= l->w || y >= l->h || it->owner[y * l->w + x] != i) continue;
                int u = p->hflip ? 7 - tx : tx, v = p->vflip ? 7 - ty : ty;
                int old = gfx_tile_px(&(GfxTiles){tiles, it->tiles.size, 0, p->bpp8}, p->off, p->bpp8, u, v);
                int now = tile_value(png, y * png->w + x, p, &it->pal);
                if (now != old) set_tile_px(t, p->bpp8, u, v, now), was = 1;
            }
        }
        changed += was;
    }
    if (changed) e->dirty = 1;
    return changed;
}

static int hd_tiles(const Png *png, const Item *it, int s) {
    const GfxLayout *l = &it->layout;
    int added = 0, n = 8 * s;
    for (int i = 0; i < l->count; i++) {
        const GfxPlace *p = &l->tiles[i];
        int bytes = p->bpp8 ? 64 : 32;
        if (p->off + (uint32_t)bytes > it->tiles.size) continue;
        const uint8_t *t = it->tiles.data + p->off;
        int blank = 1, visible = 0;
        for (int k = 0; k < bytes; k++) blank &= !t[k];
        for (int k = 0; k < 64 && !blank; k++) {
            int x = p->x + k % 8, y = p->y + k / 8;
            visible |= x >= 0 && y >= 0 && x < l->w && y < l->h && it->owner[y * l->w + x] == i;
        }
        if (blank || !visible) continue;
        void *px = malloc((size_t)n * n * (png->indexed ? 1 : 4));
        uint8_t *ix = px;
        uint32_t *cx = px;
        for (int v = 0; v < n; v++) {
            for (int u = 0; u < n; u++) {
                int nat = gfx_tile_px(&it->tiles, p->off, p->bpp8, u / s, v / s); /* the native texel, upscaled */
                int x = p->x * s + (p->hflip ? n - 1 - u : u), y = p->y * s + (p->vflip ? n - 1 - v : v);
                int own = x >= 0 && y >= 0 && x < png->w && y < png->h && it->owner[(y / s) * l->w + x / s] == i;
                if (png->indexed) {
                    int val = own ? png->idx[y * png->w + x] : nat;
                    ix[v * n + u] = (uint8_t)(p->bpp8 || !own ? val : val & 15);
                } else if (own) {
                    uint32_t c = png->rgba[y * png->w + x];
                    cx[v * n + u] = (c >> 24) < 128 ? 0 : c | 0xFF000000u;
                } else {
                    SDL_Color c = it->pal.c[p->bpp8 ? nat : p->row * 16 + nat];
                    cx[v * n + u] = nat ? 0xFF000000u | (uint32_t)c.b << 16 | (uint32_t)c.g << 8 | c.r : 0;
                }
            }
        }
        added += add_hd(t, p->bpp8, s, png->indexed, px);
    }
    return added;
}

/* ---- fonts ---- */

/* A font being edited: each glyph's advance width (-1: none), bytes per row as it was (some glyphs have more than
 * their pixels need) and pixels in its cell. */
typedef struct FontEdit { GfxFont font; int *width, *bpr; uint8_t *px; } FontEdit;

static int font_begin(const GfxFile *f, Edit **e, FontEdit *fe) {
    if (!(*e = edit_for(f))) return 0;
    GfxBlob b = {(*e)->data, (*e)->size, NULL, 0};
    if (!gfx_read_font(&b, &fe->font)) return 0;
    const GfxFont *font = &fe->font;
    fe->width = malloc((size_t)font->count * sizeof *fe->width);
    fe->bpr = malloc((size_t)font->count * sizeof *fe->bpr);
    fe->px = calloc((size_t)font->count, (size_t)font->cell_w * font->height);
    for (int g = 0; g < font->count; g++) {
        const uint8_t *r = gfx_font_glyph(font, g);
        fe->width[g] = r ? r[0] : -1;
        fe->bpr[g] = r ? r[1] : 0;
        for (int y = 0; r && y < font->height; y++) {
            for (int x = 0; x < font->cell_w; x++) fe->px[(g * font->height + y) * font->cell_w + x] = (uint8_t)gfx_font_px(font, r, x, y);
        }
    }
    return 1;
}

/* Rebuilds the font file from the edit (each glyph's rows as wide as they were, or as its pixels need); 1 if it
 * changed. */
static int font_end(Edit *e, FontEdit *fe) {
    const GfxFont *f = &fe->font;
    int per_byte = 8 / f->bpp;
    uint32_t cap = GFX_FONT_TABLE + 4u * f->count, size = cap;
    for (int g = 0; g < f->count; g++) cap += 2 + (uint32_t)f->height * ((f->cell_w + per_byte - 1) / per_byte);
    uint8_t *out = calloc(cap, 1);
    memcpy(out, f->data, GFX_FONT_TABLE);
    for (int g = 0; g < f->count; g++) {
        if (fe->width[g] < 0) continue;
        const uint8_t *px = &fe->px[(size_t)g * f->height * f->cell_w];
        int used = 0;
        for (int i = 0; i < f->height * f->cell_w; i++) used = px[i] ? SDL_max(used, i % f->cell_w + 1) : used;
        int bpr = SDL_max(fe->bpr[g], (used + per_byte - 1) / per_byte);
        memcpy(out + GFX_FONT_TABLE + 4 * g, &size, 4);
        uint8_t *r = out + size;
        r[0] = (uint8_t)fe->width[g], r[1] = (uint8_t)bpr;
        for (int y = 0; y < f->height; y++) {
            for (int x = 0; x < used; x++) r[2 + y * bpr + x / per_byte] |= (uint8_t)(px[y * f->cell_w + x] << x % per_byte * f->bpp);
        }
        size += 2 + (uint32_t)f->height * bpr;
    }
    free(fe->width);
    free(fe->bpr);
    free(fe->px);
    if (size == e->size && !memcmp(out, e->data, size)) {
        free(out);
        return 0;
    }
    free(e->data);
    e->data = out, e->size = size, e->dirty = 1;
    return 1;
}

/* fonts/<file>.png: the glyphs' pixels, at the dump's size. Indexed: pixel values as they are (others: the nearest
 * colour of the font's); RGBA: the nearest colour. Returns the glyphs changed. */
static int font_png(const char *rel, const char *path, const GfxFile *f) {
    Edit *e;
    FontEdit fe;
    if (!font_begin(f, &e, &fe)) return 0;
    const GfxFont *font = &fe.font;
    int w, h, top = (1 << font->bpp) - 1, changed = 0;
    gfx_font_size(font, &w, &h);
    Png png;
    if (!png_load(path, &png)) {
        rt_log("gfxmod: %s: cannot read: %s", rel, SDL_GetError());
    } else if (png.w != w || png.h != h) {
        rt_log("gfxmod: %s: %dx%d, not %dx%d", rel, png.w, png.h, w, h);
    } else {
        GfxPalette pal;
        gfx_font_palette(f->path, &pal);
        for (int g = 0; g < font->count; g++) {
            int x0 = g % GFX_FONT_COLUMNS * font->cell_w, y0 = g / GFX_FONT_COLUMNS * font->height, was = 0, any = 0;
            for (int y = 0; y < font->height; y++) {
                for (int x = 0; x < font->cell_w; x++) {
                    int i = (y0 + y) * w + x0 + x, v;
                    if (png.indexed) {
                        v = png.idx[i];
                        SDL_Color c = v < png.colors ? png.pal[v] : (SDL_Color){0, 0, 0, 255};
                        if (v > top) v = c.a < 128 ? 0 : nearest(&pal, 1, top + 1, c.r, c.g, c.b);
                    } else {
                        uint32_t c = png.rgba[i];
                        v = (c >> 24) < 128 ? 0 : nearest(&pal, 1, top + 1, c & 0xFF, c >> 8 & 0xFF, c >> 16 & 0xFF);
                    }
                    uint8_t *p = &fe.px[(g * font->height + y) * font->cell_w + x];
                    was |= *p != v, any |= v;
                    *p = (uint8_t)v;
                }
            }
            if (fe.width[g] < 0 && any) fe.width[g] = font->cell_w; /* a new glyph: as wide as its cell until the .txt says */
            changed += was;
        }
    }
    png_free(&png);
    font_end(e, &fe);
    return changed;
}

/* fonts/<file>.txt: lines of glyph number and advance width (- : unchanged), as the dump writes them. Returns the
 * widths changed. */
static int font_txt(const char *rel, const char *path, const GfxFile *f) {
    size_t n;
    char *text = SDL_LoadFile(path, &n);
    if (!text) {
        rt_log("gfxmod: %s: cannot read: %s", rel, SDL_GetError());
        return 0;
    }
    Edit *e;
    FontEdit fe;
    int changed = 0;
    if (font_begin(f, &e, &fe)) {
        for (char *line = text, *next; line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *end;
            long g = strtol(line, &end, 10), wd;
            if (end == line || *line == '#') continue;
            wd = strtol(end, &line, 10);
            if (line == end) continue;
            if (g < 0 || g >= fe.font.count || wd < 0 || wd > 255) {
                rt_log("gfxmod: %s: glyph %ld width %ld: out of range", rel, g, wd);
                continue;
            }
            changed += fe.width[g] != wd;
            fe.width[g] = (int)wd;
        }
        font_end(e, &fe);
    }
    SDL_free(text);
    return changed;
}

/* ---- loading ---- */

typedef struct List { char **paths; int count, cap, fonts; } List; /* fonts: the .txt files too */

static SDL_EnumerationResult SDLCALL collect(void *ctx, const char *dir, const char *name) {
    List *list = ctx;
    char path[1024];
    snprintf(path, sizeof path, "%s%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(path, collect, list);
    } else if (gfx_ends_with(name, ".png") || gfx_ends_with(name, ".PNG") || (list->fonts && gfx_ends_with(name, ".txt"))) {
        if (list->count == list->cap) {
            list->cap = list->cap ? list->cap * 2 : 256;
            list->paths = realloc(list->paths, (size_t)list->cap * sizeof *list->paths);
        }
        list->paths[list->count++] = SDL_strdup(path);
    }
    return SDL_ENUM_CONTINUE;
}

static int by_path(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The image's place under mods/graphics/, with forward slashes. */
static const char *relative(const char *path, const char *root) {
    static char rel[512];
    snprintf(rel, sizeof rel, "%s", path + strlen(root));
    for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
    size_t n = strlen(rel);
    if (n > 4 && !strcmp(rel + n - 4, ".PNG")) memcpy(rel + n - 4, ".png", 4);
    return rel;
}

void gfxmod_load(void) {
    Uint64 t0 = SDL_GetTicks();
    char root[1024];
    snprintf(root, sizeof root, "%smods/graphics/", rt_data_dir());
    List list = {0};
    static const char *const kinds[] = {"sheets", "frames", "screens", "fonts"};
    for (size_t k = 0; k < SDL_arraysize(kinds); k++) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s%s/", root, kinds[k]);
        list.fonts = !strcmp(kinds[k], "fonts");
        SDL_PathInfo info;
        if (SDL_GetPathInfo(dir, &info) && info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(dir, collect, &list);
    }
    if (!list.count) return;
    qsort(list.paths, (size_t)list.count, sizeof *list.paths, by_path);
    if (!gfx_list_files()) return;
    int *scale = calloc((size_t)list.count, sizeof *scale); /* by image: 0 not usable */
    int images = 0, colours = 0, same_tiles = 0, hd = 0, glyphs = 0, widths = 0;
    Uint64 t1 = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < list.count; i++) {
            if (pass && scale[i] < 2) continue;
            const char *rel = relative(list.paths[i], root);
            if (!strncmp(rel, "fonts/", 6)) {
                if (pass) continue;
                char path[512];
                snprintf(path, sizeof path, "%.*s", (int)strlen(rel) - 4 - 6, rel + 6);
                const GfxFile *f = gfx_ends_with(path, ".fnt") ? gfx_find(path) : NULL;
                if (!f) {
                    rt_log("gfxmod: %s: no such font in the dump", rel);
                } else {
                    images++;
                    if (gfx_ends_with(rel, ".png")) glyphs += font_png(rel, list.paths[i], f);
                    else widths += font_txt(rel, list.paths[i], f);
                }
                continue;
            }
            Item it;
            Png png;
            if (!resolve(rel, &it)) {
                if (!pass) rt_log("gfxmod: %s: no such image in the dump", rel);
                item_free(&it);
                continue;
            }
            if (!png_load(list.paths[i], &png)) {
                rt_log("gfxmod: %s: cannot read: %s", rel, SDL_GetError());
                item_free(&it);
                continue;
            }
            int s = png.w / it.layout.w;
            if (s < 1 || s > SCALE_MAX || png.w != it.layout.w * s || png.h != it.layout.h * s) {
                if (!pass) rt_log("gfxmod: %s: %dx%d, not %dx%d times 1 to %d", rel, png.w, png.h, it.layout.w, it.layout.h, SCALE_MAX);
            } else if (!pass) {
                scale[i] = s;
                images++;
                colours += apply_palette(&png, &it);
                if (s == 1) same_tiles += same_size(&png, &it);
            } else {
                hd += hd_tiles(&png, &it, s);
                hd_scale = SDL_max(hd_scale, s);
            }
            png_free(&png);
            item_free(&it);
        }
        if (!pass) t1 = SDL_GetTicks();
        if (!pass && commit_edits()) gfx_list_files(); /* the files moved */
        if (!pass) t1 = SDL_GetTicks() - t1;
    }
    for (int i = 0; i < list.count; i++) SDL_free(list.paths[i]);
    free(list.paths);
    free(scale);
    rt_log("gfxmod: %d files: %d same-size tiles changed, %d palette colours, %d HD tiles (up to %dx), %d glyphs and "
           "%d glyph widths changed in %.1f s (%.1f s writing the ROM)",
           images, same_tiles, colours, hd, hd_scale, glyphs, widths, (double)(SDL_GetTicks() - t0) / 1000, (double)t1 / 1000);
}
