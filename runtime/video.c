/* DS 2D graphics: VRAM bank mapping (VRAMCNT_A..I) and per-scanline rendering of engines A and B.
 * Reference: GBATEK "DS Video". The 3D layer (engine A BG0 in 3D mode) is not rendered yet. */
#include <SDL3/SDL.h>

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
    int obj_origin; /* sprite X counts from this many columns left of the screen (widescreen.c) */
    int obj_stretch; /* the sprites' screen columns are stretched across the margins (the warp, single_screen.c) */
    int party;      /* camera smoothing: this frame has sprites that move with the camera (party layer) */
    int sliding;    /* camera smoothing: the display slides this frame (the camera moved into it) */
    uint8_t follows[128]; /* by OAM slot: the sprite moves with the camera */
    uint16_t frame_bg[4][3]; /* widescreen: BGxCNT, HOFS, VOFS as the frame started, and its blending: */
    uint16_t frame_bld[2];   /* BLDCNT, BLDALPHA (see margin_bg) */
    uint8_t mirror_bg;       /* widescreen: text BGs whose margins show the screen reflected (see picture_256) */
} Engine;

static Engine engines[2];
static SDL_AtomicInt wide_margin; /* requested by the display (video_set_wide_margin) */
static uint32_t frame[2][VIDEO_LINE_MAX * SCREEN_H]; /* by engine, VIDEO_LINE_MAX apart */
static uint32_t present[2][SCREEN_W * SCREEN_H];      /* top, bottom */
static uint32_t present_wide[VIDEO_LINE_MAX * SCREEN_H], present_main[VIDEO_LINE_MAX * SCREEN_H];
static int present_main_w;
static SDL_Mutex *present_lock;
static uint64_t present_serial, present_time;
static int present_dx, present_dy;  /* camera motion into the published frame */
static int camera_known, camera_x, camera_y; /* the field camera at the last frame */
static int frame_dx, frame_dy;      /* camera motion into the frame being drawn */
static SDL_AtomicInt party_wanted;  /* camera smoothing is on (video_set_camera_smoothing) */
static uint8_t prev_oam[0x400];     /* the top screen's sprites in the previous frame */
static uint32_t party_frame[VIDEO_LINE_MAX * SCREEN_H]; /* party layer of the top screen (alpha 0: none) */
static uint32_t present_party[VIDEO_LINE_MAX * SCREEN_H];
static int present_party_on;

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
 * its lines the game switches BG0/BG1 from the field's 512-wide layers to 256-wide box maps. Such a map is drawn
 * in the screen's columns alone, and the margins keep the layer as the frame started (its map, scroll, priority
 * and blending), so the room goes on beside the box. */
static int margin_bg(const Engine *e, int bg) {
    return e->margin && !(reg16(e, 0x08 + 2 * bg) & 0x4000) && (e->frame_bg[bg][0] & 0x4000);
}

/* Columns x0..x1-1 of a text BG line, with these register values. */
static void bg_text_cols(const Engine *e, int bg, int y, uint32_t dispcnt, uint16_t cnt, uint32_t hofs, uint32_t vofs,
                         int x0, int x1, uint16_t *out) {
    int m = e->margin;
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
        if (bpp8) {
            uint32_t idx = vr8(e->bg, e->bg_mask, chr + tile * 64 + py * 8 + px);
            if (idx) out[x + m] = (ext ? ext16(slot, pal * 256 + idx) : pal16(e->pal + idx * 2)) | OPAQUE;
        } else {
            uint32_t b = vr8(e->bg, e->bg_mask, chr + tile * 32 + py * 4 + px / 2);
            uint32_t idx = (px & 1) ? b >> 4 : b & 0xF;
            if (idx) out[x + m] = pal16(e->pal + pal * 32 + idx * 2) | OPAQUE;
        }
    }
}

/* Widescreen: a 512-wide map holding a picture in its left 256 columns only, the rest one entry throughout, shown
 * unscrolled: a SNES screen drawn by the game (the lightning bolts of Lightning II, drawn into tiles). Its margins
 * would be empty, so they show the screen reflected at its sides, and what reaches a side goes on. Checked while a
 * battle effect sets the windows line by line (ws_window_lines), so menus and other layers are left alone. */
