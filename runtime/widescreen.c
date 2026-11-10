/* Widescreen for the field: fills the background map columns the widened view shows beyond the DS screen.
 *
 * The field keeps each background in a 512x256 map used as a ring (64 x 32 tiles, scrolled with BGxHOFS /
 * BGxVOFS), and the game only keeps the 34 columns around the 4:3 view current: when the camera moves one tile,
 * the scroll handler (ov004 0x0213bcb0 and its counterparts) generates the entering column from the room map into
 * a staging buffer (per layer: ov004 0x02137144, 0x02137734, 0x02137c4c) and the frame's upload copies it into the
 * ring. The other columns still hold whatever was there before. So before each frame the runtime writes the
 * columns of the margins itself, from the same data, the way the game would:
 *   - field scroll state *(0x020708e0): +4 camera column, +8 camera row (world tiles; the view is 32 x 24), map
 *     masks per layer at +0x1a (column, row: metatiles), ring origin per layer at +0x8e (column, row): the ring
 *     slot of the camera's tile;
 *   - room map *(0x0207b518): metatile ids, rows 0x100 apart, layer L at +0x1000 + 0x40 * L (64 per row), a 9th
 *     id bit at +0x10c0 (layer 0: bit 0, layer 1: bit 1);
 *   - metatiles *(*(0x020708d4) + 0xc) (layers 0-1) or + 0x10 (layer 2): 2x2 tiles of 3 bytes: tile number (byte 0
 *     and bits 0-1 of byte 1), flips (byte 1 bits 2-3), palette (byte 1 high nibble), priority (byte 2);
 *   - targets (checked against the game's own columns in the Millennial Fair, every cell): layer 2 -> BG0 with its
 *     palette remapped through the table at 0x02058714; layer 0 -> BG2 and layer 1 -> BG3 with the palette as it is;
 *     priority tiles of either go to BG1 instead (layer 1's first); a target without a tile gets entry 0.
 * A layer is filled only when its BG scroll is within a tile of its ring origin (a layer scrolling on its own, or a
 * map that is not 512x256, is left alone).
 * Battles (ov003 in the slot of the field's ov004) keep the room on screen but reuse its map for their own data,
 * and the field puts it back a moment after the battle ends, so the margins are filled from a copy of the room
 * map taken while the field ran: during the battle and after it, until the map matches the copy again (or the
 * room changes, or 10 s pass). Save states keep the copy.
 *
 * The world map (ov002) streams its own way (ov002 0x0211ac64 and the per-direction writers it calls, e.g.
 * 0x0211af10): layers 1-3 go to BG1-3, 512x256 rings of 32 x 16 metatiles (16 px), and the game keeps the 17
 * metatile columns from the view's left one current. Its world object, *(0x0207088c) + 0xddc0, holds per layer L:
 *   - +0x22a4 + 2L, +0x22ac + 2L: the view's top-left in the world, in pixels (x wraps at 0x600, y at 0x400; the BG
 *     scroll is that modulo the ring); layer 0 (the sea, BG0) keeps its own scroll there, times 16;
 *   - +0x22b4 + 4L: the world map, 96 x 64 metatile ids (bytes, rows 0x60 apart);
 *   - +0x2a4 + 0x800L: the metatile sheet, 32 x 32 map entries: id i is the 2x2 at column (i & 15) * 2, row
 *     (i >> 4) * 2.
 * So the margins' columns are written from the same data each frame, for each layer whose BG scroll agrees.
 * Its sprites (the boat, the Epoch...) come from the game's SNES-style sprite code (ov005 0x0212bcdc): a piece is
 * kept while its screen X + 15 is below 0x10f (X -15..255), as a 9-bit X in a SNES OAM table that ov002 0x0211cb44
 * turns into DS sprites (0x0211cbb4: X 256-511 read as -256..-1). Objects also keep an on-screen flag (ov002
 * 0x0210cd24: X - camera + 16, wrapped at 0x600, below 0x120; the birds go away once it clears). In widescreen both
 * tests are widened by the margins (rt_ws_snes_cmp) and a converted X below -128 (never kept by the game) is the
 * right margin's (rt_ws_world_sprite). Their Y has tests of the same kind, a piece's Y + 15 below 0xef (Y -15..223) and
 * an object's Y - camera + 16, wrapped at 0x400, below 0x110; zoomed out, those are widened by the rows. The SNES table
 * keeps Y in 8 bits, read as -32..223, which 248 rows and a piece above them do not fit: ws_world_sprite_limit.
 *
 * Zoom out draws rows above and below the view as well: the same fills reach them, and so does the sprite view, with
 * what sprites' 8-bit Y can no longer say kept in their attributes (rt_ws_draw_cell, rt_ws_enter_oam).
 *
 * With Fill screen sides off, the view stops at the room's ends: the game keeps its camera (it clamps it for 256 x 192
 * only), and the picture is shown shifted from it by what the wider view would pass the room by. A room smaller than
 * the view stays centred in it. The room's BGs (their scroll), the sprite view and the room's clip all move by that
 * shift; the text box, the UI and the battle effects' windows are in screen coordinates and stay. */
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "rt.h"
#include "single_screen.h"
#include "video.h"
#include "widescreen.h"

#define SCROLL GAME_ADDR(0x020708e0u, 0x02070900u)
#define TILESETS GAME_ADDR(0x020708d4u, 0x020708f4u)
#define ROOM GAME_ADDR(0x0207b518u, 0x0207b538u)
#define PAL_REMAP GAME_ADDR(0x02058714u, 0x02058744u)

static int ram_ptr(uint32_t p) { return p >> 24 == 0x02; }

static int field_rows; /* this frame's rows above and below the screen in the field (zoom out), 0 elsewhere */
static int view_x, view_y; /* the view's shift from the camera (the room's ends) for this frame's scroll */
static int next_x, next_y; /* and for the next frame's, from the camera in RAM */
static int oam_x, oam_y;   /* the one this frame's sprites were placed with (ws_sprite_lag) */
static int view_loaded;    /* a state was loaded: its sprite view says the shift */
static int view_bgs;       /* the BGs it moves */

/* The room map's metatile rows (room + 0x1000: 0x100 a row) as the field last had them. */
static struct {
    uint8_t rows[0x100 * 0x100];
    uint32_t of;       /* the room map they were copied from (0: none) */
    uint8_t masks[6];  /* and its map masks (the room) */
    int32_t after;     /* frames since a battle ended while the map still differs (0: no battle) */
} room_copy;

static void copy_room_rows(uint32_t scroll, uint32_t room) {
    uint8_t masks[6];
    int rows = 0;
    for (int i = 0; i < 6; i++) masks[i] = (uint8_t)rt_r8(scroll + 0x1a + i);
    for (int L = 0; L < 3; L++) rows = masks[2 * L + 1] + 1 > rows ? masks[2 * L + 1] + 1 : rows;
    uint32_t n = (uint32_t)rows * 0x100;
    if (room_copy.of == room && !memcmp(room_copy.masks, masks, 6)) {
        if (rt_overlay_resident(3)) { /* a battle: keep the field's */
            room_copy.after = 1;
            return;
        }
        if (room_copy.after) {
            uint32_t i = 0;
            while (i < n && room_copy.rows[i] == rt_r8(room + 0x1000 + i)) i++;
            if (i < n && room_copy.after++ < 600) return; /* not put back yet */
            room_copy.after = 0;
        }
    }
    for (uint32_t i = 0; i < n; i++) room_copy.rows[i] = (uint8_t)rt_r8(room + 0x1000 + i);
    room_copy.of = room;
    memcpy(room_copy.masks, masks, 6);
    room_copy.after = 0;
}

void ws_reset(void) { room_copy.of = 0; }

void ws_state(RtStateIO *s) {
    if (!rt_state_optional(s, "ws_room_copy", &room_copy, sizeof room_copy) && rt_state_loading(s)) room_copy.of = 0;
    if (rt_state_loading(s)) view_loaded = 1;
}

/* Layer L's camera: its top-left tile column and row in the room map, layer 0's (+4, +8). Layer 1 has its own
 * (+0xc, +0x10) where it scrolls slower: the moonlit sky beside the castle stairs, at about half the speed. Elsewhere
 * those read 0 (a castle hall whose layer 1 goes with layer 0), so layer 1 takes its own only while layer 0's does not
 * give what the game drew and its own does (own_l1, decided each frame in fill_field_margins). Layer 2 goes with
 * layer 0. */
static int own_l1;
static int layer_col(uint32_t scroll, int L) { return rt_r16(scroll + (L == 1 && own_l1 ? 0xc : 4)); }
static int layer_row(uint32_t scroll, int L) { return rt_r16(scroll + (L == 1 && own_l1 ? 0x10 : 8)); }

/* Packed tile (b0 | b1 << 8 | b2 << 16) of layer L at world tile (c, r). */
static uint32_t room_tile(uint32_t scroll, uint32_t tilesets, int L, int c, int r) {
    uint32_t i = ((uint32_t)(c >> 1) & rt_r8(scroll + 0x1a + 2 * L)) + (((uint32_t)(r >> 1) & rt_r8(scroll + 0x1b + 2 * L)) << 8);
    uint32_t id = room_copy.rows[0x40 * L + i];
    if (L < 2) id += ((room_copy.rows[0xc0 + i] >> L) & 1) << 8;
    uint32_t t = rt_r32(tilesets + (L < 2 ? 0xc : 0x10)) + (id * 4 + (c & 1) + 2 * (r & 1)) * 3;
    return rt_r8(t) | rt_r8(t + 1) << 8 | rt_r8(t + 2) << 16;
}

/* Layer L's tile row r, kept on its map when the game's own rows (from own, the view's 24) are: the zoomed-out view
 * shows rows beyond the map's top or bottom that the camera never reaches, and there the map would wrap round (the
 * canyon and sky behind Azala's tower, layer 1 on its own camera 16 rows from its map's top: the rocks at its bottom
 * stood above the sky). Its first or last row goes on instead. A layer whose own rows cross its map's end wraps. */
