/* DS 2D graphics: VRAM bank mapping (VRAMCNT_A..I) and per-scanline rendering of engines A and B.
 * Reference: GBATEK "DS Video". The 3D layer (engine A BG0 in 3D mode) is not rendered yet. */
#include <SDL3/SDL.h>

#include "gfx_mod.h"
#include "rt.h"
#include "single_screen.h"
#include "video.h"
#include "widescreen.h"

#define PAGE 0x4000u
#define PAGE_SHIFT 14

/* ---- VRAM banks ---- */

typedef struct Bank { uint32_t off, size; uint16_t cnt_reg; } Bank;
static const Bank banks[9] = {
    {0x00000, 0x20000, 0x240}, {0x20000, 0x20000, 0x241}, {0x40000, 0x20000, 0x242}, {0x60000, 0x20000, 0x243},
    {0x80000, 0x10000, 0x244}, {0x90000, 0x4000, 0x245},  {0x94000, 0x4000, 0x246},  {0x98000, 0x8000, 0x248},
    {0xA0000, 0x4000, 0x249},
};
enum { BANK_A, BANK_B, BANK_C, BANK_D, BANK_E, BANK_F, BANK_G, BANK_H, BANK_I };

static uint8_t *abg[32], *bbg[8], *aobj[16], *bobj[8], *lcdc[41];
static uint8_t *abg_ext[4], *bbg_ext[4], *aobj_ext, *bobj_ext;

static void map(uint8_t **table, unsigned table_pages, uint32_t region_off, int bank) {
    for (uint32_t k = 0; k < banks[bank].size / PAGE; k++) {
        uint32_t page = region_off / PAGE + k;
        if (page < table_pages) table[page] = &rt_vram[banks[bank].off + k * PAGE];
    }
}

void video_vramcnt_written(void) {
    memset(abg, 0, sizeof abg); memset(bbg, 0, sizeof bbg); memset(aobj, 0, sizeof aobj);
    memset(bobj, 0, sizeof bobj); memset(lcdc, 0, sizeof lcdc);
    memset(abg_ext, 0, sizeof abg_ext); memset(bbg_ext, 0, sizeof bbg_ext);
    aobj_ext = bobj_ext = NULL;
    for (int b = 0; b < 9; b++) {
        uint8_t cnt = rt_io[banks[b].cnt_reg];
        if (!(cnt & 0x80)) continue;
        int mst = cnt & 7, ofs = (cnt >> 3) & 3;
        uint8_t *mem = &rt_vram[banks[b].off];
        if (mst == 0) { map(lcdc, 41, banks[b].off, b); continue; }
        switch (b) {
        case BANK_A: case BANK_B: case BANK_C: case BANK_D:
            if (mst == 1) map(abg, 32, 0x20000u * ofs, b);
            else if (mst == 2 && b <= BANK_B) map(aobj, 16, 0x20000u * (ofs & 1), b);
            else if (mst == 4 && b == BANK_C) map(bbg, 8, 0, b);
            else if (mst == 4 && b == BANK_D) map(bobj, 8, 0, b);
            /* mst 3: texture slot (3D, later); C/D mst 2: ARM7 WRAM */
            break;
        case BANK_E:
            if (mst == 1) map(abg, 32, 0, b);
            else if (mst == 2) map(aobj, 16, 0, b);
            else if (mst == 4) for (int i = 0; i < 4; i++) abg_ext[i] = mem + 0x2000 * i;
            break;
        case BANK_F: case BANK_G: {
            uint32_t off = 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1);
            if (mst == 1) map(abg, 32, off, b);
            else if (mst == 2) map(aobj, 16, off, b);
            else if (mst == 4) { abg_ext[(ofs & 1) * 2] = mem; abg_ext[(ofs & 1) * 2 + 1] = mem + 0x2000; }
            else if (mst == 5) aobj_ext = mem;
            break;
        }
        case BANK_H:
            if (mst == 1) map(bbg, 8, 0, b);
            else if (mst == 2) for (int i = 0; i < 4; i++) bbg_ext[i] = mem + 0x2000 * i;
            break;
        case BANK_I:
            if (mst == 1) map(bbg, 8, 0x8000, b);
            else if (mst == 2) map(bobj, 8, 0, b);
            else if (mst == 3) bobj_ext = mem;
            break;
        }
    }
}

uint8_t *video_vram_ptr(uint32_t addr) {
    uint8_t *p = NULL;
    switch ((addr >> 21) & 7) {
    case 0: p = abg[(addr & 0x7FFFF) >> PAGE_SHIFT]; break;
    case 1: p = bbg[(addr & 0x1FFFF) >> PAGE_SHIFT]; break;
    case 2: p = aobj[(addr & 0x3FFFF) >> PAGE_SHIFT]; break;
    case 3: p = bobj[(addr & 0x1FFFF) >> PAGE_SHIFT]; break;
    default:
        if ((addr & 0xFFFFF) < RT_VRAM_SIZE) p = lcdc[(addr & 0xFFFFF) >> PAGE_SHIFT];
        break;
    }
    return p ? p + (addr & (PAGE - 1)) : NULL;
}

/* ---- engine state ---- */

typedef struct Engine {
    int id;             /* 0 = A, 1 = B */
    uint32_t io;        /* register base offset in rt_io */
    uint32_t pal;       /* palette base offset in rt_palette */
    uint32_t oam;       /* OAM base offset in rt_oam */
    uint8_t **bg, **obj;
    uint32_t bg_mask, obj_mask;
    uint8_t **bg_ext, **obj_ext;
    int32_t ref_x[2], ref_y[2]; /* internal affine reference points of BG2/BG3 */
    int ref_reload[2];
    int margin;     /* columns drawn beyond each side of the screen (widescreen), fixed for a frame */
    int rows;       /* zoom out: rows drawn above and below the screen, fixed for a frame; those above with line 0's
                     * registers, those below with line 191's (but see effect_rows). Rows are numbered from the screen's
                     * top (-rows..), and the buffers hold them from their first (FROW). The text box and the sprites
                     * drawn in screen coordinates go to ui_frame (192 rows) to be shown at the original scale. */
    int band_top, band_rows; /* effect_rows: the lines a battle effect is on, stretched over the picture's rows, */
    int ring;                /* or (a ring) drawn from its circles beyond them, its tables repeating every */
    int band_period;         /* band_period lines (0: the nearest line's) */
    int lines_top[2], lines_bottom[2]; /* effect_rows: the lines the effect was on this frame and the last */
    int rays;                /* effect_rows: the windows (bit per window) drawn as rays beyond the band; RAYS_KEPT: the
                              * band not stretched, the effect having had rays */
    int rays_seen;           /* the effect running has had rays */
    int rays_up, rays_down;  /* the rays go on beyond the band's top, its bottom (the lines a menu or the text box
                              * blanks); else the effect ends there */
    float ring_cx, ring_cy, ring_outer, ring_inner; /* the ring's circles (ring_inner 0: a disc) */
    float ring_width;        /* the ring's width when both its circles last showed (0: none yet) */
    int ring_keep;           /* the frames its centre still counts for, the ring having shown */
    uint8_t ring_fits;       /* by frame, the last first: the windows fit a ring */
    uint8_t ring_wins;       /* the windows the ring stands for (bit per window): both, or WIN0 alone where WIN1 shows
                              * other layers (Magus's portal: WIN0 the disc, WIN1 a frame with rounded corners) */
    uint8_t hdma_bgs;        /* widescreen, this frame: BGs scrolled line by line by the HDMA (ws_hdma_bgs) */
    int obj_rows;   /* sprite Y counts from this many rows above the screen (the widened view, widescreen.c) */
    int room_y0, room_y1; /* with room_clip: its rows */
    int obj_origin; /* sprite X counts from this many columns left of the screen (widescreen.c) */
    int obj_stretch; /* the sprites' screen columns are stretched across the margins (the warp, single_screen.c) */
    int repeat_256; /* 256-wide text BGs repeat into the margins (the race's sky, single_screen.c) */
    int view_x, view_y, view_bgs; /* the view's shift from the camera at the room's ends, the BGs it moves (ws_view_shift) */
    int bg_dx[4], bg_dy[4];       /* each of those BGs' share of it (ws_bg_shift) */
    int obj_dx, obj_dy;           /* the world's sprites are that much off it (ws_sprite_lag) */
    int room_clip, room_x0, room_x1; /* Fill screen sides off: black beyond the room's columns room_x0..x1-1 */
    int repeat_bgs; /* BGs whose screen columns repeat into the margins, the others empty beyond the room
                     * (ws_repeat_bgs: a one-screen room's layer scrolling on its own, the clouds of the Epoch's flight),
                     * only those seen at the screen's sides the frame before (shown_bgs), which the margins go on
                     * from: a layer the room covers stays hidden (location 0x71: bubbles drifting on BG0, priority 3,
                     * under the opaque room, seen only through the time gate's hole in the middle) */
    uint8_t shown_bgs, shown_now; /* BGs seen in the screen's SIDE_COLS side columns (on top, or blended under the
                                   * top) in the last frame and in this one so far, text box lines left out */
    int party;      /* camera smoothing: this frame has sprites that move with the camera (party layer) */
    int sliding;    /* camera smoothing: the display may slide this frame (the camera moved within VIDEO_GLIDE_FRAMES) */
    int box_top;    /* the first line of the field's text box this frame (see box_bg; SCREEN_H: no box) */
    int track_top;  /* this frame's top layers go to bottom_top (the bottom screen, video_set_track_bottom) */
    int panel_ui;   /* this frame's bottom_top leaves out what covers the map panel (panel_has_ui) */
    int still_bgs;  /* camera smoothing: BGs that hold still on screen while the room slides (ws_picture_bgs, the
                     * prison bridge's sky): their pixels, and the backdrop's, go to the still layer when on top */
    int sel_shift, sel_rows; /* single screen's age selector: a second picture with the sprites starting above
                              * row sel_rows drawn sel_shift columns left (ss_bottom_shift, shifted_frame), with
                              * the widescreen margins */
    uint8_t follows[128]; /* by OAM slot: the sprite moves with the camera */
    uint16_t frame_bg[4][3]; /* widescreen: BGxCNT, HOFS, VOFS as the frame started, and its blending: */
    uint16_t frame_bld[2];   /* BLDCNT, BLDALPHA (see margin_bg) */
    uint8_t mirror_bg;       /* widescreen: text BGs whose margins show the screen reflected (see picture_256) */
    uint8_t solid_bgs;       /* widescreen, this line: text BGs one opaque tile throughout (see solid_256) */
    uint64_t solid_key[4];   /* solid_256's last answer by BG this frame, and the registers it was for */
    uint8_t solid_val[4];
    uint8_t frame_solid;     /* widescreen: BG0/BG1 one-tile overlays (solid_256) as the frame started */
} Engine;

static Engine engines[2];
static SDL_AtomicInt track_bottom; /* video_set_track_bottom */
static VideoBottomTop bottom_top, present_bottom_top, shown_bottom_top; /* being drawn, finished, copied by the host */
static int present_bottom_top_on;
static uint16_t marker_now[SCREEN_H][SCREEN_W], marker_held[SCREEN_H][SCREEN_W]; /* the map panel's sprites (OPAQUE set where there is one) */
static uint16_t marker_left[SCREEN_H][SCREEN_W]; /* marker_held as the previous map left it */
static int marker_seen, marker_gap; /* the map panel had a sprite this frame; frames since it had any */
static uint32_t marker_map; /* the map marker_held was seen on (panel_map) */
#define SIDE_COLS 8 /* the screen columns at each side whose layers may repeat into the margins (repeat_bgs) */
static SDL_AtomicInt wide_margin; /* requested by the display (video_set_wide_margin) */
static SDL_AtomicInt zoom_margin, zoom_rows; /* video_set_zoom */
static uint32_t frame[2][VIDEO_LINE_MAX * VIDEO_ROWS_MAX]; /* by engine, VIDEO_LINE_MAX apart */
static uint32_t present[2][SCREEN_W * SCREEN_H];      /* top, bottom */
static uint32_t present_wide[VIDEO_LINE_MAX * VIDEO_ROWS_MAX], present_main[VIDEO_LINE_MAX * VIDEO_ROWS_MAX];
static uint32_t shifted_frame[VIDEO_LINE_MAX * SCREEN_H], present_shifted[VIDEO_LINE_MAX * SCREEN_H]; /* see Engine.sel_shift */
static uint8_t shifted_sprite[VIDEO_LINE_MAX * SCREEN_H], present_shifted_sprite[VIDEO_LINE_MAX * SCREEN_H]; /* its sprite pixels */
static int present_shifted_on;
static int present_main_w, present_main_h;
static uint32_t ui_frame[VIDEO_LINE_MAX * SCREEN_H], present_ui[VIDEO_LINE_MAX * VIDEO_ROWS_MAX]; /* see Engine.rows */
static uint8_t ui_stays[VIDEO_LINE_MAX * SCREEN_H]; /* the UI pixel is a number placed at an actor (WS_OAM_PLACED) */
static int present_ui_on, present_ui_h;
static SDL_AtomicInt ui_edges; /* video_set_ui_edges */
static SDL_Mutex *present_lock;
static uint64_t present_serial, present_time;
static int present_dx, present_dy, present_cut; /* camera motion into the published frame; a cut (no motion known) */
static int camera_known, camera_x, camera_y; /* the field camera at the last frame */
/* The camera read at the last frame: the game moves its camera (0x020760f8) a frame before the BG scroll and OAM
 * that show it (written at the next VBlank), so the camera a frame shows is the one read a frame earlier. */
static int read_known, read_x, read_y;
static int frame_dx, frame_dy, frame_cut; /* camera motion into the frame being drawn; a cut */
static int frames_since_step = VIDEO_GLIDE_FRAMES; /* frames since the camera last moved (display gliding before that) */
static SDL_AtomicInt party_wanted;  /* camera smoothing is on (video_set_camera_smoothing) */
static SDL_AtomicInt fill_sides; /* video_set_fill_sides */
static uint8_t prev_oam[0x400];     /* the top screen's sprites in the previous frame */
static uint32_t party_frame[VIDEO_LINE_MAX * VIDEO_ROWS_MAX]; /* party layer of the top screen (alpha 0: none) */
static uint32_t present_party[VIDEO_LINE_MAX * VIDEO_ROWS_MAX];
static int present_party_on;
static uint32_t still_frame[VIDEO_LINE_MAX * VIDEO_ROWS_MAX]; /* still layer of the top screen (alpha 0: none) */
static uint32_t present_still[VIDEO_LINE_MAX * VIDEO_ROWS_MAX];
static int present_still_on;

/* HD graphics mods (gfx_mod.c): while HD tiles are loaded, each pixel also records where its colour came from
 * (VideoHd), so the display can draw it again from the HD art. A layer pixel's source is its tile (looked up by
 * the tile's bytes, cached per frame by VRAM address), its texel and flips, and its palette; the composed pixel
 * keeps the top layer's, with the blending and brightness applied to it as a weight and an added colour (the
 * colour = weight / 16 * texel + added), and the colour under it, shown where the HD art is transparent. Affine
 * layers and sprites, bitmaps, extended palettes and the margins' copies of the frame's first layers have none. */
