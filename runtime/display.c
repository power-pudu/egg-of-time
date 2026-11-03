/* Presentation of the two 256x192 screens: window, layout, internal resolution, filter, aspect ratio, touch
 * mapping, and the display settings (System Settings menu, eggoftime.ini next to the executable).
 *
 * Layouts: top screen only (the default in single screen mode), stacked (the default otherwise), side by
 * side; "swap" exchanges the screens (with top-only it shows the bottom screen alone). Top-only shows the main
 * screen of single screen mode (single_screen.c), which lays the boot and title screens out on one screen;
 * the other layouts show both DS screens as they are.
 *
 * Aspect ratio is widescreen, not stretching: 5:3, 16:9, 21:9 or the window's own shape ("Fit screen") ask the
 * renderer (video_set_wide_margin) to draw the top screen wider, showing more of the world where single
 * screen mode allows it (the field); the main screen then arrives wider than 256 columns. Other scenes, and
 * Original, stay 4:3. Pixels keep their shape everywhere.
 *
 * Each window frame draws the shown screens into an internal target of N times the layout's native size
 * (Auto: the next integer multiple of the size on screen, up to 11x = 2112 rows, the largest within 4K),
 * then fits that target into the window. Filters: Pixel (nearest throughout, integer scaling), Sharp (nearest
 * to the internal size, then a smooth fit: the default), Smooth (Scale2x applied twice for 4x edge-smoothed
 * art, then smooth scaling).
 *
 * Shader (shader.c): a RetroArch shader from mods/shaders instead of the filter and internal resolution. Each shown
 * screen is drawn at its native size and runs through the shader into its place in the window, so the shader
 * sees one DS screen (as RetroArch shaders expect), not the gap between two. With shaders in mods/shaders, the
 * window uses SDL's OpenGL renderer (the one shaders can run on).
 *
 * Window modes: windowed, borderless windowed (fullscreen on the desktop's mode), fullscreen (exclusive, at
 * the desktop's mode); F11 toggles between windowed and the last other mode. EGG_WINDOWED=1 starts windowed
 * without changing the setting (automated runs). Framerate only paces the window's presentation (main.c):
 * the game keeps its own 60 Hz clock, so above 60 frames are shown again and below 60 some are skipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "display.h"
#include "gfx_mod.h"
#include "movie.h"
#include "rt.h"
#include "shader.h"
#include "single_screen.h"
#include "video.h"

#define WINDOW_SCALE 3
#define SMOOTH_SCALE 4
#define MESSAGE_MS 2000
#define RES_MAX 11
#define SCALE_MAX 8 /* HD art, at most (gfx_mod.c) */

enum { LAYOUT_TOP, LAYOUT_STACKED, LAYOUT_SIDE, LAYOUT_COUNT };
enum { WINDOW_WINDOWED, WINDOW_BORDERLESS, WINDOW_FULLSCREEN };
enum { FILTER_PIXEL, FILTER_SHARP, FILTER_SMOOTH, FILTER_LCD };
enum { ASPECT_ORIGINAL, ASPECT_5_3, ASPECT_16_9 };
static const char *const layout_names[LAYOUT_COUNT] = {"top", "stacked", "side"};
static const char *const layout_labels[LAYOUT_COUNT] = {"Top screen only", "Screens stacked", "Screens side by side"};

/* ---- settings ---- */

typedef struct Setting {
    const char *key, *title;
    int count, value;
    const char *const *names;  /* in eggoftime.ini */
    const char *const *labels; /* in the menu (NULL: computed) */
} Setting;

