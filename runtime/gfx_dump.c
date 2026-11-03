/* Graphics dump (System Settings > Dump graphics): the game's sprites, backgrounds and fonts as PNG files in
 * mods/graphics/dump, for graphics mods. It reads the ROM in memory (rt_rom.data: a translation's patched copy,
 * with the same-size graphics mods applied), on a thread of its own while the game runs. The formats: gfx_rom.c.
 *
 * Written (paths under dump/ are the ROM's, with .png added):
 *   sheets/<file>.png      each tile sheet (.NCGR, _ncg.bin), 16 tiles wide, in its first palette row
 *   frames/<file>/NNN.png  each cell of a .NCER, drawn from the sheet of the same name
 *   screens/<file>.png     each _nsc.bin, drawn from the _ncg.bin of the same name
 *   fonts/<file>.png       each message font (.fnt), its glyphs 16 to a row in cells of one size; pixel values 0-3
 *   fonts/<file>.txt       the font's advance widths and ASCII characters, by glyph
 *   palettes.txt           the palette each image was drawn with
 * The images are indexed: the pixels keep the game's palette indexes, with index 0 of each 16-colour row (of the
 * palette, for 8 bits per pixel) transparent. Without a palette for a sheet (character sprites: the game picks
 * their palettes in code) the image is a grey ramp, the indexes still exact. Graphics mods (gfx_mod.c) take the
 * same paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx_dump.h"
#include "gfx_rom.h"
#include "rt.h"

static SDL_AtomicInt running, finished, done_count, total_count, image_count;
/* Progress by the bytes of the files done (sprites with many frames take longest), for the time left. */
static SDL_AtomicInt done_bytes, total_bytes;
static char out_root[1024];
static FILE *palette_log;

/* out_root/<kind>/<name><end>, its folders made. */
static FILE *create(const char *kind, const char *name, const char *end, char *path, size_t size, const char *mode) {
    char dir[1024];
    snprintf(path, size, "%s%s/%s%s", out_root, kind, name, end);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    SDL_CreateDirectory(dir);
    return mode ? fopen(path, mode) : NULL;
}

/* w x h palette indexes as <kind>/<name>.png. */
static void save_png(const char *kind, const char *name, int w, int h, uint8_t *px, int bpp8, const GfxPalette *p,
                     const char *from) {
    char path[1024];
    create(kind, name, ".png", path, sizeof path, NULL);
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_INDEX8, px, w);
    SDL_Palette *sp = s ? SDL_CreateSurfacePalette(s) : NULL;
    if (sp) {
        SDL_Color c[256];
        for (int i = 0; i < 256; i++) c[i] = p->c[i], c[i].a = (bpp8 ? i == 0 : (i & 15) == 0) ? 0 : 255;
        SDL_SetPaletteColors(sp, c, 0, 256);
    }
    if (sp && SDL_SavePNG(s, path)) {
        SDL_AddAtomicInt(&image_count, 1);
        if (palette_log) fprintf(palette_log, "%s/%s.png\t%s\t%s\n", kind, name, from, p->from ? p->from->path : "-");
    } else {
        rt_log("gfxdump: cannot write %s: %s", path, SDL_GetError());
    }
    SDL_DestroySurface(s);
}

static void save(const char *kind, const char *name, const GfxLayout *l, const GfxTiles *t, const GfxPalette *p,
                 const char *from) {
    uint8_t *px = calloc((size_t)l->w, (size_t)l->h);
    gfx_draw(l, t, px);
    save_png(kind, name, l->w, l->h, px, l->bpp8, p, from);
    free(px);
}

/* A message font: its glyphs as a sheet, and their widths and characters in a .txt beside it. */
static void dump_font(const GfxFile *f) {
    GfxBlob b = gfx_open(f);
    GfxFont font;
    if (gfx_read_font(&b, &font)) {
        int w, h;
        gfx_font_size(&font, &w, &h);
        uint8_t *px = calloc((size_t)w, (size_t)h);
        gfx_font_draw(&font, px);
        GfxPalette p;
        gfx_font_palette(f->path, &p);
        save_png("fonts", f->path, w, h, px, 0, &p, f->path);
        free(px);
        char path[1024];
        FILE *txt = create("fonts", f->path, ".txt", path, sizeof path, "w");
        if (txt) {
            fprintf(txt, "# %s: glyph number (its place in the .png, %d to a row), advance width in pixels (- : no\n"
                         "# glyph), and the ASCII characters drawn with it\n", f->path, GFX_FONT_COLUMNS);
            for (int g = 0; g < font.count; g++) {
                const uint8_t *r = gfx_font_glyph(&font, g);
                if (r) fprintf(txt, "%d\t%d\t", g, r[0]);
                else fprintf(txt, "%d\t-\t", g);
                for (int c = 32; c < 127; c++) {
                    if ((int)gfx_le16(font.data + 12 + c * 2) == g) fprintf(txt, c == ' ' ? " space" : " %c", c);
                }
                fputc('\n', txt);
            }
            fclose(txt);
        } else {
            rt_log("gfxdump: cannot write %s", path);
        }
    }
    gfx_close(&b);
}

