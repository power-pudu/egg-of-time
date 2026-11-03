/* The game's graphics in the ROM: its files and their formats, shared by the graphics dump (gfx_dump.c) and
 * graphics mods (gfx_mod.c).
 *
 * The ROM's file system (NitroFS: the FNT names the files, the FAT places them) holds the NitroSDK formats, most
 * of them LZ77-compressed (BIOS LZ10, some files twice):
 *   - RGCN (.NCGR) tile sheets: CHAR section, 4 or 8 bits per pixel, 8x8 tiles; the sheets here are 1D (no width).
 *   - RECN (.NCER) cells, the frames of a sprite: OAM entries (shape, size, flips, tile, palette row) placed
 *     around the cell's origin; tile numbers count 32 << mapping bytes (1D), or 32-tile rows (2D).
 *   - RLCN (.NCLR) palettes: PLTT section, BGR555 colours.
 * and the game's own simpler ones for backgrounds (8-byte headers, little-endian):
 *   - NCG (*_ncg.bin): "NCG\0", tile count (16 bits), 8 bits per pixel (byte 6, bit 0), the tiles.
 *   - NCL (*_ncl.bin): "NCL\0", colour count (32 bits), the colours.
 *   - NSC (*_nsc.bin): "NSC\0", entries (16 bits), two flags (byte 6, bit 0: an affine BG), width and height in
 *     tiles (bytes 8, 9), from byte 12 the map: BG text entries (tile in bits 0-9, flips 10 and 11, palette row
 *     12-15), or a byte per tile for an affine BG (the title's clock, the minimaps).
 *   - FONT (msg/<size>/msgcmn.fnt), the message fonts: "\0\0\0\0FONT", height (byte 8), bits per pixel (byte 9),
 *     glyph count (16 bits at 10), an ASCII-to-glyph table (128 x 16 bits at 12), a 32-bit offset per glyph at
 *     0x10C (0: none), then each glyph: advance width (u8), bytes per row (u8), its rows (pixels from the low
 *     bits). Never compressed. Text draws the glyphs into tiles at run time, so fonts have no tiles of their own.
 * The palettes are never compressed. A sheet's palette is the file of the same name, else the only palette in its
 * folder; the field characters' (Chara/) and the field maps' (Map/Bg/) the game picks in code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_rom.h"
#include "rt.h"

#define CELL_MAX 512 /* a cell's width or height, at most (OAM coordinates span 512 x 256) */

static GfxFile *files;
static int file_count, file_cap;

uint32_t gfx_le16(const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t gfx_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
#define le16 gfx_le16
#define le32 gfx_le32

/* ---- the ROM's files ---- */

static void add_file(const char *path, const uint8_t *rom, size_t rom_size, uint32_t fat, uint32_t id) {
    uint32_t start = le32(rom + fat + id * 8), end = le32(rom + fat + id * 8 + 4);
    if (start > end || end > rom_size || strlen(path) >= sizeof files->path) return;
    if (file_count == file_cap) {
        file_cap = file_cap ? file_cap * 2 : 1024;
        files = realloc(files, (size_t)file_cap * sizeof *files);
    }
    GfxFile *f = &files[file_count++];
    strcpy(f->path, path);
    f->id = id;
    f->offset = start;
    f->size = end - start;
}

static void walk(const uint8_t *rom, size_t size, uint32_t fnt, uint32_t fnt_size, uint32_t fat, uint32_t dir,
                 const char *prefix, int depth) {
    uint32_t entry = fnt + (dir & 0xfff) * 8;
    if (depth > 16 || entry + 8 > fnt + fnt_size) return;
    uint32_t p = fnt + le32(rom + entry), id = le16(rom + entry + 4);
    while (p < fnt + fnt_size && rom[p]) {
        uint32_t len = rom[p] & 0x7f, is_dir = rom[p] & 0x80;
        if (p + 1 + len + (is_dir ? 2 : 0) > fnt + fnt_size) return;
        char path[256];
        snprintf(path, sizeof path, "%s%.*s", prefix, (int)len, (const char *)rom + p + 1);
        p += 1 + len;
        if (is_dir) {
            uint32_t child = le16(rom + p);
            p += 2;
            strcat(path, "/");
            walk(rom, size, fnt, fnt_size, fat, child, path, depth + 1);
        } else {
            add_file(path, rom, size, fat, id++);
        }
    }
}

int gfx_list_files(void) {
    const uint8_t *rom = rt_rom.data;
    size_t size = rt_rom.size;
    file_count = 0;
    if (size < 0x200) return 0;
    uint32_t fnt = le32(rom + 0x40), fnt_size = le32(rom + 0x44), fat = le32(rom + 0x48), fat_size = le32(rom + 0x4c);
    if ((uint64_t)fnt + fnt_size > size || (uint64_t)fat + fat_size > size) return 0;
    walk(rom, size, fnt, fnt_size, fat, 0xf000, "", 0);
    return file_count;
}

const GfxFile *gfx_file(int i) { return i >= 0 && i < file_count ? &files[i] : NULL; }

const GfxFile *gfx_find(const char *path) {
    for (int i = 0; i < file_count; i++) {
        if (!strcmp(files[i].path, path)) return &files[i];
    }
    return NULL;
}

int gfx_ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}