typedef struct HdLine { uint32_t src[VIDEO_LINE_MAX]; uint8_t pal[VIDEO_LINE_MAX]; } HdLine;
typedef struct HdCache { uint32_t *stamp, *val; } HdCache;
static int hd_on;                 /* this frame records sources */
static uint32_t hd_stamp = 1;     /* the frame, for hd_cache */
static HdCache hd_cache[2][2][2]; /* by engine, sprite, 8 bits per pixel: VRAM address / 32 -> gfxmod tile */
static VideoHd hd_frame[2][VIDEO_LINE_MAX * VIDEO_ROWS_MAX], hd_party[VIDEO_LINE_MAX * VIDEO_ROWS_MAX], hd_still[VIDEO_LINE_MAX * VIDEO_ROWS_MAX];
static VideoHd present_hd[2][SCREEN_W * SCREEN_H], present_hd_wide[VIDEO_LINE_MAX * VIDEO_ROWS_MAX];
static VideoHd present_hd_main[VIDEO_LINE_MAX * VIDEO_ROWS_MAX], present_hd_party[VIDEO_LINE_MAX * VIDEO_ROWS_MAX],
    present_hd_still[VIDEO_LINE_MAX * VIDEO_ROWS_MAX];
static uint16_t present_palette[RT_PALETTE_SIZE / 2];
static int present_hd_on;

static inline uint8_t vr8(uint8_t **t, uint32_t mask, uint32_t off) {
    off &= mask;
    uint8_t *p = t[off >> PAGE_SHIFT];
    return p ? p[off & (PAGE - 1)] : 0;
}
static inline uint16_t vr16(uint8_t **t, uint32_t mask, uint32_t off) {
    off &= mask;
    uint8_t *p = t[off >> PAGE_SHIFT];
    if (!p) return 0;
    uint16_t v;
    memcpy(&v, p + (off & (PAGE - 1)), 2);
    return v;
}
static inline uint16_t pal16(uint32_t off) { uint16_t v; memcpy(&v, &rt_palette[off & 0x7FE], 2); return v; }
static inline uint16_t ext16(uint8_t *slot, uint32_t idx) {
    if (!slot) return 0;
    uint16_t v;
    memcpy(&v, slot + ((idx * 2) & 0x1FFE), 2);
    return v;
}
static inline uint16_t reg16(const Engine *e, uint32_t off) { return io_get16(e->io + off); }
static inline uint32_t reg32(const Engine *e, uint32_t off) { return io_get32(e->io + off); }

void video_affine_ref_written(int engine, int bg) { engines[engine].ref_reload[bg] = 1; }

/* The gfxmod tile for the tile at `addr` in the engine's BG or sprite VRAM (0: none). */
static uint32_t hd_tile(const Engine *e, int obj, uint32_t addr, int bpp8) {
    uint8_t **t = obj ? e->obj : e->bg;
    uint32_t mask = obj ? e->obj_mask : e->bg_mask;
    addr &= mask & ~31u;
    HdCache *c = &hd_cache[e->id][obj][bpp8 != 0];
    if (!c->stamp) {
        c->stamp = SDL_calloc((mask + 1) >> 5, sizeof *c->stamp);
        c->val = SDL_calloc((mask + 1) >> 5, sizeof *c->val);
        if (!c->stamp || !c->val) return 0;
    }
    uint32_t i = addr >> 5;
    if (c->stamp[i] != hd_stamp) {
        uint8_t *p = t[addr >> PAGE_SHIFT];
        uint32_t in = addr & (PAGE - 1);
        c->val[i] = p && in + (bpp8 ? 64 : 32) <= PAGE ? gfxmod_find(p + in, bpp8 != 0) : 0;
        c->stamp[i] = hd_stamp;
    }
    return c->val[i];
}

/* A layer pixel's source: gfxmod tile, texel (in the tile, flips applied) and flips. */
static inline uint32_t hd_src(uint32_t tile, uint32_t px, uint32_t py, int hflip, int vflip) {
    return tile ? tile << 8 | px << 5 | py << 2 | (hflip != 0) << 1 | (vflip != 0) : 0;
}

static void reload_ref(Engine *e, int i) {
    uint32_t base = 0x28 + 0x10 * i;
    e->ref_x[i] = (int32_t)(reg32(e, base) << 4) >> 4; /* 28-bit signed, 8 fractional bits */
    e->ref_y[i] = (int32_t)(reg32(e, base + 4) << 4) >> 4;
    e->ref_reload[i] = 0;
}

void video_reset(void) {
    for (int i = 0; i < 2; i++) {
        Engine *e = &engines[i];
        memset(e, 0, sizeof *e);
        e->id = i;
        e->io = i ? 0x1000 : 0;
        e->pal = i ? 0x400 : 0;
        e->oam = i ? 0x400 : 0;
        e->bg = i ? bbg : abg;
        e->obj = i ? bobj : aobj;
        e->bg_mask = i ? 0x1FFFF : 0x7FFFF;
        e->obj_mask = i ? 0x1FFFF : 0x3FFFF;
        e->bg_ext = i ? bbg_ext : abg_ext;
        e->obj_ext = i ? &bobj_ext : &aobj_ext;
    }
    if (!present_lock) present_lock = SDL_CreateMutex();
    video_vramcnt_written();
}

/* ---- backgrounds ---- */

#define OPAQUE 0x8000u /* in layer buffers: pixel present */

/* Layer buffers hold a line from column -margin, so out[x + m] is screen column x. */
/* Widescreen: a map one screen wide would only repeat itself in the margins. The field's text box is one: for
 * its lines the game switches BG0/BG1 from the field's 512-wide layers to 256-wide box maps, or from a one-tile
 * overlay (frame_solid: the Mammon Machine's glow, BG0 over its hall) to another map. Such a map is drawn
 * in the screen's columns alone, and the margins keep the layer as the frame started (its map, scroll, priority
 * and blending), so the room goes on beside the box. In the race (repeat_256) the sky, mountains and skyline are
 * 256-wide maps that tile seamlessly, so they repeat into the margins as they would on a wider screen. */
static int box_bg(const Engine *e, int bg) {
    uint16_t cnt = reg16(e, 0x08 + 2 * bg), frame_cnt = e->frame_bg[bg][0];
    return !(cnt & 0x4000) && ((frame_cnt & 0x4000) || (cnt != frame_cnt && (e->frame_solid >> bg & 1)));
}

static int margin_bg(const Engine *e, int bg) { return e->margin && box_bg(e, bg); }

/* The buffers' row of row y, and its offset in them. */
#define FROW(e, y) (((y) + (e)->rows) * VIDEO_LINE_MAX)

/* Columns x0..x1-1 of a text BG line, with these register values. */
static void bg_text_cols(const Engine *e, int bg, int y, uint32_t dispcnt, uint16_t cnt, uint32_t hofs, uint32_t vofs,
                         int x0, int x1, uint16_t *out, HdLine *hd) {
    int m = e->margin;
    if ((cnt & 0x4000) && (e->view_bgs >> bg & 1)) hofs += (uint32_t)e->bg_dx[bg], vofs += (uint32_t)e->bg_dy[bg]; /* not a box's map */
    hofs &= 0x1FF, vofs &= 0x1FF;
    uint32_t w = (cnt & 0x4000) ? 512 : 256, h = (cnt & 0x8000) ? 512 : 256;
    uint32_t chr = ((cnt >> 2) & 0xF) * 0x4000u, scr = ((cnt >> 8) & 0x1F) * 0x800u;
    if (e->id == 0) {
        chr += ((dispcnt >> 24) & 7) * 0x10000u;
        scr += ((dispcnt >> 27) & 7) * 0x10000u;
    }
    int bpp8 = cnt & 0x80;
    int ext = bpp8 && (dispcnt & (1u << 30));
    uint8_t *slot = e->bg_ext[(bg < 2 && (cnt & 0x2000)) ? bg + 2 : bg];
    uint32_t yy = (uint32_t)(y + (int)vofs) & (h - 1);
    for (int x = x0; x < x1; x++) {
        uint32_t xx = (uint32_t)(x + (int)hofs) & (w - 1);
        uint32_t block = (xx >> 8) + (yy >> 8) * (w >> 8);
        uint16_t ent = vr16(e->bg, e->bg_mask, scr + block * 0x800 + ((yy & 255) >> 3) * 64 + ((xx & 255) >> 3) * 2);
        uint32_t tile = ent & 0x3FF, px = (xx & 7) ^ ((ent & 0x400) ? 7 : 0), py = (yy & 7) ^ ((ent & 0x800) ? 7 : 0);
        uint32_t pal = ent >> 12;
        uint32_t idx;
        if (bpp8) {
            idx = vr8(e->bg, e->bg_mask, chr + tile * 64 + py * 8 + px);
            if (idx) out[x + m] = (ext ? ext16(slot, pal * 256 + idx) : pal16(e->pal + idx * 2)) | OPAQUE;
        } else {
            uint32_t b = vr8(e->bg, e->bg_mask, chr + tile * 32 + py * 4 + px / 2);
            idx = (px & 1) ? b >> 4 : b & 0xF;
            if (idx) out[x + m] = pal16(e->pal + pal * 32 + idx * 2) | OPAQUE;
        }
        if (hd && idx) {
            hd->src[x + m] = ext ? 0 : hd_src(hd_tile(e, 0, chr + tile * (bpp8 ? 64 : 32), bpp8), px, py, ent & 0x400, ent & 0x800);
            hd->pal[x + m] = (uint8_t)((bpp8 ? 0 : pal) | e->id << 4);
        }
    }
}

/* Widescreen: a 512-wide map holding a picture in its left 256 columns only, the rest one entry throughout, shown
 * unscrolled: a SNES screen drawn by the game (the lightning bolts of Lightning II, drawn into tiles). Its margins
 * would be empty, so they show the screen reflected at its sides, and what reaches a side goes on. Checked while a
 * battle effect sets the windows line by line (ws_window_lines), so menus and other layers are left alone.
 * Unscrolled both ways: the ice of Ice II is one picture on such a map that the scroll slides onto the enemy, and
 * landed (scroll 0, 464) it was reflected, standing beside itself on both sides. */
static int picture_256(const Engine *e, int bg, uint32_t dispcnt) {
    uint16_t cnt = reg16(e, 0x08 + 2 * bg);
    if ((cnt & 0x4000) == 0 || (reg16(e, 0x10 + 4 * bg) & 0x1FF) || (reg16(e, 0x12 + 4 * bg) & 0x1FF)) return 0;
    uint32_t scr = ((cnt >> 8) & 0x1F) * 0x800u + (e->id ? 0 : ((dispcnt >> 27) & 7) * 0x10000u);
    int blocks = (cnt & 0x8000) ? 2 : 1, drawn = 0;
    uint16_t blank = vr16(e->bg, e->bg_mask, scr + 0x800);
    for (int b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < 0x800; i += 2) {
            if (vr16(e->bg, e->bg_mask, scr + (2u * b + 1) * 0x800 + i) != blank) return 0;
            drawn |= vr16(e->bg, e->bg_mask, scr + 2u * b * 0x800 + i) != blank;
        }
    }
    return drawn;
}

/* Widescreen: a 256x256 map showing one tile, fully opaque, all over the screen: a flash of colour over the picture
 * (the field's red flash when the party takes damage: BG0 on such a map, blended over the rest). Like the screen,
 * the margins and the rows beyond it show that tile throughout. Kept by frame for the registers it was found with. */
static int solid_256(Engine *e, int bg, uint32_t dispcnt) {
    uint16_t cnt = reg16(e, 0x08 + 2 * bg), hofs = reg16(e, 0x10 + 4 * bg) & 0x1FF, vofs = reg16(e, 0x12 + 4 * bg) & 0x1FF;
    if (cnt & 0xC000) return 0;
    uint64_t key = (uint64_t)cnt << 32 | (uint32_t)hofs << 16 | vofs;
    if (e->solid_key[bg] == key) return e->solid_val[bg];
    e->solid_key[bg] = key, e->solid_val[bg] = 0;
    uint32_t chr = ((cnt >> 2) & 0xF) * 0x4000u, scr = ((cnt >> 8) & 0x1F) * 0x800u;
    if (e->id == 0) {
        chr += ((dispcnt >> 24) & 7) * 0x10000u;
        scr += ((dispcnt >> 27) & 7) * 0x10000u;
    }
    uint16_t ent = vr16(e->bg, e->bg_mask, scr + ((vofs & 255) >> 3) * 64);
    for (int y = 0; y < SCREEN_H + 8; y += 8) { /* the map rows the screen shows (one more when scrolled by part of one) */
        uint32_t row = scr + (((uint32_t)SDL_min(y, SCREEN_H - 1) + vofs) & 255) / 8 * 64;
        for (uint32_t c = 0; c < 64; c += 2) if (vr16(e->bg, e->bg_mask, row + c) != ent) return 0;
    }
    int bytes = (cnt & 0x80) ? 64 : 32;
    uint32_t tile = chr + (ent & 0x3FF) * (uint32_t)bytes;
    for (int i = 0; i < bytes; i++) {
        uint8_t b = vr8(e->bg, e->bg_mask, tile + (uint32_t)i);
        if ((cnt & 0x80) ? !b : !(b & 0xF) || !(b >> 4)) return 0;
    }
    return e->solid_val[bg] = 1;
}

/* Column `from` of a line copied to column `to`, with its source. */
static inline void copy_col(uint16_t *out, HdLine *hd, int to, int from) {
    out[to] = out[from];
    if (hd) hd->src[to] = hd->src[from], hd->pal[to] = hd->pal[from];
}

/* Layer (0-3 BG, 4 OBJ, 5 backdrop) holds still on screen while camera smoothing slides the room (Engine.still_bgs). */
static int still_layer(const Engine *e, int layer) { return layer == 5 || (layer < 4 && (e->still_bgs >> layer & 1)); }

static void bg_text(const Engine *e, int bg, int y, uint32_t dispcnt, uint16_t *out, HdLine *hd) {
    int m = e->margin;
    uint16_t cnt = reg16(e, 0x08 + 2 * bg), hofs = reg16(e, 0x10 + 4 * bg), vofs = reg16(e, 0x12 + 4 * bg);
    /* A picture reflected at the screen's sides is at its top and bottom too. */
    if (m && (e->mirror_bg >> bg & 1)) y = y < 0 ? -1 - y : y >= SCREEN_H ? 2 * SCREEN_H - 1 - y : y;
    int solid = m && (e->solid_bgs >> bg & 1);
    if (solid && (y < 0 || y >= SCREEN_H)) y &= 7; /* a screen line with the same row of the tile */
    if (!m || (cnt & 0x4000) || e->repeat_256 || solid) {
        bg_text_cols(e, bg, y, dispcnt, cnt, hofs, vofs, -m, SCREEN_W + m, out, hd);
        if (m && (e->mirror_bg >> bg & 1) && !(hofs & 0x1F8)) {
            /* Reflected at the picture's own sides, which a shaking screen moves by a few columns (hofs). */
            int h = hofs & 7;
            for (int x = -m; x < -h; x++) copy_col(out, hd, m + x, m + (-1 - x - 2 * h));
            for (int x = SCREEN_W - h; x < SCREEN_W + m; x++) copy_col(out, hd, m + x, m + (2 * SCREEN_W - 1 - x - 2 * h));
        }
        if (m && (e->repeat_bgs >> bg & 1)) {
            for (int k = 0; k < m; k++) copy_col(out, hd, k, k + SCREEN_W), copy_col(out, hd, m + SCREEN_W + k, m + k);
        }
        return;
    }
    bg_text_cols(e, bg, y, dispcnt, cnt, hofs, vofs, 0, SCREEN_W, out, hd);
    if (margin_bg(e, bg)) {
        const uint16_t *f = e->frame_bg[bg];
        bg_text_cols(e, bg, y, dispcnt, f[0], f[1], f[2], -m, 0, out, hd);
        bg_text_cols(e, bg, y, dispcnt, f[0], f[1], f[2], SCREEN_W, SCREEN_W + m, out, hd);
    }
}