static int map_row(uint32_t scroll, int L, int r, int own) {
    int n = (rt_r8(scroll + 0x1b + 2 * L) + 1) * 2;
    if (own < 0 || own + SCREEN_H / 8 >= n) return r;
    return r < 0 ? 0 : r >= n ? n - 1 : r;
}

static uint16_t map_entry(int L, uint32_t t) {
    uint32_t b0 = t & 0xFF, b1 = (t >> 8) & 0xFF, pal = b1 >> 4;
    if (L == 2) pal = rt_r8(PAL_REMAP + pal);
    return (uint16_t)(b0 | (b1 & 3) << 8 | ((b1 >> 2) & 1) << 10 | ((b1 >> 3) & 1) << 11 | pal << 12);
}

static void put(int bg, int slot, int row, uint16_t v) {
    uint32_t dispcnt = io_get32(0), cnt = io_get16(0x08 + 2 * bg);
    uint32_t a = 0x06000000u + ((dispcnt >> 27) & 7) * 0x10000u + ((cnt >> 8) & 0x1F) * 0x800u + (uint32_t)(slot >> 5) * 0x800u +
                 (uint32_t)row * 64 + (uint32_t)(slot & 31) * 2;
    uint8_t *p = video_vram_ptr(a);
    if (p) memcpy(p, &v, 2);
}

/* The BG holds what the room map gives for layer L: the cells the game itself drew in the 4:3 view (the ring's
 * columns 1-30 from the origin, the screen's rows), priority tiles left out (BG1 holds those), agree with the model
 * (9 in 10, so a few cells the game changed on its own do not stop it). Not so on the prison bridge:
 * BG3 holds a fixed 256-wide sky (tiles 0x199.. in order) and BG0 is mostly empty, both at scroll 0, 0 and
 * so on their (unmoving) ring origins; filled from the room map, the margins showed that map scrolling with the
 * camera beside a still sky. */
static int bg_holds_layer(int bg, uint32_t scroll, uint32_t tilesets, int L) {
    int camc = layer_col(scroll, L), camr = layer_row(scroll, L), oc = rt_r8(scroll + 0x8e + 2 * L), orow = rt_r8(scroll + 0x8f + 2 * L);
    uint32_t dispcnt = io_get32(0), cnt = io_get16(0x08 + 2 * bg);
    uint32_t base = 0x06000000u + ((dispcnt >> 27) & 7) * 0x10000u + ((cnt >> 8) & 0x1F) * 0x800u;
    int cells = 0, same = 0;
    for (int k = 1; k < 31; k++) {
        int slot = (oc + k) & 63;
        for (int r = 0; r < SCREEN_H / 8; r++) {
            uint32_t t = room_tile(scroll, tilesets, L, camc + k, camr + r);
            if (L < 2 && (t >> 16) & 0xFF) continue;
            const uint8_t *p = video_vram_ptr(base + (uint32_t)(slot >> 5) * 0x800u + (uint32_t)((orow + r) & 31) * 64 + (uint32_t)(slot & 31) * 2);
            uint16_t v = 0;
            if (p) memcpy(&v, p, 2);
            cells++, same += v == map_entry(L, t);
        }
    }
    return cells && same * 10 >= cells * 9;
}

/* The BG agrees with layer L's ring origin and is a 512x256 text map. */
static int bg_follows(int bg, uint32_t scroll, int L) {
    uint16_t cnt = io_get16(0x08 + 2 * bg);
    return (cnt >> 14) == 1 && ((io_get16(0x10 + 4 * bg) >> 3) & 63) == rt_r8(scroll + 0x8e + 2 * L) &&
           ((io_get16(0x12 + 4 * bg) >> 3) & 31) == rt_r8(scroll + 0x8f + 2 * L);
}

/* The sprite renderer's view (NitroSDK G2D surface of the top screen, 0x02058070: top-left x, y, then width,
 * height, 20.12 fixed point; normally 0, -1, 256, 192). Sprites outside it are culled, and inside it they are
 * placed relative to its top-left. In the field it is widened to the margins, so characters beyond the 4:3 view
 * are drawn; their OAM X then counts from the left margin (ws_sprite_origin). Its top-left also moves with the
 * view's shift at the room's ends, so only its size tells how far it is widened. */
#define G2D_VIEW GAME_ADDR(0x02058070u, 0x020580a0u)
#define FX(v) ((uint32_t)((v) * 4096))

/* The field camera's top-left in the room, in pixels (0x020760f8: x, y; BG scroll moves with it). */
#define CAMERA GAME_ADDR(0x020760f8u, 0x02076118u)

#define CURRENT_SCENE GAME_ADDR(0x0207088cu, 0x020708acu)

int ws_camera(int *x, int *y) {
    /* The world map: the view's place in the world, layer 3's (x wraps at 0x600, y at 0x400). Layers 1-3 share it,
     * but where a layer scrolls on its own its place does too: Zeal's layers 1 and 2 are clouds drifting a pixel every
     * few frames over its still land on layer 3, and the smoothing glided the picture after them and back. */
    if (ss_in_overworld()) {
        uint32_t base = rt_r32(CURRENT_SCENE);
        if (!ram_ptr(base)) return 0;
        *x = rt_r16(base + 0xddc0u + 0x22a4 + 2 * 3); /* the world object (WORLD) */
        *y = rt_r16(base + 0xddc0u + 0x22ac + 2 * 3);
        return *x < 0x600 && *y < 0x400;
    }
    if (!ss_in_field()) return 0;
    *x = (int16_t)rt_r16(CAMERA);
    *y = (int16_t)rt_r16(CAMERA + 2);
    return 1;
}

/* The current location (the game's location id, as the SNES game numbers them: 0x1d0 the End of Time). */
#define LOCATION GAME_ADDR(0x020720a0u, 0x020720c0u)

/* Rooms whose bounds hold more than the room: the right bound (metatile column) the room really ends at. */
static const struct { uint16_t location; uint8_t right; } narrow_rooms[] = {
    {0x1d9, 15}, /* the Epoch parked (Take off / Get off): its map's right half holds a second Epoch */
};

/* Rooms looked at from their left end: the view stops there alone, the scene at the view's left border. */
static const uint16_t left_rooms[] = {
    0x16c, /* the Blackbird's wing: the party out of its door, on the plane's side, looks out over the clouds */
};

/* The room's fingerprint: its bounds and its map's first metatile row. A narrow room stays narrow while it
 * holds: taking off, the location already reads the destination for a few frames before the scene changes. */
static uint32_t room_print(uint32_t scroll) {
    uint32_t room = rt_r32(ROOM), h = rt_r32(scroll + 0x16);
    if (!ram_ptr(room)) return h;
    for (uint32_t i = 0; i < 0x40; i += 4) h = h * 31 + rt_r32(room + 0x1000 + i);
    return h;
}

/* The room is one of left_rooms, and stays one while it holds (room_print): leaving the Blackbird's wing, the
 * location reads the next scene's (0x16e) for a few frames, and the view went back to the middle. */
static int left_room(void) {
    static int held;
    static uint32_t held_print;
    uint32_t scroll = rt_r32(SCROLL);
    if (!ram_ptr(scroll)) return held = 0;
    uint32_t print = room_print(scroll);
    for (size_t i = 0; i < sizeof left_rooms / sizeof *left_rooms; i++)
        if (rt_r16(LOCATION) == left_rooms[i]) return held = 1, held_print = print, 1;
    return held = held && print == held_print;
}

int ws_room_span(int *x0, int *x1) {
    if (!ss_in_field()) return 0;
    uint32_t scroll = rt_r32(SCROLL);
    if (!ram_ptr(scroll)) return 0;
    /* The room's bounds in the map, metatiles (16 px), inclusive: left +0x16, right +0x17 (top +0x18, bottom
     * +0x19); the camera stays within them. The map can hold more than the room (neighbouring rooms beside a
     * house's), and wraps at its masks. */
    int left = rt_r8(scroll + 0x16), right = rt_r8(scroll + 0x17);
    static int narrow = -1;      /* the narrow room's right bound, -1: none */
    static uint32_t narrow_print; /* and its room_print */
    uint32_t print = room_print(scroll);
    int found = 0;
    for (size_t i = 0; i < sizeof narrow_rooms / sizeof *narrow_rooms; i++)
        if (rt_r16(LOCATION) == narrow_rooms[i].location) narrow = narrow_rooms[i].right, narrow_print = print, found = 1;
    if (!found && print != narrow_print) narrow = -1;
    if (narrow >= 0 && right > narrow) right = narrow;
    if (right < left) return 0;
    /* The view's left in the map: the camera's (BG0's scroll can move on its own, an animated layer). */
    int view = (int16_t)rt_r16(CAMERA);
    *x0 = left * 16 - view;
    *x1 = (right + 1) * 16 - view;
    return 1;
}

/* The tile (packed, room_tile) shows nothing but black on BG2, which holds layer 0. */
static int tile_black(uint32_t t) {
    uint16_t e = map_entry(0, t), cnt = io_get16(0x0c);
    if (cnt & 0x80) return 0;
    uint32_t chr = 0x06000000u + ((io_get32(0) >> 24) & 7) * 0x10000u + ((cnt >> 2) & 15) * 0x4000u + (e & 0x3FFu) * 32;
    for (int i = 0; i < 32; i++) {
        const uint8_t *p = video_vram_ptr(chr + (uint32_t)i);
        if (!p) return 0;
        for (int n = 0; n < 2; n++) {
            uint32_t ix = (*p >> (4 * n)) & 15, color = 0;
            if (ix) memcpy(&color, &rt_palette[(e >> 12) * 32 + ix * 2], 2);
            if (color & 0x7FFF) return 0;
        }
    }
    return 1;
}