static int picture_256(const Engine *e, int bg, uint32_t dispcnt) {
    uint16_t cnt = reg16(e, 0x08 + 2 * bg);
    if ((cnt & 0x4000) == 0 || (reg16(e, 0x10 + 4 * bg) & 0x1FF)) return 0;
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

static void bg_text(const Engine *e, int bg, int y, uint32_t dispcnt, uint16_t *out) {
    int m = e->margin;
    uint16_t cnt = reg16(e, 0x08 + 2 * bg), hofs = reg16(e, 0x10 + 4 * bg), vofs = reg16(e, 0x12 + 4 * bg);
    if (!m || (cnt & 0x4000)) {
        bg_text_cols(e, bg, y, dispcnt, cnt, hofs, vofs, -m, SCREEN_W + m, out);
        if (m && (e->mirror_bg >> bg & 1) && !(hofs & 0x1FF)) {
            for (int k = 0; k < m; k++) out[m - 1 - k] = out[m + k], out[m + SCREEN_W + k] = out[m + SCREEN_W - 1 - k];
        }
        return;
    }
    bg_text_cols(e, bg, y, dispcnt, cnt, hofs, vofs, 0, SCREEN_W, out);
    if (margin_bg(e, bg)) {
        const uint16_t *f = e->frame_bg[bg];
        bg_text_cols(e, bg, y, dispcnt, f[0], f[1], f[2], -m, 0, out);
        bg_text_cols(e, bg, y, dispcnt, f[0], f[1], f[2], SCREEN_W, SCREEN_W + m, out);
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
} ObjLine;

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static inline uint16_t oam16(const Engine *e, uint32_t off) {
    uint16_t v;
    memcpy(&v, &rt_oam[e->oam + (off & 0x3FE)], 2);
    return v;
}

/* skip_party: leave out the sprites that move with the camera (the scenery picture of camera smoothing). */
static void render_objs(const Engine *e, int y, uint32_t dispcnt, ObjLine *ol, int skip_party) {
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
        int ly = (y - (a0 & 0xFF)) & 0xFF;
        if (ly >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        ox += m - e->obj_origin;        /* buffer column */
        if (ox + bw <= 0) ox += 512;   /* X wraps at 512: beyond the left edge is the right one */
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
            }
            if (!(ol->color[sx] & OPAQUE) || prio < ol->prio[sx]) {
                ol->color[sx] = color | OPAQUE;
                ol->prio[sx] = (uint8_t)prio;
                ol->alpha[sx] = alpha;
                ol->party[sx] = e->follows[i];
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

static void find_party(Engine *e, int dx, int dy) {
    const uint8_t *c = &rt_oam[e->oam];
    e->party = 0;
    memset(e->follows, 0, sizeof e->follows);
    for (int i = 0; (dx || dy) && i < 128; i++) {
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
        if (best >= 0 && SDL_abs(bx) + SDL_abs(by) < SDL_abs(bx + dx) + SDL_abs(by + dy)) e->follows[i] = 1, e->party = 1;
    }
    memcpy(prev_oam, c, sizeof prev_oam);
}

void video_set_camera_smoothing(int on) { SDL_SetAtomicInt(&party_wanted, on); }

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

static int fit_rays(int w, int below_menu) {
    int py[2 * SCREEN_H], px[2 * SCREEN_H], n = 0, x[2], first = -1;
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
    fit_rays(0, below_menu);
    fit_rays(1, below_menu);
}

/* Screen column sx (wx: it clamped to the screen) in a window with positions h (ext: its span carried into the
 * margins, or NULL). With ext, a window cut at the right side also takes the screen's last column. */
static inline int in_window(int sx, int wx, uint16_t h, const int16_t *ext) {
    if (sx == wx) return in_range(wx, h >> 8, ext && (h & 0xFF) == 0xFF ? SCREEN_W : h & 0xFF);
    return ext && ext[0] <= ext[1] ? sx >= ext[0] && sx < ext[1] : in_range(wx, h >> 8, h & 0xFF);
}

static void render_engine_line(Engine *e, int y, uint32_t *out) {
    uint32_t dispcnt = reg32(e, 0);
    if (y == 0) {
        reload_ref(e, 0);
        reload_ref(e, 1);
        int a_top = (io_get16(0x304) >> 15) & 1; /* POWCNT1 bit 15: engine A on the top screen */
        e->margin = e->id == !a_top ? SDL_GetAtomicInt(&wide_margin) : 0; /* widescreen: the top screen */
        e->party = e->sliding = 0;
        for (int bg = 0; bg < 4; bg++) {
            e->frame_bg[bg][0] = reg16(e, 0x08 + 2 * bg);
            e->frame_bg[bg][1] = reg16(e, 0x10 + 4 * bg);
            e->frame_bg[bg][2] = reg16(e, 0x12 + 4 * bg);
        }
        e->frame_bld[0] = reg16(e, 0x50), e->frame_bld[1] = reg16(e, 0x52);
        if (e->id == !a_top) {
            ws_fill_margins(e->margin);
            win_ext_on = e->margin && ws_window_lines(win_lines);
            if (win_ext_on) window_margins();
            e->mirror_bg = 0;
            for (int bg = 0; bg < 4 && win_ext_on; bg++) e->mirror_bg |= (uint8_t)(picture_256(e, bg, dispcnt) << bg);
            e->obj_origin = ws_sprite_origin(); /* sprites placed from the left margin (widened view) */
            e->obj_stretch = e->margin && ss_in_warp();
            int cx = 0, cy = 0, known = ws_camera(&cx, &cy); /* camera motion into this frame (camera smoothing) */
            frame_dx = frame_dy = 0;
            if (known && camera_known) {
                frame_dx = cx - camera_x, frame_dy = cy - camera_y;
                if (SDL_abs(frame_dx) > VIDEO_SLIDE_MAX || SDL_abs(frame_dy) > VIDEO_SLIDE_MAX) frame_dx = frame_dy = 0; /* a cut */
            }
            camera_known = known, camera_x = cx, camera_y = cy;
            if (SDL_GetAtomicInt(&party_wanted)) find_party(e, frame_dx, frame_dy), e->sliding = frame_dx || frame_dy;
        }
    }
    int m = e->margin, width = SCREEN_W + 2 * m;
    for (int i = 0; i < 2; i++) if (e->ref_reload[i]) reload_ref(e, i);

    int disp_mode = (dispcnt >> 16) & (e->id ? 1 : 3);
    if (disp_mode == 0 || (dispcnt & 0x80)) {
        for (int x = 0; x < width; x++) out[x] = 0xFFFFFFFFu;
        goto advance;
    }
    if (disp_mode == 2) { /* VRAM display: LCDC bank as a 15-bit bitmap */
        uint8_t **bank = &lcdc[((dispcnt >> 18) & 3) * 8];
        for (int x = 0; x < width; x++) {
            int sx = x - m;
            out[x] = sx < 0 || sx >= SCREEN_W ? 0xFF000000u : to_rgba(vr16(bank, 0x1FFFF, ((uint32_t)y * 256 + sx) * 2));
        }
        goto advance;
    }
    if (disp_mode == 3) {
        for (int x = 0; x < width; x++) out[x] = 0xFF000000u; /* main memory display: not emulated */
        goto advance;
    }

    {
        static uint16_t bgline[4][VIDEO_LINE_MAX];
        static ObjLine ol, ol_all; /* ol_all: with the party, for the party layer */
        memset(bgline, 0, sizeof bgline);
        memset(&ol, 0, sizeof ol);
        uint32_t mode = dispcnt & 7;
        int enabled[4] = {0};
        for (int bg = 0; bg < 4; bg++) {
            if (!(dispcnt & (0x100u << bg))) continue;
            if (bg == 0 && e->id == 0 && (dispcnt & 8)) continue; /* 3D layer: not rendered yet */
            enabled[bg] = 1;
            int affine = (bg == 3 && mode >= 1 && mode <= 5) || (bg == 2 && (mode == 2 || mode == 4 || mode == 5));
            if (mode == 6 && e->id == 0) {
                if (bg == 2) bg_affine(e, 2, dispcnt, 1, bgline[2]);
                else if (bg != 0) enabled[bg] = 0;
                continue;
            }
            if (affine) bg_affine(e, bg, dispcnt, 0, bgline[bg]);
            else bg_text(e, bg, y, dispcnt, bgline[bg]);
        }
        /* Camera smoothing: the text box stays still while the field slides. On its lines (BG0/BG1 switched to
         * one-screen maps, margin_bg) the box's pixels go to the party layer, and the slid picture shows the room
         * under the box (the layers as the frame started, as in the margins). */
        static uint16_t under[2][VIDEO_LINE_MAX];
        int box = 0;
        for (int bg = 0; bg < 2; bg++) {
            if (!e->sliding || !enabled[bg] || !margin_bg(e, bg)) continue;
            const uint16_t *f = e->frame_bg[bg];
            memset(under[bg], 0, sizeof under[bg]);
            bg_text_cols(e, bg, y, dispcnt, f[0], f[1], f[2], -m, SCREEN_W + m, under[bg]);
            box |= 1 << bg;
        }
        if (box && !e->party) { /* the party layer starts here: the lines above have nothing on it */
            memset(party_frame, 0, (size_t)y * VIDEO_LINE_MAX * sizeof *party_frame);
            e->party = 1;
        }
        int objs = (dispcnt & 0x1000) != 0;
        if (objs || (dispcnt & 0x8000)) render_objs(e, y, dispcnt, &ol, e->party);
        if (e->obj_stretch) stretch_objs(e, &ol);
        if (e->party) {
            memset(&ol_all, 0, sizeof ol_all);
            if (objs || (dispcnt & 0x8000)) render_objs(e, y, dispcnt, &ol_all, 0);
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
        int in_w0y = win0 && in_range(y, w0v >> 8, w0v & 0xFF), in_w1y = win1 && in_range(y, w1v >> 8, w1v & 0xFF);
        int ext = m && win_ext_on && win_lines[y] == ((uint32_t)w1h << 16 | w0h); /* the line as the table has it */
        const int16_t *ext0 = ext ? win_ext[y][0] : NULL, *ext1 = ext ? win_ext[y][1] : NULL;

        /* Pass 0: the picture (without the party when it has its own layer); pass 1: the party layer, the party's
         * pixels where a party sprite ends up on top (scenery in front of it still covers it), else transparent. */
        for (int pass = 0; pass < (e->party ? 2 : 1); pass++) {
        const ObjLine *o = pass ? &ol_all : &ol;
        uint32_t *dst = pass ? &party_frame[y * VIDEO_LINE_MAX] : out;
        const uint16_t *src[4] = {bgline[0], bgline[1], bgline[2], bgline[3]};
        for (int bg = 0; bg < 2; bg++) if (!pass && (box >> bg & 1)) src[bg] = under[bg];
        for (int x = 0; x < width; x++) { /* buffer column; windows extend their edge columns into the margins */
            uint32_t ctl = 0x3F;
            int wx = SDL_clamp(x - m, 0, SCREEN_W - 1), side = x - m != wx || (!pass && box);
            int still = 0; /* a text box pixel (the party layer keeps it) */
            for (int bg = 0; bg < 2; bg++) still |= (box >> bg & 1) && x - m == wx && (bgline[bg][x] & OPAQUE);
            const int *pr = prio[side];
            uint16_t bldcnt = bld[side][0], bldalpha = bld[side][1];
            int eva = SDL_min(bldalpha & 31, 16), evb = SDL_min((bldalpha >> 8) & 31, 16), effect = (bldcnt >> 6) & 3;
            if (win0 || win1 || winobj) {
                if (in_w0y && in_window(x - m, wx, w0h, ext0)) ctl = winin & 0x3F;
                else if (in_w1y && in_window(x - m, wx, w1h, ext1)) ctl = (winin >> 8) & 0x3F;
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
                    if (enabled[bg] && pr[bg] == p && (ctl & (1u << bg)) && (src[bg][x] & OPAQUE)) {
                        layer[found] = bg; col[found] = src[bg][x] & 0x7FFF; found++;
                    }
                }
            }
            uint16_t c = col[0];
            int first = (bldcnt >> layer[0]) & 1, second = (bldcnt >> (8 + layer[1])) & 1;
            if (layer[0] == 4 && o->alpha[x] != 0xFF && second) {
                if (o->alpha[x] == 0xFE) c = blend(col[0], col[1], eva, evb);
                else c = blend(col[0], col[1], o->alpha[x], 16 - o->alpha[x]);
            } else if ((ctl & 0x20) && first) {
                if (effect == 1 && second) c = blend(col[0], col[1], eva, evb);
                else if (effect == 2) c = brighten(c, evy, 1);
                else if (effect == 3) c = brighten(c, evy, 0);
            }
            dst[x] = pass && !(layer[0] == 4 && o->party[x]) && !still ? 0 : to_rgba(c);
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
        for (int pass = 0; pass < (e->party ? 2 : 1); pass++) {
            uint32_t *line = pass ? &party_frame[y * VIDEO_LINE_MAX] : out;
            for (int x = 0; x < width; x++) {
                uint32_t p = line[x];
                if (!p) continue; /* transparent (party layer) */
                uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
                if (mmode == 1) { r += ((255 - r) * f) >> 4; g += ((255 - g) * f) >> 4; b += ((255 - b) * f) >> 4; }
                else { r -= (r * f) >> 4; g -= (g * f) >> 4; b -= (b * f) >> 4; }
                line[x] = 0xFF000000u | b << 16 | g << 8 | r;
            }
        }
    }
}

void video_render_line(int line) {
    for (int i = 0; i < 2; i++) render_engine_line(&engines[i], line, &frame[i][line * VIDEO_LINE_MAX]);
}

void video_set_wide_margin(int margin) { SDL_SetAtomicInt(&wide_margin, SDL_clamp(margin, 0, VIDEO_MARGIN_MAX)); }

void video_end_frame(void) {
    /* POWCNT1 bit 15: engine A on the top screen. */
    int a_top = (io_get16(0x304) >> 15) & 1;
    const uint32_t *ft = frame[a_top ? 0 : 1], *fb = frame[a_top ? 1 : 0];
    int m = engines[a_top ? 0 : 1].margin, w = SCREEN_W + 2 * m;
    SDL_LockMutex(present_lock);
    for (int y = 0; y < SCREEN_H; y++) {
        memcpy(&present[0][y * SCREEN_W], &ft[y * VIDEO_LINE_MAX + m], SCREEN_W * 4);
        memcpy(&present[1][y * SCREEN_W], &fb[y * VIDEO_LINE_MAX], SCREEN_W * 4);
        memcpy(&present_wide[y * w], &ft[y * VIDEO_LINE_MAX], (size_t)w * 4);
    }
    present_main_w = ss_compose(present_main, present[0], present[1], present_wide, w);
    present_party_on = engines[a_top ? 0 : 1].party && present_main_w == w; /* the field: main is the wide picture */
    for (int y = 0; present_party_on && y < SCREEN_H; y++) {
        memcpy(&present_party[y * w], &party_frame[y * VIDEO_LINE_MAX], (size_t)w * 4);
    }
    present_dx = frame_dx, present_dy = frame_dy;
    present_time = SDL_GetTicksNS();
    present_serial++;
    SDL_UnlockMutex(present_lock);
}

int video_copy_frame(uint32_t *top, uint32_t *bottom, uint32_t *main, uint32_t *party, int *party_on) {
    SDL_LockMutex(present_lock);
    memcpy(top, present[0], sizeof present[0]);
    memcpy(bottom, present[1], sizeof present[1]);
    memcpy(main, present_main, (size_t)present_main_w * SCREEN_H * 4);
    int w = present_main_w;
    *party_on = present_party_on;
    if (present_party_on) memcpy(party, present_party, (size_t)w * SCREEN_H * 4);
    SDL_UnlockMutex(present_lock);
    return w;
}

uint64_t video_frame_serial(int *camera_dx, int *camera_dy, uint64_t *time_ns) {
    SDL_LockMutex(present_lock);
    uint64_t s = present_serial;
    *camera_dx = present_dx, *camera_dy = present_dy, *time_ns = present_time;
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
        camera_known = 0;        /* no camera motion across the load */
    }
}