/* Affine-family BGs (affine, extended, large bitmap). kind: 0 affine (8-bit map), 1 extended. */
static void bg_affine(Engine *e, int bg, uint32_t dispcnt, int large, uint16_t *out) {
    int i = bg - 2;
    uint16_t cnt = reg16(e, 0x08 + 2 * bg);
    uint32_t pbase = 0x20 + 0x10 * i;
    int32_t pa = (int16_t)reg16(e, pbase), pc = (int16_t)reg16(e, pbase + 4);
    int m = e->margin;
    int32_t x0 = e->ref_x[i] - m * pa, y0 = e->ref_y[i] - m * pc;
    int wrap = cnt & 0x2000;
    uint32_t sz = cnt >> 14;
    uint32_t mode = dispcnt & 7;
    int extended = !large && ((mode == 3 && bg == 3) || (mode == 4 && bg == 3) || (mode == 5));
    uint32_t chr = ((cnt >> 2) & 0xF) * 0x4000u, scr = ((cnt >> 8) & 0x1F) * 0x800u;
    if (e->id == 0) {
        chr += ((dispcnt >> 24) & 7) * 0x10000u;
        scr += ((dispcnt >> 27) & 7) * 0x10000u;
    }

    int bitmap = large || (extended && (cnt & 0x80));
    int direct = extended && (cnt & 0x80) && (cnt & 0x04);
    uint32_t w, h;
    if (large) { w = sz & 1 ? 1024 : 512; h = sz & 1 ? 512 : 1024; }
    else if (bitmap) {
        static const uint32_t bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        w = bw[sz]; h = bh[sz];
    } else {
        w = h = 128u << sz;
    }
    uint32_t bmp = large ? 0 : ((cnt >> 8) & 0x1F) * 0x4000u;
    int ext = (dispcnt & (1u << 30)) != 0;

    for (int x = 0; x < SCREEN_W + 2 * m; x++, x0 += pa, y0 += pc) { /* x: buffer column */
        int32_t tx = x0 >> 8, ty = y0 >> 8;
        if (wrap) { tx &= (int32_t)w - 1; ty &= (int32_t)h - 1; }
        else if (tx < 0 || ty < 0 || tx >= (int32_t)w || ty >= (int32_t)h) continue;
        if (direct) {
            uint16_t c = vr16(e->bg, e->bg_mask, bmp + ((uint32_t)ty * w + (uint32_t)tx) * 2);
            if (c & 0x8000) out[x] = c;
        } else if (bitmap) {
            uint32_t idx = vr8(e->bg, e->bg_mask, bmp + (uint32_t)ty * w + (uint32_t)tx);
            if (idx) out[x] = pal16(e->pal + idx * 2) | OPAQUE;
        } else if (extended) {
            uint16_t ent = vr16(e->bg, e->bg_mask, scr + (((uint32_t)ty >> 3) * (w >> 3) + ((uint32_t)tx >> 3)) * 2);
            uint32_t px = (tx & 7) ^ ((ent & 0x400) ? 7 : 0), py = (ty & 7) ^ ((ent & 0x800) ? 7 : 0);
            uint32_t idx = vr8(e->bg, e->bg_mask, chr + (ent & 0x3FF) * 64 + py * 8 + px);
            if (idx) out[x] = (ext ? ext16(e->bg_ext[bg], (ent >> 12) * 256 + idx) : pal16(e->pal + idx * 2)) | OPAQUE;
        } else {
            uint32_t tile = vr8(e->bg, e->bg_mask, scr + ((uint32_t)ty >> 3) * (w >> 3) + ((uint32_t)tx >> 3));
            uint32_t idx = vr8(e->bg, e->bg_mask, chr + tile * 64 + (ty & 7) * 8 + (tx & 7));
            if (idx) out[x] = pal16(e->pal + idx * 2) | OPAQUE;
        }
    }
}

/* ---- sprites ---- */

typedef struct ObjLine { /* from column -margin, like the layer buffers */
    uint16_t color[VIDEO_LINE_MAX]; /* OPAQUE set when a sprite pixel is present */
    uint8_t prio[VIDEO_LINE_MAX];
    uint8_t alpha[VIDEO_LINE_MAX];  /* 0xFF: normal; 0xFE: semi-transparent; 0..16: bitmap alpha */
    uint8_t win[VIDEO_LINE_MAX];    /* OBJ window mask */
    uint8_t party[VIDEO_LINE_MAX];  /* the pixel's sprite moves with the camera */
    uint8_t ui[VIDEO_LINE_MAX];     /* the pixel's sprite is in screen coordinates (ws_tag_oam); 2: placed at an actor */
    HdLine hd;                      /* while hd_on */
} ObjLine;

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static inline uint16_t oam16(const Engine *e, uint32_t off) {
    uint16_t v;
    memcpy(&v, &rt_oam[e->oam + (off & 0x3FE)], 2);
    return v;
}

/* The map the panel shows: BG3's entries under it (the room's or era's picture), the same through a fade. */
static uint32_t panel_map(const Engine *e, uint32_t dispcnt) {
    uint16_t cnt = reg16(e, 0x0E);
    uint32_t hofs = reg16(e, 0x1C) & 0x1FF, vofs = reg16(e, 0x1E) & 0x1FF, h = 0x811C9DC5u ^ cnt;
    uint32_t w = (cnt & 0x4000) ? 512 : 256, ht = (cnt & 0x8000) ? 512 : 256;
    uint32_t scr = ((cnt >> 8) & 0x1F) * 0x800u + (e->id ? 0 : ((dispcnt >> 27) & 7) * 0x10000u);
    for (int y = SS_MAP_Y; y < SS_MAP_Y + SS_MAP_H; y += 8) {
        for (int x = SS_MAP_X; x < SS_MAP_X + SS_MAP_W; x += 8) {
            uint32_t xx = ((uint32_t)x + hofs) & (w - 1), yy = ((uint32_t)y + vofs) & (ht - 1);
            uint32_t block = (xx >> 8) + (yy >> 8) * (w >> 8);
            h = (h ^ vr16(e->bg, e->bg_mask, scr + block * 0x800 + ((yy & 255) >> 3) * 64 + ((xx & 255) >> 3) * 2)) * 0x01000193u;
        }
    }
    return h;
}

/* Whether bottom_top leaves out what covers the bottom screen's map panel. Buttons cover it (a choice in the text
 * box): sprites bigger than any marker on the map, within the panel, in pieces, so the whole frame's sprites go. The
 * icons beside the panel are as big and their boxes reach into it, with nothing drawn there. The game takes the
 * player's marker away meanwhile, a few frames longer than the buttons are there. */
static int panel_has_ui(const Engine *e) {
    enum { MARKER_GAP = 8 }; /* frames; longer without a sprite, the map has no marker */
    int sprite = 0;
    for (int i = 0; i < 128; i++) {
        uint16_t a0 = oam16(e, i * 8), a1 = oam16(e, i * 8 + 2);
        int shape = a0 >> 14, size = a1 >> 14;
        if ((!(a0 & 0x100) && (a0 & 0x200)) || shape == 3) continue;
        int w = obj_w[shape][size], h = obj_h[shape][size], oy = a0 & 0xFF, ox = a1 & 0x1FF;
        if (oy >= SCREEN_H) oy -= 256;
        if (ox >= 256) ox -= 512;
        if (ox >= SS_MAP_X + SS_MAP_W || ox + w <= SS_MAP_X || oy >= SS_MAP_Y + SS_MAP_H || oy + h <= SS_MAP_Y) continue;
        if (w > 16 || h > 16) {
            if (ox < SS_MAP_X || ox + w > SS_MAP_X + SS_MAP_W || oy < SS_MAP_Y || oy + h > SS_MAP_Y + SS_MAP_H) continue;
            marker_gap = 0;
            return 1;
        }
        sprite = 1;
    }
    marker_gap = sprite ? 0 : SDL_min(marker_gap + 1, MARKER_GAP + 1);
    return !sprite && marker_gap <= MARKER_GAP;
}

/* skip_party: leave out the sprites that move with the camera (the scenery picture of camera smoothing).
 * shift: draw the sprites starting above row e->sel_rows that many columns left (Engine.sel_shift). */
static void render_objs(const Engine *e, int y, uint32_t dispcnt, ObjLine *ol, int skip_party, int shift, int skip_ui) {
    int m = e->margin;
    for (int i = 0; i < 128; i++) {
        if (skip_party && e->follows[i]) continue;
        uint16_t a0 = oam16(e, i * 8), a1 = oam16(e, i * 8 + 2), a2 = oam16(e, i * 8 + 4);
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        int shape = a0 >> 14, size = a1 >> 14;
        if (shape == 3) continue;
        int w = obj_w[shape][size], h = obj_h[shape][size];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int ui = e->rows && !affine && (a1 & WS_OAM_UI) ? (a1 & WS_OAM_PLACED ? 2 : 1) : 0;
        if (ui && skip_ui) continue;
        int ly = (y - (a0 & 0xFF)) & 0xFF;
        if (e->rows && !ui) { /* Y 8 bits: from the view's top; the rows left over are above it, or tagged so */
            int oy = a0 & 0xFF, limit = e->obj_rows ? SCREEN_H + e->rows + e->obj_rows : ws_world_sprite_limit(e->rows);
            if (oy >= limit || (!affine && (a1 & WS_OAM_ABOVE))) oy -= 256;
            ly = y + e->obj_rows - oy - e->obj_dy;
            if (ly < 0) continue;
        }
        if (ly >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        ox += m - e->obj_origin;        /* buffer column */
        if (!ui) ox += e->obj_dx;
        if (ox + bw <= 0) ox += 512;   /* X wraps at 512: beyond the left edge is the right one */
        if (shift && ((a0 & 0xFF) < e->sel_rows || (a0 & 0xFF) >= SCREEN_H)) ox -= shift;
        int mode = (a0 >> 10) & 3, prio = (a2 >> 10) & 3;
        int bpp8 = a0 & 0x2000;
        uint32_t tile = a2 & 0x3FF, pal = a2 >> 12;
        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            uint32_t g = ((a1 >> 9) & 0x1F) * 32;
            pa = (int16_t)oam16(e, g + 6); pb = (int16_t)oam16(e, g + 14);
            pc = (int16_t)oam16(e, g + 22); pd = (int16_t)oam16(e, g + 30);
        }
        for (int lx = 0; lx < bw; lx++) {
            int sx = ox + lx;
            if (sx < 0 || sx >= SCREEN_W + 2 * m) continue;
            int tx, ty;
            if (affine) {
                int dx = lx - bw / 2, dy = ly - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            uint16_t color;
            uint8_t alpha = mode == 1 ? 0xFE : 0xFF;
            uint32_t src = 0;
            uint8_t spal = (uint8_t)(e->id << 4 | 0x20);
            if (mode == 3) { /* bitmap sprite */
                uint32_t addr;
                if (dispcnt & 0x40) addr = tile * (128u << ((dispcnt >> 22) & 1)) + ((uint32_t)ty * w + tx) * 2;
                else if (dispcnt & 0x20) addr = (tile & 0x1F) * 0x10 + (tile & 0x3E0) * 0x80 + ((uint32_t)ty * 256 + tx) * 2;
                else addr = (tile & 0xF) * 0x10 + (tile & 0x3F0) * 0x80 + ((uint32_t)ty * 128 + tx) * 2;
                uint16_t c = vr16(e->obj, e->obj_mask, addr);
                if (!(c & 0x8000) || pal == 0) continue;
                color = c;
                alpha = (uint8_t)(pal + 1);
            } else {
                uint32_t unit = bpp8 ? 64 : 32, idx;
                uint32_t addr;
                if (dispcnt & 0x10) /* 1D mapping */
                    addr = tile * (32u << ((dispcnt >> 20) & 3)) + ((uint32_t)(ty >> 3) * (w >> 3) + (tx >> 3)) * unit;
                else
                    addr = tile * 32 + (uint32_t)(ty >> 3) * 1024 + (uint32_t)(tx >> 3) * unit;
                if (bpp8) {
                    idx = vr8(e->obj, e->obj_mask, addr + (ty & 7) * 8 + (tx & 7));
                } else {
                    uint32_t b = vr8(e->obj, e->obj_mask, addr + (ty & 7) * 4 + (tx & 7) / 2);
                    idx = (tx & 1) ? b >> 4 : b & 0xF;
                }
                if (!idx) continue;
                if (mode == 2) { ol->win[sx] = 1; continue; }
                if (bpp8) color = (dispcnt & (1u << 31)) ? ext16(*e->obj_ext, pal * 256 + idx) : pal16(e->pal + 0x200 + idx * 2);
                else color = pal16(e->pal + 0x200 + pal * 32 + idx * 2);
                if (hd_on && !affine && !(bpp8 && (dispcnt & (1u << 31)))) {
                    src = hd_src(hd_tile(e, 1, addr, bpp8), tx & 7, ty & 7, a1 & 0x1000, a1 & 0x2000);
                    spal |= (uint8_t)(bpp8 ? 0 : pal);
                }
            }
            if (!(ol->color[sx] & OPAQUE) || prio < ol->prio[sx]) {
                ol->color[sx] = color | OPAQUE;
                ol->prio[sx] = (uint8_t)prio;
                ol->alpha[sx] = alpha;
                ol->party[sx] = e->follows[i];
                ol->ui[sx] = (uint8_t)ui;
                ol->hd.src[sx] = src;
                ol->hd.pal[sx] = spal;
            }
        }
    }
}

/* Widescreen in the warp: its star is 16 sprites covering the DS screen exactly, so the screen's sprite columns
 * are stretched across the margins too (nearest column). */
static void stretch_objs(const Engine *e, ObjLine *ol) {
    static ObjLine src;
    int m = e->margin, width = SCREEN_W + 2 * m;
    src = *ol;
    for (int x = 0; x < width; x++) {
        int s = m + x * SCREEN_W / width;
        ol->color[x] = src.color[s];
        ol->prio[x] = src.prio[s];
        ol->alpha[x] = src.alpha[s];
        ol->win[x] = src.win[s];
        ol->party[x] = src.party[s];
        ol->ui[x] = src.ui[s];
        ol->hd.src[x] = src.hd.src[s];
        ol->hd.pal[x] = src.hd.pal[s];
    }
}

/* ---- camera smoothing: the party layer ----
 * Between game frames the display slides the field picture with the camera (display.c). Sprites the camera
 * follows (the party) hardly move on screen, so sliding them would shake them: they are drawn on a separate layer
 * that does not slide. A sprite counts as following when, against its match in the previous frame, it moved on
 * screen less than the camera did (a sprite fixed in the world moves by the camera's step, reversed). Matches:
 * the previous frame's sprite with the same tile, palette, shape and size, the nearest; OAM slots follow depth
 * order and change between frames, while a character's parts keep their tiles. */

static int wrap_delta(int p, int c, int bits) {
    int span = 1 << bits, d = (c - p) & (span - 1);
    return d >= span / 2 ? d - span : d;
}

static int sprite_hidden(uint16_t a0) { return !(a0 & 0x100) && (a0 & 0x200); }

/* gliding: the camera did not move into this frame, but the display still glides toward its last step; a sprite
 * then follows if its match followed in the previous frame. */
static void find_party(Engine *e, int dx, int dy, int gliding) {
    static uint8_t prev_follows[128];
    static int prev_ox, prev_oy; /* the previous frame's Engine.obj_dx, obj_dy: part of a sprite's move on screen */
    int ox = e->obj_dx - prev_ox, oy = e->obj_dy - prev_oy;
    prev_ox = e->obj_dx, prev_oy = e->obj_dy;
    const uint8_t *c = &rt_oam[e->oam];
    e->party = 0;
    memset(e->follows, 0, sizeof e->follows);
    for (int i = 0; (dx || dy || gliding) && i < 128; i++) {
        uint16_t c0, c1, c2;
        memcpy(&c0, c + i * 8, 2), memcpy(&c1, c + i * 8 + 2, 2), memcpy(&c2, c + i * 8 + 4, 2);
        if (sprite_hidden(c0)) continue;
        int best = -1, bx = 0, by = 0;
        for (int j = 0; j < 128; j++) {
            uint16_t p0, p1, p2;
            memcpy(&p0, prev_oam + j * 8, 2), memcpy(&p1, prev_oam + j * 8 + 2, 2), memcpy(&p2, prev_oam + j * 8 + 4, 2);
            if (p2 != c2 || (p0 & 0xC300) != (c0 & 0xC300) || (p1 & 0xC000) != (c1 & 0xC000) || sprite_hidden(p0)) continue;
            int mx = wrap_delta(p1 & 0x1FF, c1 & 0x1FF, 9), my = wrap_delta(p0 & 0xFF, c0 & 0xFF, 8);
            if (best < 0 || SDL_abs(mx) + SDL_abs(my) < SDL_abs(bx) + SDL_abs(by)) best = j, bx = mx, by = my;
        }
        if (best < 0) continue;
        bx += ox, by += oy;
        if (dx || dy ? SDL_abs(bx) + SDL_abs(by) < SDL_abs(bx + dx) + SDL_abs(by + dy) : prev_follows[best]) {
            e->follows[i] = 1, e->party = 1;
        }
    }
    memcpy(prev_oam, c, sizeof prev_oam);
    memcpy(prev_follows, e->follows, sizeof prev_follows);
}

void video_set_camera_smoothing(int on) { SDL_SetAtomicInt(&party_wanted, on); }
void video_set_ui_edges(int on) { SDL_SetAtomicInt(&ui_edges, on); }

/* ---- composition ---- */

static inline int in_range(int v, int a, int b) { return a <= b ? (v >= a && v < b) : (v >= a || v < b); }

static uint16_t blend(uint16_t a, uint16_t b, int eva, int evb) {
    uint16_t r = 0;
    for (int s = 0; s < 15; s += 5) {
        int c = (((a >> s) & 31) * eva + ((b >> s) & 31) * evb) >> 4;
        r |= (uint16_t)((c > 31 ? 31 : c) << s);
    }
    return r;
}

static uint16_t brighten(uint16_t a, int evy, int up) {
    uint16_t r = 0;
    for (int s = 0; s < 15; s += 5) {
        int c = (a >> s) & 31;
        c = up ? c + (((31 - c) * evy) >> 4) : c - ((c * evy) >> 4);
        r |= (uint16_t)(c << s);
    }
    return r;
}

static inline uint32_t to_rgba(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xFF000000u | b << 16 | g << 8 | r;
}

/* Widescreen: a window that reaches a screen side is cut there (its 8-bit positions stop at 0 and 255). When the
 * game sets the windows line by line from a table (ws_window_lines), the frame's lines are known before it is
 * drawn, so such an edge is carried into the margin along its slope on the lines next to where it left the screen
 * (the rays of Lightning II), also on the lines next to them where the shape is off the screen altogether; the
 * other side's estimate too where the shape comes back, the nearer edge of the two (convex). A cut edge with no
 * line beside it that has that side on screen (a window as wide as the screen) covers the margin; one beside lines
 * that do not lean out (the steps the game leaves under the battle menu) stops at the screen side. */
enum { EDGE_RUN = 8 }; /* lines the slope is measured over */
static uint32_t win_lines[SCREEN_H]; /* WIN0H | WIN1H << 16 by line, as the frame started */
static int16_t win_ext[SCREEN_H][2][2]; /* by line, window: first column in it, first after it (screen columns) */
static int win_ext_on;
/* Lines before win_top are left out: in battles the game blanks the table's lines under the menu (2-45) but leaves
 * its first lines with leftovers, which the menu hides on the 4:3 screen. */
enum { WIN_LEFTOVERS_MAX = 4 };
static int win_top;

/* Window w's columns on line y (1), or 0 when it is empty, -1 when it wraps; a cut side as -1 / SCREEN_W. */
static int win_span(int y, int w, int *x1, int *x2) {
    if (y < win_top) return 0;
    uint32_t h = win_lines[y] >> (16 * w);
    *x1 = (h >> 8) & 0xFF, *x2 = h & 0xFF;
    if (*x2 == 0xFF || (*x2 == 0 && *x1 > 0)) *x2 = SCREEN_W;
    if (*x1 > *x2) return -1;
    if (*x1 == *x2) return 0;
    if (*x1 == 0) *x1 = -1;
    return 1;
}

/* Side s of window w at line y, along its slope on up to EDGE_RUN lines from line `from` on in direction dir where
 * the window has that side on screen: 1, or 0 when there are not two or the side does not move toward side
 * `toward` (0 left, 1 right) going to y, -1 when there are none. */
static int edge_at(int y, int from, int w, int s, int dir, int toward, int *at) {
    int x[2], n = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = from; i >= 0 && i < SCREEN_H && n < EDGE_RUN && win_span(i, w, &x[0], &x[1]) > 0 && x[s] >= 0 && x[s] < SCREEN_W;
         i += dir, n++) {
        if (!n) x0 = x[s], y0 = i;
        x1 = x[s], y1 = i;
    }
    if (!n) return -1;
    if (n < 2 || (toward ? x0 - x1 : x1 - x0) <= 0) return 0;
    *at = x0 + (x0 - x1) * (y - y0) / (y0 - y1);
    return 1;
}