/* A metatile row of the void between rooms: across the bounds, one black metatile on layer 0, nothing on the others.
 * Beyond the bounds (the row above their top, below their bottom), layer 2 may hold stray tiles on fewer than half
 * of the row's (the Millennial Fair's south end: specks under the black, which the camera never shows). */
static int void_row(uint32_t scroll, uint32_t tilesets, int left, int right, int r, int beyond) {
    uint32_t first[4];
    int stray = 0;
    for (int i = 0; i < 4; i++) {
        first[i] = room_tile(scroll, tilesets, 0, left * 2 + (i & 1), r * 2 + (i >> 1));
        if (!tile_black(first[i])) return 0;
    }
    for (int m = left; m <= right; m++) {
        for (int i = 0; i < 4; i++) {
            int c = m * 2 + (i & 1), row = r * 2 + (i >> 1);
            if (room_tile(scroll, tilesets, 0, c, row) != first[i]) return 0;
            if (room_tile(scroll, tilesets, 1, c, row) & 0x3FF) return 0;
            if (room_tile(scroll, tilesets, 2, c, row) & 0x3FF && (!beyond || ++stray * 2 >= (right - left + 1) * 4)) return 0;
        }
    }
    return 1;
}

/* A metatile column of the void, as void_row, over rows top..bottom. */
static int void_column(uint32_t scroll, uint32_t tilesets, int top, int bottom, int m) {
    uint32_t first[4];
    for (int i = 0; i < 4; i++) {
        first[i] = room_tile(scroll, tilesets, 0, m * 2 + (i & 1), top * 2 + (i >> 1));
        if (!tile_black(first[i])) return 0;
    }
    for (int r = top; r <= bottom; r++) {
        for (int i = 0; i < 4; i++) {
            int c = m * 2 + (i & 1), row = r * 2 + (i >> 1);
            if (room_tile(scroll, tilesets, 0, c, row) != first[i]) return 0;
            if (room_tile(scroll, tilesets, 1, c, row) & 0x3FF || room_tile(scroll, tilesets, 2, c, row) & 0x3FF) return 0;
        }
    }
    return 1;
}

/* The map beyond the room's left and right ends is the void, over the rows the view shows: Fill screen sides has
 * nothing to show there (a forest's west end: tile 0 on every layer, the map's other end wrapped round). */
static void void_beyond_ends(int *left_void, int *right_void) {
    *left_void = *right_void = 0;
    uint32_t scroll = rt_r32(SCROLL), tilesets = rt_r32(TILESETS);
    if (!ram_ptr(scroll) || !ram_ptr(tilesets) || room_copy.of != rt_r32(ROOM)) return;
    int left = rt_r8(scroll + 0x16), right = rt_r8(scroll + 0x17), top = rt_r8(scroll + 0x18), bottom = rt_r8(scroll + 0x19);
    int view = (int16_t)rt_r16(CAMERA + 2);
    int from = (view - field_rows) >> 4, to = (view + SCREEN_H - 1 + field_rows) >> 4;
    from = from < top ? top : from, to = to > bottom ? bottom : to;
    if (right < left || to < from) return;
    *left_void = void_column(scroll, tilesets, from, to, left - 1);
    *right_void = void_column(scroll, tilesets, from, to, right + 1);
}

/* The bounds can hold several rooms one above the other (the prison's towers), with the void between them; the
 * camera never shows the next one on the DS. So the rows end at the void on either side of the view's middle, though
 * never inside the 192 rows the game draws for itself. *top_void, *bottom_void (when not NULL): beyond that end of
 * the rows is the void (between rooms, or the map beyond the bounds). */
static int room_rows(int *y0, int *y1, int *top_void, int *bottom_void) {
    if (!ss_in_field()) return 0;
    uint32_t scroll = rt_r32(SCROLL), tilesets = rt_r32(TILESETS);
    if (!ram_ptr(scroll) || !ram_ptr(tilesets) || room_copy.of != rt_r32(ROOM)) return 0;
    int left = rt_r8(scroll + 0x16), right = rt_r8(scroll + 0x17), top = rt_r8(scroll + 0x18), bottom = rt_r8(scroll + 0x19);
    if (bottom < top || right < left) return 0;
    int view = (int16_t)rt_r16(CAMERA + 2), middle = (view + SCREEN_H / 2) >> 4;
    int from = top, to = bottom;
    if (middle >= top && middle <= bottom && !void_row(scroll, tilesets, left, right, middle, 0)) {
        int last_up = (view - field_rows) >> 4, last_down = (view + SCREEN_H - 1 + field_rows) >> 4;
        for (int r = middle - 1; r >= top && r >= last_up; r--) {
            if (void_row(scroll, tilesets, left, right, r, 0)) { from = r + 1; break; }
        }
        for (int r = middle + 1; r <= bottom && r <= last_down; r++) {
            if (void_row(scroll, tilesets, left, right, r, 0)) { to = r - 1; break; }
        }
    }
    *y0 = from * 16 - view;
    *y1 = (to + 1) * 16 - view;
    if (*y0 > 0 && from > top) *y0 = 0;
    if (*y1 < SCREEN_H && to < bottom) *y1 = SCREEN_H;
    /* The bounds at the map's own top or bottom: nothing of the map lies beyond (row -1 is the wrapped last one, and
     * the fill repeats the edge row there), so the view stops (a Zeal building whose roof is the map's first row). */
    int last = rt_r8(scroll + 0x1b); /* layer 0's map rows - 1 (metatiles) */
    if (top_void) *top_void = from > top || top == 0 || void_row(scroll, tilesets, left, right, top - 1, 1);
    if (bottom_void) *bottom_void = to < bottom || bottom >= last || void_row(scroll, tilesets, left, right, bottom + 1, 1);
    return 1;
}

int ws_room_rows(int *y0, int *y1) { return room_rows(y0, y1, NULL, NULL); }

int ws_sprite_origin(void) { /* the view as widened here, recognised by its shape (also after a state load) */
    uint32_t w = rt_r32(G2D_VIEW + 8);
    return w > FX(SCREEN_W) && w <= FX(SCREEN_W + 2 * VIDEO_MARGIN_MAX) ? (int)((w - FX(SCREEN_W)) / 8192) : 0;
}

int ws_sprite_rows(void) { return ws_sprite_origin() ? (int)((rt_r32(G2D_VIEW + 12) - FX(SCREEN_H)) / 8192) : 0; }

/* Widens the view to margin columns a side and rows above and below, moved by (dx, dy); 0 puts back a view widened
 * here (and leaves any other alone). */
static void set_sprite_view(int margin, int rows, int dx, int dy) {
    if (!margin && !ws_sprite_origin()) return;
    rt_w32(G2D_VIEW, FX(dx - margin));
    rt_w32(G2D_VIEW + 4, FX(dy - 1 - rows));
    rt_w32(G2D_VIEW + 8, FX(SCREEN_W + 2 * margin));
    rt_w32(G2D_VIEW + 12, FX(SCREEN_H + 2 * rows));
}

/* The field scene's renderer surface (scene secondary base *(0x0207088c) + 0xd748, view rect first). Its own
 * view is screen space: 0, 0, 1024 wide (no culling); the scene's draw (main 0x02041940) saves it, switches to
 * the G2D view above for world sprites (characters, gauges, damage numbers) and puts it back for the UI drawn in
 * screen coordinates (battle menus and their text, the command cursor). In the field that view starts at the
 * left margin, so the UI keeps its 4:3 place, centred. */
#define SCENE_SURFACE 0xd748u

static void set_screen_view(int margin) {
    uint32_t base = rt_r32(CURRENT_SCENE);
    if (!ram_ptr(base)) return;
    uint32_t s = base + SCENE_SURFACE;
    int32_t x = (int32_t)rt_r32(s);
    if (rt_r32(s + 4) || rt_r32(s + 8) != FX(1024) || x > 0 || x < -VIDEO_MARGIN_MAX * 4096) return; /* not its own view */
    rt_w32(s, FX(-margin));
}

/* A room one screen wide with a layer scrolling on its own (the clouds under the Epoch in flight: BG3 of a
 * 16-metatile room, its scroll moving down while the camera stays; the water around the parked Epoch: BG0, moving
 * both ways): that layer is a picture 256 wide, so its margins repeat it; beyond the room the other layers stay
 * empty (their map holds other rooms there, e.g. a sea of fire). The room's own layer (BG2) must agree with its
 * ring origin (a shaking screen moves every layer). Once seen off its ring origin, a layer keeps repeating while
 * it stays in the room: in passing, its scroll agrees with the origin for a frame (the clouds at VOFS 0 and 256),
 * which would blank the margins. So do BG2's own steps: walking, the origin in RAM is a tile ahead of the frame's
 * scroll for one frame at each tile (VOFS 198 with row 25), so what already repeats carries on through a BG2 a
 * tile off, and nothing starts repeating there. A layer blended onto a repeated one repeats with it (the parked Epoch's water
 * is BG0 with a grey BG3 added onto it, BLDCNT 0x014a: without it the sides were dimmer), and keeps repeating
 * while the room stays: as the Epoch warps, the game brightens instead (BLDCNT 0x00bf), and the opaque BG3 then
 * covers the water, as it must beside the screen too. */
static int repeat_bgs;
static uint32_t repeat_room; /* the room the layers were seen moving in (room map, bounds) */

int ws_repeat_bgs(void) { return repeat_bgs; }

static int picture_bgs;
int ws_picture_bgs(void) { return picture_bgs; }

/* The BG is within a tile of layer L's ring origin, both ways. */
static int bg_near(int bg, uint32_t scroll, int L) {
    int dc = (io_get16(0x10 + 4 * bg) >> 3) - rt_r8(scroll + 0x8e + 2 * L), dr = (io_get16(0x12 + 4 * bg) >> 3) - rt_r8(scroll + 0x8f + 2 * L);
    return ((dc + 1) & 63) <= 2 && ((dr + 1) & 31) <= 2;
}

