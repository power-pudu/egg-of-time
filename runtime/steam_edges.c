/* Room edges from the game's Steam release (2018), read from the player's own copy: nothing of it is stored or shipped.
 *
 * The Steam release draws its rooms wider than the DS screen. For each location (the game's own ids, the DS rooms'
 * included) its archive keeps the tiles past the room's sides, Game/field/mapext_bin/mapext_<location>.bin: a byte
 * of width and one of height (16-px cells), then four planes of width x height little-endian u16 cells. The cells
 * span the map's columns -x..right+x (right: the room's last column, x = (width - right - 1) / 2) and its rows from 0.
 * A cell is 0 (nothing) or v: page (v - 1) >> 8, item (v - 1) & 255. Pages 0 and 1 are the room's metatiles 0-511, as
 * the DS game defines them (Game/field/ChipTable/ChipTable_<assembly>.dat: 2x2 tiles of 3 bytes, the layout the DS
 * keeps in RAM), with tiles from the chip sets of the room's tileset (BGSetTable/bgsettable_<tileset>.dat: the set in
 * each of 6 slots of 128 tiles; map_bin/cg<set>.bin: a 4-byte header, then a 4-bit sheet 128 px wide). Pages 2 and up
 * are pictures the Steam release added, 256 x 256 sheets of 16 x 16 cells (map_bin/cgext<location>.bin, or
 * cgext<location>_<k>.bin as page 2 + k; one colour index a pixel after a u32 width and height). Mapinfo/
 * mapinfo_<location>.dat names the tileset (u16 1) and the assembly (u16 2). Planes 3 and 2 are the layers 1 and 0
 * below the sprites, planes 1 and 0 the tiles over them, drawn 3, 2, 1, 0.
 *
 * Here they go to the BGs the field gives those layers: layer 0 BG2, layer 1 BG3, the priority tiles BG1 (as
 * widescreen.c fills the margins). A metatile whose definition is the DS game's (in every room tried) is drawn from
 * the room's own tiles in VRAM, so it animates, takes the HD art and fades with the room; one the Steam release
 * redefined, or whose tiles the DS game does not load, and the added pictures are drawn from the Steam release's
 * pixels, in the room's palette.
 *
 * The archive (resources.bin, "ARC1"): every block is XORed with a keystream of the LCG n = n * 0x41C64E6D + 12345
 * (the byte n >> 24), n starting at 0x19000000 + the block's offset in the file; a block is a big-endian u32 length,
 * then a gzip member. The header's first 16 bytes are such a block's: "ARC1", u32, the file list's offset, its size.
 * The file list: u32 count, then count entries of 12 bytes (name's offset in the list, data offset, data size).
 *
 * Graphics mods reach the Steam release's own pixels through its sheets (se_sheet: the dump writes them, gfx_mod.c
 * reads mods/graphics/steam/<archive path>.png): one the same size replaces the sheet's pixels (se_sheet_set), a
 * larger one is HD art keyed on the sheet's tiles. Each pixel of an added picture keeps where it came from (a sheet's
 * tile or the room's, se_picture_hd), so the renderer finds that tile's HD art. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "gfx_mod.h"
#include "inflate.h"
#include "rt.h"
#include "steam_edges.h"
#include "video.h"

typedef struct Entry { const char *name; uint32_t off, size; } Entry;

static char archive[1100]; /* resources.bin's path; empty: no Steam copy */
static Entry *entries;
static int entry_count;
static SDL_AtomicInt found;

static void unmask(uint8_t *p, size_t n, uint32_t seed) {
    uint32_t v = 0x19000000u + seed;
    for (size_t i = 0; i < n; i++) {
        v = v * 0x41C64E6Du + 12345u;
        p[i] ^= (uint8_t)(v >> 24);
    }
}

static uint8_t *read_at(const char *path, uint32_t off, uint32_t size) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;
    uint8_t *p = malloc(size ? size : 1);
    if (p && (SDL_SeekIO(io, off, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, p, size) != size)) free(p), p = NULL;
    SDL_CloseIO(io);
    return p;
}

