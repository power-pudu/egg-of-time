/* Font mods: HD message fonts and TrueType / OpenType fonts (mods/graphics/fonts, read by gfx_mod.c).
 *
 * The game draws text letter by letter into tile buffers in RAM, which it copies to VRAM later: main 0x0203d518
 * (font, buffer, width in tiles, glyph, then on the stack x, y and colour) puts one glyph of a message font
 * (msgcmn.fnt, 2 bits per pixel; format: gfx_rom.c) at pixel (x, y) of a buffer of 4-bit tiles laid in rows of
 * `width` tiles, and returns its advance width; main 0x0203d4fc (font, glyph) returns the advance width alone, which
 * every layout of text asks for. A font object's first word points at the font file in RAM. Each glyph row (16
 * pixels at most) is spread over the 3 tiles from x / 8 on; per pixel value: 0 leaves the buffer's pixel, 1 and 2
 * become colours 2c + 2 and 2c + 3 of the call's colour c, 3 (the shadow) is ORed with colour 1 (with c = 6: 15, on
 * every 4th pixel of a tile row). rt_font_draw_glyph and rt_font_advance wrap them while a font mod is loaded.
 *
 * A .ttf / .otf in a font's folder replaces its glyphs that show a character it has: the font's ASCII table, and
 * characters the .txt's third column gives (gfxfont_set_char). Its letters are drawn in the game's style: pixels
 * at least half covered (a quarter at the original size) are the letter, the shadow follows it one pixel right
 * and down, the advance is the font's own plus that pixel. Font size (System Settings) scales them: 100% fits the
 * font's ascent and descent in the line (rows 1 to height - 2), centred there at other sizes. Glyphs it does not
 * replace stay the ROM's.
 *
 * HD letters: what each draw puts in each tile is kept, by the tile's RAM address: the tile's pixels before the
 * first draw and the draws since (glyph, place, colour), started again when the tile no longer holds what the last
 * draw left (the buffer was cleared or written otherwise). After each draw the touched tiles' HD art is composed
 * from them, the HD glyphs (a TrueType font's at the screens' scale, an HD .png's, or the ROM's upscaled) drawn
 * over the tile's pixels by the same rules, and kept in a table of its own keyed on the tile's bytes, which
 * gfxmod_find looks in after the HD images' table: the tile is found again wherever the buffer is copied to. The
 * table holds FT_SLOTS tiles; when it is full, those the screens have not shown for a second are dropped (else the
 * older half). Their pixels are never freed, so a frame being drawn from them (display.c) reads stale art at worst. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "dispatch.h"
#include "display.h"
#include "gfx_font.h"
#include "gfx_rom.h"
#include "rt.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define DRAW_GLYPH GAME_ADDR(0x0203d518u, 0x0203d548u)
#define ADVANCE GAME_ADDR(0x0203d4fcu, 0x0203d52cu)
#define FONTS 4
#define GLYPH_W 16 /* pixels of a glyph row the game draws */
#define HEIGHT_MAX 16
#define SCALE_MAX 8

/* ---- the fonts ---- */

typedef struct Glyph {
    uint32_t gen;                          /* built for this generation of its font (0: not yet) */
    int adv;
    uint8_t native[HEIGHT_MAX][GLYPH_W];   /* pixel values 0-3 */
    uint8_t *hd;                           /* GLYPH_W * scale x height * scale; NULL: no HD */
    int scale;
} Glyph;

typedef struct Font {
    char path[128];
    int height, count, cell_w;
    uint16_t ascii[128];
    uint32_t *chars;               /* by glyph: a character, 0 none */
    uint8_t *png;                  /* HD .png: the glyph sheet at png_scale, values 0-3 */
    int png_scale, png_w;
    uint8_t *ttf;                  /* TrueType file */
    stbtt_fontinfo info;
    int ascent, descent;
    Glyph *glyphs;
    uint32_t gen, key, data;       /* key: what the glyphs were built for; data: the font file in RAM */
} Font;

static Font fonts[FONTS];
static int font_count, any_ttf;
static SDL_AtomicInt view_scale;