/* The BG is a 512x256 text map within a tile of layer L's ring origin. */
static int bg_on_layer(int bg, uint32_t scroll, int L) {
    return (io_get16(0x08 + 2 * bg) >> 14) == 1 && bg_near(bg, scroll, L);
}

/* 0: not a 512x256 text BG; 1: one on layer L's ring origin; 2: one off it. */
static int scrolls_alone(int bg, uint32_t scroll, int L) {
    if ((io_get16(0x08 + 2 * bg) >> 14) != 1) return 0;
    return bg_follows(bg, scroll, L) ? 1 : 2;
}

static int field_margin; /* this frame's margin columns in the field, 0 elsewhere */

/* Field object visibility (ov004 0x02130dbc, r0 = object): true while the object's tile lies within the view
 * columns *(SCROLL)+4 - 4 .. *(SCROLL)+6 + 4 (clamped to the room) and the matching rows. Those two fields are
 * the camera's own (map streaming uses them too), so they are widened to the shown view only for the call: the
 * margins, moved by the view's shift at a room's end (the sprite view's, set_sprite_view). Widened by the shift on
 * both sides, the objects passed spanned more than the 512 px rt_ws_draw_cell tells apart. */
static void game_object_visible(CPU *cpu) { rt_call_wrapped(cpu, "ov004", 0x02130dbcu); } /* the original */
void rt_ws_object_visible(CPU *cpu) {
    uint32_t scroll = rt_r32(SCROLL);
    int dl = (next_x - field_margin) >> 3, dr = (next_x + field_margin + 7) >> 3; /* tiles, floor and ceiling */
    int dt = (next_y - field_rows) >> 3, db = (next_y + field_rows + 7) >> 3;
    if ((!field_margin && !field_rows && !next_x && !next_y) || !ram_ptr(scroll) || cpu->resume_pc) {
        game_object_visible(cpu);
        return;
    }
    int left = rt_r16(scroll + 4), right = rt_r16(scroll + 6), top = rt_r16(scroll + 8), bottom = rt_r16(scroll + 0xa);
    rt_w16(scroll + 4, (uint16_t)(left + dl > 0 ? left + dl : 0));
    rt_w16(scroll + 6, (uint16_t)(right + dr > 0 ? right + dr : 0));
    rt_w16(scroll + 8, (uint16_t)(top + dt > 0 ? top + dt : 0));
    rt_w16(scroll + 0xa, (uint16_t)(bottom + db > 0 ? bottom + db : 0));
    game_object_visible(cpu);
    rt_w16(scroll + 4, (uint16_t)left);
    rt_w16(scroll + 6, (uint16_t)right);
    rt_w16(scroll + 8, (uint16_t)top);
    rt_w16(scroll + 0xa, (uint16_t)bottom);
}

/* Zoom out: a sprite's Y in OAM has 8 bits, which told the DS's 192 rows and the sprites above them apart but not
 * the 248 rows here, and the text box's and menus' sprites must stay out of the zoomed picture. Both are known only
 * while the sprite is drawn, so they are kept in bits of attribute 1 that sprites without rotation leave unused
 * (WS_OAM_ABOVE, WS_OAM_UI; video.c reads them):
 *   - main 0x02019fd4 (r0 = the renderer surface, view rect first) draws a cell from the place at +0x10 of the current
 *     matrix (*(*(0x0201a3d4)) + 0x2c, or the default *(0x0201a3d8)) less the view's top-left, and its sprites lie
 *     within 8 bits of that: so it says which of the two a sprite's Y is;
 *   - the surface's own view is 1024 wide (screen coordinates: set_screen_view); world sprites use the G2D view;
 *   - in a battle, the scene's draw also uses the G2D view for its first list of sprites (main 0x02041940, the call
 *     returning to 0x02041a34), the HUD beside the menus: gauges, HP / MP, the menu's hand, one sprite each on the rows
 *     of the panel, at the top of the screen or the bottom. They become UI too, put back where the screen's own view
 *     would have them: the shift of the view at a room's ends is not theirs, with or without zoom out. The same list's
 *     other cells stay in the world: the target's bracket (4 sprites) and the hand over a target, the cell after it;
 *   - in the field, the scene's draw (main 0x0204171c) does the same for its list of 41 widgets before the characters
 *     (the call returning to 0x02041800): the hand of a text box's choices (the elevator's Up / Down / Cancel), placed
 *     in screen coordinates beside the box. They are UI, put back the same way;
 *   - in a battle, the numbers over the actors ("Miss!", damage) are UI objects drawn with the surface's own view (the
 *     scene's layer-1 objects, main 0x0203ce98: slots of 0x18 bytes at +0xb70 of the list at *(0x02041bd0), each with
 *     entries of 0x9c bytes, each entry drawn by main 0x020345dc), placed by main 0x0203cf4c (r1 = the slot) at the
 *     actor's place in screen coordinates (ov003 0x0214a2a2, from the SNES battle's screen X and Y). They go with the
 *     world: the view's shift at a room's ends is theirs (`world_slots`; they stood off their actor by it);
 *   - main 0x02048098 (r0 = the sprite's attributes) enters each sprite. While the byte at 0x020fd614 is set, it also
 *     chooses the sprite's priority by comparing its Y with the threshold at 0x020fd618, in the DS screen's rows, so
 *     for the call that threshold is put where the comparison comes out as it would have there. */
#define CELL_MATRIX_PTR 0x0201a3d4u
#define CELL_DEFAULT_MATRIX 0x0201a3d8u
#define OAM_PRIORITY_ON GAME_ADDR(0x020fd614u, 0x020fd634u)
#define OAM_PRIORITY_Y GAME_ADDR(0x020fd618u, 0x020fd638u)

#define BATTLE_HUD_LIST GAME_ADDR(0x02041a34u, 0x02041a64u)
#define BATTLE_HUD_TOP 132 /* the DS screen's row */
#define FIELD_UI_LIST GAME_ADDR(0x02041800u, 0x02041830u)

static int cell_y, cell_ui, cell_hud, cell_known;
static int cell_world; /* a UI object placed at an actor's place (world_slots) */

/* The scene's layer-1 objects placed at an actor's place, by slot; the entry being drawn is one of theirs. */
#define UI_OBJECTS GAME_ADDR(0x02041bd0u, 0x02041c00u) /* the list (a pointer to it) */
#define UI_PLACED_AT_ACTOR 0x0214a2a6u                  /* ov003, return address of the placing call */
static uint8_t world_slots[0x80];
static int drawing_world;

static uint32_t ui_slot(uint32_t list, uint32_t slot) { return list + 0xb70 + slot * 0x18; }

void rt_ws_place_ui(CPU *cpu) { /* main 0x0203cf4c (r0 = the list, r1 = the slot, r2 = x, r3 = y) */
    if (R[1] < sizeof world_slots) world_slots[R[1]] = rt_overlay_resident(3) && (R[14] & ~1u) == UI_PLACED_AT_ACTOR;
    rt_call_wrapped(cpu, "main", GAME_ADDR(0x0203cf4cu, 0x0203cf7cu));
}

/* The entry (r0) lies in a slot placed at an actor's place: its entries as main 0x0203ce98 counts them. */
static int entry_in_world_slot(uint32_t entry) {
    uint32_t list = rt_r32(UI_OBJECTS);
    if (!ram_ptr(list)) return 0;
    for (uint32_t i = 0; i < sizeof world_slots; i++) {
        if (!world_slots[i]) continue;
        uint32_t s = ui_slot(list, i), w = rt_r8(s + 0x14), base = rt_r32(s + 8);
        if (!rt_r8(s) || !w) continue;
        uint32_t n = ((rt_r16(s + 0xc) + w - 1) / w) * ((rt_r16(s + 0xe) + 7u) / 8);
        if (entry >= base && entry < base + n * 0x9c) return 1;
    }
    return 0;
}

void rt_ws_draw_ui_entry(CPU *cpu) { /* main 0x020345dc (r0 = the entry) */
    int was = drawing_world;
    drawing_world = (next_x || next_y) && entry_in_world_slot(R[0]);
    rt_call_wrapped(cpu, "main", GAME_ADDR(0x020345dcu, 0x020345e8u));
    drawing_world = was;
}

/* In the current call chain, within the scene's draw: the return address into it. */
static int called_from(const CPU *cpu, uint32_t ret) {
    for (uint32_t i = cpu->ret_depth, n = 0; i > 0 && n < 12; i--, n++) {
        if (cpu->ret_stack[i - 1] == ret) return 1;
    }
    return 0;
}

/* A cell of the HUD's list (cell: its entry in the 8-byte table, sprite count first) at this row on the DS screen. */
static int battle_hud_cell(const CPU *cpu, uint32_t cell, int row) {
    return rt_overlay_resident(3) && rt_r16(cell) == 1 && rt_r16(cell - 8) != 4 &&
           (row < SCREEN_H - BATTLE_HUD_TOP || row >= BATTLE_HUD_TOP) && called_from(cpu, BATTLE_HUD_LIST);
}

/* Actors that leave the DS screen and stop there: the game parks an actor just past its edge when it is done with it
 * (the dactyls flying off with the party, 16 rows above the screen until the fade), out of sight on the DS but in the
 * margins or the zoom out's rows here, where they hung at the view's edge. Such an actor is drawn going on at the
 * speed it had, until it is out of the view too: the drawing only, the game's place for it stays. Told by its sprite
 * instance (main 0x02019fd4's r1, the same every frame) when drawn as an actor (the call returning to 0x020482d4).
 * Its speed is that over its last DRIFT_SPAN moves (actors move and are drawn every other frame or so); not moved for
 * DRIFT_STILL frames it has stopped, and goes on from its last place while all its sprites are off the DS screen
 * there, having left it in those moves (one pacing beyond the screen stays put). Moved again, it is drawn where it is.
 * Its moves are in the room (its place from the camera, plus the camera), so the camera scrolling is none of them. Only
 * in DRIFT_LOCATION, the dactyls' scene: elsewhere actors stopped off the DS screen stay where the game has them (a
 * woman in the castle, left beyond the screen as the camera stopped at the room's left end, flew off to the right). */