/* A block of the archive, unpacked; NULL when it is damaged. */
static uint8_t *unpack(const char *path, uint32_t off, uint32_t size, size_t *len) {
    uint8_t *raw = size >= 4 ? read_at(path, off, size) : NULL;
    if (!raw) return NULL;
    unmask(raw, size, off);
    uint32_t n = (uint32_t)raw[0] << 24 | (uint32_t)raw[1] << 16 | (uint32_t)raw[2] << 8 | raw[3];
    uint8_t *out = malloc(n ? n : 1);
    long got = out ? rt_gunzip(raw + 4, size - 4, out, n) : -1;
    free(raw);
    if (got != (long)n) {
        free(out);
        return NULL;
    }
    *len = n;
    return out;
}

static int by_name(const void *a, const void *b) { return strcmp(((const Entry *)a)->name, ((const Entry *)b)->name); }

/* A file of the archive, unpacked (NULL: none); *len its size. */
static uint8_t *steam_file(const char *name, size_t *len) {
    Entry key = {name, 0, 0};
    const Entry *e = entries ? bsearch(&key, entries, (size_t)entry_count, sizeof *entries, by_name) : NULL;
    return e ? unpack(archive, e->off, e->size, len) : NULL;
}

/* ---- the sheets ---- */

/* A sheet's pixels set by a graphics mod, by its name. */
typedef struct Override { char name[96]; int w, h; uint8_t *px; } Override;
static Override *overrides;
static int override_count;

/* The sheets: map_bin/cg<set>.bin, a 4-byte header and 4 bits a pixel 128 px wide (the high nibble first); cg_bg.bin
 * and cgext*.bin, a u32 width and height and a byte a pixel. */
static int sheet_bpp8(const char *name) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return !strncmp(base, "cg_bg", 5) || !strncmp(base, "cgext", 5);
}

/* The archive's sheets, by entry: listed once connected (list_sheets). */
static int *sheets, sheet_count;
static void list_sheets(void) {
    free(sheets);
    sheets = malloc((size_t)(entry_count ? entry_count : 1) * sizeof *sheets);
    sheet_count = 0;
    for (int i = 0; sheets && i < entry_count; i++) {
        const char *n = entries[i].name;
        size_t len = strlen(n);
        if (!strncmp(n, "Game/field/map_bin/cg", 21) && len > 4 && !strcmp(n + len - 4, ".bin")) sheets[sheet_count++] = i;
    }
}

int se_sheet_count(void) { return se_available() ? sheet_count : 0; }
const char *se_sheet_name(int i) { return i >= 0 && i < sheet_count ? entries[sheets[i]].name : NULL; }

uint8_t *se_sheet(const char *name, int *w, int *h, int *bpp8) {
    *bpp8 = sheet_bpp8(name);
    for (int i = 0; i < override_count; i++) {
        if (strcmp(overrides[i].name, name)) continue;
        uint8_t *px = malloc((size_t)overrides[i].w * overrides[i].h);
        if (px) memcpy(px, overrides[i].px, (size_t)overrides[i].w * overrides[i].h);
        *w = overrides[i].w, *h = overrides[i].h;
        return px;
    }
    size_t n = 0;
    uint8_t *d = steam_file(name, &n), *px = NULL;
    *w = *h = 0;
    if (d && *bpp8 && n >= 8) {
        uint32_t sw = d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
        uint32_t sh = d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
        if (sw && sh && sw <= 4096 && sh <= 4096 && 8 + (size_t)sw * sh <= n && (px = malloc((size_t)sw * sh))) {
            memcpy(px, d + 8, (size_t)sw * sh);
            *w = (int)sw, *h = (int)sh;
        }
    } else if (d && n > 4) {
        int sh = (int)((n - 4) * 2 / 128) / 8 * 8;
        if (sh && (px = malloc((size_t)128 * sh))) {
            for (size_t i = 0; i < (size_t)128 * sh; i++) px[i] = i & 1 ? d[4 + i / 2] & 15 : d[4 + i / 2] >> 4;
            *w = 128, *h = sh;
        }
    }
    free(d);
    return px;
}

void se_sheet_set(const char *name, int w, int h, const uint8_t *px) {
    Override *o = NULL;
    for (int i = 0; i < override_count && !o; i++) if (!strcmp(overrides[i].name, name)) o = &overrides[i];
    if (!o) {
        Override *more = realloc(overrides, (size_t)(override_count + 1) * sizeof *overrides);
        if (!more) return;
        overrides = more, o = &overrides[override_count++];
        snprintf(o->name, sizeof o->name, "%s", name);
        o->px = NULL;
    }
    free(o->px);
    o->w = w, o->h = h;
    if ((o->px = malloc((size_t)w * h))) memcpy(o->px, px, (size_t)w * h);
}