/* The first line from y on in direction dir where window w is not cut at side s (or SCREEN_H / -1). */
static int past_cut(int y, int w, int s, int dir) {
    int x[2];
    while (y >= 0 && y < SCREEN_H && win_span(y, w, &x[0], &x[1]) > 0 && (x[s] < 0 || x[s] >= SCREEN_W)) y += dir;
    return y;
}

/* Ray shapes: a window drawn from two straight edges A and B (through the rays' centre) shows, on each line, the
 * part between them, or the part left or right of both (the game picks one a line: a DS window has one span).
 * Such a window is drawn in the margins from the edges, each line as its own part, so a ray keeps its form beyond
 * the screen, also on the lines behind the battle menu when it reaches the menu (they take the part of the first
 * line below it, the first line either window is on) and on
 * lines where it is off the screen altogether (the part of the nearest line on the same side of the centre, when
 * it is wholly in the margins).
 * The edges are found among the window's on-screen edge points (RANSAC: the line through two points that most
 * others lie on, then refined by least squares); one edge alone is A = B. A window whose lines mostly fit no part
 * keeps the estimates above. */
enum { FIT_MIN_POINTS = 12, FIT_TOLERANCE = 2 };
typedef struct EdgeLine { float x0, dx; } EdgeLine; /* x = x0 + dx * y */

static int fit_edge(const int *py, const int *px, int n, const uint8_t *taken, EdgeLine *l, uint8_t *mark) {
    int best = 0, step = SDL_max(1, n / 32);
    EdgeLine bl = {0, 0};
    for (int i = 0; i < n; i += step) {
        if (taken[i]) continue;
        for (int j = i + 1; j < n; j += step) {
            if (taken[j] || SDL_abs(py[j] - py[i]) < 8) continue;
            float dx = (float)(px[j] - px[i]) / (float)(py[j] - py[i]), x0 = (float)px[i] - dx * (float)py[i];
            int c = 0;
            for (int k = 0; k < n; k++) c += !taken[k] && SDL_fabsf(x0 + dx * (float)py[k] - (float)px[k]) <= 1.5f;
            if (c > best) best = c, bl = (EdgeLine){x0, dx};
        }
    }
    if (best < FIT_MIN_POINTS) return 0;
    double sy = 0, sx = 0, syy = 0, sxy = 0;
    int c = 0;
    for (int k = 0; k < n; k++) {
        mark[k] = !taken[k] && SDL_fabsf(bl.x0 + bl.dx * (float)py[k] - (float)px[k]) <= 1.5f;
        if (!mark[k]) continue;
        sy += py[k], sx += px[k], syy += (double)py[k] * py[k], sxy += (double)py[k] * px[k], c++;
    }
    double d = c * syy - sy * sy;
    if (d > 0) bl.dx = (float)((c * sxy - sy * sx) / d), bl.x0 = (float)((sx - bl.dx * sy) / c);
    *l = bl;
    return 1;
}

/* Part k of line y (0 between the edges, 1 left of both, 2 right of both), unbounded. */
static void ray_span(const EdgeLine *a, const EdgeLine *b, int k, int y, float *lo, float *hi) {
    float xa = a->x0 + a->dx * (float)y, xb = b->x0 + b->dx * (float)y;
    float mn = SDL_min(xa, xb), mx = SDL_max(xa, xb);
    *lo = k == 1 ? -1e6f : k == 2 ? mx : mn;
    *hi = k == 1 ? mn : k == 2 ? 1e6f : mx;
}

/* Part k of line y as the screen shows it: 0 when that is nothing. */
static int ray_on_screen(const EdgeLine *a, const EdgeLine *b, int k, int y, float *e1, float *e2) {
    float lo, hi;
    ray_span(a, b, k, y, &lo, &hi);
    *e1 = SDL_max(lo, 0.0f), *e2 = SDL_min(hi, (float)SCREEN_W);
    return *e2 - *e1 >= 1.0f;
}

/* By window, this frame: its rays fit (fit_rays), their edges, and the parts of the first and last lines they are on
 * (what the rows beyond those lines take, effect_rows). */
static int ray_fit[2], ray_first_part[2], ray_last_part[2];
enum { RAYS_KEPT = 4 };
static EdgeLine ray_a[2], ray_b[2];

static int fit_rays(int w, int below_menu) {
    int py[2 * SCREEN_H], px[2 * SCREEN_H], n = 0, x[2], first = -1;
    ray_fit[w] = 0;
    uint8_t taken[2 * SCREEN_H] = {0}, mark[2 * SCREEN_H];
    for (int y = win_top; y < SCREEN_H; y++) {
        if (win_span(y, w, &x[0], &x[1]) <= 0) continue;
        if (first < 0) first = y;
        if (x[0] >= 0) py[n] = y, px[n++] = x[0];
        if (x[1] < SCREEN_W) py[n] = y, px[n++] = x[1];
    }
    EdgeLine a, b;
    if (n < FIT_MIN_POINTS || !fit_edge(py, px, n, taken, &a, mark)) return 0;
    for (int k = 0; k < n; k++) taken[k] = mark[k];
    if (!fit_edge(py, px, n, taken, &b, mark)) b = a;
    /* The part of each line with the window on it (-1: none fits, -2: empty). */
    int8_t part[SCREEN_H];
    int lines = 0, fitted = 0;
    for (int y = 0; y < SCREEN_H; y++) {
        int span = y < win_top ? 0 : win_span(y, w, &x[0], &x[1]);
        part[y] = span ? -1 : -2;
        if (span <= 0) continue;
        lines++;
        for (int k = 0; k < 3 && part[y] == -1; k++) {
            float e1, e2;
            if (ray_on_screen(&a, &b, k, y, &e1, &e2) && SDL_fabsf((float)SDL_max(x[0], 0) - e1) <= FIT_TOLERANCE &&
                SDL_fabsf((float)x[1] - e2) <= FIT_TOLERANCE)
                part[y] = (int8_t)k;
        }
        fitted += part[y] >= 0;
    }
    if (!lines || fitted * 10 < lines * 9) return 0;
    ray_fit[w] = 1, ray_a[w] = a, ray_b[w] = b, ray_first_part[w] = ray_last_part[w] = -1;
    for (int y = 0; y < SCREEN_H; y++) {
        if (part[y] < 0) continue;
        if (ray_first_part[w] < 0) ray_first_part[w] = part[y];
        ray_last_part[w] = part[y];
    }
    float cross = a.dx != b.dx ? (b.x0 - a.x0) / (a.dx - b.dx) : 1e6f; /* where the edges meet: the rays' centre */
    for (int y = 0; y < SCREEN_H; y++) {
        int k = part[y];
        if (k == -1) continue; /* keeps its estimate */
        if (k == -2) {
            win_ext[y][w][0] = win_ext[y][w][1] = 0;
            int near = -1;
            for (int d = 1; d < SCREEN_H && near < 0; d++) {
                if (y + d < SCREEN_H && part[y + d] >= 0) near = y + d;
                else if (y - d >= 0 && part[y - d] >= 0) near = y - d;
            }
            if (near < 0 || ((float)y < cross) != ((float)near < cross)) continue; /* another ray */
            float e1, e2;
            int behind_menu = y < first && near == first && first == below_menu;
            if (!behind_menu && ray_on_screen(&a, &b, part[near], y, &e1, &e2)) continue; /* it would show here */
            k = part[near];
        }
        float lo, hi;
        ray_span(&a, &b, k, y, &lo, &hi);
        win_ext[y][w][0] = (int16_t)SDL_clamp(SDL_lroundf(lo), -0x7FFF, 0x7FFF);
        win_ext[y][w][1] = (int16_t)SDL_clamp(SDL_lroundf(hi), -0x7FFF, 0x7FFF);
    }
    return 1;
}

static void window_margins(void) {
    int x[2];
    win_top = 0;
    for (int y = 0; y < WIN_LEFTOVERS_MAX; y++) {
        if (!win_span(y, 0, &x[0], &x[1]) && !win_span(y, 1, &x[0], &x[1])) {
            win_top = y;
            break;
        }
    }
    for (int y = 0; y < SCREEN_H; y++) {
        for (int w = 0; w < 2; w++) {
            int16_t *ext = win_ext[y][w];
            if (y < win_top) { /* as the registers have it on the screen, nothing in the margins */
                uint32_t h = win_lines[y] >> (16 * w);
                int x1 = (h >> 8) & 0xFF, x2 = h & 0xFF;
                if (x2 == 0 && x1 > 0) x2 = SCREEN_W;
                ext[0] = (int16_t)x1, ext[1] = (int16_t)SDL_max(x1, x2);
                continue;
            }
            int span = win_span(y, w, &x[0], &x[1]);
            ext[0] = ext[1] = 0;
            if (span < 0) {
                ext[0] = 1; /* wraps: as the registers have it */
                continue;
            }
            if (span) {
                for (int s = 0; s < 2; s++) {
                    int out = x[s], a, b, ha, hb;
                    if (x[s] < 0 || x[s] >= SCREEN_W) {
                        ha = edge_at(y, past_cut(y, w, s, -1), w, s, -1, s, &a);
                        hb = edge_at(y, past_cut(y, w, s, 1), w, s, 1, s, &b);
                        out = ha < 0 && hb < 0 ? (s ? 0x7FFF : -0x7FFF) : s ? SCREEN_W : 0;
                        if (ha > 0 && hb > 0) out = s ? SDL_min(a, b) : SDL_max(a, b);
                        else if (ha > 0 || hb > 0) out = ha > 0 ? a : b;
                        out = s ? SDL_max(out, SCREEN_W) : SDL_min(out, 0);
                    }
                    ext[s] = (int16_t)out;
                }
                continue;
            }
            /* Empty on screen: the part of a shape that left the screen past a side on a nearby line (the far edge
             * of a ray), the other side carried on along its slope while it is still within the margins. */
            for (int dir = -1; dir <= 1; dir += 2) {
                int b = y + dir;
                while (b >= 0 && b < SCREEN_H && !win_span(b, w, &x[0], &x[1])) b += dir;
                if (b < 0 || b >= SCREEN_H || win_span(b, w, &x[0], &x[1]) < 0) continue;
                for (int s = 0; s < 2; s++) {
                    int o = !s, at;
                    if ((x[s] >= 0 && x[s] < SCREEN_W) || x[o] < 0 || x[o] >= SCREEN_W) continue;
                    if (edge_at(y, b, w, o, dir, s, &at) <= 0) continue;
                    if (s ? at >= SCREEN_W + VIDEO_MARGIN_MAX : at < -VIDEO_MARGIN_MAX) continue;
                    ext[o] = (int16_t)(s ? SDL_max(at, SCREEN_W) : SDL_min(at, 0));
                    ext[s] = (int16_t)(s ? 0x7FFF : -0x7FFF);
                }
            }
        }
    }
    int below_menu = win_top;
    while (below_menu < SCREEN_H && !win_span(below_menu, 0, &x[0], &x[1]) && !win_span(below_menu, 1, &x[0], &x[1])) below_menu++;
    ray_fit[0] = ray_fit[1] = 0;
    fit_rays(0, below_menu);
    fit_rays(1, below_menu);
}

