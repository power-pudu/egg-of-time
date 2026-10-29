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
 * A layer is filled only when its BG scroll agrees with its ring origin (a layer scrolling on its own, or a map
 * that is not 512x256, is left alone).
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
 * right margin's (rt_ws_world_sprite). */
#include "dispatch.h"
#include "rt.h"
#include "single_screen.h"
#include "video.h"
#include "widescreen.h"

#define SCROLL 0x020708e0u
#define TILESETS 0x020708d4u
#define ROOM 0x0207b518u
#define PAL_REMAP 0x02058714u

static int ram_ptr(uint32_t p) { return p >> 24 == 0x02; }

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

void ws_state(RtStateIO *s) {
    if (!rt_state_optional(s, "ws_room_copy", &room_copy, sizeof room_copy) && rt_state_loading(s)) room_copy.of = 0;
}

/* Packed tile (b0 | b1 << 8 | b2 << 16) of layer L at world tile (c, r). */
static uint32_t room_tile(uint32_t scroll, uint32_t tilesets, int L, int c, int r) {
    uint32_t i = ((uint32_t)(c >> 1) & rt_r8(scroll + 0x1a + 2 * L)) + (((uint32_t)(r >> 1) & rt_r8(scroll + 0x1b + 2 * L)) << 8);
    uint32_t id = room_copy.rows[0x40 * L + i];
    if (L < 2) id += ((room_copy.rows[0xc0 + i] >> L) & 1) << 8;
    uint32_t t = rt_r32(tilesets + (L < 2 ? 0xc : 0x10)) + (id * 4 + (c & 1) + 2 * (r & 1)) * 3;
    return rt_r8(t) | rt_r8(t + 1) << 8 | rt_r8(t + 2) << 16;
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

/* The BG agrees with layer L's ring origin and is a 512x256 text map. */
static int bg_follows(int bg, uint32_t scroll, int L) {
    uint16_t cnt = io_get16(0x08 + 2 * bg);
    return (cnt >> 14) == 1 && ((io_get16(0x10 + 4 * bg) >> 3) & 63) == rt_r8(scroll + 0x8e + 2 * L) &&
           ((io_get16(0x12 + 4 * bg) >> 3) & 31) == rt_r8(scroll + 0x8f + 2 * L);
}

/* The sprite renderer's view (NitroSDK G2D surface of the top screen, 0x02058070: top-left x, y, then width,
 * height, 20.12 fixed point; normally 0, -1, 256, 192). Sprites outside it are culled, and inside it they are
 * placed relative to its top-left. In the field it is widened to the margins, so characters beyond the 4:3 view
 * are drawn; their OAM X then counts from the left margin (ws_sprite_origin). */
#define G2D_VIEW 0x02058070u
#define FX(v) ((uint32_t)((v) * 4096))

/* The field camera's top-left in the room, in pixels (0x020760f8: x, y; BG scroll moves with it). */
#define CAMERA 0x020760f8u

int ws_camera(int *x, int *y) {
    if (!ss_in_field()) return 0;
    *x = (int16_t)rt_r16(CAMERA);
    *y = (int16_t)rt_r16(CAMERA + 2);
    return 1;
}

int ws_sprite_origin(void) { /* the view as widened here, recognised by its shape (also after a state load) */
    int32_t x = (int32_t)rt_r32(G2D_VIEW);
    return x < 0 && x >= -VIDEO_MARGIN_MAX * 4096 && rt_r32(G2D_VIEW + 8) == FX(SCREEN_W) - 2 * (uint32_t)x ? -x / 4096 : 0;
}

/* Widens the view to margin columns a side; 0 puts back a view widened here (and leaves any other alone). */
static void set_sprite_view(int margin) {
    if (ws_sprite_origin() == margin || (!margin && !ws_sprite_origin())) return;
    rt_w32(G2D_VIEW, FX(-margin));
    rt_w32(G2D_VIEW + 8, FX(SCREEN_W + 2 * margin));
}

/* The field scene's renderer surface (scene secondary base *(0x0207088c) + 0xd748, view rect first). Its own
 * view is screen space: 0, 0, 1024 wide (no culling); the scene's draw (main 0x02041940) saves it, switches to
 * the G2D view above for world sprites (characters, gauges, damage numbers) and puts it back for the UI drawn in
 * screen coordinates (battle menus and their text, the command cursor). In the field that view starts at the
 * left margin, so the UI keeps its 4:3 place, centred. */
#define CURRENT_SCENE 0x0207088cu
#define SCENE_SURFACE 0xd748u

static void set_screen_view(int margin) {
    uint32_t base = rt_r32(CURRENT_SCENE);
    if (!ram_ptr(base)) return;
    uint32_t s = base + SCENE_SURFACE;
    int32_t x = (int32_t)rt_r32(s);
    if (rt_r32(s + 4) || rt_r32(s + 8) != FX(1024) || x > 0 || x < -VIDEO_MARGIN_MAX * 4096) return; /* not its own view */
    rt_w32(s, FX(-margin));
}

static int field_margin; /* this frame's margin columns in the field, 0 elsewhere */

/* Field object visibility (ov004 0x02130dbc, r0 = object): true while the object's tile lies within the view
 * columns *(SCROLL)+4 - 4 .. *(SCROLL)+6 + 4 (clamped to the room) and the matching rows. Those two fields are
 * the camera's own (map streaming uses them too), so they are widened by the margins only for the call. */
static void game_object_visible(CPU *cpu) { rt_call_wrapped(cpu, "ov004", 0x02130dbcu); } /* the original */
void rt_ws_object_visible(CPU *cpu) {
    uint32_t scroll = rt_r32(SCROLL);
    int extra = (field_margin + 7) >> 3; /* tiles */
    if (!extra || !ram_ptr(scroll) || cpu->resume_pc) {
        game_object_visible(cpu);
        return;
    }
    uint32_t left = rt_r16(scroll + 4), right = rt_r16(scroll + 6);
    rt_w16(scroll + 4, left > (uint32_t)extra ? left - (uint32_t)extra : 0);
    rt_w16(scroll + 6, right + (uint32_t)extra);
    game_object_visible(cpu);
    rt_w16(scroll + 4, left);
    rt_w16(scroll + 6, right);
}

#define WORLD 0xddc0u

static int world_margin; /* this frame's margin columns in the world map, 0 elsewhere */

/* The SNES-style code's 16-bit compare (main 0x020305ac, r0 = the accumulator's address, r1 = the operand): sets
 * the emulated flags (0x0207b43c: carry bit 0, zero bit 1, negative bit 7) from A - M. Its calls in the world map's
 * X tests (by return address: the sprite pieces' X + 15 against 0x10f, ov005; the objects' X - camera + 16 against
 * 0x120, ov002, negative values wrapped at 0x600) compare A + margin against M + 2 * margin instead, a value just
 * below zero (the left margin) taken as negative. */
#define SNES_FLAGS 0x0207b43cu
#define SPRITE_X_TEST 0x0212c0e4u
#define OBJECT_X_TEST 0x0210ce50u

void rt_ws_snes_cmp(CPU *cpu) {
    uint32_t a = rt_r16(R[0]), m = R[1] & 0xFFFF, wrap = 0;
    if (world_margin && R[14] == SPRITE_X_TEST && rt_overlay_resident(5)) wrap = 0x10000;
    if (world_margin && R[14] == OBJECT_X_TEST) wrap = 0x600;
    if (wrap) {
        uint32_t w = (uint32_t)world_margin;
        a = (a >= wrap - w ? a - wrap : a) + w; /* the left margin's values (just below zero) wrapped back */
        a &= 0xFFFF;
        m = (m + 2 * w) & 0xFFFF;
    }
    uint32_t d = a + (~m & 0xFFFF) + 1;
    rt_w8(SNES_FLAGS, (rt_r8(SNES_FLAGS) & 0x7c) | ((d >> 16) & 1) | ((d & 0xFFFF) == 0) << 1 | ((d & 0x8000) >> 8));
}

/* World map sprite piece to DS (ov002 0x0211cc20, r2 = X, signed 16-bit). */
static void game_world_sprite(CPU *cpu) { rt_call_wrapped(cpu, "ov002", 0x0211cc20u); } /* the original */
void rt_ws_world_sprite(CPU *cpu) {
    if (world_margin && !cpu->resume_pc && (int16_t)R[2] < -128) R[2] = (uint32_t)((int16_t)R[2] + 512) & 0xFFFF;
    game_world_sprite(cpu);
}

static void fill_world_margins(int margin) {
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
        for (int c = left; c <= right; c++) {
            if (c >= own && c <= own + 16) continue; /* the game's own columns */
            int wc = (c % 0x60 + 0x60) % 0x60;
            for (int r = y >> 4; r <= (y + SCREEN_H - 1) >> 4; r++) {
                uint32_t id = rt_r8(map + (uint32_t)(r & 63) * 0x60 + (uint32_t)wc);
                uint32_t e = sheet + ((id >> 4) * 2) * 64 + (id & 15) * 4;
                int slot = (c * 2) & 63, row = (r * 2) & 31;
                put(L, slot, row, (uint16_t)rt_r16(e));
                put(L, slot + 1, row, (uint16_t)rt_r16(e + 2));
                put(L, slot, row + 1, (uint16_t)rt_r16(e + 64));
                put(L, slot + 1, row + 1, (uint16_t)rt_r16(e + 66));
            }
        }
    }
}