/* The tile at (x, y) of a sheet as the DS keeps tiles (4 bits a pixel, the low nibble first; else a byte a pixel). */
static void sheet_tile(const uint8_t *px, int w, int x, int y, int bpp8, uint8_t *t) {
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            uint8_t c = px[(y + v) * w + x + u];
            if (bpp8) t[v * 8 + u] = c;
            else if (u & 1) t[v * 4 + u / 2] |= (uint8_t)(c << 4);
            else t[v * 4 + u / 2] = c;
        }
}

uint8_t *se_sheet_tile_bytes(const uint8_t *px, int w, int x, int y, int bpp8, uint8_t *t) {
    sheet_tile(px, w, x, y, bpp8, t);
    return t;
}

/* path is the Steam release's resources.bin: reads its file list. */
static int open_archive(const char *path) {
    uint8_t *h = read_at(path, 0, 16);
    if (!h) return 0;
    unmask(h, 16, 0);
    int ok = !memcmp(h, "ARC1", 4);
    uint32_t list_off = h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
    uint32_t list_size = h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
    free(h);
    size_t n = 0;
    uint8_t *list = ok ? unpack(path, list_off, list_size, &n) : NULL;
    if (!list || n < 4) {
        free(list);
        return 0;
    }
    uint32_t count = list[0] | (uint32_t)list[1] << 8 | (uint32_t)list[2] << 16 | (uint32_t)list[3] << 24;
    if (count > (n - 4) / 12) count = 0;
    entries = calloc(count ? count : 1, sizeof *entries);
    /* list stays: the entries' names point into it */
    int k = 0;
    for (uint32_t i = 0; i < count && entries; i++) {
        const uint8_t *p = list + 4 + 12 * i;
        uint32_t at = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        if (at >= n || !memchr(list + at, 0, n - at)) continue;
        entries[k].name = (const char *)list + at;
        entries[k].off = p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        entries[k].size = p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
        k++;
    }
    entry_count = k;
    if (!k) return 0;
    qsort(entries, (size_t)k, sizeof *entries, by_name);
    snprintf(archive, sizeof archive, "%s", path);
    return 1;
}

/* dir is a Steam copy of the game, or its resources.bin. */
static int try_copy(const char *dir) {
    if (!dir || !*dir) return 0;
    char path[1100];
    size_t n = strlen(dir);
    if (n >= 13 && !SDL_strcasecmp(dir + n - 13, "resources.bin")) snprintf(path, sizeof path, "%s", dir);
    else snprintf(path, sizeof path, "%s%sresources.bin", dir, dir[n - 1] == '/' || dir[n - 1] == '\\' ? "" : "/");
    return open_archive(path);
}

/* The game in Steam's library at lib (steamapps/common/Chrono Trigger). */
static int try_library(const char *lib) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/steamapps/common/Chrono Trigger", lib);
    return try_copy(dir);
}