static const char *const window_names[] = {"windowed", "borderless", "fullscreen"};
static const char *const window_labels[] = {"Windowed", "Borderless windowed", "Fullscreen"};
static const char *const res_names[RES_MAX + 1] = {"auto", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
static const char *const aspect_names[] = {"original", "5x3", "16x9", "ultrawide", "fit"};
static const char *const aspect_labels[] = {"Original (4:3)", "5:3", "16:9", "Ultrawide (21:9)", "Fit screen"};
static const float aspect_ratios[] = {4.0f / 3, 5.0f / 3, 16.0f / 9, 21.0f / 9, 0}; /* 0: the window's */
static const char *const fps_names[] = {"30", "60", "90", "120", "144", "unlimited"};
static const char *const fps_labels[] = {"30", "60", "90", "120", "144", "Unlimited"};
static const int fps_values[] = {30, 60, 90, 120, 144, 0};
static const char *const onoff_names[] = {"off", "on"};
static const char *const onoff_labels[] = {"Off", "On"};
static const char *const yesno_names[] = {"no", "yes"};
static const char *const yesno_labels[] = {"No", "Yes"};
static const char *const filter_names[] = {"pixel", "sharp", "smooth", "lcd"};
static const char *const filter_labels[] = {"Off (Pixel)", "Sharp", "Smooth", "LCD (DS)"};
static const char *const color_names[] = {"ds", "light", "moderate", "strong"};
static const char *const color_labels[] = {"Original (DS)", "SNES light", "SNES moderate", "SNES strong"};
/* SNES gamma: the DS version's palette was brightened for the DS screen, so on a PC display it looks washed out.
 * Each channel is raised to an exponent: black and white stay, midtones darken and colors deepen. */
static const float color_exponents[] = {1.0f, 1.1f, 1.2f, 1.4f};
static const char *const language_names[] = {"english", "french"};
static const char *const language_labels[] = {"English", "French"};
static const char *const ff_music_names[] = {"fast", "normal"};
static const char *const ff_music_labels[] = {"Sped up", "Normal speed"};
static const char *const off_names[] = {"off"};
static const char **shader_names; /* "off", then shader_key() of each listed shader */

static Setting settings[SET_COUNT] = {
    /* The DS's system language, which the game reads at power-on (boot.c): changes apply at the next start. */
    [SET_LANGUAGE] = {"language", "Language", 2, LANGUAGE_ENGLISH, language_names, language_labels},
    [SET_WINDOW] = {"window", "Window mode", 3, WINDOW_BORDERLESS, window_names, window_labels},
    [SET_RESOLUTION] = {"resolution", "Internal resolution", RES_MAX + 1, 0, res_names, NULL},
    [SET_ASPECT] = {"aspect", "Aspect ratio", 5, ASPECT_16_9, aspect_names, aspect_labels},
    /* Widescreen in rooms narrower than the view: No leaves the room as it is, black beside it (video.c). */
    [SET_FILL] = {"fillsides", "Fill screen sides", 2, 0, yesno_names, yesno_labels},
    [SET_FRAMERATE] = {"framerate", "Framerate", 6, 1, fps_names, fps_labels},
    [SET_VSYNC] = {"vsync", "V-Sync", 2, 1, onoff_names, onoff_labels},
    [SET_FILTER] = {"filter", "Graphics filter", 4, FILTER_SHARP, filter_names, filter_labels},
    [SET_SHADER] = {"shader", "Shader", 1, 0, off_names, NULL}, /* the list: list_shaders */
    [SET_COLOR] = {"color", "Color", 4, 1, color_names, color_labels},
    [SET_MODS] = {"mods", "Mods", 2, 1, onoff_names, onoff_labels}, /* replacement cutscenes and music (movie.c, music.c) */
    /* Fast forward (main.c, sched.c); Normal speed keeps the music sequences on the host's clock (snd.c). */
    [SET_FF_MUSIC] = {"ffmusic", "Fast forward music", 2, FF_MUSIC_FAST, ff_music_names, ff_music_labels},
};
#define SETTING(id) (settings[id].value)

static SDL_Window *win;
static SDL_Renderer *ren;
static int layout = EGG_SINGLE_SCREEN ? LAYOUT_TOP : LAYOUT_STACKED, swapped;
static int last_full = WINDOW_BORDERLESS; /* F11 from windowed returns here */
static SDL_Texture *native[4], *smooth[4], *internal; /* top, bottom, main, party; internal: N x the layout */
static SDL_Texture *shader_in[2], *shader_out[2];      /* per screen: its picture, then shaded at its window size */
static uint64_t shader_frames;
static char saved_shader[1024];                        /* eggoftime.ini's shader, until the list is known */
static int party_on;                                   /* the party layer is shown (camera smoothing) */
static int main_w = SCREEN_W;                          /* the main screen's width (widescreen) */
static int main_guard;                                 /* hidden columns each side of it (camera smoothing) */
static float slide_x, slide_y;                         /* camera smoothing: where the view is shown from */
static int asked_margin, asked_guard;                  /* last margin requested from the renderer */
static int reupload;                                   /* the filter changed: upload the screens again */
static uint32_t scale2x_tmp[VIDEO_LINE_MAX * 2 * SCREEN_H * 2];
static uint32_t scaled[VIDEO_LINE_MAX * SMOOTH_SCALE * SCREEN_H * SMOOTH_SCALE];
static uint32_t corrected[VIDEO_LINE_MAX * SCREEN_H]; /* a screen with the Color setting applied */
static SDL_Texture *hd_tex[4];                         /* the screens drawn from HD art (graphics mods) */
static int hd_k[4];                                    /* their scale; 0: not drawn this way */
static uint32_t *hd_px;                                /* a screen being drawn from HD art */
static const VideoHdFrame *hd_frame;                   /* the sources of the screens being uploaded */
static uint8_t gamma_lut[256];
static int gamma_lut_for = -1;                         /* the Color value gamma_lut was built for */
static char ini_path[1024];
static char capture_path[1024]; /* save the next presented window here (bring-up aid) */
static char message[64];
static Uint64 message_until;

static void show_message(const char *fmt, const char *arg) {
    snprintf(message, sizeof message, fmt, arg);
    message_until = SDL_GetTicks() + MESSAGE_MS;
}

void display_message(const char *text, unsigned ms) {
    snprintf(message, sizeof message, "%s", text);
    message_until = SDL_GetTicks() + ms;
}

/* The last change, in SDL's built-in 8x8 font, scaled with the window, top left on a dark box. */
static void draw_message(void) {
    if (!message[0] || SDL_GetTicks() >= message_until) return;
    int ww, wh;
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    float k = SDL_max(2.0f, SDL_floorf(wh / 270.0f));
    float w = (float)strlen(message) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 8, h = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 8;
    SDL_SetRenderScale(ren, k, k);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
    SDL_RenderFillRect(ren, &(SDL_FRect){4, 4, w, h});
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderDebugText(ren, 8, 8, message);
    SDL_SetRenderScale(ren, 1, 1);
}

static int lookup(const char *const *names, int n, const char *v) {
    for (int i = 0; i < n; i++) {
        if (!strcmp(names[i], v)) return i;
    }
    return -1;
}

static void load_settings(void) {
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[1100], key[32];
    while (fgets(line, sizeof line, f)) {
        int at = -1;
        if (sscanf(line, " %31[a-z] = %n", key, &at) != 1 || at < 0) continue;
        char *val = line + at;
        size_t len = strcspn(val, "\r\n");
        while (len && (val[len - 1] == ' ' || val[len - 1] == '\t')) len--;
        val[len] = 0;
        int v;
        if (!strcmp(key, "shader")) snprintf(saved_shader, sizeof saved_shader, "%s", val);
        if (!strcmp(key, "layout") && (v = lookup(layout_names, LAYOUT_COUNT, val)) >= 0) layout = v;
        else if (!strcmp(key, "swap")) swapped = val[0] == '1';
        for (int i = 0; i < SET_COUNT; i++) {
            if (!strcmp(key, settings[i].key) && (v = lookup(settings[i].names, settings[i].count, val)) >= 0) {
                settings[i].value = v;
            }
        }
    }
    fclose(f);
}

static void save_settings(void) {
    FILE *f = fopen(ini_path, "w");
    if (!f) return;
    fprintf(f, "# Egg of Time display settings (System Settings menu; F2 layout, F3 swap screens, F4 filter)\n");
    fprintf(f, "layout = %s\nswap = %d\n", layout_names[layout], swapped);
    for (int i = 0; i < SET_COUNT; i++) fprintf(f, "%s = %s\n", settings[i].key, settings[i].names[settings[i].value]);
    fclose(f);
}

/* ---- geometry ---- */

static int main_shown(void) { return layout == LAYOUT_TOP && !swapped; } /* top-only shows the main screen */

static void content_size(int *w, int *h) {
    *w = layout == LAYOUT_SIDE ? SCREEN_W * 2 : main_shown() ? main_w - 2 * main_guard : SCREEN_W;
    *h = layout == LAYOUT_STACKED ? SCREEN_H * 2 : SCREEN_H;
}

/* Camera smoothing (framerates above 60): between game frames, the field picture is shown slid back toward
 * where the camera was, by the fraction of the frame still to come (main.c), so scrolling moves on every
 * window frame and at sub-pixel steps of the internal resolution. Background and sprites move together, nothing
 * is blended. The renderer draws VIDEO_SLIDE_MAX hidden columns each side for it; a vertical slide repeats the
 * edge row for its one or two pixels. */
static int camera_smoothing(void) { return fps_values[SETTING(SET_FRAMERATE)] != 60 && fps_values[SETTING(SET_FRAMERATE)] != 30; }

/* Columns beyond each side of the top screen for the chosen aspect ratio (only the main screen shows them). */
static int wide_margin(void) {
    if (!EGG_SINGLE_SCREEN || !main_shown()) return 0;
    float ratio = aspect_ratios[SETTING(SET_ASPECT)];
    if (ratio == 0) {
        int ww, wh;
        SDL_GetRenderOutputSize(ren, &ww, &wh);
        ratio = wh > 0 ? (float)ww / wh : 4.0f / 3;
    }
    return SDL_clamp(((int)SDL_lroundf(SCREEN_H * ratio) - SCREEN_W) / 2, 0, VIDEO_MARGIN_MAX);
}

/* Where a screen goes in the layout, in native pixels; screen 0 = top, 1 = bottom. 0 when it is not shown. */
static int screen_slot(int screen, int *x, int *y) {
    int slot = screen ^ swapped; /* position: 0 first (top/left), 1 second */
    if (layout == LAYOUT_TOP && slot != 0) return 0;
    *x = slot && layout == LAYOUT_SIDE ? SCREEN_W : 0;
    *y = slot && layout == LAYOUT_STACKED ? SCREEN_H : 0;
    return 1;
}

/* The window rectangle the layout is shown in. */
static SDL_FRect dest_rect(void) {
    int cw, ch, ww, wh;
    content_size(&cw, &ch);
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    float h = SDL_min((float)wh, (float)ww * ch / cw);
    if (SETTING(SET_FILTER) == FILTER_PIXEL && !SETTING(SET_SHADER) && h >= ch) {
        h = SDL_floorf(h / ch) * ch; /* integer scaling */
    }
    float w = h * cw / ch;
    return (SDL_FRect){SDL_floorf((ww - w) / 2), SDL_floorf((wh - h) / 2), w, h};
}

/* Internal resolution: N times the layout's native size. */
static int internal_scale(const SDL_FRect *dest) {
    if (SETTING(SET_RESOLUTION)) return SETTING(SET_RESOLUTION);
    int cw, ch;
    content_size(&cw, &ch);
    return SDL_clamp((int)SDL_ceilf(dest->h / ch), 1, RES_MAX);
}

static void resize_window(void) {
    int cw, ch;
    content_size(&cw, &ch);
    if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        SDL_SetWindowSize(win, cw * WINDOW_SCALE, ch * WINDOW_SCALE);
    }
}

