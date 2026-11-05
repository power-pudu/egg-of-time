/* The Minimap setting without its background: which pixels of the bottom screen's map panel are the map.
 *
 * The bottom screen's map has the parchment baked in: BG3 is the room's or era's picture (menu/bg/minimap_NNN_*,
 * minimap_W-*), parchment included, under BG1, the plain parchment (minimap_000) with holes where the map has been
 * explored, under the sprites (the player's marker). So the parchment is whatever BG1 shows, and whatever BG3 shows
 * in the plain parchment's colour at that place. Those are the map's own colours for the rooms, whose palette is
 * minimap_001's; the world maps' pictures were quantized to their own palette (minimap_W, in another row), so each
 * colour of the plain parchment is matched to the nearest one of the row BG3 is drawn with.
 *
 * A land's colour matches the parchment here and there, which would leave holes in it and specks around it. So only
 * parchment reached from the panel's edge goes, and small pieces of map on their own go too. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx_rom.h"
#include "minimap.h"
#include "rt.h"
#include "single_screen.h"

#define PLAIN_NSC "menu/bg/minimap_000_nsc.bin"
#define PLAIN_NCG "menu/bg/minimap_000_ncg.bin"
#define PLAIN_NCL "menu/bg/minimap_001_ncl.bin"
#define COLORS 16
#define MIN_PIECE 8 /* smaller pieces of map, in pixels, are specks */
enum { LAYER_BG1 = 1, LAYER_BG3 = 3, LAYER_BACKDROP = 5 };

static uint8_t plain[SCREEN_H][SCREEN_W]; /* the plain parchment, as palette indexes */
static uint16_t plain_colors[COLORS];
static int plain_state; /* 0: not loaded yet, 1: loaded, -1: failed */

static int load_plain(void) {
    if (!gfx_find(PLAIN_NSC)) gfx_list_files(); /* not listed yet: no graphics mods */
    const GfxFile *fs = gfx_find(PLAIN_NSC), *fg = gfx_find(PLAIN_NCG), *fp = gfx_find(PLAIN_NCL);
    uint32_t at, count;
    if (!fs || !fg || !fp || !gfx_palette_colors(fp, &at, &count) || count < COLORS) return 0;
    for (int i = 0; i < COLORS; i++) plain_colors[i] = (uint16_t)(gfx_le16(rt_rom.data + at + (uint32_t)i * 2) & 0x7FFF);
    GfxBlob bs = gfx_open(fs), bg = gfx_open(fg);
    GfxTiles t;
    GfxLayout l = {0};
    int ok = gfx_read_tiles(&bg, &t) && gfx_screen_layout(&bs, t.bpp8, &l) && l.w == SCREEN_W && l.h == SCREEN_H;
    if (ok) gfx_draw(&l, &t, &plain[0][0]);
    free(l.tiles);
    gfx_close(&bs);
    gfx_close(&bg);
    return ok;
}

static int color_distance(uint16_t a, uint16_t b) {
    int dr = (a & 31) - (b & 31), dg = (a >> 5 & 31) - (b >> 5 & 31), db = (a >> 10 & 31) - (b >> 10 & 31);
    return dr * dr + dg * dg + db * db;
}

/* Flood of the pixels equal to `from` through 8 neighbours from (x0, y0), set to `to`; returns how many. */
static int flood(uint8_t *px, int x0, int y0, uint8_t from, uint8_t to, int16_t *stack) {
    int n = 0, count = 0;
    px[y0 * SS_MAP_W + x0] = to;
    stack[n++] = (int16_t)x0, stack[n++] = (int16_t)y0;
    while (n) {
        int y = stack[--n], x = stack[--n];
        count++;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= SS_MAP_W || ny >= SS_MAP_H || px[ny * SS_MAP_W + nx] != from) continue;
                px[ny * SS_MAP_W + nx] = to;
                stack[n++] = (int16_t)nx, stack[n++] = (int16_t)ny;
            }
        }
    }
    return count;
}