static Font *font_for(const char *path) {
    for (int i = 0; i < font_count; i++) {
        if (!strcmp(fonts[i].path, path)) return &fonts[i];
    }
    const GfxFile *file = gfx_find(path);
    if (!file || font_count == FONTS) return NULL;
    GfxBlob b = gfx_open(file);
    GfxFont gf;
    Font *f = NULL;
    if (gfx_read_font(&b, &gf) && gf.height <= HEIGHT_MAX && gf.bpp == 2) {
        f = &fonts[font_count++];
        memset(f, 0, sizeof *f);
        snprintf(f->path, sizeof f->path, "%s", path);
        f->height = gf.height, f->count = gf.count, f->cell_w = gf.cell_w;
        f->chars = calloc((size_t)gf.count, sizeof *f->chars);
        f->glyphs = calloc((size_t)gf.count, sizeof *f->glyphs);
        for (int c = 0; c < 128; c++) f->ascii[c] = (uint16_t)gfx_le16(gf.data + 12 + c * 2);
        for (int c = 32; c < 127; c++) {
            if (f->ascii[c] < gf.count && !f->chars[f->ascii[c]]) f->chars[f->ascii[c]] = (uint32_t)c;
        }
    }
    gfx_close(&b);
    return f;
}

void gfxfont_set_hd(const char *path, int scale, uint8_t *px) {
    Font *f = font_for(path);
    if (!f || scale < 2 || scale > SCALE_MAX) {
        free(px);
        return;
    }
    free(f->png);
    f->png = px, f->png_scale = scale, f->png_w = GFX_FONT_COLUMNS * f->cell_w * scale;
}

int gfxfont_set_ttf(const char *path, const char *file) {
    Font *f = font_for(path);
    size_t n;
    void *data = f ? SDL_LoadFile(file, &n) : NULL;
    if (!data) return 0;
    stbtt_fontinfo info;
    int at = stbtt_GetFontOffsetForIndex(data, 0);
    if (at < 0 || !stbtt_InitFont(&info, data, at)) {
        SDL_free(data);
        return 0;
    }
    SDL_free(f->ttf);
    f->ttf = data, f->info = info;
    int gap;
    stbtt_GetFontVMetrics(&info, &f->ascent, &f->descent, &gap);
    if (f->ascent <= f->descent) f->ascent = f->descent + 1;
    any_ttf = 1;
    return 1;
}

void gfxfont_set_char(const char *path, int g, uint32_t c) {
    Font *f = font_for(path);
    if (f && g >= 0 && g < f->count) f->chars[g] = c;
}

int gfxfont_has_ttf(void) { return any_ttf; }

int gfxfont_hd_scale(void) {
    int s = 0;
    for (int i = 0; i < font_count; i++) s = SDL_max(s, fonts[i].ttf ? SCALE_MAX : fonts[i].png_scale);
    return s;
}

void gfxfont_view_scale(int scale) { SDL_SetAtomicInt(&view_scale, SDL_clamp(scale, 1, SCALE_MAX)); }

static int size_percent(void) { return FONT_SIZE_MIN + FONT_SIZE_STEP * display_setting_get(SET_FONT_SIZE); }

/* The scale a font's HD glyphs are drawn at (0: none). */
static int font_scale(const Font *f) {
    if (f->ttf) {
        int s = SDL_GetAtomicInt(&view_scale);
        return s ? s : 2;
    }
    return f->png_scale;
}