static void apply_window_mode(int mode) {
    if (mode == WINDOW_WINDOWED) {
        SDL_SetWindowFullscreen(win, false);
        return;
    }
    if (mode == WINDOW_FULLSCREEN) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(win);
        const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(d);
        SDL_DisplayMode m;
        if (desk && SDL_GetClosestFullscreenDisplayMode(d, desk->w, desk->h, desk->refresh_rate, true, &m)) {
            SDL_SetWindowFullscreenMode(win, &m);
        }
    } else {
        SDL_SetWindowFullscreenMode(win, NULL); /* borderless, at the desktop's mode */
    }
    SDL_SetWindowFullscreen(win, true);
    last_full = mode;
}

/* ---- filters ---- */

/* Scale2x (EPX): each pixel becomes 2x2, taking a neighbour's colour along edges between two equal neighbours. */
static void scale2x(const uint32_t *src, int w, int h, uint32_t *dst) {
    for (int y = 0; y < h; y++) {
        const uint32_t *up = src + (y > 0 ? y - 1 : y) * w, *row = src + y * w, *down = src + (y < h - 1 ? y + 1 : y) * w;
        uint32_t *d0 = dst + (2 * y) * (2 * w), *d1 = d0 + 2 * w;
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x], a = up[x], d = down[x];
            uint32_t c = row[x > 0 ? x - 1 : x], b = row[x < w - 1 ? x + 1 : x];
            int edge = a != d && c != b;
            d0[2 * x] = edge && c == a ? a : p;
            d0[2 * x + 1] = edge && a == b ? b : p;
            d1[2 * x] = edge && c == d ? c : p;
            d1[2 * x + 1] = edge && d == b ? d : p;
        }
    }
}