/* ---- LZ10 ---- */

uint8_t *gfx_lz10(const uint8_t *s, uint32_t n, uint32_t *out_size) {
    if (n < 4 || s[0] != 0x10) return NULL;
    uint32_t size = s[1] | s[2] << 8 | s[3] << 16, o = 0, i = 4;
    if (!size) return NULL;
    uint8_t *d = malloc(size);
    while (o < size) {
        if (i >= n) goto bad;
        uint8_t flags = s[i++];
        for (int b = 0; b < 8 && o < size; b++, flags <<= 1) {
            if (flags & 0x80) {
                if (i + 1 >= n) goto bad;
                uint32_t len = (s[i] >> 4) + 3, disp = ((s[i] & 15u) << 8 | s[i + 1]) + 1;
                i += 2;
                if (disp > o) goto bad;
                for (; len && o < size; len--, o++) d[o] = d[o - disp];
            } else {
                if (i >= n) goto bad;
                d[o++] = s[i++];
            }
        }
    }
    *out_size = size;
    return d;
bad:
    free(d);
    return NULL;
}

/* Greedy, through hash chains of 3-byte prefixes. Distances of at least 2, as the BIOS's VRAM variant needs. */
uint8_t *gfx_lz10_compress(const uint8_t *d, uint32_t n, uint32_t *out_size) {
    enum { HASH = 1 << 14, CHAIN = 128 };
    uint8_t *out = malloc(4 + n + n / 8 + 8);
    int32_t *head = malloc(HASH * sizeof *head), *prev = malloc((n ? n : 1) * sizeof *prev);
    for (int i = 0; i < HASH; i++) head[i] = -1;
    out[0] = 0x10, out[1] = (uint8_t)n, out[2] = (uint8_t)(n >> 8), out[3] = (uint8_t)(n >> 16);
    uint32_t o = 4, i = 0;
#define HASH3(p) ((((uint32_t)d[p] << 10) ^ ((uint32_t)d[(p) + 1] << 5) ^ d[(p) + 2]) & (HASH - 1))
#define INSERT(p) do { if ((p) + 2 < n) { uint32_t h_ = HASH3(p); prev[p] = head[h_]; head[h_] = (int32_t)(p); } } while (0)
    while (i < n) {
        uint32_t flag_at = o++;
        uint8_t flags = 0;
        for (int b = 0; b < 8 && i < n; b++) {
            uint32_t best = 0, best_disp = 0;
            if (i + 2 < n) {
                int32_t c = head[HASH3(i)];
                for (int steps = 0; c >= 0 && steps < CHAIN && i - (uint32_t)c <= 0x1000; steps++, c = prev[c]) {
                    uint32_t disp = i - (uint32_t)c, len = 0;
                    if (disp < 2) continue;
                    while (len < 18 && i + len < n && d[c + len] == d[i + len]) len++;
                    if (len > best) best = len, best_disp = disp;
                    if (len == 18) break;
                }
            }
            if (best >= 3) {
                flags |= (uint8_t)(0x80 >> b);
                out[o++] = (uint8_t)((best - 3) << 4 | (best_disp - 1) >> 8);
                out[o++] = (uint8_t)(best_disp - 1);
                for (uint32_t k = 0; k < best; k++) INSERT(i + k);
                i += best;
            } else {
                INSERT(i);
                out[o++] = d[i++];
            }
        }
        out[flag_at] = flags;
    }
#undef HASH3
#undef INSERT
    while (o & 3) out[o++] = 0;
    free(head);
    free(prev);
    *out_size = o;
    return out;
}