/* The palette row BG3 is drawn with: the one holding most of a sample of its pixels. */
static const uint16_t *bg3_row(const VideoBottomTop *top) {
    enum { STEP = 12 };
    int best_row = 0, best = -1;
    for (int r = 0; r < 256 / COLORS; r++) {
        const uint16_t *row = &top->palette[r * COLORS];
        int found = 0;
        for (int y = SS_MAP_Y; y < SS_MAP_Y + SS_MAP_H; y += STEP) {
            for (int x = SS_MAP_X; x < SS_MAP_X + SS_MAP_W; x += STEP) {
                if (top->layer[y][x] != LAYER_BG3) continue;
                for (int k = 1; k < COLORS; k++) {
                    if (row[k] == top->color[y][x]) {
                        found++;
                        break;
                    }
                }
            }
        }
        if (found > best) best = found, best_row = r;
    }
    return &top->palette[best_row * COLORS];
}

int minimap_cutout(const VideoBottomTop *top, uint8_t *keep) {
    enum { PARCHMENT, MAP, OUTSIDE, MAP_SEEN };
    static int16_t stack[SS_MAP_W * SS_MAP_H * 2];
    if (!plain_state) plain_state = load_plain() ? 1 : -1;
    if (plain_state < 0) return 0;
    uint16_t shown[COLORS]; /* each colour of the plain parchment in the palette in use */
    const uint16_t *row = bg3_row(top);
    for (int i = 0; i < COLORS; i++) {
        int best = INT_MAX;
        for (int k = 1; k < COLORS; k++) {
            int d = color_distance(plain_colors[i], row[k]);
            if (d < best) best = d, shown[i] = row[k];
        }
    }
    for (int y = 0; y < SS_MAP_H; y++) {
        for (int x = 0; x < SS_MAP_W; x++) {
            int sx = SS_MAP_X + x, sy = SS_MAP_Y + y, layer = top->layer[sy][sx], is_map = layer != LAYER_BG1 && layer != LAYER_BACKDROP;
            if (layer == LAYER_BG3) { /* BG3's maps are one screen each, a room's second one at row 256 of the BG */
                int py = (sy + top->bg3_vofs) & 511, px = (sx + top->bg3_hofs) & 255;
                py = py >= 256 ? py - 256 : py;
                int parchment = py < SCREEN_H && plain[py][px] && top->color[sy][sx] == shown[plain[py][px] & 15];
                is_map = !parchment;
            }
            keep[y * SS_MAP_W + x] = is_map ? MAP : PARCHMENT;
        }
    }
    for (int x = 0; x < SS_MAP_W; x++) {
        if (keep[x] == PARCHMENT) flood(keep, x, 0, PARCHMENT, OUTSIDE, stack);
        if (keep[(SS_MAP_H - 1) * SS_MAP_W + x] == PARCHMENT) flood(keep, x, SS_MAP_H - 1, PARCHMENT, OUTSIDE, stack);
    }
    for (int y = 0; y < SS_MAP_H; y++) {
        if (keep[y * SS_MAP_W] == PARCHMENT) flood(keep, 0, y, PARCHMENT, OUTSIDE, stack);
        if (keep[y * SS_MAP_W + SS_MAP_W - 1] == PARCHMENT) flood(keep, SS_MAP_W - 1, y, PARCHMENT, OUTSIDE, stack);
    }
    /* What is left of the parchment is enclosed by the map: it counts as map, for the pieces too. */
    for (int i = 0; i < SS_MAP_W * SS_MAP_H; i++) keep[i] = keep[i] == PARCHMENT ? MAP : keep[i];
    for (int y = 0; y < SS_MAP_H; y++) {
        for (int x = 0; x < SS_MAP_W; x++) {
            if (keep[y * SS_MAP_W + x] != MAP) continue;
            if (flood(keep, x, y, MAP, MAP_SEEN, stack) < MIN_PIECE) flood(keep, x, y, MAP_SEEN, OUTSIDE, stack);
            else flood(keep, x, y, MAP_SEEN, OUTSIDE + 2, stack); /* a kept piece, done with */
        }
    }
    for (int i = 0; i < SS_MAP_W * SS_MAP_H; i++) keep[i] = keep[i] != OUTSIDE;
    return 1;
}