static int screen_width(int tex) { return tex >= 2 ? main_w : SCREEN_W; }

/* px with the Color setting applied (px itself for Original). */
static const uint32_t *color_corrected(const uint32_t *px, int w) {
    int v = SETTING(SET_COLOR);
    if (!v) return px;
    if (gamma_lut_for != v) {
        for (int i = 0; i < 256; i++) gamma_lut[i] = (uint8_t)SDL_lroundf(255 * SDL_powf(i / 255.0f, color_exponents[v]));
        gamma_lut_for = v;
    }
    for (int i = 0; i < w * SCREEN_H; i++) {
        uint32_t c = px[i];
        corrected[i] = (c & 0xFF000000u) | (uint32_t)gamma_lut[(c >> 16) & 0xFF] << 16 |
                       (uint32_t)gamma_lut[(c >> 8) & 0xFF] << 8 | gamma_lut[c & 0xFF];
    }
    return corrected;
}

/* ---- HD graphics mods ---- */

static inline uint32_t rgb555(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16;
}

/* A texel of the HD art, weighted and with the added colour, as the renderer composed the native pixel; through
 * the Color setting's curve (lut, NULL for Original). */
static inline uint32_t hd_mix(uint32_t c, int weight, uint32_t add, const uint8_t *lut) {
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        uint32_t v = (((c >> s) & 0xFF) * (uint32_t)weight >> 4) + ((add >> s) & 0xFF);
        v = v > 255 ? 255 : v;
        out |= (uint32_t)(lut ? lut[v] : v) << s;
    }
    return out;
}

/* Rows y0..y1-1 of a screen (w columns) drawn at k times its size (job of compose_hd). */
typedef struct HdJob { const uint32_t *px; const VideoHd *src; const uint16_t *palette; const uint8_t *lut; uint32_t *out; int w, k, y0, y1; } HdJob;

static void compose_rows(const HdJob *j) {
    int w = j->w, k = j->k, ow = w * k;
    uint8_t at[SCALE_MAX + 1][RES_MAX]; /* by HD scale: the HD texel under each of a pixel's k columns */
    for (int es = 1; es <= SCALE_MAX; es++) for (int sx = 0; sx < k; sx++) at[es][sx] = (uint8_t)(sx * es / k);
    for (int y = j->y0; y < j->y1; y++) {
        for (int x = 0; x < w; x++) {
            const VideoHd *r = &j->src[y * w + x];
            const GfxHdTile *t = r->tile ? gfxmod_tile(r->tile >> 8) : NULL;
            uint32_t *o = j->out + (size_t)y * k * ow + (size_t)x * k;
            if (!t) {
                uint32_t c = j->px[y * w + x];
                for (int sy = 0; sy < k; sy++) for (int sx = 0; sx < k; sx++) o[sy * ow + sx] = c;
                continue;
            }
            int es = t->scale, n = 8 * es, tx = (r->tile >> 5) & 7, ty = (r->tile >> 2) & 7;
            int hflip = (r->tile >> 1) & 1, vflip = r->tile & 1;
            uint32_t add = rgb555(r->added), below = 0xFF000000u | rgb555(r->below);
            if (j->lut) below = hd_mix(below, 16, 0, j->lut);
            const uint16_t *pal = j->palette + ((r->pal >> 4) & 1) * 0x200 + ((r->pal >> 5) & 1) * 0x100 +
                                  (t->bpp8 ? 0 : (r->pal & 15) * 16);
            const uint8_t *col = at[es];
            for (int sy = 0; sy < k; sy++) {
                int v = ty * es + (vflip ? es - 1 - col[sy] : col[sy]);
                uint32_t *line = o + sy * ow;
                if (t->indexed) {
                    const uint8_t *texels = (const uint8_t *)t->px + v * n + tx * es;
                    for (int sx = 0; sx < k; sx++) {
                        uint8_t i = texels[hflip ? es - 1 - col[sx] : col[sx]];
                        line[sx] = i ? hd_mix(rgb555(pal[i]), r->weight, add, j->lut) : below;
                    }
                } else {
                    const uint32_t *texels = (const uint32_t *)t->px + v * n + tx * es;
                    for (int sx = 0; sx < k; sx++) {
                        uint32_t a = texels[hflip ? es - 1 - col[sx] : col[sx]];
                        line[sx] = a >> 24 ? hd_mix(a, r->weight, add, j->lut) : below;
                    }
                }
            }
        }
    }
}