#define ACTOR_CELL GAME_ADDR(0x020482d4u, 0x02048304u)
#define DRIFT_LOCATION 0x127
#define DRIFT_SLOTS 32
#define DRIFT_SPAN 8
#define DRIFT_STILL 6
#define DRIFT_GAP 4 /* frames an instance may go undrawn and still be the same */
typedef struct Drift {
    uint32_t cell, seen;           /* the sprite instance, the frame it was last drawn */
    int32_t x[DRIFT_SPAN + 1], y[DRIFT_SPAN + 1]; /* its places (fixed point), the last first */
    uint32_t at[DRIFT_SPAN + 1];   /* the frames it moved to them */
    int moves;                     /* places kept (up to DRIFT_SPAN + 1) */
    int off, l, t, r, b;           /* its sprites were all off the DS screen; their extent there this frame */
    int on;                        /* it was on the DS screen since these moves began */
} Drift;
static Drift drifts[DRIFT_SLOTS], *drawn_drift;
static int drift_dx, drift_dy; /* the drawn instance's drift (pixels) */
static uint32_t ws_frame; /* counts ws_fill_margins calls: the game's frames */

static Drift *drift_of(uint32_t cell) {
    Drift *free = NULL;
    for (int i = 0; i < DRIFT_SLOTS; i++) {
        Drift *d = &drifts[i];
        if (d->cell == cell && ws_frame - d->seen <= DRIFT_GAP) return d;
        if (!free || ws_frame - d->seen > ws_frame - free->seen) free = d;
    }
    memset(free, 0, sizeof *free);
    free->cell = cell, free->seen = ws_frame - DRIFT_GAP - 1; /* new */
    return free;
}

/* How far to draw the actor's instance from its place (fixed point): its drift this frame. */
static void drift_by(uint32_t cell, int32_t x, int32_t y, int32_t *dx, int32_t *dy) {
    Drift *d = drift_of(cell);
    drawn_drift = d;
    if (d->seen != ws_frame) { /* its first draw this frame */
        int fresh = ws_frame - d->seen > DRIFT_GAP;
        int drawn = d->r > d->l;
        if (drawn) d->off = d->r <= 0 || d->l >= SCREEN_W || d->b <= 0 || d->t >= SCREEN_H; /* last frame's (none: culled, gone on) */
        d->l = d->t = INT32_MAX, d->r = d->b = INT32_MIN;
        d->seen = ws_frame;
        if (fresh || x != d->x[0] || y != d->y[0]) { /* new, or moving (again after a stop: its speed anew) */
            if (fresh || ws_frame - d->at[0] >= DRIFT_STILL) d->moves = 0, d->on = 0;
            memmove(d->x + 1, d->x, DRIFT_SPAN * sizeof *d->x), memmove(d->y + 1, d->y, DRIFT_SPAN * sizeof *d->y);
            memmove(d->at + 1, d->at, DRIFT_SPAN * sizeof *d->at);
            d->x[0] = x, d->y[0] = y, d->at[0] = ws_frame;
            if (d->moves <= DRIFT_SPAN) d->moves++;
        }
        if (drawn && !d->off) d->on = 1; /* not one pacing beyond the screen */
    }
    *dx = *dy = 0;
    uint32_t still = ws_frame - d->at[0], span = d->at[0] - d->at[DRIFT_SPAN];
    if (d->moves > DRIFT_SPAN && still >= DRIFT_STILL && d->off && d->on && span) {
        int64_t t = still < 1024 ? still : 1024; /* 1024 frames: far beyond any view */
        *dx = (int32_t)((int64_t)(d->x[0] - d->x[DRIFT_SPAN]) * t / span);
        *dy = (int32_t)((int64_t)(d->y[0] - d->y[DRIFT_SPAN]) * t / span);
    }
}

static void game_draw_cell(CPU *cpu) { rt_call_wrapped(cpu, "main", 0x02019fd4u); } /* the original */
void rt_ws_draw_cell(CPU *cpu) {
    if ((!field_rows && !next_x && !next_y) || cpu->resume_pc) {
        game_draw_cell(cpu);
        return;
    }
    uint32_t surface = R[0], core = rt_r32(rt_r32(CELL_MATRIX_PTR)), matrix = ram_ptr(core) ? rt_r32(core + 0x2c) : 0;
    if (!matrix) matrix = rt_r32(CELL_DEFAULT_MATRIX);
    cell_y = (int32_t)(rt_r32(matrix + 0x14) - rt_r32(surface + 4)) / 4096;
    cell_ui = rt_r32(surface + 8) == FX(1024);
    /* Another surface (the bottom screen's) is none of this. */
    cell_known = cell_ui || (rt_r32(surface) == rt_r32(G2D_VIEW) && rt_r32(surface + 8) == rt_r32(G2D_VIEW + 8));
    cell_hud = !cell_ui && (battle_hud_cell(cpu, R[1], cell_y - field_rows + next_y) || called_from(cpu, FIELD_UI_LIST));
    cell_world = cell_ui && drawing_world;
    /* The game places an object's cell at its X from the camera in 9 bits, -128..383 (the DS screen's middle +-256):
     * at a room's left end the view's shift and the right margin reach past 383 (85 + 93 px beyond the 256), and the
     * creatures there showed at the left, culled, until the camera came closer. Of X and X +- 512, the one nearest
     * the view's middle (the objects drawn lie within 512 px around it, rt_ws_object_visible). */
    uint32_t place = rt_r32(matrix + 0x10);
    if (!cell_ui && cell_known && field_margin) {
        int64_t x = (int32_t)place, middle = (int64_t)(int32_t)rt_r32(surface) + (int32_t)rt_r32(surface + 8) / 2;
        int64_t best = x;
        for (int k = -1; k <= 1; k += 2) {
            int64_t c = x + (int64_t)k * FX(512);
            if (llabs(c - middle) < llabs(best - middle)) best = c;
        }
        if (best != x) rt_w32(matrix + 0x10, (uint32_t)(int32_t)best);
    }
    uint32_t height = rt_r32(matrix + 0x14);
    drawn_drift = NULL;
    if (!cell_ui && cell_known && rt_r16(LOCATION) == DRIFT_LOCATION && called_from(cpu, ACTOR_CELL)) {
        int32_t dx, dy, room_x = (int32_t)rt_r32(matrix + 0x10) + FX((int16_t)rt_r16(CAMERA));
        drift_by(R[1], room_x, (int32_t)height + FX((int16_t)rt_r16(CAMERA + 2)), &dx, &dy);
        if (dx || dy) {
            rt_w32(matrix + 0x10, rt_r32(matrix + 0x10) + (uint32_t)dx), rt_w32(matrix + 0x14, height + (uint32_t)dy);
            cell_y += dy / 4096;
        }
        drift_dy = dy / 4096, drift_dx = dx / 4096;
    }
    game_draw_cell(cpu);
    rt_w32(matrix + 0x10, place);
    rt_w32(matrix + 0x14, height);
    drawn_drift = NULL;
    cell_known = cell_world = 0;
}

static void game_enter_oam(CPU *cpu) { rt_call_wrapped(cpu, "main", GAME_ADDR(0x02048098u, 0x020480c8u)); } /* the original */
void rt_ws_enter_oam(CPU *cpu) {
    uint32_t e0 = rt_r16(R[0]), e1 = rt_r16(R[0] + 2);
    if (drawn_drift && !cpu->resume_pc && !(e0 & 0x100)) { /* its place on the DS screen, without the drift */
        static const uint8_t w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
        static const uint8_t h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
        int shape = (e0 >> 14) % 3, size = e1 >> 14, y = e0 & 0xFF, x = e1 & 0x1FF;
        int row = cell_y + (int8_t)(y - cell_y) - field_rows + next_y - drift_dy;
        int col = (x >= 384 ? x - 512 : x) - field_margin + next_x - drift_dx;
        Drift *d = drawn_drift;
        if (col < d->l) d->l = col;
        if (col + w[shape][size] > d->r) d->r = col + w[shape][size];
        if (row < d->t) d->t = row;
        if (row + h[shape][size] > d->b) d->b = row + h[shape][size];
    }    if ((!field_rows && !cell_hud && !cell_world) || !cell_known || cpu->resume_pc) {
        game_enter_oam(cpu);
        return;
    }
    uint32_t a = R[0];
    uint32_t a0 = rt_r16(a), a1 = rt_r16(a + 2) & ~(WS_OAM_UI | WS_OAM_ABOVE | WS_OAM_PLACED);
    int y = a0 & 0xFF, real = cell_y + (int8_t)(y - cell_y); /* in the view, from its top */
    int affine = a0 & 0x100, shown = cell_ui ? y : real - field_rows + next_y; /* its row on the DS screen */
    if (!affine) {
        if (cell_hud) { /* back to where the screen's own view would have put it */
            a0 = (a0 & ~0xFFu) | ((uint32_t)shown & 0xFF);
            a1 = (a1 & ~0x1FFu) | ((a1 + (uint32_t)next_x) & 0x1FF);
        }
        if (cell_world) { /* moved with the world's view instead (still in screen coordinates) */
            shown -= next_y;
            a0 = (a0 & ~0xFFu) | ((uint32_t)shown & 0xFF);
            a1 = (a1 & ~0x1FFu) | ((a1 - (uint32_t)next_x) & 0x1FF);
        }
        if (cell_ui || cell_hud) a1 |= WS_OAM_UI | (cell_world ? WS_OAM_PLACED : 0);
        else if (real < 0) a1 |= WS_OAM_ABOVE;
        else if (real >= SCREEN_H + 2 * field_rows) a0 |= 0x200; /* beyond the view: hidden */
        rt_w16(a, (uint16_t)a0);
        rt_w16(a + 2, (uint16_t)a1);
    }
    uint32_t threshold = rt_r32(OAM_PRIORITY_Y);
    int choose = rt_r8(OAM_PRIORITY_ON), read = y >= 0xC0 ? y - 256 : y;
    if (choose) rt_w32(OAM_PRIORITY_Y, (uint32_t)(shown > (int32_t)threshold ? read - 1 : read));
    game_enter_oam(cpu);
    if (choose) rt_w32(OAM_PRIORITY_Y, threshold);
}