/* A Steam installation at root: its own library, then the others steamapps/libraryfolders.vdf lists ("path" "..."). */
static int try_steam(const char *root) {
    if (!root || !*root) return 0;
    if (try_library(root)) return 1;
    char vdf[1100];
    snprintf(vdf, sizeof vdf, "%s/steamapps/libraryfolders.vdf", root);
    size_t n = 0;
    char *text = SDL_LoadFile(vdf, &n);
    int ok = 0;
    for (char *p = text; p && !ok && (p = strstr(p, "\"path\"")) != NULL;) {
        p += 6;
        char *a = strchr(p, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        if (!b) break;
        char lib[1024];
        size_t k = 0;
        for (char *c = a + 1; c < b && k + 1 < sizeof lib; c++) {
            if (c[0] == '\\' && c[1] == '\\') c++; /* the file escapes its backslashes */
            lib[k++] = *c;
        }
        lib[k] = 0;
        ok = try_library(lib);
        p = b + 1;
    }
    SDL_free(text);
    return ok;
}

#ifdef _WIN32
static int try_registry(HKEY hive, const char *key, const char *value) {
    char root[1024];
    DWORD size = sizeof root;
    if (RegGetValueA(hive, key, value, RRF_RT_REG_SZ, NULL, root, &size) != ERROR_SUCCESS) return 0;
    return try_steam(root);
}
#endif

/* The translations the copy holds: the codes of its Localize/<code>/msg/ folders, in the archive's order. */
static char languages[16][16];
static int language_count;
static void list_languages(void) {
    language_count = 0;
    for (int i = 0; i < entry_count && language_count < (int)SDL_arraysize(languages); i++) {
        const char *n = entries[i].name, *slash;
        if (strncmp(n, "Localize/", 9) || !(slash = strchr(n + 9, '/')) || strncmp(slash, "/msg/", 5)) continue;
        size_t len = (size_t)(slash - n - 9);
        if (!len || len >= sizeof languages[0]) continue;
        if (language_count && !strncmp(languages[language_count - 1], n + 9, len) && !languages[language_count - 1][len]) continue;
        memcpy(languages[language_count], n + 9, len);
        languages[language_count++][len] = 0;
    }
}

void se_init(const char *dir) {
    const char *env = getenv("EGG_STEAM_DIR");
    int ok = 0;
    if (dir && *dir) ok = try_copy(dir);
    if (!ok && env) {
        ok = try_copy(env);
    } else if (!ok) {
#ifdef _WIN32
        ok = try_registry(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath") ||
             try_registry(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath") ||
             try_registry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath") ||
             try_steam("C:/Program Files (x86)/Steam") || try_steam("C:/Program Files/Steam");
#else
        static const char *const roots[] = {".steam/steam", ".local/share/Steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                                            "Library/Application Support/Steam"};
        const char *home = getenv("HOME");
        for (size_t i = 0; home && !ok && i < SDL_arraysize(roots); i++) {
            char root[1100];
            snprintf(root, sizeof root, "%s/%s", home, roots[i]);
            ok = try_steam(root);
        }
#endif
    }
    if (ok) rt_log("Steam version: %s", archive);
    if (ok) list_languages(), list_sheets();
    SDL_SetAtomicInt(&found, ok);
}

int se_available(void) { return SDL_GetAtomicInt(&found); }

int se_connect(const char *dir) {
    if (se_available()) return 1;
    if (!try_copy(dir)) return 0;
    rt_log("Steam version: %s", archive);
    list_languages(), list_sheets();
    SDL_SetAtomicInt(&found, 1); /* after the file list: the game thread reads it once this is set */
    return 1;
}

const char *se_dir(void) {
    static char dir[sizeof archive];
    if (!se_available()) return NULL;
    snprintf(dir, sizeof dir, "%s", archive);
    char *a = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > a) a = b;
    if (a) *a = 0;
    return dir;
}

int se_language_count(void) { return se_available() ? language_count : 0; }
const char *se_language(int i) { return languages[i]; }

/* ---- a room's edges ---- */

/* A room's edges are drawn from its first frame from the Steam release's pixels alone, which need nothing of the DS
 * game's: drawn only once the room had held, the sides were black through the fade into it. Once the room has held
 * this long (its tiles in VRAM), they are built again with the room's own tiles where they are the same (animated,
 * with HD art): the same pixels. */
#define SETTLE_FRAMES 8
#define MARGIN_MAX 8 /* the Steam release's edges: 4 to 6 columns a side */
enum { SLOTS = 6, SET_SIZE = 128 * 64 }; /* a chip set: 128 tiles of 8x8, one colour index a pixel */

static struct {
    int location, left, right;
    uint32_t metatiles, chr;
    int held;            /* frames the room has held */
    int built, ready;    /* built: 1 from the Steam pixels, 2 with the room's tiles too; ready: it has edges */
    int w, h, xoff;      /* cells, and the columns left of the map's column 0 */
    int x0, x1;          /* the world columns from the first cell beside the room to the last (pixels) */
    int32_t *grid[4];    /* by BG: (2w x 2h) 8x8 tiles, se_at's codes */
    uint8_t *pictures;   /* 64 bytes each */
    SeSource *sources;   /* with HD art loaded: 64 each, where each picture pixel came from */
    int pictures_n, pictures_cap;
} room;

static uint8_t *chips[SLOTS];     /* the tileset's chip sets, loaded when a tile needs them */
static uint32_t *chips_hd[SLOTS]; /* with HD art: each chip's HD tile (gfxmod_find) */
static int chips_loaded;
static uint8_t *pages[16];        /* page 1..15: the added pictures (256 x 256), loaded when a tile needs them */
static uint32_t *pages_hd[16];    /* with HD art: each 8x8 tile's, 32 a row */
static int pages_loaded[16];
static int tileset, room_location_id;

static void forget_edges(void) {
    for (int b = 0; b < 4; b++) free(room.grid[b]), room.grid[b] = NULL;
    free(room.pictures), room.pictures = NULL, room.pictures_n = room.pictures_cap = 0;
    free(room.sources), room.sources = NULL;
    room.ready = 0;
}

static void forget_room(void) {
    forget_edges();
    for (int i = 0; i < SLOTS; i++) free(chips[i]), chips[i] = NULL, free(chips_hd[i]), chips_hd[i] = NULL;
    for (int i = 0; i < 16; i++) free(pages[i]), pages[i] = NULL, free(pages_hd[i]), pages_hd[i] = NULL, pages_loaded[i] = 0;
    chips_loaded = 0;
    room.ready = 0;
}

static void load_chips(void) {
    chips_loaded = 1;
    char name[96];
    size_t n = 0;
    snprintf(name, sizeof name, "Game/field/BGSetTable/bgsettable_%d.dat", tileset);
    uint8_t *table = steam_file(name, &n);
    for (int s = 0; s < SLOTS && table && (size_t)s < n; s++) {
        if (table[s] == 0xFF) continue;
        size_t len = 0;
        snprintf(name, sizeof name, "Game/field/map_bin/cg%d.bin", table[s]);
        int w, h, bpp8;
        uint8_t *sheet = se_sheet(name, &w, &h, &bpp8);
        if (!sheet) continue;
        uint8_t *set = calloc(1, SET_SIZE);
        uint32_t *hd = gfxmod_hd_scale() ? calloc(128, sizeof *hd) : NULL;
        /* tile t at (t % 16, t / 16) * 8 */
        for (int t = 0; set && t < 128 && t / 16 * 8 + 8 <= h; t++) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) set[t * 64 + y * 8 + x] = sheet[(t / 16 * 8 + y) * w + t % 16 * 8 + x];
            uint8_t bytes[32];
            if (hd) hd[t] = gfxmod_find(se_sheet_tile_bytes(sheet, w, t % 16 * 8, t / 16 * 8, 0, bytes), 0);
        }
        (void)len;
        free(sheet);
        chips[s] = set, chips_hd[s] = hd;
    }
    free(table);
}

/* An added picture page's file, as the Steam release loads them: its Lavos rooms share one, two rooms take another
 * picture as page 1, some rooms have several pages. */
static uint8_t *load_page(int page) {
    if (page < 1 || page >= 16) return NULL;
    if (pages_loaded[page]) return pages[page];
    pages_loaded[page] = 1;
    int loc = room_location_id;
    char name[96];
    if (page == 1) {
        if (loc != 0x1C && loc != 0x15B) return NULL;
        snprintf(name, sizeof name, "Game/field/map_bin/cg_bg.bin");
    } else if (page == 2 && ((loc >= 0x15A && loc <= 0x15E) || (loc >= 0x1C8 && loc <= 0x1CB))) {
        snprintf(name, sizeof name, "Game/field/map_bin/cgext_lavos.bin");
    } else {
        snprintf(name, sizeof name, "Game/field/map_bin/cgext%d_%d.bin", loc, page - 2);
    }
    int w, h, bpp8;
    uint8_t *d = se_sheet(name, &w, &h, &bpp8);
    if (!d && page == 2) {
        snprintf(name, sizeof name, "Game/field/map_bin/cgext%d.bin", loc);
        d = se_sheet(name, &w, &h, &bpp8);
    }
    if (d && w == 256 && h == 256) {
        pages[page] = d, d = NULL;
        if (gfxmod_hd_scale() && (pages_hd[page] = calloc(1024, sizeof *pages_hd[page]))) {
            uint8_t bytes[64];
            for (int t = 0; t < 1024; t++)
                pages_hd[page][t] = gfxmod_find(se_sheet_tile_bytes(pages[page], 256, t % 32 * 8, t / 32 * 8, 1, bytes), 1);
        }
    }
    free(d);
    return pages[page];
}

/* Each pixel of an 8x8 picture from one tile: its HD tile, texel (y * 8 + x within it) and flips, palette row. */
static void same_source(SeSource *src, uint32_t tile, int hflip, int vflip, int pal) {
    for (int i = 0; src && i < 64; i++) {
        int x = i % 8, y = i / 8;
        src[i] = (SeSource){tile, (uint8_t)(hflip ? 7 - x : x), (uint8_t)(vflip ? 7 - y : y), (uint8_t)((hflip ? 1 : 0) | (vflip ? 2 : 0)), (uint8_t)pal};
    }
}

/* A DS map entry (tile, flips, palette) as an 8x8 picture, from the room's tiles in VRAM (4 bits a pixel); src (with
 * HD art) where its pixels come from. */
static void entry_pixels(uint32_t entry, uint8_t px[64], SeSource *src) {
    uint32_t tile = entry & 0x3FF, pal = (entry >> 12) & 15;
    if (src) {
        uint8_t bytes[32];
        for (uint32_t i = 0; i < 32; i++) {
            const uint8_t *p = video_vram_ptr(room.chr + tile * 32 + i);
            bytes[i] = p ? *p : 0;
        }
        same_source(src, gfxmod_find(bytes, 0), entry & 0x400, entry & 0x800, (int)pal);
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int sx = entry & 0x400 ? 7 - x : x, sy = entry & 0x800 ? 7 - y : y;
            const uint8_t *p = video_vram_ptr(room.chr + tile * 32 + (uint32_t)(sy * 4 + sx / 2));
            uint8_t c = p ? (sx & 1 ? *p >> 4 : *p & 15) : 0;
            px[y * 8 + x] = c ? (uint8_t)(pal * 16 + c) : 0;
        }
}

/* The room's tile in VRAM has no pixel (the DS game does not load it). */
static int entry_blank(uint32_t entry) {
    for (uint32_t i = 0; i < 32; i++) {
        const uint8_t *p = video_vram_ptr(room.chr + (entry & 0x3FF) * 32 + i);
        if (!p || *p) return 0;
    }
    return 1;
}

/* A metatile corner of the Steam release's (3 bytes) as an 8x8 picture, from its chip sets. */
static void corner_pixels(const uint8_t *d, uint8_t px[64], SeSource *src) {
    if (!chips_loaded) load_chips();
    int chip = d[0] | (d[1] & 3) << 8, pal = d[1] >> 4;
    const uint8_t *set = chip >> 7 < SLOTS ? chips[chip >> 7] : NULL;
    const uint32_t *hd = chip >> 7 < SLOTS ? chips_hd[chip >> 7] : NULL;
    same_source(src, hd ? hd[chip & 127] : 0, d[1] & 4, d[1] & 8, pal);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int sx = d[1] & 4 ? 7 - x : x, sy = d[1] & 8 ? 7 - y : y;
            uint8_t c = set ? set[(chip & 127) * 64 + sy * 8 + sx] : 0;
            px[y * 8 + x] = c ? (uint8_t)(pal * 16 + c) : 0;
        }
}