/* compose_rows over the whole screen, in bands on worker threads (up to HD_WORKERS, leaving the game's and this
 * thread's cores) and this one. */
#define HD_WORKERS 6
static HdJob hd_jobs[HD_WORKERS + 1];
static SDL_Semaphore *hd_go[HD_WORKERS], *hd_done;
static int hd_workers = -1;

static int SDLCALL hd_worker(void *arg) {
    int i = (int)(intptr_t)arg;
    for (;;) {
        SDL_WaitSemaphore(hd_go[i]);
        compose_rows(&hd_jobs[i]);
        SDL_SignalSemaphore(hd_done);
    }
    return 0;
}

static void compose_hd(const HdJob *job) {
    if (hd_workers < 0) {
        int want = SDL_clamp(SDL_GetNumLogicalCPUCores() - 2, 0, HD_WORKERS);
        hd_done = SDL_CreateSemaphore(0);
        for (hd_workers = 0; hd_done && hd_workers < want; hd_workers++) {
            if (!(hd_go[hd_workers] = SDL_CreateSemaphore(0))) break;
            SDL_Thread *t = SDL_CreateThread(hd_worker, "hd", (void *)(intptr_t)hd_workers);
            if (!t) break;
            SDL_DetachThread(t);
        }
    }
    int bands = hd_workers + 1;
    for (int i = 0; i < bands; i++) {
        hd_jobs[i] = *job;
        hd_jobs[i].y0 = SCREEN_H * i / bands, hd_jobs[i].y1 = SCREEN_H * (i + 1) / bands;
    }
    for (int i = 0; i < hd_workers; i++) SDL_SignalSemaphore(hd_go[i]);
    compose_rows(&hd_jobs[hd_workers]);
    for (int i = 0; i < hd_workers; i++) SDL_WaitSemaphore(hd_done);
}

/* The screen's sources, when it is drawn from HD art now (not through a shader, and Mods on). */
static const VideoHd *hd_source(int tex) {
    if (!hd_frame || !hd_frame->on || SETTING(SET_SHADER) || !SETTING(SET_MODS) || !gfxmod_hd_scale()) return NULL;
    return tex == 0 ? hd_frame->top : tex == 1 ? hd_frame->bottom : tex == 2 ? hd_frame->main : hd_frame->party;
}

/* Draws and uploads screen `tex` (its pixels px with the Color setting applied) from HD art at k times its size;
 * 0 if it cannot. */