/* ---- decoding ---- */

static int known(const uint8_t *d, uint32_t n) {
    static const char *const magics[] = {"RGCN", "RECN", "RLCN", "NCG", "NCL", "NSC"}; /* the last three end in 0 */
    for (size_t i = 0; n >= 16 && i < SDL_arraysize(magics); i++) {
        if (!memcmp(d, magics[i], 4)) return 1;
    }
    return n >= 16 && !memcmp(d, "\0\0\0\0FONT", 8);
}

GfxBlob gfx_open(const GfxFile *f) {
    GfxBlob b = {rt_rom.data + f->offset, f->size, NULL, 0};
    while (b.levels < 3 && !known(b.data, b.size)) {
        uint32_t n;
        uint8_t *u = gfx_lz10(b.data, b.size, &n);
        if (!u) break;
        free(b.owned);
        b = (GfxBlob){u, n, u, b.levels + 1};
    }
    if (!known(b.data, b.size)) {
        free(b.owned);
        b = (GfxBlob){NULL, 0, NULL, 0};
    }
    return b;
}

void gfx_close(GfxBlob *b) {
    free(b->owned);
    *b = (GfxBlob){NULL, 0, NULL, 0};
}

static void grey(GfxPalette *p, int bpp8) {
    for (int i = 0; i < 256; i++) {
        Uint8 v = (Uint8)(bpp8 ? i : (i & 15) * 17);
        p->c[i] = (SDL_Color){v, v, v, 255};
    }
    p->from = NULL;
    p->colors = 0;
}

int gfx_palette_colors(const GfxFile *f, uint32_t *offset, uint32_t *count) {
    const uint8_t *d = rt_rom.data + f->offset;
    uint32_t n = f->size;
    if (n >= 0x28 && !memcmp(d, "RLCN", 4) && !memcmp(d + 0x10, "TTLP", 4)) {
        uint32_t size = le32(d + 0x20), off = 0x18 + le32(d + 0x24);
        if (off > n || size > n - off) return 0;
        *offset = f->offset + off, *count = size / 2;
        return 1;
    }
    if (n >= 8 && !memcmp(d, "NCL", 4)) {
        *offset = f->offset + 8, *count = SDL_min(le32(d + 4), (n - 8) / 2);
        return 1;
    }
    return 0;
}

int gfx_read_palette(const GfxFile *f, GfxPalette *p) {
    uint32_t at, n;
    if (!gfx_palette_colors(f, &at, &n)) return 0;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i < n ? le16(rt_rom.data + at + i * 2) : 0;
        uint32_t r = c & 31, g = c >> 5 & 31, bl = c >> 10 & 31;
        p->c[i] = (SDL_Color){(Uint8)(r << 3 | r >> 2), (Uint8)(g << 3 | g >> 2), (Uint8)(bl << 3 | bl >> 2), 255};
    }
    p->from = f;
    p->colors = SDL_min(n, 256u);
    return 1;
}

