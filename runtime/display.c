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
 * Window modes: windowed, borderless windowed (fullscreen on the desktop's mode), fullscreen (exclusive, at
 * the desktop's mode); F11 toggles between windowed and the last other mode. EGG_WINDOWED=1 starts windowed
 * without changing the setting (automated runs). Framerate only paces the window's presentation (main.c):
 * the game keeps its own 60 Hz clock, so above 60 frames are shown again and below 60 some are skipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "display.h"
#include "movie.h"
#include "single_screen.h"
#include "video.h"

#define WINDOW_SCALE 3
#define SMOOTH_SCALE 4
#define MESSAGE_MS 2000
#define RES_MAX 11

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
static const char *const filter_names[] = {"pixel", "sharp", "smooth", "lcd"};
static const char *const filter_labels[] = {"Off (Pixel)", "Sharp", "Smooth", "LCD (DS)"};
static const char *const color_names[] = {"ds", "light", "moderate", "strong"};
static const char *const color_labels[] = {"Original (DS)", "SNES light", "SNES moderate", "SNES strong"};
/* SNES gamma: the DS version's palette was brightened for the DS screen, so on a PC display it looks washed out.
 * Each channel is raised to an exponent: black and white stay, midtones darken and colors deepen. */
static const float color_exponents[] = {1.0f, 1.1f, 1.2f, 1.4f};

static Setting settings[SET_COUNT] = {
    [SET_WINDOW] = {"window", "Window mode", 3, WINDOW_BORDERLESS, window_names, window_labels},
    [SET_RESOLUTION] = {"resolution", "Internal resolution", RES_MAX + 1, 0, res_names, NULL},
    [SET_ASPECT] = {"aspect", "Aspect ratio", 5, ASPECT_16_9, aspect_names, aspect_labels},
    [SET_FRAMERATE] = {"framerate", "Framerate", 6, 1, fps_names, fps_labels},
    [SET_VSYNC] = {"vsync", "V-Sync", 2, 1, onoff_names, onoff_labels},
    [SET_FILTER] = {"filter", "Graphics filter", 4, FILTER_SHARP, filter_names, filter_labels},
    [SET_COLOR] = {"color", "Color", 4, 1, color_names, color_labels},
    [SET_MODS] = {"mods", "Mods", 2, 1, onoff_names, onoff_labels}, /* replacement cutscenes and music (movie.c, music.c) */
};
#define SETTING(id) (settings[id].value)

static SDL_Window *win;
static SDL_Renderer *ren;
static int layout = EGG_SINGLE_SCREEN ? LAYOUT_TOP : LAYOUT_STACKED, swapped;
static int last_full = WINDOW_BORDERLESS; /* F11 from windowed returns here */
static SDL_Texture *native[4], *smooth[4], *internal; /* top, bottom, main, party; internal: N x the layout */
static int party_on;                                   /* the party layer is shown (camera smoothing) */
static int main_w = SCREEN_W;                          /* the main screen's width (widescreen) */
static int main_guard;                                 /* hidden columns each side of it (camera smoothing) */
static float slide_x, slide_y;                         /* camera smoothing: where the view is shown from */
static int asked_margin, asked_guard;                  /* last margin requested from the renderer */
static int reupload;                                   /* the filter changed: upload the screens again */
static uint32_t scale2x_tmp[VIDEO_LINE_MAX * 2 * SCREEN_H * 2];
static uint32_t scaled[VIDEO_LINE_MAX * SMOOTH_SCALE * SCREEN_H * SMOOTH_SCALE];
static uint32_t corrected[VIDEO_LINE_MAX * SCREEN_H]; /* a screen with the Color setting applied */
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
    char line[128], key[32], val[32];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %31[a-z] = %31[a-z0-9]", key, val) != 2) continue;
        int v;
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
    if (SETTING(SET_FILTER) == FILTER_PIXEL && h >= ch) {
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

static void upload(int tex, const uint32_t *px) {
    int w = screen_width(tex);
    px = color_corrected(px, w);
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

/* Draws the shown screens into the internal target, then the target into the window. */
static void draw_screens(void) {
    int cw, ch;
    content_size(&cw, &ch);
    SDL_FRect dest = dest_rect();
    int n = internal_scale(&dest);
    float tw = 0, th = 0;
    if (internal) SDL_GetTextureSize(internal, &tw, &th);
    if ((int)tw != cw * n || (int)th != ch * n) {
        SDL_DestroyTexture(internal);
        internal = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, cw * n, ch * n);
        if (!internal) return;
    }
    SDL_SetRenderTarget(ren, internal);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_FRect shown[2];
    int nshown = 0;
    for (int screen = 0; screen < 2; screen++) {
        int x, y;
        if (!screen_slot(screen, &x, &y)) continue;
        int tex = screen == 0 && main_shown() ? 2 : screen; /* the main screen alone */
        int k = SETTING(SET_FILTER) == FILTER_SMOOTH ? SMOOTH_SCALE : 1;
        SDL_Texture *t = k > 1 ? smooth[tex] : native[tex];
        if (tex != 2 || !main_guard) {
            int w = screen_width(tex);
            SDL_FRect src = {0, 0, (float)w * k, (float)SCREEN_H * k};
            SDL_FRect r = {(float)x * n, (float)y * n, (float)w * n, (float)SCREEN_H * n};
            SDL_RenderTexture(ren, t, &src, &r);
            shown[nshown++] = r;
            continue;
        }
        /* The field with its hidden columns, slid (camera smoothing); rows beyond the picture repeat its edge. */
        float vw = (float)(main_w - 2 * main_guard), sx = ((float)main_guard + slide_x) * k, o = slide_y;
        shown[nshown++] = (SDL_FRect){(float)x * n, (float)y * n, vw * n, (float)SCREEN_H * n};
        float rows = SCREEN_H - SDL_fabsf(o), top = o < 0 ? -o : 0;
        SDL_RenderTexture(ren, t, &(SDL_FRect){sx, (o > 0 ? o : 0) * k, vw * k, rows * k},
                          &(SDL_FRect){(float)x * n, (top + y) * n, vw * n, rows * n});
        if (o != 0) {
            float edge = o > 0 ? SCREEN_H - 1 : 0, at = o > 0 ? rows : 0;
            SDL_RenderTexture(ren, t, &(SDL_FRect){sx, edge * k, vw * k, (float)k},
                              &(SDL_FRect){(float)x * n, (at + y) * n, vw * n, SDL_fabsf(o) * n});
        }
        if (party_on) { /* the sprites the camera follows: not slid */
            SDL_RenderTexture(ren, k > 1 ? smooth[3] : native[3],
                              &(SDL_FRect){(float)main_guard * k, 0, vw * k, (float)SCREEN_H * k},
                              &(SDL_FRect){(float)x * n, (float)y * n, vw * n, (float)SCREEN_H * n});
        }
    }
    SDL_Texture *grid = SETTING(SET_FILTER) == FILTER_LCD ? lcd_grid(n) : NULL;
    for (int i = 0; grid && i < nshown; i++) SDL_RenderTextureTiled(ren, grid, NULL, 1, &shown[i]);
    SDL_SetRenderTarget(ren, NULL);
    SDL_SetTextureScaleMode(internal, SETTING(SET_FILTER) == FILTER_PIXEL ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(ren, internal, NULL, &dest);
}

/* ---- interface ---- */

int display_init(void) {
    const char *base = SDL_GetBasePath();
    snprintf(ini_path, sizeof ini_path, "%seggoftime.ini", base ? base : "");
    load_settings();
    int cw, ch;
    content_size(&cw, &ch);
    if (!SDL_CreateWindowAndRenderer("Egg of Time", cw * WINDOW_SCALE, ch * WINDOW_SCALE, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE, &win, &ren)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return 0;
    }
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
                   float slide_dy) {
    /* A main screen of the margin asked for (plus guard) is the field: its guard columns are hidden. */
    main_guard = asked_guard && width == SCREEN_W + 2 * asked_margin ? asked_guard : 0;
    slide_x = main_guard ? slide_dx : 0;
    slide_y = main_guard ? slide_dy : 0;
    asked_guard = camera_smoothing() && EGG_SINGLE_SCREEN && main_shown() ? VIDEO_SLIDE_MAX : 0;
    asked_margin = SDL_min(wide_margin() + asked_guard, VIDEO_MARGIN_MAX);
    video_set_wide_margin(asked_margin);
    video_set_camera_smoothing(asked_guard != 0);
    main_w = width;
    party_on = main_guard && party;
    if (new_frame || reupload) {
        for (int i = 0; i < 3; i++) upload(i, screens[i]);
        if (party_on) upload(3, party);
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

const char *display_setting_label(int id, int v) {
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