#define WORLD 0xddc0u

static int world_margin; /* this frame's margin columns in the world map, 0 elsewhere */
static int world_rows;   /* and its rows above and below the screen (zoom out) */

/* A piece's row must be told from 8 bits, and a piece (16 rows) above the view can show as well as one below. Zoomed
 * out the two ends take what does not fit evenly, a few of a piece's rows at the view's edge. */
int ws_world_sprite_limit(int rows) {
    int end = SCREEN_H + rows, over = end + rows + 15 - 256;
    return over > 0 ? end - over / 2 : end;
}

/* The SNES-style code's 16-bit compare (main 0x020305ac, r0 = the accumulator's address, r1 = the operand): sets
 * the emulated flags (0x0207b43c: carry bit 0, zero bit 1, negative bit 7) from A - M. Its calls in the world map's
 * X tests (by return address: the sprite pieces' X + 15 against 0x10f, ov005; the objects' X - camera + 16 against
 * 0x120, ov002, negative values wrapped at 0x600) compare A + margin against M + 2 * margin instead, a value just
 * below zero (the left margin) taken as negative. */
#define SNES_FLAGS GAME_ADDR(0x0207b43cu, 0x0207b45cu)
#define SPRITE_X_TEST 0x0212c0e4u
#define OBJECT_X_TEST GAME_ADDR(0x0210ce50u, 0x0210ce70u)
#define SPRITE_Y_TEST 0x0212c178u
#define OBJECT_Y_TEST GAME_ADDR(0x0210ce80u, 0x0210cea0u)

void rt_ws_snes_cmp(CPU *cpu) {
    uint32_t a = rt_r16(R[0]), m = R[1] & 0xFFFF, wrap = 0, w = 0;
    if (world_margin && R[14] == SPRITE_X_TEST && rt_overlay_resident(5)) wrap = 0x10000, w = (uint32_t)world_margin;
    if (world_margin && R[14] == OBJECT_X_TEST) wrap = 0x600, w = (uint32_t)world_margin;
    if (world_rows && R[14] == OBJECT_Y_TEST) wrap = 0x400, w = (uint32_t)world_rows;
    if (wrap) {
        a = (a >= wrap - w ? a - wrap : a) + w; /* the left margin's values (just below zero) wrapped back */
        a &= 0xFFFF;
        m = (m + 2 * w) & 0xFFFF;
    }
    if (world_rows && R[14] == SPRITE_Y_TEST && rt_overlay_resident(5)) { /* within what 8 bits can tell */
        a = (a - 15 + 256 - (uint32_t)ws_world_sprite_limit(world_rows)) & 0xFFFF;
        m = 256;
    }
    uint32_t d = a + (~m & 0xFFFF) + 1;
    rt_w8(SNES_FLAGS, (rt_r8(SNES_FLAGS) & 0x7c) | ((d >> 16) & 1) | ((d & 0xFFFF) == 0) << 1 | ((d & 0x8000) >> 8));
}

/* World map sprite piece to DS (ov002 0x0211cc20, r2 = X, signed 16-bit). */
static void game_world_sprite(CPU *cpu) { rt_call_wrapped(cpu, "ov002", GAME_ADDR(0x0211cc20u, 0x0211cc40u)); } /* the original */
void rt_ws_world_sprite(CPU *cpu) {
    if (world_margin && !cpu->resume_pc && (int16_t)R[2] < -128) R[2] = (uint32_t)((int16_t)R[2] + 512) & 0xFFFF;
    if (world_rows && !cpu->resume_pc && (int16_t)R[3] >= ws_world_sprite_limit(world_rows)) R[3] = (uint32_t)((int16_t)R[3] - 256) & 0xFFFF;
    game_world_sprite(cpu);
}

static void fill_world_margins(int margin, int rows) {
    uint32_t base = rt_r32(CURRENT_SCENE);
    if (!ram_ptr(base)) return;
    uint32_t w = base + WORLD;
    for (int L = 1; L < 4; L++) {
        int x = rt_r16(w + 0x22a4 + 2 * L), y = rt_r16(w + 0x22ac + 2 * L);
        uint32_t map = rt_r32(w + 0x22b4 + 4 * L), sheet = w + 0x2a4 + 0x800u * (uint32_t)L;
        if (!ram_ptr(map) || x >= 0x600 || y >= 0x400 || (io_get16(0x08 + 2 * L) >> 14) != 1 ||
            (io_get16(0x10 + 4 * L) & 511) != (x & 511) || (io_get16(0x12 + 4 * L) & 255) != (y & 255))
            continue;
        int own = x >> 4, left = (x - margin) >> 4, right = (x + SCREEN_W - 1 + margin) >> 4;
        int own_top = y >> 4, own_bottom = (y + SCREEN_H - 1) >> 4;
        for (int c = left; c <= right; c++) {
            int wc = (c % 0x60 + 0x60) % 0x60;
            /* By tile row: zoomed out, a metatile row may show at both edges of the ring, a tile row of it at each. */
            for (int t = (y - rows) >> 3; t <= (y + SCREEN_H - 1 + rows) >> 3; t++) {
                int r = t >> 1;
                if (c >= own && c <= own + 16 && r >= own_top && r <= own_bottom) continue; /* the game's own */
                uint32_t id = rt_r8(map + (uint32_t)(r & 63) * 0x60 + (uint32_t)wc);
                uint32_t e = sheet + ((id >> 4) * 2) * 64 + (id & 15) * 4 + (uint32_t)(t & 1) * 64;
                int slot = (c * 2) & 63, row = t & 31;
                put(L, slot, row, (uint16_t)rt_r16(e));
                put(L, slot + 1, row, (uint16_t)rt_r16(e + 2));
            }
        }
    }
}

/* The view's shift at a room's end moves each BG by its own share of it: a layer on its own camera scrolls at its
 * rate to the room's (the sky behind Azala's tower at half), so it moves as the camera would have moved it. Moved by
 * the whole shift, the sky crept the wrong way while the view held still at the room's bottom end, then turned. The
 * rate is measured by axis, in 1/256: how far each BG's scroll moved while BG2's (the room) did, since the room was
 * entered, the frames where a BG holds another map (a text box) or jumps left out. Before the room has moved much,
 * layer 1 on its own camera starts from its place against the room's (its tile row and column over layer 0's). */
#define RATE_PRIOR 16 /* the pixels of the room's motion that start is worth */
static struct {
    uint32_t room;           /* the room measured (room map, bounds) */
    uint16_t last[4][2];     /* scroll by BG, axis, the frame before */
    uint16_t cnt[4];         /* BGxCNT the frame before */
    int32_t moved[4][2];     /* each BG's motion along the room's */
    int32_t size[4][2];      /* and the room's over the same frames */
    int rate[4][2];
} rates;

static void measure_rates(uint32_t scroll, uint32_t room) {
    uint32_t here = room ^ rt_r32(scroll + 0x16);
    int fresh = here != rates.room;
    if (fresh) memset(&rates, 0, sizeof rates), rates.room = here;
    for (int a = 0; a < 2; a++) {
        int d[4], ok[4];
        for (int bg = 0; bg < 4; bg++) {
            uint16_t cur = io_get16(0x10 + 4 * bg + 2 * a) & 511, cnt = io_get16(0x08 + 2 * bg);
            d[bg] = ((cur - rates.last[bg][a] + 256) & 511) - 256;
            ok[bg] = !fresh && cnt == rates.cnt[bg] && (cnt & 0x4000) && abs(d[bg]) <= 16;
            rates.last[bg][a] = cur;
        }
        if (ok[2] && d[2]) {
            for (int bg = 0; bg < 4; bg++) if (ok[bg]) rates.moved[bg][a] += d[2] > 0 ? d[bg] : -d[bg], rates.size[bg][a] += abs(d[2]);
        }
        int pos0 = a ? layer_row(scroll, 0) : layer_col(scroll, 0), pos1 = a ? layer_row(scroll, 1) : layer_col(scroll, 1);
        for (int bg = 0; bg < 4; bg++) {
            int prior = 256;
            if (bg == 3 && own_l1 && pos0 >= 8 && pos1 >= 0 && pos1 <= 2 * pos0) prior = pos1 * 256 / pos0;
            rates.rate[bg][a] = bg == 2 ? 256 : (prior * RATE_PRIOR + rates.moved[bg][a] * 256) / (RATE_PRIOR + rates.size[bg][a]);
        }
    }
    for (int bg = 0; bg < 4; bg++) rates.cnt[bg] = io_get16(0x08 + 2 * bg);
}

/* The share of the view's shift v that BG bg moves by along axis a. */
static int bg_shift(int bg, int a, int v) {
    int r = rates.room ? rates.rate[bg][a] : 256, p = v * r;
    return (p + (p >= 0 ? 128 : -128)) / 256;
}

void ws_bg_shift(int bg, int *x, int *y) {
    *x = bg_shift(bg, 0, view_x), *y = bg_shift(bg, 1, view_y);
}