void gfx_palette_for(const char *path, int bpp8, GfxPalette *p) {
    char base[256], try[300];
    const char *slash = strrchr(path, '/');
    size_t dir_len = slash ? (size_t)(slash - path) + 1 : 0;
    snprintf(base, sizeof base, "%s", path);
    char *cut = gfx_ends_with(base, ".NCGR") ? base + strlen(base) - 5
                : gfx_ends_with(base, "_ncg.bin") || gfx_ends_with(base, "_nsc.bin") ? base + strlen(base) - 8 : NULL;
    if (cut) {
        *cut = 0;
        static const char *const ends[] = {".NCLR", "_ncl.bin"};
        for (size_t i = 0; i < SDL_arraysize(ends); i++) {
            snprintf(try, sizeof try, "%s%s", base, ends[i]);
            const GfxFile *f = gfx_find(try);
            if (f && gfx_read_palette(f, p)) return;
        }
        /* The field characters: Chara_xxxx's palette is ObjPlt_xxxx (paired in the game's code). */
        if (!strncmp(base, "Chara/Chara_", 12)) {
            snprintf(try, sizeof try, "Chara/ObjPlt_%s.NCLR", base + 12);
            const GfxFile *f = gfx_find(try);
            if (f && gfx_read_palette(f, p)) return;
        }
    }
    const GfxFile *only = NULL;
    int n = 0;
    for (int i = 0; i < file_count; i++) {
        const char *q = files[i].path;
        if (strncmp(q, path, dir_len) || strchr(q + dir_len, '/') || !(gfx_ends_with(q, ".NCLR") || gfx_ends_with(q, "_ncl.bin"))) continue;
        only = &files[i];
        n++;
    }
    if (n == 1 && gfx_read_palette(only, p)) return;
    grey(p, bpp8);
}

int gfx_read_tiles(const GfxBlob *b, GfxTiles *t) {
    const uint8_t *d = b->data;
    if (!d) return 0;
    if (!memcmp(d, "RGCN", 4) && b->size >= 0x30 && !memcmp(d + 0x10, "RAHC", 4)) {
        uint32_t size = le32(d + 0x28), off = 0x18 + le32(d + 0x2c);
        if (off > b->size || size > b->size - off) return 0;
        *t = (GfxTiles){d + off, size, off, le32(d + 0x1c) == 4};
        return 1;
    }
    if (!memcmp(d, "NCG", 4)) {
        uint32_t count = le16(d + 4);
        int bpp8 = (d[6] & 1) && count * 64 <= b->size - 8;
        *t = (GfxTiles){d + 8, SDL_min(count * (bpp8 ? 64 : 32), b->size - 8), 8, bpp8};
        return 1;
    }
    return 0;
}

int gfx_tile_px(const GfxTiles *t, uint32_t off, int bpp8, int x, int y) {
    if (bpp8) return off + 64 <= t->size ? t->data[off + y * 8 + x] : 0;
    if (off + 32 > t->size) return 0;
    uint8_t v = t->data[off + y * 4 + x / 2];
    return x & 1 ? v >> 4 : v & 15;
}

/* ---- layouts ---- */

static GfxPlace *place_at(GfxLayout *l, int *cap) {
    if (l->count == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        l->tiles = realloc(l->tiles, (size_t)*cap * sizeof *l->tiles);
    }
    return &l->tiles[l->count++];
}

int gfx_sheet_layout(const GfxTiles *t, GfxLayout *l) {
    int tile_bytes = t->bpp8 ? 64 : 32, count = (int)(t->size / (uint32_t)tile_bytes);
    if (!count) return 0;
    int w = SDL_min(count, 16), cap = count;
    *l = (GfxLayout){w * 8, (count + w - 1) / w * 8, t->bpp8, 0, malloc((size_t)count * sizeof *l->tiles)};
    for (int i = 0; i < count; i++) *place_at(l, &cap) = (GfxPlace){(uint32_t)(i * tile_bytes), i % w * 8, i / w * 8, t->bpp8, 0, 0, 0};
    return 1;
}

static const int obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const int obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

typedef struct Obj { int x, y, w, h, box_w, box_h, bpp8, hflip, vflip, tile, row; } Obj;

static int read_obj(const uint8_t *o, Obj *ob) {
    uint32_t a0 = le16(o), a1 = le16(o + 2), a2 = le16(o + 4), shape = a0 >> 14, size = a1 >> 14;
    if (shape == 3) return 0;
    int affine = (a0 & 0x100) != 0, dbl = affine && (a0 & 0x200);
    *ob = (Obj){(int)(a1 & 0x1ff) - (a1 & 0x100 ? 512 : 0), (int8_t)(a0 & 0xff), obj_w[shape][size], obj_h[shape][size],
                0, 0, (a0 & 0x2000) != 0, !affine && (a1 & 0x1000), !affine && (a1 & 0x2000), (int)(a2 & 0x3ff), (int)(a2 >> 12)};
    ob->box_w = ob->w << dbl;
    ob->box_h = ob->h << dbl;
    return 1;
}