static int32_t new_picture(const uint8_t px[64], const SeSource *src) {
    if (room.pictures_n == room.pictures_cap) {
        int cap = room.pictures_cap ? room.pictures_cap * 2 : 256;
        uint8_t *p = realloc(room.pictures, (size_t)cap * 64);
        if (!p) return 0;
        room.pictures = p;
        if (src) {
            SeSource *ps = realloc(room.sources, (size_t)cap * 64 * sizeof *ps);
            if (!ps) return 0;
            if (!room.sources) memset(ps, 0, (size_t)cap * 64 * sizeof *ps);
            room.sources = ps;
        }
        room.pictures_cap = cap;
    }
    memcpy(room.pictures + (size_t)room.pictures_n * 64, px, 64);
    if (src && room.sources) memcpy(room.sources + (size_t)room.pictures_n * 64, src, 64 * sizeof *src);
    return SE_PICTURE | room.pictures_n++;
}

const uint8_t *se_picture(int32_t code) { return room.pictures + (size_t)(code & 0xFFFF) * 64; }
const SeSource *se_picture_hd(int32_t code) { return room.sources ? room.sources + (size_t)(code & 0xFFFF) * 64 : NULL; }

/* Lays an 8x8 tile on BG bg at tile (tx, ty) of the grid: a DS map entry (px NULL) or a picture (src: where its
 * pixels come from, with HD art), over what is there. */