/* Zoom out while a battle effect sets the windows line by line: the game blanks the effect on the lines under the
 * menu at the top and the text box at the bottom (its table empty there, but for a few leftover lines at the
 * screen's edges), and the rows above and below the screen, drawn as lines 0 and 191, showed none of it (an enemy's
 * Rainbow Storm: the starry BG3 coloured line by line through palette entries, under BG0's black bubbles, scrolled
 * line by line, stopped at the 4:3 picture's top and bottom). The effect is made for its lines alone (its tables,
 * its map's rows), and repeated beyond them it looked wrong, so the band is stretched over all the picture's rows,
 * the screen's included: each row is drawn as the band's line at the same height (its windows, the rows of the BGs
 * not the room's, render_engine_line) with what the game's software HDMA writes for that line (ws_hdma_line,
 * video_render_line); sprites and the room's BGs (the stars too, coloured by the line's palette) keep their rows. */
/* A circle through points (Kasa's least squares): 1 with its centre and radius, else 0. */
static int fit_circle(const float *px, const float *py, const uint8_t *use, int n, float *cx, float *cy, float *r) {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0, sz = 0, k = 0;
    for (int i = 0; i < n; i++) {
        if (use && !use[i]) continue;
        double x = px[i], y = py[i], z = x * x + y * y;
        sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y, sxz += x * z, syz += y * z, sz += z, k++;
    }
    /* [sxx sxy sx; sxy syy sy; sx sy k] (D E F) = -(sxz syz sz), the circle x^2 + y^2 + Dx + Ey + F = 0 */
    double a[3][4] = {{sxx, sxy, sx, -sxz}, {sxy, syy, sy, -syz}, {sx, sy, k, -sz}};
    for (int c = 0; c < 3; c++) {
        int piv = c;
        for (int r2 = c + 1; r2 < 3; r2++) if (SDL_fabs(a[r2][c]) > SDL_fabs(a[piv][c])) piv = r2;
        if (SDL_fabs(a[piv][c]) < 1e-9) return 0;
        for (int j = 0; j < 4; j++) { double t = a[c][j]; a[c][j] = a[piv][j], a[piv][j] = t; }
        for (int r2 = 0; r2 < 3; r2++) {
            if (r2 == c) continue;
            double f = a[r2][c] / a[c][c];
            for (int j = c; j < 4; j++) a[r2][j] -= f * a[c][j];
        }
    }
    double d = a[0][3] / a[0][0], ee = a[1][3] / a[1][1], f = a[2][3] / a[2][2];
    double x0 = -d / 2, y0 = -ee / 2, r2 = x0 * x0 + y0 * y0 - f;
    if (r2 <= 0) return 0;
    *cx = (float)x0, *cy = (float)y0, *r = (float)SDL_sqrt(r2);
    return 1;
}

/* The circle most of the points not yet taken lie on (RANSAC over triples, then refined): their count, marking
 * them in mark. */