/* A TrueType glyph at `scale`, drawn into px (GLYPH_W * scale x height * scale, values 0-3); its advance. */
static int ttf_glyph(const Font *f, int gi, int scale, uint8_t *px) {
    int h = f->height, w = GLYPH_W * scale, rows = h * scale;
    float k = (float)(h - 2) * (float)size_percent() / 100.0f / (float)(f->ascent - f->descent); /* pixels per unit */
    float base = (1 + (float)(h - 2) / 2 + (float)(f->ascent + f->descent) / 2 * k) * (float)scale; /* the baseline */
    float sc = k * (float)scale, shift = base - floorf(base);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&f->info, gi, sc, sc, 0, shift, &x0, &y0, &x1, &y1);
    uint8_t *letter = calloc((size_t)w, (size_t)rows);
    if (x1 > x0 && y1 > y0) {
        int bw = x1 - x0, bh = y1 - y0;
        uint8_t *bmp = malloc((size_t)bw * bh);
        stbtt_MakeGlyphBitmapSubpixel(&f->info, bmp, bw, bh, bw, sc, sc, 0, shift, gi);
        int top = (int)floorf(base) + y0;
        for (int j = 0; j < bh; j++) {
            for (int i = 0; i < bw; i++) {
                int x = x0 + i, y = top + j;
                if (x >= 0 && y >= 0 && x < w && y < rows && bmp[j * bw + i] >= (scale > 1 ? 128 : 64)) letter[y * w + x] = 1;
            }
        }
        free(bmp);
    }
    /* The shadow: the letter moved by 0 to `scale` texels right and down. */
    uint8_t *right = calloc((size_t)w, (size_t)rows);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < w; x++) {
            for (int i = 0; i <= scale && i <= x && !right[y * w + x]; i++) right[y * w + x] = letter[y * w + x - i];
        }
    }
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < w; x++) {
            int shadow = 0;
            for (int j = 0; j <= scale && j <= y && !shadow; j++) shadow = right[(y - j) * w + x];
            px[y * w + x] = letter[y * w + x] ? 1 : shadow ? 3 : 0;
        }
    }
    free(letter);
    free(right);
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&f->info, gi, &aw, &lsb);
    return SDL_clamp((int)lroundf((float)aw * k) + 1, 1, GLYPH_W);
}

/* Where the HD glyph has pixels the native one lacks (thin strokes, a dot), the native pixel becomes shadow (3), so
 * the tiles under every part of the HD letter change when it is drawn and get its HD art. */
static void cover(Glyph *gl, int h) {
    int s = gl->scale, w = GLYPH_W * s;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < GLYPH_W; x++) {
            int any = 0;
            for (int v = 0; v < s && !any; v++) {
                for (int u = 0; u < s && !any; u++) any = gl->hd[(y * s + v) * w + x * s + u] != 0;
            }
            if (any && !gl->native[y][x]) gl->native[y][x] = 3;
        }
    }
}

/* Glyph g of font f (its file in RAM at f->data), built for the current generation; NULL past its last glyph. */
static Glyph *glyph(Font *f, int g) {
    if (g < 0 || g >= f->count) return NULL;
    Glyph *gl = &f->glyphs[g];
    if (gl->gen == f->gen) return gl;
    int scale = font_scale(f), h = f->height, w = GLYPH_W * scale;
    gl->gen = f->gen;
    gl->scale = scale;
    free(gl->hd);
    gl->hd = scale > 1 ? calloc((size_t)w, (size_t)h * scale) : NULL;
    memset(gl->native, 0, sizeof gl->native);
    int gi = f->ttf && f->chars[g] ? stbtt_FindGlyphIndex(&f->info, (int)f->chars[g]) : 0;
    if (gi) {
        uint8_t px[GLYPH_W * HEIGHT_MAX];
        gl->adv = ttf_glyph(f, gi, 1, px);
        for (int y = 0; y < h; y++) memcpy(gl->native[y], px + y * GLYPH_W, GLYPH_W);
        if (gl->hd) ttf_glyph(f, gi, scale, gl->hd), cover(gl, h);
        return gl;
    }
    /* The ROM's (as the game holds it), and its HD .png's or itself upscaled. */
    uint32_t rec = f->data + rt_r32(f->data + GFX_FONT_TABLE + 4u * (uint32_t)g);
    int bpr = (int)rt_r8(rec + 1);
    gl->adv = (int)rt_r8(rec);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < GLYPH_W && x < bpr * 4; x++) gl->native[y][x] = (uint8_t)(rt_r8(rec + 2 + (uint32_t)(y * bpr + x / 4)) >> (x % 4 * 2) & 3);
    }
    if (!gl->hd) return gl;
    for (int y = 0; y < h * scale; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t v = gl->native[y / scale][x / scale];
            if (f->png && x < f->cell_w * scale) {
                int px = g % GFX_FONT_COLUMNS * f->cell_w * f->png_scale + x * f->png_scale / scale;
                int py = g / GFX_FONT_COLUMNS * h * f->png_scale + y * f->png_scale / scale;
                v = f->png[(size_t)py * f->png_w + px];
            }
            gl->hd[y * w + x] = v;
        }
    }
    cover(gl, h);
    return gl;
}