static int upload_hd(int tex, const uint32_t *px, const VideoHd *src, int k) {
    int w = screen_width(tex), ow = w * k, oh = SCREEN_H * k;
    float tw = 0, th = 0;
    if (hd_tex[tex]) SDL_GetTextureSize(hd_tex[tex], &tw, &th);
    int full_w = (tex >= 2 ? VIDEO_LINE_MAX : SCREEN_W) * k;
    if ((int)tw != full_w || (int)th != oh) {
        SDL_DestroyTexture(hd_tex[tex]);
        hd_tex[tex] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, full_w, oh);
        if (!hd_tex[tex]) return 0;
        SDL_SetTextureScaleMode(hd_tex[tex], SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(hd_tex[tex], tex == 3 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    }
    static size_t have;
    size_t need = (size_t)VIDEO_LINE_MAX * SCREEN_H * k * k;
    if (have < need) {
        SDL_free(hd_px);
        hd_px = SDL_malloc(need * 4);
        have = hd_px ? need : 0;
        if (!hd_px) return 0;
    }
    compose_hd(&(HdJob){px, src, hd_frame->palette, SETTING(SET_COLOR) ? gamma_lut : NULL, hd_px, w, k, 0, SCREEN_H});
    SDL_UpdateTexture(hd_tex[tex], &(SDL_Rect){0, 0, ow, oh}, hd_px, ow * 4);
    return 1;
}

static int internal_scale(const SDL_FRect *dest);

/* shown: the screen is in the layout (only those are drawn from HD art). */
static void upload(int tex, const uint32_t *px, int shown) {
    int w = screen_width(tex);
    px = color_corrected(px, w);
    const VideoHd *src = shown ? hd_source(tex) : NULL;
    SDL_FRect dest = dest_rect();
    int k = src ? SDL_min(gfxmod_hd_scale(), internal_scale(&dest)) : 0;
    hd_k[tex] = k > 1 && upload_hd(tex, px, src, k) ? k : 0;
    SDL_UpdateTexture(native[tex], &(SDL_Rect){0, 0, w, SCREEN_H}, px, w * 4);
    if (SETTING(SET_FILTER) == FILTER_SMOOTH) {
        scale2x(px, w, SCREEN_H, scale2x_tmp);
        scale2x(scale2x_tmp, w * 2, SCREEN_H * 2, scaled);
        SDL_UpdateTexture(smooth[tex], &(SDL_Rect){0, 0, w * SMOOTH_SCALE, SCREEN_H * SMOOTH_SCALE}, scaled,
                          w * SMOOTH_SCALE * 4);
    }
}

/* LCD (DS) filter: the DS screen's pixel grid, after Gigaherz's lcd3x shader (public domain): within each
 * screen pixel, a row factor (scanlines between rows) times a column factor per color channel (the red, green
 * and blue subpixel stripes), (brighten + sin(2 pi t + phase)) / (brighten + 1) with lcd3x's defaults. The
 * factors depend only on the place within the pixel, so they are one pixel's worth (n x n at internal scale n),
 * tiled over each screen and multiplied in (SDL_BLENDMODE_MOD): what the shader computes, on any renderer. */
#define LCD_BRIGHTEN_SCANLINES 16.0f
#define LCD_BRIGHTEN_STRIPES 4.0f

static SDL_Texture *lcd_mask;
static int lcd_mask_n;

static SDL_Texture *lcd_grid(int n) {
    if (lcd_mask && lcd_mask_n == n) return lcd_mask;
    SDL_DestroyTexture(lcd_mask);
    lcd_mask = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, n, n);
    lcd_mask_n = n;
    if (!lcd_mask) return NULL;
    static const float phase[3] = {SDL_PI_F / 2, SDL_PI_F / 2 - 2 * SDL_PI_F / 3, SDL_PI_F / 2 - 4 * SDL_PI_F / 3};
    uint32_t *px = SDL_malloc((size_t)n * n * 4);
    if (!px) return NULL;
    for (int j = 0; j < n; j++) {
        float row = (LCD_BRIGHTEN_SCANLINES + SDL_sinf(2 * SDL_PI_F * (j + 0.5f) / n)) / (LCD_BRIGHTEN_SCANLINES + 1);
        for (int i = 0; i < n; i++) {
            uint32_t c = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                float f = row * (LCD_BRIGHTEN_STRIPES + SDL_sinf(2 * SDL_PI_F * (i + 0.5f) / n + phase[ch])) /
                          (LCD_BRIGHTEN_STRIPES + 1);
                c |= (uint32_t)SDL_lroundf(255 * f) << (8 * ch); /* ABGR8888: red in the low byte */
            }
            px[j * n + i] = c;
        }
    }
    SDL_UpdateTexture(lcd_mask, NULL, px, n * 4);
    SDL_free(px);
    SDL_SetTextureBlendMode(lcd_mask, SDL_BLENDMODE_MOD);
    SDL_SetTextureScaleMode(lcd_mask, SDL_SCALEMODE_NEAREST);
    return lcd_mask;
}

/* *t as a w x h render target, made again when its size differs. */
static SDL_Texture *sized_target(SDL_Texture **t, int w, int h) {
    float tw = 0, th = 0;
    if (*t) SDL_GetTextureSize(*t, &tw, &th);
    if ((int)tw != w || (int)th != h) {
        SDL_DestroyTexture(*t);
        *t = w > 0 && h > 0 ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h) : NULL;
        if (*t) SDL_SetTextureScaleMode(*t, SDL_SCALEMODE_NEAREST);
        if (*t) SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_NONE); /* copied as they are (shaders may leave alpha 0) */
    }
    return *t;
}

/* The shown screens through the shader, each from its place in the internal target (native size) to its place
 * in the window. 0 if the shader cannot run: nothing is drawn. */
static int draw_shaded(const SDL_FRect *shown, const int *screens, int nshown, int cw, int ch, const SDL_FRect *dest) {
    float kx = dest->w / cw, ky = dest->h / ch;
    SDL_FRect at[2];
    for (int i = 0; i < nshown; i++) {
        const SDL_FRect *r = &shown[i];
        at[i] = (SDL_FRect){SDL_roundf(dest->x + r->x * kx), SDL_roundf(dest->y + r->y * ky), SDL_roundf(r->w * kx),
                            SDL_roundf(r->h * ky)};
        int s = screens[i];
        if (!sized_target(&shader_in[s], (int)r->w, (int)r->h) || !sized_target(&shader_out[s], (int)at[i].w, (int)at[i].h)) {
            return 0;
        }
        SDL_SetRenderTarget(ren, shader_in[s]);
        SDL_RenderTexture(ren, internal, r, NULL);
        SDL_SetRenderTarget(ren, NULL);
        if (!shader_run(s, SETTING(SET_SHADER) - 1, shader_in[s], shader_out[s], shader_frames)) return 0;
    }
    for (int i = 0; i < nshown; i++) SDL_RenderTexture(ren, shader_out[screens[i]], NULL, &at[i]);
    shader_frames++;
    return 1;
}

/* Draws the shown screens into the internal target, then the target into the window (through the shader if one
 * is chosen: then the target is the native size). */