enum { RING_POINTS_MAX = 4 * SCREEN_H, RING_MIN = 12, RING_RADIUS_MAX = 320 };
#define RING_TOLERANCE 1.5f
static int on_circle(float x, float y, float cx, float cy, float r) {
    return SDL_fabsf(SDL_sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r) <= RING_TOLERANCE;
}
static int ring_circle(const float *px, const float *py, const uint8_t *taken, int n, uint8_t *mark, float *cx, float *cy, float *r) {
    uint32_t seed = 0x2545F491u;
    int best = 0;
    float bx = 0, by = 0, br = 0;
    for (int it = 0; it < 400; it++) {
        int t[3];
        for (int k = 0; k < 3; k++) {
            seed = seed * 1664525u + 1013904223u;
            t[k] = (int)((seed >> 8) % (uint32_t)n);
        }
        if (taken[t[0]] || taken[t[1]] || taken[t[2]] || t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        float qx[3] = {px[t[0]], px[t[1]], px[t[2]]}, qy[3] = {py[t[0]], py[t[1]], py[t[2]]}, x0, y0, r0;
        if (!fit_circle(qx, qy, NULL, 3, &x0, &y0, &r0) || r0 > RING_RADIUS_MAX) continue;
        int c = 0;
        for (int i = 0; i < n; i++) c += !taken[i] && on_circle(px[i], py[i], x0, y0, r0);
        if (c > best) best = c, bx = x0, by = y0, br = r0;
    }
    if (best < RING_MIN) return 0;
    for (int i = 0; i < n; i++) mark[i] = !taken[i] && on_circle(px[i], py[i], bx, by, br);
    if (!fit_circle(px, py, mark, n, cx, cy, r) || *r > RING_RADIUS_MAX) return 0;
    return best;
}

/* The ring's spans on row y (effect_rows), in screen columns from x0 to before x1: their count, 0-2. */
static int ring_spans(const Engine *e, int y, int sp[2][2]) {
    float dy = (float)y - e->ring_cy, ro = e->ring_outer, ri = e->ring_inner;
    if (SDL_fabsf(dy) >= ro) return 0;
    float h = SDL_sqrtf(ro * ro - dy * dy);
    int a = (int)SDL_lroundf(e->ring_cx - h), b = (int)SDL_lroundf(e->ring_cx + h);
    if (SDL_fabsf(dy) >= ri) {
        sp[0][0] = a, sp[0][1] = b;
        return 1;
    }
    float hi = SDL_sqrtf(ri * ri - dy * dy);
    sp[0][0] = a, sp[0][1] = (int)SDL_lroundf(e->ring_cx - hi);
    sp[1][0] = (int)SDL_lroundf(e->ring_cx + hi), sp[1][1] = b;
    return 2;
}

/* The ring's windows as the game draws them on the band's lines: 1 if they are the circles' on nearly all of them,
 * but for a pixel or two at an edge (not a shape that only has some of its edges on them). */
static int ring_matches(const Engine *e, int top, int bottom) {
    int bad = 0, x[2], sp[2][2];
    for (int y = top; y <= bottom; y++) {
        uint8_t row[SCREEN_W] = {0};
        for (int w = 0; w < 2; w++) {
            if (!(e->ring_wins >> w & 1)) continue;
            int k = win_span(y, w, &x[0], &x[1]);
            if (k < 0) return 0;
            for (int c = SDL_max(x[0], 0); k && c < x[1]; c++) row[c] |= 1;
        }
        int n = ring_spans(e, y, sp);
        for (int k = 0; k < n; k++) for (int c = SDL_max(sp[k][0], 0); c < SDL_min(sp[k][1], SCREEN_W); c++) row[c] |= 2;
        int miss = 0;
        for (int c = 0; c < SCREEN_W; c++) miss += row[c] == 1 || row[c] == 2;
        bad += miss > 8;
    }
    return bad * 20 <= bottom - top + 1;
}

/* The radius most of the points not yet marked lie at from (cx, cy): their count (at least 2), marking them k. */
enum { RING_KEEP = 4 };
static int ring_radius(const float *px, const float *py, int n, float cx, float cy, uint8_t k, uint8_t *mark, float *r) {
    static float d[RING_POINTS_MAX];
    if (k == 1) memset(mark, 0, (size_t)n);
    for (int i = 0; i < n; i++) d[i] = SDL_sqrtf((px[i] - cx) * (px[i] - cx) + (py[i] - cy) * (py[i] - cy));
    int best = 0;
    float at = 0;
    for (int i = 0; i < n; i++) {
        if (mark[i]) continue;
        int c = 0;
        for (int j = 0; j < n; j++) c += !mark[j] && SDL_fabsf(d[j] - d[i]) <= RING_TOLERANCE;
        if (c > best) best = c, at = d[i];
    }
    if (best < 2) return 0;
    double sum = 0;
    int c = 0;
    for (int i = 0; i < n; i++) if (!mark[i] && SDL_fabsf(d[i] - at) <= RING_TOLERANCE) sum += d[i], c++;
    *r = (float)(sum / c);
    best = 0;
    for (int i = 0; i < n; i++) if (!mark[i] && SDL_fabsf(d[i] - *r) <= RING_TOLERANCE) mark[i] = k, best++;
    return best;
}

/* A ring (the Rainbow Beam of an enemy: an expanding circle, its arcs left and right of the hole in WIN0 and WIN1)
 * or a disc: the window edges on the band's lines that do not stop at a screen side lie on one circle or two
 * concentric ones. Its spans beyond the screen's columns and on the lines the game blanks are then drawn from the
 * circles, as a wider and taller screen would show it, instead of carrying its edges on (straight up and down). */
static int fit_ring(Engine *e, int top, int bottom) {
    static float px[RING_POINTS_MAX], py[RING_POINTS_MAX];
    static uint8_t taken[RING_POINTS_MAX], mark[RING_POINTS_MAX], right[RING_POINTS_MAX];
    int n = 0, x[2];
    for (int y = top; y <= bottom; y++) {
        for (int w = 0; w < 2; w++) {
            if (!(e->ring_wins >> w & 1) || win_span(y, w, &x[0], &x[1]) <= 0) continue;
            for (int s = 0; s < 2; s++) if (x[s] > 0 && x[s] < SCREEN_W) px[n] = (float)x[s], py[n] = (float)y, right[n] = !s, n++;
        }
    }
    float cx = e->ring_cx, cy = e->ring_cy, r = 0, r2 = 0;
    int a = 0, b = 0;
    if (e->ring_keep) {
        /* A ring just shown keeps its centre as it grows: only its radii from its edges, however few show (as it
         * leaves the screen, a few lines in a corner), or none (its arcs past both sides): it stays as it was. */
        if (!n) {
            e->ring = 1, e->ring_keep = RING_KEEP;
            return 1;
        }
        a = ring_radius(px, py, n, cx, cy, 1, mark, &r);
        if (a) b = ring_radius(px, py, n, cx, cy, 2, mark, &r2);
        if (b < 3) b = 0;
        if ((a + b) * 100 < n * 90) a = b = 0;
    }
    if (!a) {
        if (n < RING_MIN) return 0;
        float cx2, cy2;
        memset(taken, 0, (size_t)n);
        a = ring_circle(px, py, taken, n, mark, &cx, &cy, &r);
        if (!a) return 0;
        for (int i = 0; i < n; i++) taken[i] |= mark[i];
        b = ring_circle(px, py, taken, n, taken, &cx2, &cy2, &r2); /* taken: marks circle 1's as 1 too */
        if (b && SDL_fabsf(cx2 - cx) + SDL_fabsf(cy2 - cy) > 4) b = 0; /* not concentric: no ring */
        if ((a + b) * 100 < n * 90) return 0; /* the edges are not all on them */
        /* Centred off the screen: a shape's flat edge (Rainbow Storm's clouds' oval past the screen's sides). */
        if (cx < 0 || cx >= SCREEN_W || cy < 0 || cy >= SCREEN_H) return 0;
    }
    Engine ring = *e; /* the fit, checked before it is taken */
    ring.ring_cx = cx, ring.ring_cy = cy;
    if (b) {
        ring.ring_outer = SDL_max(r, r2), ring.ring_inner = SDL_min(r, r2);
        if (!ring_matches(&ring, top, bottom)) return 0;
        e->ring = 1, e->ring_keep = RING_KEEP, e->ring_cx = cx, e->ring_cy = cy;
        e->ring_outer = ring.ring_outer, e->ring_inner = ring.ring_inner;
        e->ring_width = e->ring_outer - e->ring_inner;
        return 1;
    }
    /* One circle: a disc's edge, its window inside it, or a ring's hole's (as the ring grows past the screen's
     * sides), its window outside it: the ring then as wide as it last showed. */
    int out = 0;
    for (int i = 0; i < n; i++) if (mark[i] == 1) out += right[i] == (px[i] > cx) ? 1 : -1; /* a span going away from the centre */
    if (out <= 0) ring.ring_outer = r, ring.ring_inner = 0;
    else ring.ring_inner = r, ring.ring_outer = e->ring_width ? r + e->ring_width : 4 * RING_RADIUS_MAX;
    if (!ring_matches(&ring, top, bottom)) return 0;
    e->ring = 1, e->ring_keep = RING_KEEP, e->ring_cx = cx, e->ring_cy = cy;
    e->ring_outer = ring.ring_outer, e->ring_inner = ring.ring_inner;
    return 1;
}

enum { BAND_MIN = 48, BATTLE_TOP = 46, BATTLE_BOTTOM = 166 }; /* the lines between the battle menu and the text box */
static void effect_rows(Engine *e) {
    int x[2], top = win_top, bottom = SCREEN_H - 1;
#define WIN_EMPTY(y) (!win_span(y, 0, &x[0], &x[1]) && !win_span(y, 1, &x[0], &x[1]))
    for (int y = SCREEN_H - 1; y >= SCREEN_H - WIN_LEFTOVERS_MAX; y--) {
        if (WIN_EMPTY(y)) {
            bottom = y;
            break;
        }
    }
    while (top < SCREEN_H && WIN_EMPTY(top)) top++;
    while (bottom >= top && WIN_EMPTY(bottom)) bottom--;
#undef WIN_EMPTY
    if (top > bottom) return;
    /* A ring fits two frames running: Rainbow Storm's clouds' oval fits one now and then. */
    /* The ring is both windows where they show the same layers (an enemy's Rainbow Beam), else WIN0 alone. */
    uint8_t wins = (reg16(e, 0x48) & 0x3F) == (reg16(e, 0x48) >> 8 & 0x3F) ? 3 : 1;
    if (wins != e->ring_wins) e->ring_keep = 0, e->ring_width = 0;
    e->ring_wins = wins;
    if (fit_ring(e, top, bottom) && (e->ring_fits |= 1, e->ring_fits & 2 || (e->ring = 0))) {
        e->band_top = top, e->band_rows = bottom - top + 1;
        e->band_period = ws_hdma_period(top, bottom);
        return;
    }
    /* The band takes the lines of the frame before too: Lightning II's rays alternate frame by frame between two
     * sets, one of them clear of the top lines, and stretched from each in turn the bolts jumped up and down. */
    e->lines_top[0] = top, e->lines_bottom[0] = bottom;
    if (e->lines_top[1] < top) top = e->lines_top[1];
    if (e->lines_bottom[1] > bottom) bottom = e->lines_bottom[1];
    /* Rays are not stretched either (their centre, on the target, would move with the stretch: Lightning II's met
     * below Crono): the rows beyond their lines are drawn from their edges, carried on, and the effect's BGs keep their
     * own rows (the bolts' SNES picture has more of them above and below the DS's 192). */
    /* Lightning II's fit on some frames only (a ray cut by the screen's sides into too few points), so once they have,
     * the band stays unstretched while the effect lasts, a window that fits none drawn from its nearest line there. */
    int rays = 0;
    for (int w = 0; w < 2; w++) rays |= ray_fit[w] << w;
    if (rays) e->rays_seen = 1;
    /* So is an effect hanging from the battle menu, or rising from the text box, that ends short of the other: it
     * has a place on screen (Azala's red pillar, down to just below Marle, before its rounded end fits as rays),
     * stretched from those lines its end went lower until the rays took over and it went back. */
    if (top >= BATTLE_TOP && bottom < BATTLE_BOTTOM && (top == BATTLE_TOP) != (bottom == BATTLE_BOTTOM - 1)) e->rays_seen = 1;
    if (e->rays_seen) {
        e->rays = rays | RAYS_KEPT, e->band_top = top, e->band_rows = bottom - top + 1;
        /* They go on only where the game blanks them, under a menu or the text box, or past the screen's edge;
         * elsewhere the effect ends on its own lines (Azala's red pillar, a WIN0 column from under the top menu down
         * to just below Marle, went on to the bottom of the picture; growing, it passed line 145, the bottom menu's
         * edge when the menu is at the bottom, and went on for those frames). */
        e->rays_up = top <= 0 || top == BATTLE_TOP;
        e->rays_down = bottom >= SCREEN_H - 1 || bottom == BATTLE_BOTTOM - 1 ||
                       (bottom == SCREEN_H - BATTLE_TOP - 1 && top < BATTLE_TOP); /* the menu at the bottom, not the top */
        return;
    }
    /* An effect between the battle menu and the text box is stretched from those lines, whatever it fills of them:
     * stretched from its own, it slid as it grew (Rainbow Storm's clouds open as an oval from the middle) and
     * jumped when the text box closed (its lines then taken too, those under the box left out). */
    if (top >= BATTLE_TOP) {
        e->band_top = BATTLE_TOP, e->band_rows = BATTLE_BOTTOM - BATTLE_TOP;
        return;
    }
    /* A band of a few lines (as the effect starts) stretched would streak the rows: the picture stays as it is. */
    if (bottom - top + 1 < BAND_MIN) return;
    e->band_top = top, e->band_rows = bottom - top + 1;
}

/* The line row y is drawn as: the effect's band's line at the same height in the picture (effect_rows), else
 * itself, or the screen's nearest line beyond it. */
static int row_line(const Engine *e, int y) {
    if (!e->band_rows) return SDL_clamp(y, 0, SCREEN_H - 1);
    int top = e->band_top, n = e->band_rows, p = e->band_period;
    if (e->ring) return y >= top && y < top + n ? y : p ? top + ((y - top) % p + p) % p : SDL_clamp(y, top, top + n - 1);
    if (e->rays) return SDL_clamp(y, top, top + n - 1);
    return top + (y + e->rows) * n / (SCREEN_H + 2 * e->rows);
}

/* Screen column sx (wx: it clamped to the screen) in a window with positions h (ext: its span carried into the
 * margins, or NULL). With ext, a window cut at the right side also takes the screen's last column. */
static inline int in_window(int sx, int wx, uint16_t h, const int16_t *ext) {
    if (sx == wx) return in_range(wx, h >> 8, ext && (h & 0xFF) == 0xFF ? SCREEN_W : h & 0xFF);
    return ext && ext[0] <= ext[1] ? sx >= ext[0] && sx < ext[1] : in_range(wx, h >> 8, h & 0xFF);
}

static inline uint32_t master_bright(uint32_t p, int f, int mmode) {
    uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    if (mmode == 1) { r += ((255 - r) * f) >> 4; g += ((255 - g) * f) >> 4; b += ((255 - b) * f) >> 4; }
    else { r -= (r * f) >> 4; g -= (g * f) >> 4; b -= (b * f) >> 4; }
    return 0xFF000000u | b << 16 | g << 8 | r;
}

/* What holds for a whole frame, before its first row. */
static void engine_frame_start(Engine *e) {
    uint32_t dispcnt = reg32(e, 0);
    {
        reload_ref(e, 0);
        reload_ref(e, 1);
        int a_top = (io_get16(0x304) >> 15) & 1; /* POWCNT1 bit 15: engine A on the top screen */
        e->sel_shift = e->id == !a_top ? 0 : ss_bottom_shift(&e->sel_rows);
        /* Widescreen: the top screen, and the bottom one when the age selector's picture is the main screen. */
        e->margin = e->id == !a_top || e->sel_shift ? SDL_GetAtomicInt(&wide_margin) : 0;
        /* Zoom out: the field and the world map, on the top screen. */
        e->rows = e->id == !a_top && (ss_in_field() || ss_in_overworld()) ? SDL_GetAtomicInt(&zoom_rows) : 0;
        if (e->rows) e->margin = SDL_GetAtomicInt(&zoom_margin);
        for (int i = 0; i < 2; i++) { /* the rows above the screen come before its first */
            e->ref_x[i] -= e->rows * (int16_t)reg16(e, 0x22 + 0x10 * i);
            e->ref_y[i] -= e->rows * (int16_t)reg16(e, 0x26 + 0x10 * i);
        }
        if (e->rows) memset(ui_frame, 0, sizeof ui_frame), memset(ui_stays, 0, sizeof ui_stays);
        e->band_rows = e->ring = e->rays = 0;
        e->lines_top[1] = e->lines_top[0], e->lines_bottom[1] = e->lines_bottom[0];
        e->lines_top[0] = SCREEN_H, e->lines_bottom[0] = -1; /* none yet */
        if (e->lines_top[1] == SCREEN_H) e->rays_seen = 0; /* no effect last frame */
        if (e->ring_keep) e->ring_keep--;
        e->ring_fits <<= 1;
        e->party = e->sliding = e->still_bgs = 0;
        e->view_x = e->view_y = e->view_bgs = e->obj_dx = e->obj_dy = 0;
        SDL_memset(e->bg_dx, 0, sizeof e->bg_dx), SDL_memset(e->bg_dy, 0, sizeof e->bg_dy);
        e->box_top = SCREEN_H;
        e->track_top = e->id == a_top && SDL_GetAtomicInt(&track_bottom);
        e->panel_ui = e->track_top && panel_has_ui(e);
        if (e->track_top) marker_seen = 0;
        for (int bg = 0; bg < 4; bg++) {
            e->frame_bg[bg][0] = reg16(e, 0x08 + 2 * bg);
            e->frame_bg[bg][1] = reg16(e, 0x10 + 4 * bg);
            e->frame_bg[bg][2] = reg16(e, 0x12 + 4 * bg);
        }
        e->frame_bld[0] = reg16(e, 0x50), e->frame_bld[1] = reg16(e, 0x52);
        memset(e->solid_key, 0xFF, sizeof e->solid_key);
        e->frame_solid = 0;
        for (int bg = 0; bg < 2 && e->margin; bg++) e->frame_solid |= (uint8_t)(solid_256(e, bg, dispcnt) << bg);
        if (e->id == !a_top) {
            /* The view stops at the room's ends, as far as it is shown (not the guard columns and rows); with Fill screen
             * sides, at those with nothing to fill beyond them. */
            int shown_margin = e->margin - (SDL_GetAtomicInt(&party_wanted) ? VIDEO_SLIDE_MAX : 0);
            int shown_rows = SDL_max(e->rows - (SDL_GetAtomicInt(&party_wanted) ? VIDEO_SLIDE_ROWS : 0), 0);
            int fill = SDL_GetAtomicInt(&fill_sides);
            ws_fill_margins(e->margin, e->rows, SDL_max(shown_margin, 0), shown_rows, fill);
            ws_view_shift(&e->view_x, &e->view_y, &e->view_bgs);
            for (int bg = 0; bg < 4; bg++) ws_bg_shift(bg, &e->bg_dx[bg], &e->bg_dy[bg]);
            ws_sprite_lag(&e->obj_dx, &e->obj_dy);
            win_ext_on = e->margin && ws_window_lines(win_lines);
            if (win_ext_on) window_margins();
            if (win_ext_on && e->rows) effect_rows(e);
            e->hdma_bgs = e->margin ? (uint8_t)ws_hdma_bgs() : 0;
            e->mirror_bg = 0;
            for (int bg = 0; bg < 4 && win_ext_on; bg++) e->mirror_bg |= (uint8_t)(picture_256(e, bg, dispcnt) << bg);
            if (e->margin) e->mirror_bg |= (uint8_t)ws_picture_bgs();
            e->view_bgs &= ~e->mirror_bg;
            e->obj_origin = ws_sprite_origin(); /* sprites placed from the left margin (widened view) */
            e->obj_rows = ws_sprite_rows();
            e->obj_stretch = e->margin && ss_in_warp();
            e->repeat_256 = e->margin && ss_in_race();
            e->shown_bgs = e->shown_now, e->shown_now = 0;
            e->repeat_bgs = e->margin ? ws_repeat_bgs() & e->shown_bgs : 0;
            e->room_clip = e->margin && (!SDL_GetAtomicInt(&fill_sides) || e->repeat_bgs) &&
                           ws_room_span(&e->room_x0, &e->room_x1);
            int rx = 0, ry = 0, rk = ws_camera(&rx, &ry); /* camera motion into this frame (camera smoothing) */
            int cx = read_x, cy = read_y, known = read_known;
            read_known = rk, read_x = rx, read_y = ry;
            frame_dx = frame_dy = 0, frame_cut = 1;
            if (known && camera_known) {
                frame_dx = cx + e->view_x - camera_x, frame_dy = cy + e->view_y - camera_y, frame_cut = 0; /* the view, shifted */
                if (SDL_abs(frame_dx) > VIDEO_SLIDE_MAX || SDL_abs(frame_dy) > VIDEO_SLIDE_MAX) frame_dx = frame_dy = 0, frame_cut = 1;
            }
            camera_known = known, camera_x = cx + e->view_x, camera_y = cy + e->view_y;
            /* ws_room_span goes by the camera in RAM, already the next frame's: the room's edge goes with this one's scroll. */
            if (e->room_clip) {
                int to = (known && rk && SDL_abs(rx - cx) <= VIDEO_SLIDE_MAX ? rx - cx : 0) - e->view_x;
                e->room_x0 += to, e->room_x1 += to;
            }
            e->room_y0 = -e->rows, e->room_y1 = SCREEN_H + e->rows;
            if (e->room_clip && shown_rows && ws_room_rows(&e->room_y0, &e->room_y1)) {
                int to = (known && rk && SDL_abs(ry - cy) <= VIDEO_SLIDE_MAX ? ry - cy : 0) - e->view_y;
                e->room_y0 += to, e->room_y1 += to;
            }
            frames_since_step = frame_dx || frame_dy ? 0 : frame_cut ? VIDEO_GLIDE_FRAMES : SDL_min(frames_since_step + 1, VIDEO_GLIDE_FRAMES);
            int gliding = frames_since_step < VIDEO_GLIDE_FRAMES;
            if (SDL_GetAtomicInt(&party_wanted)) find_party(e, frame_dx, frame_dy, gliding), e->sliding = gliding;
            e->still_bgs = e->sliding && e->margin ? ws_picture_bgs() : 0;
            if (e->still_bgs) {
                memset(still_frame, 0, sizeof still_frame);
                if (hd_on) memset(hd_still, 0, sizeof hd_still);
            }
        }
    }
}

/* Row y of the frame: a line of the screen (0..191), or one the zoom out draws beyond it. */
static void render_engine_line(Engine *e, int y, uint32_t *out) {
    uint32_t dispcnt = reg32(e, 0);
    int m = e->margin, width = SCREEN_W + 2 * m;
    int on_screen = y >= 0 && y < SCREEN_H, wy = row_line(e, y); /* the line whose windows it has */
    int by = e->band_rows && !e->ring && !e->rays ? wy : y; /* and whose BG rows, for the BGs not the room's (own_bgs: the effect's map) */
    if (e->track_top && y == SS_MAP_Y) {
        /* The marker as it last was belongs to its map: entering a house, the world map's pins and marker are gone
         * from the room's map, which the game shows a few frames without sprites. Read at the panel's first line,
         * the map as this frame shows it (the game writes it during VBlank, after the frame's start). For a frame or
         * two the new map still has the old one's sprites (the pins, on the room's map, black): those are not held. */
        uint32_t map = panel_map(e, dispcnt);
        if (map != marker_map) {
            memcpy(marker_left, marker_held, sizeof marker_left);
            memset(marker_held, 0, sizeof marker_held);
            marker_map = map;
        }
    }
    if (on_screen) for (int i = 0; i < 2; i++) if (e->ref_reload[i]) reload_ref(e, i);
    VideoHd *hd_row = hd_on ? &hd_frame[e->id][FROW(e, y)] : NULL;
    if (hd_row) memset(hd_row, 0, (size_t)width * sizeof *hd_row);

    int disp_mode = (dispcnt >> 16) & (e->id ? 1 : 3);
    if (disp_mode == 0 || (dispcnt & 0x80)) {
        for (int x = 0; x < width; x++) out[x] = 0xFFFFFFFFu;
        goto advance;
    }
    if (disp_mode == 2) { /* VRAM display: LCDC bank as a 15-bit bitmap */
        uint8_t **bank = &lcdc[((dispcnt >> 18) & 3) * 8];
        for (int x = 0; x < width; x++) {
            int sx = x - m;
            out[x] = sx < 0 || sx >= SCREEN_W ? 0xFF000000u : to_rgba(vr16(bank, 0x1FFFF, ((uint32_t)wy * 256 + sx) * 2));
        }
        goto advance;
    }
    if (disp_mode == 3) {
        for (int x = 0; x < width; x++) out[x] = 0xFF000000u; /* main memory display: not emulated */
        goto advance;
    }

    {
        static uint16_t bgline[4][VIDEO_LINE_MAX];
        static HdLine bghd[4];
        static ObjLine ol, ol_all; /* ol_all: with the party, for the party layer (or the shifted picture) */
        static ObjLine ol_ui;      /* with the sprites in screen coordinates (zoom out) */
        memset(bgline, 0, sizeof bgline);
        if (hd_on) memset(bghd, 0, sizeof bghd);
        memset(&ol, 0, sizeof ol);
        uint32_t mode = dispcnt & 7;
        int enabled[4] = {0};
        /* Widescreen, beyond the room (room_clip): BGs that are not the room's (its rings are 512x256 text maps, a
         * box's 256 wide), such as a battle effect on a 512x512 map (BG0's bubbles in an enemy's Rainbow Storm over
         * the starry one-screen room), or one the HDMA scrolls line by line (hdma_bgs: the rippling BG0 in Magus's
         * portal), go on as drawn. */
        int own_bgs = 0;
        for (int bg = 0; bg < 4; bg++) {
            if (!(dispcnt & (0x100u << bg))) continue;
            if (bg == 0 && e->id == 0 && (dispcnt & 8)) continue; /* 3D layer: not rendered yet */
            enabled[bg] = 1;
            int affine = (bg == 3 && mode >= 1 && mode <= 5) || (bg == 2 && (mode == 2 || mode == 4 || mode == 5));
            if (affine || (reg16(e, 0x08 + 2 * bg) & 0xC000) == 0xC000 || (e->hdma_bgs >> bg & 1)) own_bgs |= 1 << bg;
            e->solid_bgs = (uint8_t)(e->solid_bgs & ~(1u << bg));
            if (m && !affine && solid_256(e, bg, dispcnt)) own_bgs |= 1 << bg, e->solid_bgs |= (uint8_t)(1u << bg);
            if (mode == 6 && e->id == 0) {
                if (bg == 2) bg_affine(e, 2, dispcnt, 1, bgline[2]);
                else if (bg != 0) enabled[bg] = 0;
                continue;
            }
            if (affine) bg_affine(e, bg, dispcnt, 0, bgline[bg]);
            else bg_text(e, bg, (own_bgs >> bg & 1) ? by : y, dispcnt, bgline[bg], hd_on ? &bghd[bg] : NULL);
        }
        /* Camera smoothing: the text box stays still while the field slides. On its lines (BG0/BG1 switched to
         * one-screen maps, margin_bg) the box's pixels go to the party layer, and the slid picture shows the room
         * under the box (the layers as the frame started, as in the margins). */
        static uint16_t under[2][VIDEO_LINE_MAX];
        int box = 0;
        if (on_screen && y < e->box_top && ((enabled[0] && box_bg(e, 0)) || (enabled[1] && box_bg(e, 1)))) e->box_top = y;
        for (int bg = 0; bg < 2; bg++) {
            if (!(e->sliding || e->rows) || !enabled[bg] || !margin_bg(e, bg)) continue;
            const uint16_t *f = e->frame_bg[bg];
            memset(under[bg], 0, sizeof under[bg]);
            int uy = (e->frame_solid >> bg & 1) && !on_screen ? y & 7 : y; /* a one-tile overlay: as bg_text has it */
            bg_text_cols(e, bg, uy, dispcnt, f[0], f[1], f[2], -m, SCREEN_W + m, under[bg], NULL);
            box |= 1 << bg;
        }
        if (box && !e->party && !e->rows) { /* the party layer starts here: the lines above have nothing on it */
            memset(party_frame, 0, (size_t)y * VIDEO_LINE_MAX * sizeof *party_frame);
            if (hd_on) memset(hd_party, 0, (size_t)y * VIDEO_LINE_MAX * sizeof *hd_party);
            e->party = 1;
        }
        int objs = (dispcnt & 0x1000) != 0;
        if (objs || (dispcnt & 0x8000)) render_objs(e, y, dispcnt, &ol, e->party, 0, 1);
        if (e->obj_stretch) stretch_objs(e, &ol);
        if (e->party) {
            memset(&ol_all, 0, sizeof ol_all);
            if (objs || (dispcnt & 0x8000)) render_objs(e, y, dispcnt, &ol_all, 0, 0, 1);
        }
        if (e->sel_shift) {
            memset(&ol_all, 0, sizeof ol_all);
            if (objs || (dispcnt & 0x8000)) render_objs(e, y, dispcnt, &ol_all, 0, e->sel_shift, 1);
        }
        /* Zoom out: this line has the text box or a sprite in screen coordinates, for the UI layer. */
        int ui_line = 0;
        if (e->rows && on_screen) {
            memset(&ol_ui, 0, sizeof ol_ui);
            if (objs || (dispcnt & 0x8000)) render_objs(e, y, dispcnt, &ol_ui, 0, 0, 0);
            ui_line = box || memchr(ol_ui.ui, 1, (size_t)width) || memchr(ol_ui.ui, 2, (size_t)width);
        }

        /* [0]: the screen's columns, [1]: the margins (as the frame started for layers margin_bg keeps). */
        int prio[2][4], margin_kept = 0;
        for (int bg = 0; bg < 4; bg++) {
            prio[0][bg] = prio[1][bg] = reg16(e, 0x08 + 2 * bg) & 3;
            if (margin_bg(e, bg)) prio[1][bg] = e->frame_bg[bg][0] & 3, margin_kept = 1;
        }
        uint16_t bld[2][2] = {{reg16(e, 0x50), reg16(e, 0x52)}, {reg16(e, 0x50), reg16(e, 0x52)}};
        if (margin_kept) bld[1][0] = e->frame_bld[0], bld[1][1] = e->frame_bld[1];
        int evy = reg16(e, 0x54) & 31;
        if (evy > 16) evy = 16;
        uint16_t backdrop = pal16(e->pal);
        int win0 = dispcnt & 0x2000, win1 = dispcnt & 0x4000, winobj = dispcnt & 0x8000;
        uint16_t winin = reg16(e, 0x48), winout = reg16(e, 0x4A);
        uint16_t w0h = reg16(e, 0x40), w1h = reg16(e, 0x42), w0v = reg16(e, 0x44), w1v = reg16(e, 0x46);
        int in_w0y = win0 && in_range(wy, w0v >> 8, w0v & 0xFF), in_w1y = win1 && in_range(wy, w1v >> 8, w1v & 0xFF);
        int ext = m && win_ext_on && win_lines[wy] == ((uint32_t)w1h << 16 | w0h); /* the line as the table has it */
        const int16_t *ext0 = ext ? win_ext[wy][0] : NULL, *ext1 = ext ? win_ext[wy][1] : NULL;
        /* A ring (effect_rows): its windows from its circles in the margins and on the rows beyond its band. */
        int ring_sp[2][2], ring_n = e->ring ? ring_spans(e, y, ring_sp) : 0;
        int ring_row = e->ring && (y < e->band_top || y >= e->band_top + e->band_rows);
        float ray_sp[2][2] = {{0, 0}, {0, 0}}; /* rays: the windows' spans on a row beyond their lines */
        int ray_row = e->rays && (y < e->band_top || y >= e->band_top + e->band_rows);
        for (int w = 0; w < 2 && ray_row; w++) {
            if (e->rays >> w & 1) ray_span(&ray_a[w], &ray_b[w], y < e->band_top ? ray_first_part[w] : ray_last_part[w], y, &ray_sp[w][0], &ray_sp[w][1]);
        }

        /* Pass 0: the picture (without the party when it has its own layer); pass 1: the shifted picture of the
         * age selector (sel_shift), or the party layer, the party's
         * pixels where a party sprite ends up on top or blended under a translucent layer on top (the darkness
         * overlay of dark rooms, BG0 at priority 0), else transparent (scenery in front of it still covers it);
         * pass 2: the UI layer of the zoom out, the text box and the sprites in screen coordinates. */
        for (int pass = 0; pass < 3; pass++) {
        if (pass == 1 && !e->party && !e->sel_shift) continue;
        if (pass == 2 && !ui_line) continue;
        const ObjLine *o = pass == 2 ? &ol_ui : pass ? &ol_all : &ol;
        uint32_t *dst = pass == 2 ? &ui_frame[y * VIDEO_LINE_MAX] : !pass ? out : e->sel_shift ? &shifted_frame[y * VIDEO_LINE_MAX] : &party_frame[FROW(e, y)];
        VideoHd *hd = !hd_on || pass == 2 ? NULL : !pass ? hd_row : e->sel_shift ? NULL : &hd_party[FROW(e, y)];
        const uint16_t *src[4] = {bgline[0], bgline[1], bgline[2], bgline[3]};
        for (int bg = 0; bg < 2; bg++) if (!pass && (box >> bg & 1)) src[bg] = under[bg];
        for (int x = 0; x < width; x++) { /* buffer column; windows extend their edge columns into the margins */
            uint32_t ctl = 0x3F;
            int wx = SDL_clamp(x - m, 0, SCREEN_W - 1), side = x - m != wx || (!pass && box) || !on_screen;
            int still = 0; /* a text box pixel (the party layer keeps it) */
            int beyond = pass != 2 && e->room_clip && (x - m < e->room_x0 || x - m >= e->room_x1 || (e->rows && (y < e->room_y0 || y >= e->room_y1)));
            int bgs = beyond ? e->repeat_bgs | own_bgs : 0xF; /* the BGs shown in this column */
            for (int bg = 0; bg < 2; bg++) still |= (pass == 2 || !e->rows) && (box >> bg & 1) && x - m == wx && (bgline[bg][x] & OPAQUE);
            const int *pr = prio[side];
            uint16_t bldcnt = bld[side][0], bldalpha = bld[side][1];
            int eva = SDL_min(bldalpha & 31, 16), evb = SDL_min((bldalpha >> 8) & 31, 16), effect = (bldcnt >> 6) & 3;
            /* The game's screen column the windows are tested at: the view's shift at a room's end moves the picture,
             * and the windows the game placed on it with it (Frog's Water falls in WIN0, on the enemy). */
            int vx = x - m + e->view_x, vwx = SDL_clamp(vx, 0, SCREEN_W - 1);
            if (e->ring && (ring_row || vx != vwx)) {
                int in = 0;
                for (int k = 0; k < ring_n; k++) in |= vx >= ring_sp[k][0] && vx < ring_sp[k][1];
                ctl = in ? winin & 0x3F
                         : e->ring_wins == 1 && in_w1y && in_window(vx, vwx, w1h, ext1) ? (winin >> 8) & 0x3F
                         : winobj && o->win[x] ? (winout >> 8) & 0x3F
                                               : winout & 0x3F;
            } else if (ray_row && (win0 || win1 || winobj) && !(y < e->band_top ? e->rays_up : e->rays_down)) {
                ctl = winobj && o->win[x] ? (winout >> 8) & 0x3F : winout & 0x3F; /* the effect ended above, below */
            } else if (ray_row && (win0 || win1 || winobj)) {
                int in0 = win0 && (e->rays & 1 ? vx >= ray_sp[0][0] && vx < ray_sp[0][1] : in_w0y && in_window(vx, vwx, w0h, ext0));
                int in1 = win1 && (e->rays & 2 ? vx >= ray_sp[1][0] && vx < ray_sp[1][1] : in_w1y && in_window(vx, vwx, w1h, ext1));
                ctl = in0 ? winin & 0x3F : in1 ? (winin >> 8) & 0x3F : winobj && o->win[x] ? (winout >> 8) & 0x3F : winout & 0x3F;
            } else if (win0 || win1 || winobj) {
                if (in_w0y && in_window(vx, vwx, w0h, ext0)) ctl = winin & 0x3F;
                else if (in_w1y && in_window(vx, vwx, w1h, ext1)) ctl = (winin >> 8) & 0x3F;
                else if (winobj && o->win[x]) ctl = (winout >> 8) & 0x3F;
                else ctl = winout & 0x3F;
            }
            /* Top two layers: 0-3 BG, 4 OBJ, 5 backdrop. */
            int layer[2] = {5, 5};
            uint16_t col[2] = {backdrop, backdrop};
            int found = 0;
            for (int p = 0; p < 4 && found < 2; p++) {
                if (objs && (ctl & 0x10) && (o->color[x] & OPAQUE) && o->prio[x] == p) {
                    layer[found] = 4; col[found] = o->color[x] & 0x7FFF; found++;
                }
                for (int bg = 0; bg < 4 && found < 2; bg++) {
                    if (enabled[bg] && (bgs >> bg & 1) && pr[bg] == p && (ctl & (1u << bg)) && (src[bg][x] & OPAQUE)) {
                        layer[found] = bg; col[found] = src[bg][x] & 0x7FFF; found++;
                    }
                }
            }
            uint16_t c = col[0];
            if (e->track_top && !pass && x - m == wx) {
                int top = layer[0], on_panel = wx >= SS_MAP_X && wx < SS_MAP_X + SS_MAP_W && y >= SS_MAP_Y && y < SS_MAP_Y + SS_MAP_H;
                uint16_t top_col = col[0];
                if (e->panel_ui) { /* the map's layers under the buttons, with the marker as it last was */
                    top = 5, top_col = backdrop;
                    for (int p = 0; p < 4 && top == 5; p++) {
                        for (int bg = 1; bg < 4 && top == 5; bg += 2) {
                            if (enabled[bg] && pr[bg] == p && (ctl & (1u << bg)) && (src[bg][x] & OPAQUE)) top = bg, top_col = src[bg][x];
                        }
                    }
                    if (on_panel && (marker_held[y][wx] & OPAQUE)) top = 4, top_col = marker_held[y][wx];
                } else if (on_panel) {
                    marker_now[y][wx] = top == 4 ? top_col | OPAQUE : 0;
                    marker_seen |= top == 4;
                }
                bottom_top.color[y][wx] = top_col & 0x7FFF;
                bottom_top.layer[y][wx] = (uint8_t)top;
            }
            int first = (bldcnt >> layer[0]) & 1, second = (bldcnt >> (8 + layer[1])) & 1;
            int sprite_shows = layer[0] == 4; /* the sprite on top, or seen through a translucent layer on top */
            int weight = 16;    /* the top layer's share, and the colour added (HD sources) */
            uint16_t added = 0;
            int blended = 0;    /* the second layer shows through the top one */
            if (layer[0] == 4 && o->alpha[x] != 0xFF && second) {
                blended = 1;
                int ea = o->alpha[x] == 0xFE ? eva : o->alpha[x], eb = o->alpha[x] == 0xFE ? evb : 16 - o->alpha[x];
                c = blend(col[0], col[1], ea, eb);
                weight = ea, added = blend(col[1], 0, eb, 0);
            } else if ((ctl & 0x20) && first) {
                if (effect == 1 && second) {
                    blended = 1;
                    c = blend(col[0], col[1], eva, evb), sprite_shows |= layer[1] == 4;
                    weight = eva, added = blend(col[1], 0, evb, 0);
                } else if (effect == 2) {
                    c = brighten(c, evy, 1);
                    weight = 16 - evy, added = blend(0x7FFF, 0, evy, 0);
                } else if (effect == 3) {
                    c = brighten(c, evy, 0);
                    weight = 16 - evy;
                }
            }
            dst[x] = pass && !e->sel_shift && !(sprite_shows && (pass == 2 ? o->ui[x] : o->party[x])) && !still ? 0 : to_rgba(c);
            if (pass == 2) ui_stays[y * VIDEO_LINE_MAX + x] = dst[x] && !still && o->ui[x] == 2;
            if (!pass && !side && !margin_kept && (wx < SIDE_COLS || wx >= SCREEN_W - SIDE_COLS)) { /* what the screen shows of each BG (repeat_bgs) */
                if (layer[0] < 4) e->shown_now |= (uint8_t)(1 << layer[0]);
                if (layer[1] < 4 && blended) e->shown_now |= (uint8_t)(1 << layer[1]);
            }
            if (pass && e->sel_shift) shifted_sprite[y * VIDEO_LINE_MAX + x] = (uint8_t)sprite_shows;
            /* Beyond the room with no layer repeating, black but where a BG of its own is on top. A one-tile overlay
             * blended over the room (the Mammon Machine's glow over its hall) tints the room alone, the margins black;
             * on a text box's lines the margins show it as the frame started (under). */
            int solid = layer[0] < 4 && ((layer[0] < 2 && src[layer[0]] == under[layer[0]] ? e->frame_solid : e->solid_bgs) >> layer[0] & 1);
            int tint = solid && (ctl & 0x20) && first && effect == 1;
            int cut = beyond && !e->repeat_bgs && !(layer[0] < 4 && (own_bgs >> layer[0] & 1) && !tint);
            if (cut) dst[x] = pass ? 0 : 0xFF000000u;
            /* The still layer: a pixel of a still BG or the backdrop on top, not blended with a layer that slides. */
            int to_still = !pass && e->still_bgs && !cut && still_layer(e, layer[0]) &&
                           (!blended || still_layer(e, layer[1]));
            if (to_still) still_frame[FROW(e, y) + x] = dst[x], dst[x] = 0;
            if (hd) {
                const HdLine *h = layer[0] == 4 ? &o->hd : layer[0] < 4 && src[layer[0]] == bgline[layer[0]] ? &bghd[layer[0]] : NULL;
                uint32_t s = h && (dst[x] || to_still) && !cut ? h->src[x] : 0;
                VideoHd r = s ? (VideoHd){s, h->pal[x], (uint8_t)weight, added, found > 1 ? col[1] : backdrop} : (VideoHd){0};
                if (to_still) hd_still[FROW(e, y) + x] = r, hd[x] = (VideoHd){0};
                else hd[x] = r;
            }
        }
        }
    }

advance:
    for (int i = 0; i < 2; i++) {
        e->ref_x[i] += (int16_t)reg16(e, 0x22 + 0x10 * i);
        e->ref_y[i] += (int16_t)reg16(e, 0x26 + 0x10 * i);
    }
    /* Master brightness. */
    uint16_t mb = reg16(e, 0x6C);
    int f = mb & 31, mmode = mb >> 14;
    if (f > 16) f = 16;
    if (f && (mmode == 1 || mmode == 2)) {
        int layers = e->party || e->sel_shift ? 2 : 1;
        for (int pass = 0; pass < layers + (e->still_bgs != 0); pass++) {
            int still = pass == layers; /* the still layer, after the others */
            uint32_t *line = still ? &still_frame[FROW(e, y)] : !pass ? out : e->sel_shift ? &shifted_frame[y * VIDEO_LINE_MAX] : &party_frame[FROW(e, y)];
            VideoHd *hd = !hd_on ? NULL : still ? &hd_still[FROW(e, y)] : !pass ? hd_row : e->sel_shift ? NULL : &hd_party[FROW(e, y)];
            for (int x = 0; hd && x < width; x++) {
                VideoHd *r = &hd[x];
                if (!r->tile) continue;
                r->weight = (uint8_t)(r->weight * (16 - f) >> 4);
                r->added = (uint16_t)(blend(r->added, mmode == 1 ? 0x7FFF : 0, 16 - f, f));
                r->below = brighten(r->below, f, mmode == 1);
            }
            for (int x = 0; x < width; x++) {
                uint32_t p = line[x];
                if (!p) continue; /* transparent (party and still layers, main where the still layer shows) */
                line[x] = master_bright(p, f, mmode);
            }
        }
        for (int x = 0; e->rows && on_screen && x < width; x++) { /* the UI layer */
            uint32_t p = ui_frame[y * VIDEO_LINE_MAX + x];
            if (p) ui_frame[y * VIDEO_LINE_MAX + x] = master_bright(p, f, mmode);
        }
    }
    if (e->track_top) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t p = out[x + m];
            if (e->panel_ui) {
                p = to_rgba(bottom_top.color[y][x]);
                if (f && (mmode == 1 || mmode == 2)) p = master_bright(p, f, mmode);
            }
            bottom_top.rgba[y][x] = p;
        }
    }
}