static int cells_header(const GfxBlob *b, uint32_t *cells, uint32_t *entry, uint32_t *table, uint32_t *mapping) {
    const uint8_t *d = b->data;
    if (!d || memcmp(d, "RECN", 4) || b->size < 0x24 || memcmp(d + 0x10, "KBEC", 4)) return 0;
    *cells = le16(d + 0x18), *entry = le16(d + 0x1a) == 1 ? 16 : 8, *table = 0x18 + le32(d + 0x1c), *mapping = le32(d + 0x20);
    return *table + *cells * *entry <= b->size;
}

uint32_t gfx_cell_count(const GfxBlob *b) {
    uint32_t cells, entry, table, mapping;
    return cells_header(b, &cells, &entry, &table, &mapping) ? cells : 0;
}

int gfx_cell_layout(const GfxBlob *b, uint32_t c, GfxLayout *l) {
    uint32_t cells, entry, table, mapping;
    if (!cells_header(b, &cells, &entry, &table, &mapping) || c >= cells) return 0;
    const uint8_t *d = b->data, *e = d + table + c * entry;
    uint32_t n = le16(e), at = table + cells * entry + le32(e + 4);
    if (!n || at > b->size || n > (b->size - at) / 6) return 0;
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN, bpp8 = 0, cap = 0;
    for (uint32_t k = 0; k < n; k++) {
        Obj o;
        if (!read_obj(d + at + k * 6, &o)) continue;
        x0 = SDL_min(x0, o.x), y0 = SDL_min(y0, o.y), x1 = SDL_max(x1, o.x + o.box_w), y1 = SDL_max(y1, o.y + o.box_h);
        bpp8 |= o.bpp8;
    }
    if (x0 >= x1 || x1 - x0 > CELL_MAX || y1 - y0 > CELL_MAX) return 0;
    *l = (GfxLayout){x1 - x0, y1 - y0, bpp8, 0, NULL};
    for (uint32_t k = n; k-- > 0;) { /* the first entry is drawn on top, as on the DS */
        Obj o;
        if (!read_obj(d + at + k * 6, &o)) continue;
        int tile_bytes = o.bpp8 ? 64 : 32, cols = o.w / 8;
        for (int ty = 0; ty < o.h / 8; ty++) {
            for (int tx = 0; tx < cols; tx++) {
                uint32_t off = mapping < 4 ? ((uint32_t)o.tile << mapping) * 32 + (uint32_t)((ty * cols + tx) * tile_bytes)
                                           : (uint32_t)(o.tile + ty * 32 + tx * (tile_bytes / 32)) * 32;
                int sx = o.hflip ? o.w - 8 - tx * 8 : tx * 8, sy = o.vflip ? o.h - 8 - ty * 8 : ty * 8;
                *place_at(l, &cap) = (GfxPlace){off, o.x - x0 + (o.box_w - o.w) / 2 + sx, o.y - y0 + (o.box_h - o.h) / 2 + sy,
                                                o.bpp8, o.hflip, o.vflip, o.row};
            }
        }
    }
    return 1;
}

int gfx_screen_layout(const GfxBlob *b, int bpp8, GfxLayout *l) {
    const uint8_t *d = b->data;
    if (!d || memcmp(d, "NSC", 4)) return 0;
    int w = d[8], h = d[9], cap = w * h;
    uint32_t entry = d[6] & 1 ? 1 : 2;
    if (!w || !h || (uint64_t)w * h * entry > b->size - 12) return 0;
    *l = (GfxLayout){w * 8, h * 8, bpp8, 0, malloc((size_t)cap * sizeof *l->tiles)};
    int tile_bytes = bpp8 ? 64 : 32;
    for (int i = 0; i < w * h; i++) {
        uint32_t v = entry == 2 ? le16(d + 12 + i * 2) : d[12 + i], tile = entry == 2 ? v & 0x3ff : v;
        *place_at(l, &cap) = (GfxPlace){tile * (uint32_t)tile_bytes, i % w * 8, i / w * 8, bpp8, entry == 2 && (v & 0x400),
                                        entry == 2 && (v & 0x800), entry == 2 ? (int)(v >> 12) : 0};
    }
    return 1;
}