static void fill_field_margins(int margin, int rows) {
    int seen = repeat_bgs;
    repeat_bgs = picture_bgs = view_bgs = 0;
    if (!field_margin) return;
    uint32_t scroll = rt_r32(SCROLL), room = rt_r32(ROOM), tilesets = rt_r32(TILESETS);
    if (!ram_ptr(scroll) || !ram_ptr(room) || !ram_ptr(tilesets)) return;
    /* A BG on its layer's ring origin, or a tile off it for a frame at each step, shows the room. */
    view_bgs = bg_near(2, scroll, 0) << 2 | bg_near(3, scroll, 1) << 3 | bg_near(0, scroll, 2) | bg_near(1, scroll, 0) << 1;
    /* So does a battle effect on a 512x512 map (the room's are 512x256 rings), drawn where the game's screen has its
     * target: Frog's Water at a room's end fell beside the enemy, on the party. */
    for (int bg = 0; bg < 4; bg++) view_bgs |= ((io_get16(0x08 + 2 * bg) >> 14) == 3) << bg;
    int x0, x1;
    int one_screen = ws_room_span(&x0, &x1) && x0 == 0 && x1 == SCREEN_W;
    if (one_screen) {
        uint32_t here = room ^ rt_r32(scroll + 0x16); /* the room's bounds */
        if (here != repeat_room) seen = 0, repeat_room = here;
        if (!bg_follows(2, scroll, 0) && bg_near(2, scroll, 0)) repeat_bgs = seen;
    }
    if (one_screen && bg_follows(2, scroll, 0)) {
        static const int layer_of[4] = {2, -1, -1, 1}; /* BG1: priority tiles; BG2: the room itself */
        for (int bg = 0; bg < 4; bg++) {
            int s = layer_of[bg] < 0 ? 0 : scrolls_alone(bg, scroll, layer_of[bg]);
            if (s == 2 || (s == 1 && (seen >> bg & 1))) repeat_bgs |= 1 << bg;
        }
        uint16_t bld = io_get16(0x50);
        if (((bld >> 6) & 3) == 1 && (bld >> 8 & repeat_bgs)) repeat_bgs |= bld & 9; /* BG0, BG3 blended onto them */
    }
    /* A layer scrolling on its own is no room layer, even while its scroll passes its ring origin (the stars of a
     * one-screen battle room, every 256 frames for 12): moved by the view's shift then, it jumped up and back. */
    view_bgs &= ~repeat_bgs;
    /* Walking, the origins in RAM are a tile ahead of the frame's scroll for one frame at each tile: that frame shows
     * a column or row the fill must have written too, so a BG a tile off is filled, by its own scroll. */
    int ok0 = bg_on_layer(2, scroll, 0), ok1 = bg_on_layer(3, scroll, 1), ok2 = bg_on_layer(0, scroll, 2);
    /* Layer 1 drifting: BG3's scroll moves while the camera and BG2's hold still (the sky behind Azala's tower moves
     * only with the camera; a shaking screen moves BG2 too). Its own map loops seamlessly: the clouds past the Blackbird's wing, a 256-row loop the game scrolls through
     * BG3's map. Filled only while that scroll passed near the layer's place in the room, the rows beyond the screen
     * jumped for those frames (from the map's last row on). Left to the game while the room holds. */
    static uint32_t drift_room;
    static int drift, last_cam, last_vofs, last_room_vofs;
    uint32_t here = room ^ rt_r32(scroll + 0x16);
    int cam = (int16_t)rt_r16(CAMERA + 2), vofs = io_get16(0x1e) & 511, room_vofs = io_get16(0x1a) & 511;
    if (here != drift_room) drift = 0, drift_room = here;
    else if (cam == last_cam && room_vofs == last_room_vofs && vofs != last_vofs) drift = 1;
    last_cam = cam, last_vofs = vofs, last_room_vofs = room_vofs;
    if (drift) ok1 = 0;
    if (!ok0 && !ok1 && !ok2) return;
    copy_room_rows(scroll, room);
    ok0 = ok0 && bg_holds_layer(2, scroll, tilesets, 0);
    own_l1 = 0;
    int held1 = ok1 && bg_holds_layer(3, scroll, tilesets, 1);
    if (ok1 && !held1 && (rt_r16(scroll + 0xc) != rt_r16(scroll + 4) || rt_r16(scroll + 0x10) != rt_r16(scroll + 8))) {
        own_l1 = 1;
        held1 = bg_holds_layer(3, scroll, tilesets, 1);
        own_l1 = held1;
    }
    measure_rates(scroll, room); /* layer 1's own camera known */
    int pic1 = ok1 && !held1, pic2 = ok2 && !bg_holds_layer(0, scroll, tilesets, 2);
    ok1 = ok1 && !pic1, ok2 = ok2 && !pic2;
    /* At scroll 0, or a few pixels off it while the screen shakes (the Dragon Tank's charge). */
    picture_bgs = (pic1 && !(io_get16(0x1c) & 0x1F8)) << 3 | (pic2 && !(io_get16(0x10) & 0x1F8));
    view_bgs &= ~(pic1 << 3 | pic2);
    int okhi = ok0 && bg_on_layer(1, scroll, 0); /* BG1: the priority tiles of the filled layers 0 and 1 */
    int camc = layer_col(scroll, 0), camr = layer_row(scroll, 0);
    int dc1 = layer_col(scroll, 1) - camc, dr1 = layer_row(scroll, 1) - camr; /* layer 1's tiles, from its own camera */
    /* The tiles the filled layers show: they agree on the tile, not always on the pixel (Zenan Bridge: BG0 still
     * at 0, 0, BG2 and BG3 6 rows down, so they show a 25th row). */
    int left = camc + 1, right = camc - 1, top = camr, bottom = camr;
    for (int bg = 0; bg < 4; bg++) {
        if (!(bg == 2 ? ok0 : bg == 3 ? ok1 : bg == 0 ? ok2 : okhi)) continue;
        int hofs = io_get16(0x10 + 4 * bg), vofs = io_get16(0x12 + 4 * bg), L = bg == 2 || bg == 1 ? 0 : bg == 3 ? 1 : 2;
        int fine = hofs & 7, vfine = vofs & 7;
        int c0 = camc + ((((hofs >> 3) - rt_r8(scroll + 0x8e + 2 * L) + 1) & 63) - 1); /* the column and row it starts at */
        int r0 = camr + ((((vofs >> 3) - rt_r8(scroll + 0x8f + 2 * L) + 1) & 31) - 1);
        int vx = bg_shift(bg, 0, view_x), vy = bg_shift(bg, 1, view_y);
        int l = c0 + ((fine - margin + vx) >> 3), r = c0 + ((fine + SCREEN_W - 1 + margin + vx) >> 3);
        int t = r0 + ((vfine - rows + vy) >> 3), b = r0 + ((vfine + SCREEN_H - 1 + rows + vy) >> 3);
        left = l < left ? l : left, right = r > right ? r : right, bottom = b > bottom ? b : bottom, top = t < top ? t : top;
    }
    for (int c = left; c <= right; c++) {
        for (int r = top; r <= bottom; r++) {
            /* The game's own: its columns, the rows of the 4:3 view and the 25th that a fine scroll shows. Told by their
             * place in the ring: while a room loads, the camera can be outside the new room's bounds for a few frames
             * (0, 16 after the Lab 32 race), and the view's shift to the room's ends then takes the range around the
             * ring, onto the game's own cells. */
            if (((c - camc) & 63) < 32 && ((r - camr) & 31) <= SCREEN_H / 8) continue;
            uint32_t t0 = room_tile(scroll, tilesets, 0, c, map_row(scroll, 0, r, camr));
            uint32_t t1 = room_tile(scroll, tilesets, 1, c + dc1, map_row(scroll, 1, r + dr1, camr + dr1));
            int pri0 = (t0 >> 16) & 0xFF, pri1 = (t1 >> 16) & 0xFF;
            for (int L = 0; L < 3; L++) {
                static const int bg_of[3] = {2, 3, 0};
                int ok = L == 0 ? ok0 : L == 1 ? ok1 : ok2;
                if (!ok) continue;
                int slot = (rt_r8(scroll + 0x8e + 2 * L) + c - camc) & 63, row = (rt_r8(scroll + 0x8f + 2 * L) + r - camr) & 31;
                uint32_t t = L == 0 ? t0 : L == 1 ? t1 : room_tile(scroll, tilesets, 2, c, map_row(scroll, 2, r, camr));
                put(bg_of[L], slot, row, L == 0 && pri0 ? 0 : L == 1 && pri1 ? 0 : map_entry(L, t));
                if (L == 0 && okhi) put(1, slot, row, ok1 && pri1 ? map_entry(1, t1) : pri0 ? map_entry(0, t0) : 0);
            }
        }
    }
}

/* How far a view of size + 2 * pad, around a camera's 0..size-1, moves to stay within the room's lo..hi-1, at the
 * ends it stops at (at_lo, at_hi); a room smaller than the view, stopping at both, is centred in it. */
static int view_shift(int lo, int hi, int size, int pad, int at_lo, int at_hi) {
    if (at_lo && at_hi && hi - lo <= size + 2 * pad) return (lo + hi - size) >> 1;
    return at_lo && lo + pad > 0 ? lo + pad : at_hi && hi - size - pad < 0 ? hi - size - pad : 0;
}