void video_render_line(int line) {
    if (line == 0) hd_on = gfxmod_hd_scale() > 0;
    for (int i = 0; i < 2; i++) {
        Engine *e = &engines[i];
        if (line == 0) engine_frame_start(e);
        int y0 = line ? line : -e->rows, y1 = line + (line == SCREEN_H - 1 ? e->rows : 0);
        for (int y = y0; y <= y1; y++) {
            /* A row drawn as another line (effect_rows): with what the game's HDMA writes for that line. */
            int as = row_line(e, y);
            static WsHdmaSaved saved;
            if (as != line) ws_hdma_line(as, &saved);
            render_engine_line(e, y, &frame[i][FROW(e, y)]);
            if (as != line) ws_hdma_restore(&saved);
        }
    }
}

void video_set_fill_sides(int on) { SDL_SetAtomicInt(&fill_sides, on); }

void video_set_track_bottom(int on) { SDL_SetAtomicInt(&track_bottom, on); }

const VideoBottomTop *video_bottom_top(void) { return &shown_bottom_top; }

int video_box_top(void) { return engines[(io_get16(0x304) >> 15) & 1 ? 0 : 1].box_top; }

void video_set_wide_margin(int margin) { SDL_SetAtomicInt(&wide_margin, SDL_clamp(margin, 0, VIDEO_MARGIN_MAX)); }