static void draw_screens(void) {
    int cw, ch;
    content_size(&cw, &ch);
    SDL_FRect dest = dest_rect();
    int shader = SETTING(SET_SHADER) != 0;
    int n = shader ? 1 : internal_scale(&dest);
    if (!sized_target(&internal, cw * n, ch * n)) return;
    SDL_SetRenderTarget(ren, internal);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_FRect shown[2];
    int screens[2], nshown = 0;
    for (int screen = 0; screen < 2; screen++) {
        int x, y;
        if (!screen_slot(screen, &x, &y)) continue;
        int tex = screen == 0 && main_shown() ? 2 : screen; /* the main screen alone */
        int k = hd_k[tex] ? hd_k[tex] : !shader && SETTING(SET_FILTER) == FILTER_SMOOTH ? SMOOTH_SCALE : 1;
        SDL_Texture *t = hd_k[tex] ? hd_tex[tex] : k > 1 ? smooth[tex] : native[tex];
        if (tex != 2 || !main_guard) {
            int w = screen_width(tex);
            SDL_FRect src = {0, 0, (float)w * k, (float)SCREEN_H * k};
            SDL_FRect r = {(float)x * n, (float)y * n, (float)w * n, (float)SCREEN_H * n};
            SDL_RenderTexture(ren, t, &src, &r);
            screens[nshown] = screen;
            shown[nshown++] = r;
            continue;
        }
        /* The field with its hidden columns, slid (camera smoothing); rows beyond the picture repeat its edge. */
        float vw = (float)(main_w - 2 * main_guard), sx = ((float)main_guard + slide_x) * k, o = slide_y;
        screens[nshown] = screen;
        shown[nshown++] = (SDL_FRect){(float)x * n, (float)y * n, vw * n, (float)SCREEN_H * n};
        float rows = SCREEN_H - SDL_fabsf(o), top = o < 0 ? -o : 0;
        SDL_RenderTexture(ren, t, &(SDL_FRect){sx, (o > 0 ? o : 0) * k, vw * k, rows * k},
                          &(SDL_FRect){(float)x * n, (top + y) * n, vw * n, rows * n});
        if (o != 0) {
            float edge = o > 0 ? SCREEN_H - 1 : 0, at = o > 0 ? rows : 0;
            SDL_RenderTexture(ren, t, &(SDL_FRect){sx, edge * k, vw * k, (float)k},
                              &(SDL_FRect){(float)x * n, (at + y) * n, vw * n, SDL_fabsf(o) * n});
        }
        if (party_on) { /* the sprites the camera follows: not slid (drawn at the main screen's scale) */
            SDL_RenderTexture(ren, hd_k[2] ? (hd_k[3] == k ? hd_tex[3] : native[3]) : k > 1 ? smooth[3] : native[3],
                              &(SDL_FRect){(float)main_guard * k, 0, vw * k, (float)SCREEN_H * k},
                              &(SDL_FRect){(float)x * n, (float)y * n, vw * n, (float)SCREEN_H * n});
        }
    }
    SDL_Texture *grid = !shader && SETTING(SET_FILTER) == FILTER_LCD ? lcd_grid(n) : NULL;
    for (int i = 0; grid && i < nshown; i++) SDL_RenderTextureTiled(ren, grid, NULL, 1, &shown[i]);
    SDL_SetRenderTarget(ren, NULL);
    if (shader && draw_shaded(shown, screens, nshown, cw, ch, &dest)) return;
    SDL_SetTextureScaleMode(internal, SETTING(SET_FILTER) == FILTER_PIXEL ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(ren, internal, NULL, &dest);
}

/* ---- interface ---- */

/* The Shader setting's choices: Off, then the shaders shader_init kept; the saved one chosen if listed. */
static void list_shaders(void) {
    int n = shader_count();
    shader_names = SDL_calloc((size_t)n + 1, sizeof *shader_names);
    if (!shader_names) return;
    shader_names[0] = off_names[0];
    for (int i = 0; i < n; i++) shader_names[i + 1] = shader_key(i);
    int v = lookup(shader_names, n + 1, saved_shader);
    settings[SET_SHADER] = (Setting){"shader", "Shader", n + 1, v > 0 ? v : 0, shader_names, NULL};
}

int display_init(void) {
    snprintf(ini_path, sizeof ini_path, "%seggoftime.ini", rt_data_dir());
    load_settings();
    shader_scan();
    int cw, ch;
    content_size(&cw, &ch);
    win = SDL_CreateWindow("Egg of Time", cw * WINDOW_SCALE, ch * WINDOW_SCALE,
                           SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | (shader_count() ? SDL_WINDOW_OPENGL : 0));
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    /* Shaders run on the OpenGL renderer; without them (or OpenGL 3.3), SDL's default renderer. */
    ren = shader_count() ? SDL_CreateRenderer(win, "opengl") : NULL;
    if (ren && !shader_init(ren)) {
        SDL_DestroyRenderer(ren);
        ren = NULL;
    }
    if (!ren && !(ren = SDL_CreateRenderer(win, NULL))) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 0;
    }
    list_shaders();
    SDL_SetRenderVSync(ren, SETTING(SET_VSYNC));
    if (SETTING(SET_WINDOW) != WINDOW_WINDOWED) last_full = SETTING(SET_WINDOW);
    if (!SDL_getenv("EGG_WINDOWED")) apply_window_mode(SETTING(SET_WINDOW));
    for (int i = 0; i < 4; i++) {
        int w = i >= 2 ? VIDEO_LINE_MAX : SCREEN_W;
        native[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, SCREEN_H);
        smooth[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w * SMOOTH_SCALE,
                                      SCREEN_H * SMOOTH_SCALE);
        if (!native[i] || !smooth[i]) return 0;
        SDL_SetTextureScaleMode(native[i], SDL_SCALEMODE_NEAREST);
        SDL_SetTextureScaleMode(smooth[i], SDL_SCALEMODE_LINEAR);
    }
    SDL_SetTextureBlendMode(native[3], SDL_BLENDMODE_BLEND); /* the party layer is transparent elsewhere */
    SDL_SetTextureBlendMode(smooth[3], SDL_BLENDMODE_BLEND);
    return 1;
}