void ws_fill_margins(int margin, int rows, int limit, int limit_rows, int fill) {
    ws_frame++;
    world_margin = margin > 0 && ss_in_overworld() ? margin : 0;
    world_rows = world_margin ? rows : 0;
    if (world_margin) fill_world_margins(margin, rows);
    int field = margin > 0 && ss_in_field();
    field_margin = field ? margin : 0;
    field_rows = field ? rows : 0;
    if (view_loaded) {
        int origin = ws_sprite_origin();
        view_x = next_x = origin ? (int32_t)rt_r32(G2D_VIEW) / 4096 + origin : 0;
        view_y = next_y = origin ? (int32_t)rt_r32(G2D_VIEW + 4) / 4096 + 1 + ws_sprite_rows() : 0;
        view_loaded = 0;
    }
    oam_x = view_x, oam_y = view_y;
    view_x = next_x, view_y = next_y;
    if (ss_in_field()) set_screen_view(field_margin);
    fill_field_margins(margin, rows);
    next_x = next_y = 0;
    int lo, hi;
    if (field && limit) {
        int at_lo = 1, at_hi = 1;
        /* Fill screen sides: only where it has nothing to show. A layer repeating into the margins (the clouds of
         * the Epoch's flight) has the rest black beyond the room all the same (video.c room_clip): there as without. */
        int fills = fill && !repeat_bgs;
        if (fills) void_beyond_ends(&at_lo, &at_hi);
        if (left_room()) at_lo = 1, at_hi = 0;
        if (ws_room_span(&lo, &hi)) next_x = view_shift(lo, hi, SCREEN_W, limit, at_lo, at_hi);
        if (limit_rows && room_rows(&lo, &hi, &at_lo, &at_hi)) {
            if (!fills) at_lo = at_hi = 1;
            next_y = view_shift(lo, hi, SCREEN_H, limit_rows, at_lo, at_hi);
        }
    }
    set_sprite_view(field_margin, field_rows, next_x, next_y);
}

void ws_view_shift(int *x, int *y, int *bgs) {
    *x = view_x, *y = view_y, *bgs = view_bgs;
}

/* The game moves its camera after the sprite view is set here, then places the sprites with that camera and this
 * view; they are on screen the frame after, with the BGs of that camera. So a frame's sprites have the view of two
 * frames' starts ago, the BGs that of the last one. While the shift changes (the view reaching a room's end) the
 * sprites are that much off. */
void ws_sprite_lag(int *x, int *y) {
    *x = oam_x - view_x, *y = oam_y - view_y;
}

/* The game's software HDMA (the HBlank handler, ITCM 0x01ff8580): channels of 0x28 bytes from 0x027e0068 up to
 * *(0x02057944): +0 flags (bit 0 on, bit 1 words, bit 3 no source step per line), +4 source, +8 destination,
 * +0x14 source step per line, +0x1c units, +0x20 / +0x24 first / last line. Before each line it copies the line's
 * entry to the destination. Battle effects drawn with the windows (the rays of Lightning II...) use one with
 * WIN0H/WIN1H as one word a line; the portal Magus opens in his castle one halfword channel each (WIN0H: the
 * growing disc, WIN1H: a frame with rounded corners), with BG0's scroll set line by line (the disc's ripples). */
#define HDMA_CHANNELS 0x027e0068u
#define HDMA_END GAME_ADDR(0x02057944u, 0x02057974u)

int ws_window_lines(uint32_t h[SCREEN_H]) {
    uint32_t end = rt_r32(HDMA_END);
    if (end < HDMA_CHANNELS || end > HDMA_CHANNELS + 0x28 * 32) return 0;
    int found = 0; /* bit per window set by a halfword channel */
    for (uint32_t c = HDMA_CHANNELS; c < end; c += 0x28) {
        uint32_t flags = rt_r32(c), src = rt_r32(c + 4), dst = rt_r32(c + 8);
        if (!(flags & 1) || (flags & 8) || rt_r32(c + 0x1c) != 1 || rt_r32(c + 0x20) != 0 || rt_r32(c + 0x24) < SCREEN_H - 1 ||
            !ram_ptr(src))
            continue;
        if ((flags & 2) && dst == 0x04000040u && rt_r32(c + 0x14) == 4) {
            for (int y = 0; y < SCREEN_H; y++) h[y] = rt_r32(src + 4u * (uint32_t)y);
            return 1;
        }
        if (!(flags & 2) && (dst == 0x04000040u || dst == 0x04000042u) && rt_r32(c + 0x14) == 2) {
            int w = dst == 0x04000042u;
            if (!found) for (int y = 0; y < SCREEN_H; y++) h[y] = io_get16(0x40) | (uint32_t)io_get16(0x42) << 16;
            for (int y = 0; y < SCREEN_H; y++) h[y] = (h[y] & (0xFFFF0000u >> 16 * w)) | rt_r16(src + 2u * (uint32_t)y) << 16 * w;
            found |= 1 << w;
        }
    }
    return found != 0;
}

static uint32_t hdma_entry(uint32_t c, int line, uint32_t *halves);

/* BG bg's BGxCNT as the HDMA writes it for line `line`, else 0x10000 (it keeps the register). */
static uint32_t hdma_cnt(uint32_t end, int bg, int line) {
    for (uint32_t c = HDMA_CHANNELS; c < end; c += 0x28) {
        uint32_t halves, from = hdma_entry(c, line, &halves), dst = rt_r32(c + 8);
        for (uint32_t i = 0; from && i < halves; i++) {
            if (dst + 2 * i == 0x04000008u + 2u * (uint32_t)bg) return rt_r16(from + 2 * i);
        }
    }
    return 0x10000;
}

/* The BGs (bit per BG) whose scroll the HDMA changes line by line: an effect's picture (the ripples in Magus's
 * portal), not a room layer. Battles set BG1-BG3's scroll through the HDMA too, the same on every line: taken for an
 * effect's, the room's layers were stretched with its band and the battle seemed to zoom in (Frog's Water). Only
 * lines on the same map count: a text box's lines switch BG0/BG1 to the box's maps, with their own scroll, through
 * the HDMA (the Blackbird's cell), and the room's BG0 beyond its ends was taken for an effect's and shown. */
int ws_hdma_bgs(void) {
    uint32_t end = rt_r32(HDMA_END);
    if (end < HDMA_CHANNELS || end > HDMA_CHANNELS + 0x28 * 32) return 0;
    int bgs = 0;
    for (uint32_t c = HDMA_CHANNELS; c < end; c += 0x28) {
        uint32_t flags = rt_r32(c), dst = rt_r32(c + 8), src = rt_r32(c + 4), step = rt_r32(c + 0x14);
        if ((flags & 9) != 1 || !step || dst < 0x04000010u || dst >= 0x04000020u || !ram_ptr(src)) continue;
        uint32_t first = rt_r32(c + 0x20), last = rt_r32(c + 0x24), v = rt_r32(src);
        int bg = (int)((dst - 0x04000010u) >> 2);
        uint32_t map = hdma_cnt(end, bg, (int)first);
        for (uint32_t y = first + 1; y <= last && y < SCREEN_H; y++) {
            if (hdma_cnt(end, bg, (int)y) != map) continue;
            if (rt_r32(src + step * (y - first)) != v) {
                bgs |= 1 << bg;
                break;
            }
        }
    }
    return bgs;
}

/* Channel c's entry for line `line`, when it writes one (halves: its halfwords): its address, else 0. */
static uint32_t hdma_entry(uint32_t c, int line, uint32_t *halves) {
    uint32_t flags = rt_r32(c), src = rt_r32(c + 4), dst = rt_r32(c + 8), first = rt_r32(c + 0x20);
    *halves = rt_r32(c + 0x1c) * (flags & 2 ? 2 : 1);
    if (!(flags & 1) || !ram_ptr(src) || dst >> 24 < 4 || dst >> 24 > 5 || !*halves || *halves > WS_HDMA_UNITS ||
        (uint32_t)line < first || (uint32_t)line > rt_r32(c + 0x24))
        return 0;
    return flags & 8 ? src : src + rt_r32(c + 0x14) * ((uint32_t)line - first);
}

int ws_hdma_period(int top, int bottom) {
    uint32_t end = rt_r32(HDMA_END);
    if (end < HDMA_CHANNELS || end > HDMA_CHANNELS + 0x28 * WS_HDMA_MAX) return 0;
    for (int p = 1; bottom - top + 1 - p >= WS_PERIOD_LINES; p++) {
        int ok = 1;
        for (uint32_t c = HDMA_CHANNELS; c < end && ok; c += 0x28) {
            uint32_t halves, halves2;
            if ((rt_r32(c + 8) & ~2u) == 0x04000040u || !hdma_entry(c, top, &halves) || !hdma_entry(c, bottom, &halves2)) continue;
            for (int y = top; ok && y + p <= bottom; y++) {
                uint32_t a = hdma_entry(c, y, &halves), b = hdma_entry(c, y + p, &halves);
                for (uint32_t i = 0; ok && i < halves; i++) ok = rt_r16(a + 2 * i) == rt_r16(b + 2 * i);
            }
        }
        if (ok) return p;
    }
    return 0;
}

void ws_hdma_line(int line, WsHdmaSaved *saved) {
    saved->n = 0;
    uint32_t end = rt_r32(HDMA_END);
    if (end < HDMA_CHANNELS || end > HDMA_CHANNELS + 0x28 * WS_HDMA_MAX) return;
    for (uint32_t c = HDMA_CHANNELS; c < end; c += 0x28) {
        uint32_t halves, from = hdma_entry(c, line, &halves), dst = rt_r32(c + 8);
        if (!from) continue;
        WsHdmaWrite *w = &saved->w[saved->n++];
        w->dst = dst, w->halves = (int)halves;
        for (uint32_t i = 0; i < halves; i++) {
            w->old[i] = (uint16_t)rt_r16(dst + 2 * i);
            rt_w16(dst + 2 * i, rt_r16(from + 2 * i));
        }
    }
}

void ws_hdma_restore(const WsHdmaSaved *saved) {
    for (int k = saved->n - 1; k >= 0; k--) {
        const WsHdmaWrite *w = &saved->w[k];
        for (int i = 0; i < w->halves; i++) rt_w16(w->dst + 2 * (uint32_t)i, w->old[i]);
    }
}