/* The _nsc.bin `f` drawn from its _ncg.bin. */
static void dump_screen(const GfxFile *f) {
    char path[300];
    snprintf(path, sizeof path, "%.*s_ncg.bin", (int)(strlen(f->path) - 8), f->path);
    const GfxFile *gf = gfx_find(path);
    if (!gf) return;
    GfxBlob b = gfx_open(f), gb = gfx_open(gf);
    GfxTiles t;
    GfxLayout l;
    if (gfx_read_tiles(&gb, &t) && gfx_screen_layout(&b, t.bpp8, &l)) {
        GfxPalette p;
        gfx_palette_for(f->path, t.bpp8, &p);
        save("screens", f->path, &l, &t, &p, f->path);
        free(l.tiles);
    }
    gfx_close(&gb);
    gfx_close(&b);
}

/* A tile sheet, and the cells of the .NCER of the same name. */
static void dump_graphics(const GfxFile *f) {
    GfxBlob b = gfx_open(f);
    GfxTiles t;
    GfxLayout l;
    if (gfx_read_tiles(&b, &t) && gfx_sheet_layout(&t, &l)) {
        GfxPalette p;
        gfx_palette_for(f->path, t.bpp8, &p);
        save("sheets", f->path, &l, &t, &p, f->path);
        free(l.tiles);
        char path[300];
        snprintf(path, sizeof path, "%.*s.NCER", (int)(strlen(f->path) - 5), f->path);
        const GfxFile *cf = gfx_ends_with(f->path, ".NCGR") ? gfx_find(path) : NULL;
        if (cf) {
            GfxBlob cb = gfx_open(cf);
            for (uint32_t c = 0, n = gfx_cell_count(&cb); c < n; c++) {
                if (!gfx_cell_layout(&cb, c, &l)) continue;
                char name[300];
                snprintf(name, sizeof name, "%.*s/%03u", (int)(strlen(cf->path) - 5), cf->path, (unsigned)c);
                save("frames", name, &l, &t, &p, cf->path);
                free(l.tiles);
            }
            gfx_close(&cb);
        }
    }
    gfx_close(&b);
}

static int is_unit(const GfxFile *f) {
    return gfx_ends_with(f->path, ".NCGR") || gfx_ends_with(f->path, "_ncg.bin") || gfx_ends_with(f->path, "_nsc.bin") ||
           gfx_ends_with(f->path, ".fnt");
}

/* A unit's share of the work: its file, and its cells for a sheet. */
static int unit_bytes(const GfxFile *f) {
    uint32_t n = f->size;
    if (gfx_ends_with(f->path, ".NCGR")) {
        char path[300];
        snprintf(path, sizeof path, "%.*s.NCER", (int)(strlen(f->path) - 5), f->path);
        const GfxFile *cf = gfx_find(path);
        if (cf) n += cf->size * 32; /* frames cost more than the sheet */
    }
    return (int)SDL_min(n, 1u << 24);
}

static int worker(void *arg) {
    (void)arg;
    Uint64 t0 = SDL_GetTicks();
    snprintf(out_root, sizeof out_root, "%smods/graphics/dump/", rt_data_dir());
    SDL_CreateDirectory(out_root);
    char log_path[1100];
    snprintf(log_path, sizeof log_path, "%spalettes.txt", out_root);
    palette_log = fopen(log_path, "w");
    if (palette_log) fprintf(palette_log, "# image\tROM file\tpalette (- : grey, none found)\n");
    int files = gfx_list_files(), total = 0, bytes = 0;
    for (int i = 0; i < files; i++) {
        if (is_unit(gfx_file(i))) total++, bytes += unit_bytes(gfx_file(i));
    }
    SDL_SetAtomicInt(&total_bytes, bytes);
    SDL_SetAtomicInt(&total_count, total);
    for (int i = 0; i < files; i++) {
        const GfxFile *f = gfx_file(i);
        if (!is_unit(f)) continue;
        if (gfx_ends_with(f->path, "_nsc.bin")) dump_screen(f);
        else if (gfx_ends_with(f->path, ".fnt")) dump_font(f);
        else dump_graphics(f);
        SDL_AddAtomicInt(&done_bytes, unit_bytes(f));
        SDL_AddAtomicInt(&done_count, 1);
    }
    if (palette_log) fclose(palette_log);
    palette_log = NULL;
    rt_log("gfxdump: %d images from %d files in %.1f s, in %s", SDL_GetAtomicInt(&image_count), total,
           (double)(SDL_GetTicks() - t0) / 1000, out_root);
    SDL_SetAtomicInt(&finished, 1);
    SDL_SetAtomicInt(&running, 0);
    return 0;
}

void gfxdump_start(void) {
    if (!SDL_CompareAndSwapAtomicInt(&running, 0, 1)) return;
    SDL_SetAtomicInt(&done_count, 0);
    SDL_SetAtomicInt(&total_count, 0);
    SDL_SetAtomicInt(&done_bytes, 0);
    SDL_SetAtomicInt(&total_bytes, 0);
    SDL_SetAtomicInt(&image_count, 0);
    SDL_Thread *t = SDL_CreateThread(worker, "gfxdump", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&running, 0);
}

int gfxdump_running(void) { return SDL_GetAtomicInt(&running); }

void gfxdump_progress(int *done, int *total, float *share) {
    *done = SDL_GetAtomicInt(&done_count);
    *total = SDL_GetAtomicInt(&total_count);
    int bytes = SDL_GetAtomicInt(&total_bytes);
    *share = bytes ? (float)SDL_GetAtomicInt(&done_bytes) / (float)bytes : 0;
}

int gfxdump_finished(int *images) {
    if (!SDL_CompareAndSwapAtomicInt(&finished, 1, 0)) return 0;
    *images = SDL_GetAtomicInt(&image_count);
    return 1;
}