/* ---- the HD tiles ---- */

#define FT_SLOTS 4096
#define FT_INDEX (FT_SLOTS * 2)
typedef struct FtTile { GfxHdTile hd; uint8_t bytes[32], *px; uint64_t hash, used; int live; } FtTile;
static FtTile ft[FT_SLOTS];
static uint32_t ft_index[FT_INDEX]; /* slot + 1, 0 empty */
static uint32_t ft_free[FT_SLOTS], ft_free_count;
static int ft_ready;

static uint64_t tile_hash(const uint8_t *t) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 32; i++) h = (h ^ t[i]) * 1099511628211ull;
    return h | 1;
}

static uint32_t ft_lookup(const uint8_t *t, uint64_t h) {
    for (uint32_t s = (uint32_t)h & (FT_INDEX - 1);; s = (s + 1) & (FT_INDEX - 1)) {
        uint32_t n = ft_index[s];
        if (!n) return 0;
        if (ft[n - 1].hash == h && !memcmp(ft[n - 1].bytes, t, 32)) return n;
    }
}

static void ft_place(uint32_t i) {
    uint32_t s = (uint32_t)ft[i].hash & (FT_INDEX - 1);
    while (ft_index[s]) s = (s + 1) & (FT_INDEX - 1);
    ft_index[s] = i + 1;
}

static int by_used(const void *a, const void *b) {
    uint64_t x = ft[*(const uint32_t *)a].used, y = ft[*(const uint32_t *)b].used;
    return x < y ? -1 : x > y;
}

/* Frees the tiles not shown for a second, else the older half. */
static void ft_evict(void) {
    static uint32_t order[FT_SLOTS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < FT_SLOTS; i++) {
        if (ft[i].live && rt_frame_count - ft[i].used > 60) ft[i].live = 0;
        if (ft[i].live) order[n++] = i;
    }
    if (n > FT_SLOTS / 2) {
        qsort(order, n, sizeof *order, by_used);
        for (uint32_t i = 0; i < n - FT_SLOTS / 2; i++) ft[order[i]].live = 0;
    }
    memset(ft_index, 0, sizeof ft_index);
    ft_free_count = 0;
    for (uint32_t i = FT_SLOTS; i-- > 0;) {
        if (ft[i].live) ft_place(i);
        else ft_free[ft_free_count++] = i;
    }
}

/* The HD tile for native tile bytes t: its pixels (8 * scale square) to fill in. */
static uint8_t *ft_add(const uint8_t *t, int scale) {
    if (!ft_ready) {
        for (uint32_t i = 0; i < FT_SLOTS; i++) ft_free[i] = FT_SLOTS - 1 - i;
        ft_free_count = FT_SLOTS;
        ft_ready = 1;
    }
    uint64_t h = tile_hash(t);
    uint32_t n = ft_lookup(t, h);
    if (!n) {
        if (!ft_free_count) ft_evict();
        uint32_t i = ft_free[--ft_free_count];
        if (!ft[i].px && !(ft[i].px = malloc(8 * SCALE_MAX * 8 * SCALE_MAX))) {
            ft_free_count++;
            return NULL;
        }
        memcpy(ft[i].bytes, t, 32);
        ft[i].hash = h, ft[i].live = 1;
        ft_place(i);
        n = i + 1;
    }
    FtTile *e = &ft[n - 1];
    e->used = rt_frame_count;
    e->hd = (GfxHdTile){scale, 1, 0, e->px};
    return e->px;
}

uint32_t gfxfont_find(const uint8_t *t) {
    if (!ft_ready) return 0;
    uint32_t n = ft_lookup(t, tile_hash(t));
    if (!n) return 0;
    ft[n - 1].used = rt_frame_count;
    return GFXFONT_TILES + n;
}