void ws_fill_margins(int margin) {
    world_margin = margin > 0 && ss_in_overworld() ? margin : 0;
    if (world_margin) fill_world_margins(margin);
    int field = margin > 0 && ss_in_field();
    field_margin = field ? margin : 0;
    set_sprite_view(field_margin);
    if (ss_in_field()) set_screen_view(field_margin);
    if (!field) return;
    uint32_t scroll = rt_r32(SCROLL), room = rt_r32(ROOM), tilesets = rt_r32(TILESETS);
    if (!ram_ptr(scroll) || !ram_ptr(room) || !ram_ptr(tilesets)) return;
    int ok0 = bg_follows(2, scroll, 0), ok1 = bg_follows(3, scroll, 1), ok2 = bg_follows(0, scroll, 2);
    int okhi = ok0 && ok1 && bg_follows(1, scroll, 0);
    if (!ok0 && !ok1 && !ok2) return;
    copy_room_rows(scroll, room);
    int camc = rt_r16(scroll + 4), camr = rt_r16(scroll + 8);
    int fine = io_get16(0x10) & 7, vfine = io_get16(0x12) & 7; /* BG0: every filled layer agrees on the tile */
    int left = camc + ((fine - margin) >> 3), right = camc + ((fine + SCREEN_W - 1 + margin) >> 3);
    int bottom = camr + ((vfine + SCREEN_H - 1) >> 3);
    for (int c = left; c <= right; c++) {
        if (c >= camc && c < camc + 32) continue; /* the game's own columns */
        for (int r = camr; r <= bottom; r++) {
            uint32_t t0 = room_tile(scroll, tilesets, 0, c, r), t1 = room_tile(scroll, tilesets, 1, c, r);
            int pri0 = (t0 >> 16) & 0xFF, pri1 = (t1 >> 16) & 0xFF;
            for (int L = 0; L < 3; L++) {
                static const int bg_of[3] = {2, 3, 0};
                int ok = L == 0 ? ok0 : L == 1 ? ok1 : ok2;
                if (!ok) continue;
                int slot = (rt_r8(scroll + 0x8e + 2 * L) + c - camc) & 63, row = (rt_r8(scroll + 0x8f + 2 * L) + r - camr) & 31;
                uint32_t t = L == 0 ? t0 : L == 1 ? t1 : room_tile(scroll, tilesets, 2, c, r);
                put(bg_of[L], slot, row, L == 0 && pri0 ? 0 : L == 1 && pri1 ? 0 : map_entry(L, t));
                if (L == 0 && okhi) put(1, slot, row, pri1 ? map_entry(1, t1) : pri0 ? map_entry(0, t0) : 0);
            }
        }
    }
}