void video_set_zoom(int rows, int margin) {
    SDL_SetAtomicInt(&zoom_margin, SDL_clamp(margin, 0, VIDEO_MARGIN_MAX));
    SDL_SetAtomicInt(&zoom_rows, SDL_clamp(rows, 0, (VIDEO_ROWS_MAX - SCREEN_H) / 2));
}

/* Zoomed out with the smaller text, the UI layer as tall as the picture: the DS screen's 192 rows in its middle, but
 * a box in the screen's upper half (its rows together with the sprites on them, such as the battle's panels and the
 * hand of a box's choices) at the top of the view and one in the lower half at its bottom, where the DS screen has
 * them; a box across the middle stays. Left in the middle, the boxes covered the room around the party, the view's
 * edges empty. The numbers placed at an actor ("Miss!", damage) stay over it. shown: the view's rows above and below
 * the screen (rows less the guard rows of camera smoothing). */
static void ui_to_edges(int w, int rows, int shown) {
    static uint8_t moves[SCREEN_H];
    for (int y = 0; y < SCREEN_H; y++) {
        moves[y] = 0;
        for (int x = 0; x < w && !moves[y]; x++) moves[y] = ui_frame[y * VIDEO_LINE_MAX + x] && !ui_stays[y * VIDEO_LINE_MAX + x];
    }
    memset(present_ui, 0, (size_t)w * (SCREEN_H + 2 * rows) * 4);
    for (int y0 = 0, y1; y0 < SCREEN_H; y0 = y1) {
        for (y1 = y0 + 1; y1 < SCREEN_H && moves[y1] == moves[y0]; y1++) {}
        int dy = !moves[y0] ? 0 : y1 <= SCREEN_H / 2 ? -shown : y0 >= SCREEN_H / 2 ? shown : 0; /* the band y0..y1-1 */
        for (int y = y0; y < y1; y++) {
            const uint32_t *src = &ui_frame[y * VIDEO_LINE_MAX];
            const uint8_t *stays = &ui_stays[y * VIDEO_LINE_MAX];
            uint32_t *to = &present_ui[(y + rows + dy) * w], *at = &present_ui[(y + rows) * w];
            for (int x = 0; x < w; x++) {
                if (src[x]) (stays[x] ? at : to)[x] = src[x];
            }
        }
    }
}

void video_end_frame(void) {
    /* POWCNT1 bit 15: engine A on the top screen. */
    int a_top = (io_get16(0x304) >> 15) & 1;
    const uint32_t *ft = frame[a_top ? 0 : 1], *fb = frame[a_top ? 1 : 0];
    const Engine *eb = &engines[a_top ? 1 : 0];
    const Engine *et = &engines[a_top ? 0 : 1];
    int m = et->margin, mb = eb->margin, w = SCREEN_W + 2 * m;
    int rows = et->rows, h = SCREEN_H + 2 * rows; /* the top screen's picture, with the zoom out's rows */
    SDL_LockMutex(present_lock);
    for (int y = 0; y < SCREEN_H; y++) {
        memcpy(&present[0][y * SCREEN_W], &ft[(y + rows) * VIDEO_LINE_MAX + m], SCREEN_W * 4);
        memcpy(&present[1][y * SCREEN_W], &fb[y * VIDEO_LINE_MAX + mb], SCREEN_W * 4);
    }
    for (int y = 0; y < h; y++) memcpy(&present_wide[y * w], &ft[y * VIDEO_LINE_MAX], (size_t)w * 4);
    present_shifted_on = eb->sel_shift && mb == m; /* as wide as the top screen's (the margin read for each) */
    for (int y = 0; present_shifted_on && y < SCREEN_H; y++) {
        memcpy(&present_shifted[y * w], &shifted_frame[y * VIDEO_LINE_MAX], (size_t)w * 4);
        memcpy(&present_shifted_sprite[y * w], &shifted_sprite[y * VIDEO_LINE_MAX], (size_t)w);
    }
    int main_from;
    present_main_w = ss_compose(present_main, present[0], present[1], present_wide, w, h, present_shifted_on ? present_shifted : NULL,
                                present_shifted_sprite, &main_from);
    present_main_h = main_from == SS_FROM_WIDE ? h : SCREEN_H;
    present_party_on = et->party && present_main_w == w; /* the field: main is the wide picture */
    for (int y = 0; present_party_on && y < h; y++) {
        memcpy(&present_party[y * w], &party_frame[y * VIDEO_LINE_MAX], (size_t)w * 4);
    }
    present_still_on = et->still_bgs && present_main_w == w;
    for (int y = 0; present_still_on && y < h; y++) {
        memcpy(&present_still[y * w], &still_frame[y * VIDEO_LINE_MAX], (size_t)w * 4);
    }
    present_ui_on = rows && present_main_h == h;
    present_ui_h = present_ui_on && SDL_GetAtomicInt(&ui_edges) ? h : SCREEN_H;
    if (present_ui_h == SCREEN_H) {
        for (int y = 0; present_ui_on && y < SCREEN_H; y++) {
            memcpy(&present_ui[y * w], &ui_frame[y * VIDEO_LINE_MAX], (size_t)w * 4);
        }
    } else {
        ui_to_edges(w, rows, SDL_max(rows - (SDL_GetAtomicInt(&party_wanted) ? VIDEO_SLIDE_ROWS : 0), 0));
    }
    present_hd_on = hd_on;
    if (hd_on) {
        const VideoHd *ht = hd_frame[a_top ? 0 : 1], *hb = hd_frame[a_top ? 1 : 0];
        for (int y = 0; y < SCREEN_H; y++) {
            memcpy(&present_hd[0][y * SCREEN_W], &ht[(y + rows) * VIDEO_LINE_MAX + m], SCREEN_W * sizeof *ht);
            memcpy(&present_hd[1][y * SCREEN_W], &hb[y * VIDEO_LINE_MAX + mb], SCREEN_W * sizeof *hb);
            for (int x = 0; x < SCREEN_W; x++) { /* no sources under the runtime's texts (ss_compose) */
                if (present[1][y * SCREEN_W + x] != fb[y * VIDEO_LINE_MAX + mb + x]) present_hd[1][y * SCREEN_W + x] = (VideoHd){0};
            }
        }
        for (int y = 0; y < h; y++) {
            memcpy(&present_hd_wide[y * w], &ht[y * VIDEO_LINE_MAX], (size_t)w * sizeof *ht);
            if (present_party_on) memcpy(&present_hd_party[y * w], &hd_party[y * VIDEO_LINE_MAX], (size_t)w * sizeof *hd_party);
            if (present_still_on) memcpy(&present_hd_still[y * w], &hd_still[y * VIDEO_LINE_MAX], (size_t)w * sizeof *hd_still);
        }
        /* The main screen: a straight copy of one of them (ss_compose), its pixels' sources where they are as there
         * (not under the runtime's texts). */
        const uint32_t *from = main_from == SS_FROM_WIDE ? present_wide : main_from == SS_FROM_TOP ? present[0]
                               : main_from == SS_FROM_BOTTOM ? present[1] : NULL;
        const VideoHd *from_hd = main_from == SS_FROM_WIDE ? present_hd_wide : main_from == SS_FROM_TOP ? present_hd[0] : present_hd[1];
        for (int i = 0; i < present_main_w * present_main_h; i++) {
            present_hd_main[i] = from && present_main[i] == from[i] ? from_hd[i] : (VideoHd){0};
        }
        memcpy(present_palette, rt_palette, sizeof present_palette);
        hd_stamp++;
    }
    present_bottom_top_on = eb->track_top;
    if (eb->track_top) {
        for (int i = 0; i < 256; i++) bottom_top.palette[i] = pal16(eb->pal + (uint32_t)i * 2) & 0x7FFF;
        bottom_top.bg3_hofs = reg16(eb, 0x1C) & 0x1FF, bottom_top.bg3_vofs = reg16(eb, 0x1E) & 0x1FF;
        present_bottom_top = bottom_top;
        if (marker_seen && !eb->panel_ui && memcmp(marker_now, marker_left, sizeof marker_now) != 0) {
            memcpy(marker_held, marker_now, sizeof marker_held);
        }
    }
    present_dx = frame_dx, present_dy = frame_dy, present_cut = frame_cut;
    present_time = SDL_GetTicksNS();
    present_serial++;
    SDL_UnlockMutex(present_lock);
}

int video_copy_frame(uint32_t *top, uint32_t *bottom, uint32_t *main, int *main_h, uint32_t *party, int *party_on, uint32_t *still,
                     int *still_on, uint32_t *ui, int *ui_on, VideoHdFrame *hd) {
    SDL_LockMutex(present_lock);
    memcpy(top, present[0], sizeof present[0]);
    memcpy(bottom, present[1], sizeof present[1]);
    int w = present_main_w, h = *main_h = present_main_h;
    memcpy(main, present_main, (size_t)w * h * 4);
    *ui_on = present_ui_on;
    if (present_ui_on) memcpy(ui, present_ui, (size_t)w * present_ui_h * 4);
    if (present_bottom_top_on) shown_bottom_top = present_bottom_top;
    *party_on = present_party_on;
    if (present_party_on) memcpy(party, present_party, (size_t)w * h * 4);
    *still_on = present_still_on;
    if (present_still_on) memcpy(still, present_still, (size_t)w * h * 4);
    if (hd && (hd->on = present_hd_on)) {
        memcpy(hd->top, present_hd[0], sizeof present_hd[0]);
        memcpy(hd->bottom, present_hd[1], sizeof present_hd[1]);
        memcpy(hd->main, present_hd_main, (size_t)w * h * sizeof *present_hd_main);
        if (present_party_on) memcpy(hd->party, present_hd_party, (size_t)w * h * sizeof *present_hd_party);
        if (present_still_on) memcpy(hd->still, present_hd_still, (size_t)w * h * sizeof *present_hd_still);
        memcpy(hd->palette, present_palette, sizeof present_palette);
    }
    SDL_UnlockMutex(present_lock);
    return w;
}

uint64_t video_frame_serial(int *camera_dx, int *camera_dy, int *cut, uint64_t *time_ns) {
    SDL_LockMutex(present_lock);
    uint64_t s = present_serial;
    *camera_dx = present_dx, *camera_dy = present_dy, *cut = present_cut, *time_ns = present_time;
    SDL_UnlockMutex(present_lock);
    return s;
}

void video_state(RtStateIO *s) {
    for (int i = 0; i < 2; i++) {
        RT_STATE(s, engines[i].ref_x);
        RT_STATE(s, engines[i].ref_y);
        RT_STATE(s, engines[i].ref_reload);
    }
    if (rt_state_loading(s)) {
        video_vramcnt_written(); /* VRAM bank mapping follows the restored VRAMCNT */
        camera_known = read_known = 0; /* no camera motion across the load */
    }
}