const GfxHdTile *gfxfont_tile(uint32_t n) {
    n -= GFXFONT_TILES;
    return n >= 1 && n <= FT_SLOTS && ft[n - 1].px ? &ft[n - 1].hd : NULL;
}

/* ---- what the draws put in each tile ---- */

#define DRAWS 8
#define RECS 16384
typedef struct Draw { uint16_t glyph; uint8_t font, color; int8_t dx, dy; } Draw; /* the glyph's place in the tile */
typedef struct Rec { uint32_t addr; uint8_t base[32], now[32]; int n; Draw d[DRAWS]; } Rec;
static Rec *recs;
static int rec_count;

static void recs_clear(void) {
    if (recs) memset(recs, 0, RECS * sizeof *recs);
    rec_count = 0;
}

static Rec *rec_for(uint32_t addr) {
    if (!recs && !(recs = calloc(RECS, sizeof *recs))) return NULL;
    for (uint32_t s = (addr >> 5) * 2654435761u & (RECS - 1);; s = (s + 1) & (RECS - 1)) {
        if (recs[s].addr == addr) return &recs[s];
        if (!recs[s].addr) {
            if (rec_count >= RECS * 3 / 4) {
                recs_clear();
                return rec_for(addr);
            }
            rec_count++;
            recs[s].addr = addr;
            recs[s].n = -1; /* not known */
            return &recs[s];
        }
    }
}

static void read_tile(uint32_t addr, uint8_t *t) {
    for (int i = 0; i < 8; i++) {
        uint32_t w = rt_r32(addr + 4u * i);
        memcpy(t + 4 * i, &w, 4);
    }
}

/* The tile's HD art at `scale`, from its pixels before the draws and the draws. */
static void compose(const Rec *r, int scale) {
    uint8_t *px = ft_add(r->now, scale);
    if (!px) return;
    int n = 8 * scale;
    for (int v = 0; v < n; v++) {
        for (int u = 0; u < n; u++) px[v * n + u] = r->base[(v / scale) * 4 + u / scale / 2] >> (u / scale % 2 * 4) & 15;
    }
    for (int i = 0; i < r->n; i++) {
        const Draw *d = &r->d[i];
        Font *f = &fonts[d->font];
        const Glyph *gl = glyph(f, d->glyph);
        if (!gl) continue;
        int gs = gl->scale, gw = GLYPH_W * gs, gh = f->height * gs;
        for (int v = 0; v < n; v++) {
            int gy = (v - d->dy * scale) * gs / scale;
            if (v < d->dy * scale || gy >= gh) continue;
            for (int u = 0; u < n; u++) {
                int gx = (u - d->dx * scale) * gs / scale;
                if (u < d->dx * scale || gx >= gw) continue;
                int val = gl->hd ? gl->hd[gy * gw + gx] : gl->native[gy / gs][gx / gs];
                uint8_t *p = &px[v * n + u];
                if (val == 1 || val == 2) *p = (uint8_t)((2 * d->color + 1 + val) & 15);
                else if (val == 3) *p |= d->color == 6 ? (u / scale % 4 ? 0 : 15) : 1;
            }
        }
    }
}

/* ---- the game's calls ---- */

/* The font whose file in RAM is at `data` (by its height and glyph count), its glyphs built for the current size
 * and screens' scale. */
static Font *font_at(uint32_t data) {
    if (!data) return NULL;
    int height = (int)rt_r8(data + 8), count = (int)rt_r16(data + 10);
    for (int i = 0; i < font_count; i++) {
        Font *f = &fonts[i];
        if (f->height != height || f->count != count) continue;
        uint32_t key = (uint32_t)display_setting_get(SET_FONT_SIZE) << 8 | (uint32_t)font_scale(f);
        f->data = data; /* the game may hold more than one copy of a font: the same file */
        if (f->key != key) {
            f->key = key;
            f->gen++;
            recs_clear(); /* the tiles' records refer to glyphs as they were */
        }
        return f;
    }
    return NULL;
}