void display_frame(const uint32_t *const screens[3], const uint32_t *party, int width, int new_frame, float slide_dx,
                   float slide_dy, const VideoHdFrame *hd) {
    /* A main screen of the margin asked for (plus guard) is the field: its guard columns are hidden. */
    main_guard = asked_guard && width == SCREEN_W + 2 * asked_margin ? asked_guard : 0;
    slide_x = main_guard ? slide_dx : 0;
    slide_y = main_guard ? slide_dy : 0;
    asked_guard = camera_smoothing() && EGG_SINGLE_SCREEN && main_shown() ? VIDEO_SLIDE_MAX : 0;
    asked_margin = SDL_min(wide_margin() + asked_guard, VIDEO_MARGIN_MAX);
    video_set_wide_margin(asked_margin);
    video_set_fill_sides(SETTING(SET_FILL));
    video_set_camera_smoothing(asked_guard != 0);
    main_w = width;
    party_on = main_guard && party;
    if (new_frame || reupload) {
        hd_frame = hd;
        int shown[3] = {0};
        for (int screen = 0, x, y; screen < 2; screen++) {
            if (screen_slot(screen, &x, &y)) shown[screen == 0 && main_shown() ? 2 : screen] = 1;
        }
        for (int i = 0; i < 3; i++) upload(i, screens[i], shown[i]);
        if (party_on) upload(3, party, shown[2]);
        hd_frame = NULL;
        reupload = 0;
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (!movie_draw(ren)) draw_screens();
    draw_message();
    if (capture_path[0]) {
        SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
        if (shot) {
            SDL_SaveBMP(shot, capture_path);
            SDL_DestroySurface(shot);
        }
        capture_path[0] = 0;
    }
    SDL_RenderPresent(ren);
}

int display_key(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_F2:
        layout = (layout + 1) % LAYOUT_COUNT;
        resize_window();
        show_message("Layout: %s", layout_labels[layout]);
        break;
    case SDL_SCANCODE_F3:
        swapped = !swapped;
        show_message("%s", swapped ? "Screens swapped" : "Screens in normal order");
        break;
    case SDL_SCANCODE_F4:
        display_setting_set(SET_FILTER, (SETTING(SET_FILTER) + 1) % settings[SET_FILTER].count);
        show_message("Filter: %s", filter_labels[SETTING(SET_FILTER)]);
        return 1;
    case SDL_SCANCODE_F11:
        display_setting_set(SET_WINDOW, SETTING(SET_WINDOW) == WINDOW_WINDOWED ? last_full : WINDOW_WINDOWED);
        return 1;
    default: return 0;
    }
    save_settings();
    return 1;
}

const char *display_setting_title(int id) { return settings[id].title; }
int display_setting_count(int id) { return settings[id].count; }
int display_setting_get(int id) { return settings[id].value; }
int display_setting_shown(int id) {
    switch (id) {
    case SET_FILL: return SETTING(SET_ASPECT) != ASPECT_ORIGINAL;
    case SET_SHADER: return settings[SET_SHADER].count > 1;
    case SET_RESOLUTION:
    case SET_FILTER: return !SETTING(SET_SHADER); /* a shader replaces them */
    default: return 1;
    }
}

const char *display_shader_path(void) { return SETTING(SET_SHADER) ? settings[SET_SHADER].names[SETTING(SET_SHADER)] : NULL; }

const char *display_setting_label(int id, int v) {
    if (id == SET_SHADER) return v ? shader_label(v - 1) : "Off";
    if (id != SET_RESOLUTION) return settings[id].labels[v];
    if (v == 0) return "Auto";
    static char buf[32];
    int cw, ch;
    content_size(&cw, &ch);
    snprintf(buf, sizeof buf, "%dx (%dx%d)", v, cw * v, ch * v);
    return buf;
}

void display_setting_set(int id, int v) {
    settings[id].value = v;
    switch (id) {
    case SET_WINDOW: apply_window_mode(v); break;
    case SET_VSYNC: SDL_SetRenderVSync(ren, v); break;
    case SET_FILTER:
    case SET_COLOR: reupload = 1; break;
    }
    save_settings();
}

int display_camera_smoothing(void) { return camera_smoothing(); }

uint64_t display_frame_ns(void) {
    int fps = fps_values[SETTING(SET_FRAMERATE)];
    return fps ? SDL_NS_PER_SECOND / (uint64_t)fps : 0;
}

int display_touch(float wx, float wy, int *x, int *y) {
    int cw, ch, sx, sy;
    float px, py;
    if (!screen_slot(1, &sx, &sy)) return 0;
    content_size(&cw, &ch);
    SDL_RenderCoordinatesFromWindow(ren, wx, wy, &px, &py);
    SDL_FRect d = dest_rect();
    float cx = (px - d.x) * cw / d.w - sx, cy = (py - d.y) * ch / d.h - sy; /* native pixels in the screen */
    if (cx < 0 || cy < 0 || cx >= SCREEN_W || cy >= SCREEN_H) return 0;
    *x = (int)cx;
    *y = (int)cy;
    return 1;
}

void display_capture(const char *path) { snprintf(capture_path, sizeof capture_path, "%s", path); }