static void lay(int bg, int tx, int ty, uint32_t entry, const uint8_t *px, const SeSource *src) {
    int32_t *at = &room.grid[bg][ty * 2 * room.w + tx];
    if (!*at) {
        *at = px ? new_picture(px, src) : (int32_t)(SE_ENTRY | entry);
        return;
    }
    int hd = gfxmod_hd_scale() != 0;
    uint8_t under[64], over[64];
    SeSource under_src[64], over_src[64];
    if (*at & SE_PICTURE) {
        memcpy(under, se_picture(*at), 64);
        if (se_picture_hd(*at)) memcpy(under_src, se_picture_hd(*at), sizeof under_src);
        else memset(under_src, 0, sizeof under_src);
    } else {
        entry_pixels((uint32_t)*at & 0xFFFF, under, hd ? under_src : NULL);
    }
    if (px) {
        memcpy(over, px, 64);
        if (src) memcpy(over_src, src, sizeof over_src);
        else memset(over_src, 0, sizeof over_src);
    } else {
        entry_pixels(entry, over, hd ? over_src : NULL);
    }
    for (int i = 0; i < 64; i++)
        if (over[i]) under[i] = over[i], under_src[i] = over_src[i];
    if (*at & SE_PICTURE) {
        memcpy(room.pictures + (size_t)(*at & 0xFFFF) * 64, under, 64);
        if (room.sources) memcpy(room.sources + (size_t)(*at & 0xFFFF) * 64, under_src, sizeof under_src);
    } else {
        *at = new_picture(under, hd ? under_src : NULL);
    }
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* own_tiles: the room's tiles where its metatiles are the Steam release's (else its pixels everywhere). */
static void build_room(int own_tiles) {
    forget_edges();
    room.built = own_tiles ? 2 : 1;
    room_location_id = room.location;
    char name[96];
    size_t info_n = 0, ext_n = 0, table_n = 0;
    snprintf(name, sizeof name, "Game/field/Mapinfo/mapinfo_%d.dat", room.location);
    uint8_t *info = steam_file(name, &info_n);
    snprintf(name, sizeof name, "Game/field/mapext_bin/mapext_%d.bin", room.location);
    uint8_t *ext = steam_file(name, &ext_n);
    int w = ext && ext_n >= 2 ? ext[0] : 0, h = ext && ext_n >= 2 ? ext[1] : 0;
    int beyond = w - (room.right + 1);
    if (!info || info_n < 6 || !w || !h || ext_n < 2 + (size_t)8 * w * h || beyond < 0 || beyond & 1 || beyond > 2 * MARGIN_MAX) {
        free(info), free(ext);
        return;
    }
    tileset = le16(info + 2);
    snprintf(name, sizeof name, "Game/field/ChipTable/ChipTable_%04d.dat", le16(info + 4));
    free(info);
    uint8_t *table = steam_file(name, &table_n);
    room.w = w, room.h = h, room.xoff = beyond / 2;
    for (int b = 1; b < 4; b++) room.grid[b] = calloc((size_t)4 * w * h, sizeof *room.grid[b]);
    int first = w, last = -1;
    static const int8_t below[4] = {1, 1, 2, 3}; /* by plane: its BG (the priority tiles all go to BG1) */
    for (int plane = 3; plane >= 0 && room.grid[1] && room.grid[2] && room.grid[3]; plane--) {
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                int v = le16(ext + 2 + 2 * ((size_t)plane * w * h + (size_t)r * w + c));
                int mc = c - room.xoff;
                if (!v || (mc >= room.left && mc <= room.right)) continue; /* the room draws itself */
                first = c < first ? c : first, last = c > last ? c : last;
                int page = (v - 1) >> 8, item = (v - 1) & 255;
                const uint8_t *sheet = page >= 2 || (page == 1 && (room.location == 0x1C || room.location == 0x15B)) ? load_page(page) : NULL;
                for (int k = 0; k < 4; k++) {
                    int tx = 2 * c + (k & 1), ty = 2 * r + (k >> 1);
                    uint8_t px[64];
                    SeSource src[64], *hd_src = gfxmod_hd_scale() ? src : NULL;
                    if (sheet || page >= 2) {
                        int sx = item % 16 * 16 + (k & 1) * 8, sy = item / 16 * 16 + (k >> 1) * 8;
                        for (int y = 0; y < 8; y++)
                            for (int x = 0; x < 8; x++) px[y * 8 + x] = sheet ? sheet[(sy + y) * 256 + sx + x] : 0;
                        same_source(hd_src, sheet && pages_hd[page] ? pages_hd[page][sy / 8 * 32 + sx / 8] : 0, 0, 0, 0);
                        lay(below[plane], tx, ty, 0, px, hd_src);
                        continue;
                    }
                    size_t at = (size_t)(v - 1) * 12 + (size_t)k * 3;
                    if (!table || at + 3 > table_n) continue;
                    const uint8_t *d = table + at;
                    int bg = plane >= 2 && (d[2] & 1) ? 1 : below[plane];
                    uint32_t entry = (uint32_t)(d[0] | (d[1] & 3) << 8 | ((d[1] >> 2) & 1) << 10 | ((d[1] >> 3) & 1) << 11 | (d[1] >> 4) << 12);
                    uint8_t own[3] = {0}; /* the DS game's definition of that corner */
                    for (int i = 0; i < 3 && own_tiles; i++) own[i] = (uint8_t)rt_r8(room.metatiles + (uint32_t)at + (uint32_t)i);
                    if (own_tiles && !memcmp(own, d, 3) && !entry_blank(entry)) {
                        lay(bg, tx, ty, entry, NULL, NULL);
                    } else {
                        corner_pixels(d, px, hd_src);
                        lay(bg, tx, ty, 0, px, hd_src);
                    }
                }
            }
    }
    free(ext), free(table);
    if (last < 0 || !room.grid[1] || !room.grid[2] || !room.grid[3]) return;
    room.x0 = (first - room.xoff) * 16, room.x1 = (last - room.xoff + 1) * 16;
    room.ready = 1;
    if (getenv("EGG_STEAM_LOG"))
        rt_log("Steam room edges: location 0x%x, %d x %d cells, columns %d..%d, %d pictures%s", room.location, w, h, room.x0, room.x1,
               room.pictures_n, own_tiles ? ", the room's tiles" : "");
}