void rt_font_draw_glyph(CPU *cpu) {
    Font *f = display_setting_get(SET_MODS) ? font_at(rt_r32(R[0])) : NULL;
    if (!f) {
        rt_call_wrapped(cpu, "main", DRAW_GLYPH);
        return;
    }
    uint32_t buf = R[1], wt = R[2], sp = R[13];
    int x = (int)rt_r16(sp), y = (int)rt_r16(sp + 4), color = (int)rt_r8(sp + 8);
    Glyph *gl = glyph(f, (int)R[3]);
    if (!gl) {
        R[0] = 0;
        return;
    }
    int hd = font_scale(f) > 1, h = f->height;
    int tx0 = x >> 3, ty0 = y >> 3, ty1 = (y + h - 1) >> 3, tiles = 0;
    Rec *touched[3 * (HEIGHT_MAX / 8 + 2)];
    int at_x[3 * (HEIGHT_MAX / 8 + 2)], at_y[3 * (HEIGHT_MAX / 8 + 2)];
    for (int ty = ty0; hd && ty <= ty1; ty++) {
        for (int tx = tx0; tx < tx0 + 3; tx++) {
            Rec *r = rec_for(buf + (uint32_t)tx * 32 + (uint32_t)ty * wt * 32);
            if (!r) continue;
            uint8_t cur[32];
            read_tile(r->addr, cur);
            if (r->n < 0 || memcmp(cur, r->now, 32)) memcpy(r->base, cur, 32), memcpy(r->now, cur, 32), r->n = 0;
            at_x[tiles] = tx, at_y[tiles] = ty;
            touched[tiles++] = r;
        }
    }
    /* The game's own drawing (main 0x0203d518), word by word. */
    uint32_t shadow = color == 6 ? 0x000F000Fu : 0x11111111u, add = (uint32_t)((2 * color + 1) & 0xFF) * 0x11111111u;
    uint32_t at = buf + (uint32_t)tx0 * 32 + (uint32_t)(y & 7) * 4 + (uint32_t)ty0 * wt * 32;
    int s = (x & 7) * 4;
    for (int row = 0; row < h; row++) {
        uint32_t lo = 0, hi = 0;
        for (int i = 0; i < 8; i++) lo |= (uint32_t)gl->native[row][i] << 4 * i, hi |= (uint32_t)gl->native[row][8 + i] << 4 * i;
        if (lo | hi) {
            uint32_t words[3] = {lo << s, hi << s | (s ? lo >> (32 - s) : 0), s ? hi >> (32 - s) : 0};
            for (int i = 0; i < 3; i++) {
                uint32_t w = words[i];
                if (!w) continue;
                uint32_t m3 = ((w + 0x11111111u) & 0x44444444u) >> 2;
                m3 *= 15;
                uint32_t keep = shadow & m3, v = w & ~m3;
                uint32_t m12 = ((((v & 0x77777777u) + 0x77777777u) | v) & 0x88888888u) >> 3;
                m12 *= 15;
                v = (v + (add & m12)) | keep;
                uint32_t a = at + 32u * (uint32_t)i;
                rt_w32(a, (rt_r32(a) & ~m12) | v);
            }
        }
        at += (y + row + 1) & 7 ? 4 : (wt - 1) * 32 + 4;
    }
    for (int i = 0; i < tiles; i++) {
        Rec *r = touched[i];
        uint8_t cur[32];
        read_tile(r->addr, cur);
        if (!memcmp(cur, r->now, 32)) continue; /* nothing drawn there */
        memcpy(r->now, cur, 32);
        if (r->n == DRAWS) {
            r->n = -1; /* too many to keep: no HD art for this tile */
            continue;
        }
        r->d[r->n++] = (Draw){(uint16_t)R[3], (uint8_t)(f - fonts), (uint8_t)color, (int8_t)(x - at_x[i] * 8), (int8_t)(y - at_y[i] * 8)};
        compose(r, font_scale(f));
    }
    R[0] = (uint32_t)gl->adv;
}

void rt_font_advance(CPU *cpu) {
    Font *f = display_setting_get(SET_MODS) ? font_at(rt_r32(R[0])) : NULL;
    Glyph *gl = f && f->ttf ? glyph(f, (int)R[1]) : NULL;
    if (gl) R[0] = (uint32_t)gl->adv;
    else rt_call_wrapped(cpu, "main", ADVANCE);
}