/* The game's software HDMA (the HBlank handler, ITCM 0x01ff8580): channels of 0x28 bytes from 0x027e0068 up to
 * *(0x02057944): +0 flags (bit 0 on, bit 1 words, bit 3 no source step per line), +4 source, +8 destination,
 * +0x14 source step per line, +0x1c units, +0x20 / +0x24 first / last line. Before each line it copies the line's
 * entry to the destination. Battle effects drawn with the windows (the rays of Lightning II...) use one with
 * WIN0H/WIN1H as one word a line. */
#define HDMA_CHANNELS 0x027e0068u
#define HDMA_END 0x02057944u

int ws_window_lines(uint32_t h[SCREEN_H]) {
    uint32_t end = rt_r32(HDMA_END);
    if (end < HDMA_CHANNELS || end > HDMA_CHANNELS + 0x28 * 32) return 0;
    for (uint32_t c = HDMA_CHANNELS; c < end; c += 0x28) {
        uint32_t flags = rt_r32(c), src = rt_r32(c + 4);
        if ((flags & 0xB) != 3 || rt_r32(c + 8) != 0x04000040u || rt_r32(c + 0x14) != 4 || rt_r32(c + 0x1c) != 1 ||
            rt_r32(c + 0x20) != 0 || rt_r32(c + 0x24) < SCREEN_H - 1 || !ram_ptr(src))
            continue;
        for (int y = 0; y < SCREEN_H; y++) h[y] = rt_r32(src + 4u * (uint32_t)y);
        return 1;
    }
    return 0;
}