/* The location's edges fit a room whose last column is right: their width is the room's and up to MARGIN_MAX columns a
 * side, or it has none (a room of its own all the same). Remembered for the last location asked. */
static int fits(int location, int right) {
    static int asked = -1, asked_right, answer;
    if (location == asked && right == asked_right) return answer;
    char name[96];
    size_t n = 0;
    snprintf(name, sizeof name, "Game/field/mapext_bin/mapext_%d.bin", location);
    uint8_t *ext = steam_file(name, &n);
    int beyond = ext && n >= 2 ? ext[0] - (right + 1) : 0;
    free(ext);
    asked = location, asked_right = right, answer = !ext || (beyond >= 0 && !(beyond & 1) && beyond <= 2 * MARGIN_MAX);
    return answer;
}

int se_room(int location, int left, int right, uint32_t metatiles, uint32_t chr) {
    if (!se_available()) return 0;
    /* Leaving a room, the game sets the next scene's location some frames before its room (the Telepod's room, 0x8,
     * read the fair's 0x1b7 for 15 frames): a location whose edges do not fit the room on screen is not its own, and
     * the room keeps its edges. Taken for the room, the fair's 48-column map was laid against the Telepod's 16
     * columns, its strip far beyond the screen, and the sides went black until the fade. */
    if (location != room.location && left == room.left && right == room.right && metatiles == room.metatiles && chr == room.chr &&
        !fits(location, right))
        location = room.location;
    if (location != room.location || left != room.left || right != room.right || metatiles != room.metatiles || chr != room.chr) {
        forget_room();
        room.location = location, room.left = left, room.right = right, room.metatiles = metatiles, room.chr = chr;
        room.held = 0, room.built = 0;
    }
    if (room.held < SETTLE_FRAMES) room.held++;
    int want = room.held < SETTLE_FRAMES ? 1 : 2;
    if (room.built < want) {
        uint64_t t = SDL_GetTicksNS();
        build_room(want == 2);
        if (getenv("EGG_STEAM_LOG")) rt_log("Steam room edges: built in %.1f ms", (double)(SDL_GetTicksNS() - t) / 1e6);
    }
    return room.ready;
}

void se_span(int *x0, int *x1) { *x0 = room.x0, *x1 = room.x1; }

int32_t se_at(int bg, int x, int y) {
    if (!room.ready || bg < 1 || bg > 3) return SE_OUT;
    int mc = x >> 4, r = y >> 4;
    if ((mc >= room.left && mc <= room.right) || mc + room.xoff < 0 || mc + room.xoff >= room.w || r < 0 || r >= room.h) return SE_OUT;
    return room.grid[bg][(y >> 3) * 2 * room.w + (x >> 3) + 2 * room.xoff];
}