void gfx_draw(const GfxLayout *l, const GfxTiles *t, uint8_t *px) {
    for (int i = 0; i < l->count; i++) {
        const GfxPlace *p = &l->tiles[i];
        for (int ty = 0; ty < 8; ty++) {
            for (int tx = 0; tx < 8; tx++) {
                int v = gfx_tile_px(t, p->off, p->bpp8, p->hflip ? 7 - tx : tx, p->vflip ? 7 - ty : ty);
                int dx = p->x + tx, dy = p->y + ty;
                if (v && dx >= 0 && dy >= 0 && dx < l->w && dy < l->h) px[dy * l->w + dx] = (uint8_t)(p->bpp8 ? v : p->row * 16 + v);
            }
        }
    }
}

/* ---- fonts ---- */

int gfx_read_font(const GfxBlob *b, GfxFont *f) {
    const uint8_t *d = b->data;
    if (!d || b->size < GFX_FONT_TABLE || memcmp(d, "\0\0\0\0FONT", 8)) return 0;
    int bpp = d[9], count = (int)le16(d + 10);
    if (!d[8] || (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) || (uint64_t)GFX_FONT_TABLE + 4u * count > b->size) return 0;
    *f = (GfxFont){d, b->size, d[8], bpp, count, 8};
    for (int g = 0; g < count; g++) {
        const uint8_t *r = gfx_font_glyph(f, g);
        if (r) f->cell_w = SDL_max(f->cell_w, r[1] * 8 / bpp);
    }
    return 1;
}

const uint8_t *gfx_font_glyph(const GfxFont *f, int g) {
    if (g < 0 || g >= f->count) return NULL;
    uint32_t o = le32(f->data + GFX_FONT_TABLE + 4 * g);
    if (!o || o > f->size - 2 || (uint32_t)f->height * f->data[o + 1] > f->size - o - 2) return NULL;
    return f->data + o;
}

int gfx_font_px(const GfxFont *f, const uint8_t *glyph, int x, int y) {
    int bit = x * f->bpp;
    if (x < 0 || y < 0 || y >= f->height || bit / 8 >= glyph[1]) return 0;
    return glyph[2 + y * glyph[1] + bit / 8] >> bit % 8 & ((1 << f->bpp) - 1);
}

void gfx_font_palette(const char *path, GfxPalette *p) {
    gfx_palette_for(path, 0, p);
    const char *slash = strrchr(path, '/');
    if (!p->from && slash) {
        char up[300];
        int dir = (int)(slash - path);
        while (dir > 0 && path[dir - 1] != '/') dir--;
        snprintf(up, sizeof up, "%.*sfont", dir, path); /* a file in the folder above */
        gfx_palette_for(up, 0, p);
    }
    if (!p->from) return;
    /* The game draws a glyph's letter (1) in colour 2 (white) and its shadow (3) in colour 1 (black), as text
     * shows on screen; 2, unused, is taken as colour 3. */
    SDL_Color c1 = p->c[1];
    p->c[1] = p->c[2], p->c[2] = p->c[3], p->c[3] = c1;
}

void gfx_font_size(const GfxFont *f, int *w, int *h) {
    *w = GFX_FONT_COLUMNS * f->cell_w;
    *h = (f->count + GFX_FONT_COLUMNS - 1) / GFX_FONT_COLUMNS * f->height;
}

void gfx_font_draw(const GfxFont *f, uint8_t *px) {
    int w, h;
    gfx_font_size(f, &w, &h);
    for (int g = 0; g < f->count; g++) {
        const uint8_t *r = gfx_font_glyph(f, g);
        int x0 = g % GFX_FONT_COLUMNS * f->cell_w, y0 = g / GFX_FONT_COLUMNS * f->height;
        for (int y = 0; r && y < f->height; y++) {
            for (int x = 0; x < f->cell_w; x++) px[(y0 + y) * w + x0 + x] = (uint8_t)gfx_font_px(f, r, x, y);
        }
    }
}
