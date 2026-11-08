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
 * screen mode allows it (the field), also in the main window beside the second screen window; the main screen
 * then arrives wider than 256 columns. Other scenes, and Original, stay 4:3. Pixels keep their shape everywhere.
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
 * Second screen window: the other screen in a window of its own, for a second monitor (DESIGN.md); each
 * window is a View, with its own renderer and textures.
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
#include "minimap.h"
#include "movie.h"
#include "rt.h"
#include "shader.h"
#include "single_screen.h"
#include "system_menu.h"
#include "ui_font.h"
#include "ui_text.h"
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
static const char *const zoom_names[] = {"off", "90", "85", "77"};
static const char *const zoom_labels[] = {"Off", "90%", "85%", "77%"};
static const char *const zoom_ui_names[] = {"smaller", "original"};
static const char *const zoom_ui_labels[] = {"Smaller", "Original size"};
static const int zoom_totals[] = {0, 214, 226, VIDEO_ROWS_MAX}; /* rows of the picture (192 at the original scale; 214: 89.7%) */
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
static const char *const language_labels[] = {"English", "Fran\xC3\xA7" "ais"}; /* each in its own language */
static const char *const language_jp_names[] = {"japanese", "english"};
static const char *const language_jp_labels[] = {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "English"};
static const char *const ff_music_names[] = {"fast", "normal"};
static const char *const ff_music_labels[] = {"Sped up", "Normal speed"};
static const char *const ff_sound_names[] = {"all", "music"};
static const char *const ff_sound_labels[] = {"All", "Music only"};
static const char *const minimap_size_names[] = {"big", "medium", "small"};
static const char *const minimap_size_labels[] = {"Big", "Medium", "Small"};
static const float minimap_scales[] = {1.0f / 2, 3.0f / 8, 1.0f / 4}; /* of the panel on the bottom screen */
static const char *const off_names[] = {"off"};
static const char **shader_names; /* "off", then shader_key() of each listed shader */

static Setting settings[SET_COUNT] = {
    /* The DS's system language, which the game reads at power-on (boot.c): changes apply at the next start. */
    [SET_LANGUAGE] = {"language", "Language", 2, LANGUAGE_ENGLISH, language_names, language_labels},
    [SET_LANGUAGE_JP] = {"languagejp", "Language", 2, LANGUAGE_JP_JAPANESE, language_jp_names, language_jp_labels},
    [SET_WINDOW] = {"window", "Window mode", 3, WINDOW_BORDERLESS, window_names, window_labels},
    [SET_LAYOUT] = {"layout", "Layout", LAYOUT_COUNT, EGG_SINGLE_SCREEN ? LAYOUT_TOP : LAYOUT_STACKED, layout_names, layout_labels}, /* also F2 */
    /* The other DS screen in a window of its own, for a second monitor; the main window shows the top only. */
    [SET_SECOND] = {"secondscreen", "Second screen window", 2, 0, onoff_names, onoff_labels},
    [SET_RESOLUTION] = {"resolution", "Internal resolution", RES_MAX + 1, 0, res_names, NULL},
    [SET_ASPECT] = {"aspect", "Aspect ratio", 5, ASPECT_16_9, aspect_names, aspect_labels},
    /* Zoom out: more rows in the same place, in the field and the world map (video.c). */
    [SET_ZOOM] = {"zoomout", "Zoom out", 4, 0, zoom_names, zoom_labels},
    /* Zoomed out, the text boxes and the battle HUD: zoomed out too, or at their original size (draw_ui). */
    [SET_ZOOM_UI] = {"zoomtext", "Zoomed out text", 2, 0, zoom_ui_names, zoom_ui_labels},
    /* Widescreen in rooms narrower than the view: No leaves the room as it is, black beside it (video.c). */
    [SET_FILL] = {"fillsides", "Fill screen sides", 2, 0, yesno_names, yesno_labels},
    /* The bottom screen's map, small, over the main screen (single_screen.c). */
    [SET_MINIMAP] = {"minimap", "Minimap", 2, 1, onoff_names, onoff_labels},
    [SET_MINIMAP_SIZE] = {"minimapsize", "Minimap size", 3, 1, minimap_size_names, minimap_size_labels},
    [SET_MINIMAP_BG] = {"minimapbg", "Minimap background", 2, 0, onoff_names, onoff_labels}, /* off: the map alone (minimap.c) */
    [SET_FRAMERATE] = {"framerate", "Framerate", 6, 1, fps_names, fps_labels},
    [SET_VSYNC] = {"vsync", "V-Sync", 2, 1, onoff_names, onoff_labels},
    [SET_FILTER] = {"filter", "Graphics filter", 4, FILTER_SHARP, filter_names, filter_labels},
    [SET_SHADER] = {"shader", "Shader", 1, 0, off_names, NULL}, /* the list: list_shaders */
    [SET_COLOR] = {"color", "Color", 4, 1, color_names, color_labels},
    [SET_MODS] = {"mods", "Mods", 2, 1, onoff_names, onoff_labels}, /* replacement cutscenes and music (movie.c, music.c) */
    /* Fast forward (main.c, sched.c); Normal speed keeps the music sequences on the host's clock (snd.c). */
    [SET_FF_MUSIC] = {"ffmusic", "Fast forward music", 2, FF_MUSIC_FAST, ff_music_names, ff_music_labels},
    /* Music only mutes every channel but the BGM players' while fast forwarding (snd.c). */
    [SET_FF_SOUND] = {"ffsound", "Fast forward sound", 2, FF_SOUND_ALL, ff_sound_names, ff_sound_labels},
};
#define SETTING(id) (settings[id].value)

/* A window and what it draws with: the main window, or the second screen window (SDL textures belong to one
 * renderer). Screens: top, bottom, main, party; internal: N x the layout. */
typedef struct View {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *native[6], *smooth[6], *internal; /* top, bottom, main, party, still, UI (zoom out) */
    SDL_Texture *hd_tex[6]; /* the screens drawn from HD art (graphics mods) */
    int hd_k[6];            /* their scale; 0: not drawn this way */
    SDL_Texture *lcd_mask;  /* LCD filter: one pixel's grid at lcd_mask_n */
    int lcd_mask_n;
} View;

static View primary, secondary; /* secondary.win: NULL while Second screen window is off */
static int swapped;
static int last_full = WINDOW_BORDERLESS; /* F11 from windowed returns here */
static int force_windowed;                /* EGG_WINDOWED */
static SDL_Rect second_rect;              /* the second window's last windowed place (w 0: none yet) */
static Uint64 second_moved;               /* when it was moved or resized (saved half a second later) */
static SDL_Texture *shader_in[2], *shader_out[2];      /* per screen: its picture, then shaded at its window size */
static uint64_t shader_frames;
static char saved_shader[1024];                        /* eggoftime.ini's shader, until the list is known */
static int party_on;                                   /* the party layer is shown (camera smoothing) */
static int still_on;                                   /* the still layer is shown (camera smoothing) */
static int ui_on;                                      /* the UI layer is shown (zoom out) */
static int main_w = SCREEN_W;                          /* the main screen's width (widescreen) */
static int main_h = SCREEN_H;                          /* its rows (more when zoomed out) */
static int main_guard;                                 /* hidden columns each side of it (camera smoothing) */
static int main_guard_rows;                            /* hidden rows above and below it (camera smoothing) */
static float slide_x, slide_y;                         /* camera smoothing: where the view is shown from */
static int asked_margin, asked_guard;                  /* last margin requested from the renderer */
static int asked_zoom_rows, asked_zoom_margin;         /* and for the zoomed out scenes */
static int reupload;                                   /* the filter changed: upload the screens again */
static uint32_t scale2x_tmp[VIDEO_LINE_MAX * 2 * VIDEO_ROWS_MAX * 2];
static uint32_t scaled[VIDEO_LINE_MAX * SMOOTH_SCALE * VIDEO_ROWS_MAX * SMOOTH_SCALE];
static uint32_t corrected[VIDEO_LINE_MAX * VIDEO_ROWS_MAX]; /* a screen with the Color setting applied */
static uint32_t *hd_px;                               /* a screen being drawn from HD art */
static const VideoHdFrame *hd_frame;                   /* the sources of the screens being uploaded */
static uint8_t gamma_lut[256];
static int gamma_lut_for = -1;                         /* the Color value gamma_lut was built for */
static char ini_path[1024];
static char capture_path[1024]; /* save the next presented window here (bring-up aid) */
/* The message box (display_message): drawn into message_px with the runtime's pixel font, which has the menu's
 * languages. */
#define MESSAGE_W 400
#define MESSAGE_H 16
static char message[128];
static uint32_t message_px[MESSAGE_W * MESSAGE_H];
static int message_w, message_dirty;
static Uint64 message_until;
static SDL_Texture *message_tex;
/* The popup (display_popup): drawn into popup_px with the runtime's pixel font, shown scaled like the message box. */
#define POPUP_W 200
#define POPUP_H 41
#define POPUP_SLIDE_MS 250
static uint32_t popup_px[POPUP_W * POPUP_H];
static int popup_w, popup_dirty;
static Uint64 popup_from, popup_until;
static SDL_Texture *popup_tex;
static SDL_Texture *minimap_tex; /* the bottom screen's map panel (main window) */

static void message_changed(unsigned ms) {
    message_w = SDL_min(ui_text_width(message) + 8, MESSAGE_W);
    for (int i = 0; i < message_w * MESSAGE_H; i++) message_px[i] = 0xB4000000u;
    ui_draw_text_in(message_px, message_w, message, 4, 4, 0xFFFFFFFFu);
    message_dirty = 1;
    message_until = SDL_GetTicks() + ms;
}

static void show_message(const char *fmt, const char *arg) {
    snprintf(message, sizeof message, fmt, arg);
    message_changed(MESSAGE_MS);
}

void display_text_input(int on) {
    if (on) SDL_StartTextInput(primary.win);
    else SDL_StopTextInput(primary.win);
}

void display_message(const char *text, unsigned ms) {
    snprintf(message, sizeof message, "%s", text);
    message_changed(ms);
}

/* The last change, scaled with the window, top left on a dark box. */
static void draw_message(void) {
    if (!message[0] || SDL_GetTicks() >= message_until) return;
    int ww, wh;
    SDL_Renderer *ren = primary.ren;
    if (!message_tex) {
        message_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, MESSAGE_W, MESSAGE_H);
        if (!message_tex) return;
        SDL_SetTextureBlendMode(message_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(message_tex, SDL_SCALEMODE_NEAREST);
    }
    if (message_dirty) {
        SDL_UpdateTexture(message_tex, &(SDL_Rect){0, 0, message_w, MESSAGE_H}, message_px, message_w * 4);
        message_dirty = 0;
    }
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    float k = SDL_max(2.0f, SDL_floorf(wh / 270.0f));
    SDL_RenderTexture(ren, message_tex, &(SDL_FRect){0, 0, (float)message_w, MESSAGE_H},
                      &(SDL_FRect){4 * k, 4 * k, message_w * k, MESSAGE_H * k});
}

void display_popup(const uint32_t *image, int size, const char *line1, const char *line2, unsigned ms) {
    enum { PAD = 6, GAP = 6 };
    size = image ? SDL_min(size, POPUP_H - 2 * PAD) : 0;
    int tx = PAD + (size ? size + GAP : 0), tw = POPUP_W - tx - PAD;
    char a[128], b[128];
    snprintf(a, sizeof a, "%s", line1);
    snprintf(b, sizeof b, "%s", line2);
    for (char *t = a; ui_text_width(t) > tw;) t[strlen(t) - 1] = 0; /* cut to the widest popup */
    for (char *t = b; ui_text_width(t) > tw;) t[strlen(t) - 1] = 0;
    popup_w = tx + SDL_max(ui_text_width(a), ui_text_width(b)) + PAD;
    for (int y = 0; y < POPUP_H; y++) { /* a dark translucent panel with a light rim and a teal line at the top */
        for (int x = 0; x < popup_w; x++) {
            int rim = x == 0 || y == 0 || x == popup_w - 1 || y == POPUP_H - 1;
            popup_px[y * popup_w + x] = y < 2 ? 0xFFADC618u : rim ? 0xF0707070u : 0xE0201C18u;
        }
    }
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            uint32_t c = image[y * size + x], *d = &popup_px[(PAD + 1 + y) * popup_w + PAD + x], al = c >> 24, out = 0;
            for (int k = 0; k < 32; k += 8) out |= (((c >> k & 0xFF) * al + (*d >> k & 0xFF) * (255 - al)) / 255) << k;
            *d = out | 0xFF000000u;
        }
    }
    ui_draw_text_in(popup_px, popup_w, a, tx, 10, 0xFFFFFFFFu);
    ui_draw_text_in(popup_px, popup_w, b, tx, 23, 0xFFB0B0B0u);
    popup_from = SDL_GetTicks();
    popup_until = popup_from + ms;
    popup_dirty = 1;
}

/* The popup, top right, scaled as the message box is, sliding in from the right edge and back out. */
static void draw_popup(void) {
    Uint64 now = SDL_GetTicks();
    if (!popup_w || now >= popup_until) return;
    SDL_Renderer *ren = primary.ren;
    if (!popup_tex) {
        popup_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, POPUP_W, POPUP_H);
        if (!popup_tex) return;
        SDL_SetTextureBlendMode(popup_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(popup_tex, SDL_SCALEMODE_NEAREST);
    }
    if (popup_dirty) {
        SDL_UpdateTexture(popup_tex, &(SDL_Rect){0, 0, popup_w, POPUP_H}, popup_px, popup_w * 4);
        popup_dirty = 0;
    }
    int ww, wh;
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    float k = SDL_max(2.0f, SDL_floorf(wh / 270.0f)), w = popup_w * k;
    Uint64 edge = SDL_min(now - popup_from, popup_until - now); /* ms from the nearer end */
    float in = edge >= POPUP_SLIDE_MS ? 1.0f : (float)edge / POPUP_SLIDE_MS;
    in = 1 - (1 - in) * (1 - in); /* eased */
    SDL_FRect dst = {ww - (4 * k + w) * in, 4 * k, w, POPUP_H * k};
    SDL_RenderTexture(ren, popup_tex, &(SDL_FRect){0, 0, (float)popup_w, POPUP_H}, &dst);
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
        if (!strcmp(key, "swap")) swapped = val[0] == '1';
        else if (!strcmp(key, "secondplace")) {
            SDL_Rect r;
            if (sscanf(val, "%d , %d , %d , %d", &r.x, &r.y, &r.w, &r.h) == 4 && r.w > 0 && r.h > 0) second_rect = r;
        }
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
    fprintf(f, "swap = %d\n", swapped);
    if (second_rect.w) fprintf(f, "secondplace = %d, %d, %d, %d\n", second_rect.x, second_rect.y, second_rect.w, second_rect.h);
    for (int i = 0; i < SET_COUNT; i++) fprintf(f, "%s = %s\n", settings[i].key, settings[i].names[settings[i].value]);
    fclose(f);
}

/* ---- geometry ---- */

/* The view's layout: with the second screen window, each window shows one screen, the second window the one the
 * main window does not. */
static int view_layout(const View *v) { return v == &primary && !secondary.win ? SETTING(SET_LAYOUT) : LAYOUT_TOP; }
static int view_swapped(const View *v) { return swapped ^ (v == &secondary); }

/* Top-only shows the main screen. */
static int main_shown(const View *v) { return view_layout(v) == LAYOUT_TOP && !view_swapped(v); }

static void content_size(const View *v, int *w, int *h) {
    int l = view_layout(v);
    *w = l == LAYOUT_SIDE ? SCREEN_W * 2 : main_shown(v) ? main_w - 2 * main_guard : SCREEN_W;
    *h = l == LAYOUT_STACKED ? SCREEN_H * 2 : main_shown(v) ? main_h - 2 * main_guard_rows : SCREEN_H;
}

/* The view runs the chosen shader (the main window only: the second window uses the graphics filter). */
static int view_shader(const View *v) { return v == &primary && SETTING(SET_SHADER) != 0; }

/* Camera smoothing (framerates above 60): between game frames, the field picture is shown slid back toward
 * where the camera was, by the fraction of the frame still to come (main.c), so scrolling moves on every
 * window frame and at sub-pixel steps of the internal resolution. Background and sprites move together, nothing
 * is blended. The renderer draws VIDEO_SLIDE_MAX hidden columns each side for it and VIDEO_SLIDE_ROWS hidden rows
 * above and below; a vertical slide beyond those (the camera's few big steps) repeats the edge row. */
static int camera_smoothing(void) { return fps_values[SETTING(SET_FRAMERATE)] != 60 && fps_values[SETTING(SET_FRAMERATE)] != 30; }

/* The main window shows the main screen of a layout that can be wide: single screen mode's, or the top screen
 * beside the second window (its field is as wide as in single screen mode). */
static int wide_view(void) { return (ss_enabled() || secondary.win) && main_shown(&primary); }

#define WIDE_MARGIN_MAX (96 + VIDEO_SLIDE_MAX) /* 21:9 at 192 rows, with camera smoothing's hidden columns */

/* The chosen aspect ratio. */
static float view_ratio(void) {
    float ratio = aspect_ratios[SETTING(SET_ASPECT)];
    if (ratio == 0) {
        int ww, wh;
        SDL_GetRenderOutputSize(primary.ren, &ww, &wh);
        ratio = wh > 0 ? (float)ww / wh : 4.0f / 3;
    }
    return ratio;
}

/* Columns beyond each side of the top screen for the chosen aspect ratio (only the main screen shows them). */
static int wide_margin(void) {
    if (!wide_view()) return 0;
    return SDL_clamp(((int)SDL_lroundf(SCREEN_H * view_ratio()) - SCREEN_W) / 2, 0, WIDE_MARGIN_MAX);
}

/* Zoom out: rows above and below the top screen and the columns each side, guard columns and rows included: the same
 * shape as the aspect ratio, as far as the BG maps' 512 columns go (a wider ratio then zooms out less). The BG maps' rows
 * have no room for guard rows beyond VIDEO_ROWS_MAX, so with those the most zoomed out view shows fewer rows (240, 80%).
 * Not zoomed out, the guard rows alone. */
static void zoom_view(int guard, int *rows, int *margin) {
    *rows = *margin = 0;
    if (!wide_view()) return;
    int guard_rows = guard ? VIDEO_SLIDE_ROWS : 0;
    int total = zoom_totals[SETTING(SET_ZOOM)], most = SCREEN_W + 2 * (VIDEO_MARGIN_MAX - guard);
    float ratio = view_ratio();
    if (total * ratio > most) total = (int)((float)most / ratio);
    int shown = total > SCREEN_H + 1 ? SDL_min((total - SCREEN_H) / 2, (VIDEO_ROWS_MAX - SCREEN_H) / 2 - guard_rows) : 0;
    *rows = shown + guard_rows;
    if (!*rows) return;
    *margin = shown ? SDL_clamp(((int)SDL_lroundf((float)(SCREEN_H + 2 * shown) * ratio) - SCREEN_W) / 2 + guard, 0, VIDEO_MARGIN_MAX)
                    : SDL_min(wide_margin() + guard, WIDE_MARGIN_MAX);
}

/* Where a screen goes in the view's layout, in native pixels; screen 0 = top, 1 = bottom. 0 when it is not
 * shown there. */
static int screen_slot(const View *v, int screen, int *x, int *y) {
    int slot = screen ^ view_swapped(v), l = view_layout(v); /* position: 0 first (top/left), 1 second */
    if (!v->win || (l == LAYOUT_TOP && slot != 0)) return 0;
    *x = slot && l == LAYOUT_SIDE ? SCREEN_W : 0;
    *y = slot && l == LAYOUT_STACKED ? SCREEN_H : 0;
    return 1;
}

/* The window rectangle the view's layout is shown in. */
static SDL_FRect dest_rect(const View *v) {
    int cw, ch, ww, wh;
    content_size(v, &cw, &ch);
    SDL_GetRenderOutputSize(v->ren, &ww, &wh);
    float h = SDL_min((float)wh, (float)ww * ch / cw);
    if (SETTING(SET_FILTER) == FILTER_PIXEL && !view_shader(v) && h >= ch) {
        h = SDL_floorf(h / ch) * ch; /* integer scaling */
    }
    float w = h * cw / ch;
    return (SDL_FRect){SDL_floorf((ww - w) / 2), SDL_floorf((wh - h) / 2), w, h};
}

/* Internal resolution: N times the layout's native size. */
static int internal_scale(const View *v, const SDL_FRect *dest) {
    if (SETTING(SET_RESOLUTION)) return SETTING(SET_RESOLUTION);
    int cw, ch;
    content_size(v, &cw, &ch);
    return SDL_clamp((int)SDL_ceilf(dest->h / ch), 1, RES_MAX);
}

static void resize_window(void) {
    int cw, ch;
    content_size(&primary, &cw, &ch);
    if (!(SDL_GetWindowFlags(primary.win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        if (main_shown(&primary)) cw = cw * SCREEN_H / ch, ch = SCREEN_H; /* the same window zoomed out */
        SDL_SetWindowSize(primary.win, cw * WINDOW_SCALE, ch * WINDOW_SCALE);
    }
}

static void apply_window_mode(SDL_Window *win, int mode) {
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
}

/* The second window follows Window mode on a monitor of its own; on the main window's, it stays windowed. */
static void apply_second_mode(void) {
    if (!secondary.win) return;
    int mode = force_windowed ? WINDOW_WINDOWED : SETTING(SET_WINDOW);
    if (SDL_GetDisplayForWindow(secondary.win) == SDL_GetDisplayForWindow(primary.win)) mode = WINDOW_WINDOWED;
    apply_window_mode(secondary.win, mode);
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
static int ui_tall(void) { return !SETTING(SET_ZOOM_UI); } /* the UI layer as tall as the picture (video_set_ui_edges) */
static int screen_height(int tex) { return tex >= 2 && (tex < 5 || ui_tall()) ? main_h : SCREEN_H; }

/* px with the Color setting applied (px itself for Original). */
static const uint32_t *color_corrected(const uint32_t *px, int w, int h) {
    int v = SETTING(SET_COLOR);
    if (!v) return px;
    if (gamma_lut_for != v) {
        for (int i = 0; i < 256; i++) gamma_lut[i] = (uint8_t)SDL_lroundf(255 * SDL_powf(i / 255.0f, color_exponents[v]));
        gamma_lut_for = v;
    }
    for (int i = 0; i < w * h; i++) {
        uint32_t c = px[i];
        corrected[i] = (c & 0xFF000000u) | (uint32_t)gamma_lut[(c >> 16) & 0xFF] << 16 |
                       (uint32_t)gamma_lut[(c >> 8) & 0xFF] << 8 | gamma_lut[c & 0xFF];
    }
    return corrected;
}

/* ---- minimap ---- */

/* The setting can apply to the view: it shows the main screen of single screen mode. */
static int minimap_allowed(const View *v) { return v == &primary && ss_enabled() && main_shown(v); }

#define MINIMAP_MARGIN 4

static float minimap_scale(void) { return minimap_scales[SETTING(SET_MINIMAP_SIZE)]; }

/* A text box stays uncovered: the map's last row must be above its first. */
static int minimap_shown(const View *v) {
    return SETTING(SET_MINIMAP) && minimap_allowed(v) && !sysmenu_is_open() &&
           ss_minimap_rows() >= MINIMAP_MARGIN + (int)SDL_ceilf(SS_MAP_H * minimap_scale());
}

/* The map alone is asked for: the renderer tells which pixels are the parchment. */
static int minimap_cut(void) { return SETTING(SET_MINIMAP) && !SETTING(SET_MINIMAP_BG) && minimap_allowed(&primary); }

/* The map panel of the bottom screen, with the Color setting, its rounded corners (black beside it) transparent;
 * with Minimap background off, its parchment too. */
static void upload_minimap(void) {
    if (!minimap_tex) {
        minimap_tex = SDL_CreateTexture(primary.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, SS_MAP_W, SS_MAP_H);
        if (!minimap_tex) return;
        SDL_SetTextureBlendMode(minimap_tex, SDL_BLENDMODE_BLEND);
    }
    static uint32_t px[SS_MAP_W * SS_MAP_H];
    const uint32_t *in = color_corrected(&video_bottom_top()->rgba[0][0], SCREEN_W, SCREEN_H);
    for (int y = 0; y < SS_MAP_H; y++) {
        const uint32_t *src = &in[(SS_MAP_Y + y) * SCREEN_W + SS_MAP_X];
        uint32_t *dst = &px[y * SS_MAP_W];
        int x0 = 0, x1 = SS_MAP_W;
        while (x0 < x1 && src[x0] == 0xFF000000u) x0++;
        while (x1 > x0 && src[x1 - 1] == 0xFF000000u) x1--;
        for (int x = 0; x < SS_MAP_W; x++) dst[x] = x >= x0 && x < x1 ? src[x] : 0;
    }
    static uint8_t keep[SS_MAP_W * SS_MAP_H];
    if (minimap_cut() && minimap_cutout(video_bottom_top(), keep)) {
        for (int i = 0; i < SS_MAP_W * SS_MAP_H; i++) px[i] = keep[i] ? px[i] : 0;
    }
    SDL_UpdateTexture(minimap_tex, NULL, px, SS_MAP_W * 4);
}

/* Into the internal target (n x native), at the view's top left. Nearest at a whole multiple of the panel's pixels;
 * otherwise nearest would drop or double some of them. */
static void draw_minimap(SDL_Renderer *ren, float n) {
    if (!minimap_tex) return;
    float k = minimap_scale() * n, at = MINIMAP_MARGIN * n;
    SDL_SetTextureScaleMode(minimap_tex, k >= 1 && k == SDL_floorf(k) ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(ren, minimap_tex, NULL, &(SDL_FRect){at, at, SDL_roundf(SS_MAP_W * k), SDL_roundf(SS_MAP_H * k)});
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
        hd_jobs[i].y0 = job->y1 * i / bands, hd_jobs[i].y1 = job->y1 * (i + 1) / bands;
    }
    for (int i = 0; i < hd_workers; i++) SDL_SignalSemaphore(hd_go[i]);
    compose_rows(&hd_jobs[hd_workers]);
    for (int i = 0; i < hd_workers; i++) SDL_WaitSemaphore(hd_done);
}

/* The screen's sources, when the view draws it from HD art now (not through a shader, and Mods on). */
static const VideoHd *hd_source(const View *v, int tex) {
    if (!hd_frame || !hd_frame->on || view_shader(v) || !SETTING(SET_MODS) || !gfxmod_hd_scale()) return NULL;
    return tex == 0 ? hd_frame->top : tex == 1 ? hd_frame->bottom : tex == 2 ? hd_frame->main
           : tex == 3 ? hd_frame->party : hd_frame->still;
}

/* Draws and uploads screen `tex` (its pixels px with the Color setting applied) from HD art at k times its size,
 * to the view; 0 if it cannot. */
static int upload_hd(View *v, int tex, const uint32_t *px, const VideoHd *src, int k) {
    int w = screen_width(tex), h = screen_height(tex), ow = w * k, oh = h * k;
    float tw = 0, th = 0;
    SDL_Texture **hd_tex = v->hd_tex;
    if (hd_tex[tex]) SDL_GetTextureSize(hd_tex[tex], &tw, &th);
    int full_w = (tex >= 2 ? VIDEO_LINE_MAX : SCREEN_W) * k, full_h = (tex >= 2 ? VIDEO_ROWS_MAX : SCREEN_H) * k;
    if ((int)tw != full_w || (int)th != full_h) {
        SDL_DestroyTexture(hd_tex[tex]);
        hd_tex[tex] = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, full_w, full_h);
        if (!hd_tex[tex]) return 0;
        SDL_SetTextureScaleMode(hd_tex[tex], SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(hd_tex[tex], tex >= 2 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE); /* see create_textures */
    }
    static size_t have;
    size_t need = (size_t)VIDEO_LINE_MAX * VIDEO_ROWS_MAX * k * k;
    if (have < need) {
        SDL_free(hd_px);
        hd_px = SDL_malloc(need * 4);
        have = hd_px ? need : 0;
        if (!hd_px) return 0;
    }
    compose_hd(&(HdJob){px, src, hd_frame->palette, SETTING(SET_COLOR) ? gamma_lut : NULL, hd_px, w, k, 0, h});
    SDL_UpdateTexture(hd_tex[tex], &(SDL_Rect){0, 0, ow, oh}, hd_px, ow * 4);
    return 1;
}

/* Uploads screen `tex` to the view; shown: the screen is in its layout (only those are drawn from HD art). */
static void upload(View *v, int tex, const uint32_t *px, int shown) {
    int w = screen_width(tex), h = screen_height(tex);
    px = color_corrected(px, w, h);
    const VideoHd *src = shown ? hd_source(v, tex) : NULL;
    SDL_FRect dest = dest_rect(v);
    int k = src ? SDL_min(gfxmod_hd_scale(), internal_scale(v, &dest)) : 0;
    v->hd_k[tex] = k > 1 && upload_hd(v, tex, px, src, k) ? k : 0;
    SDL_UpdateTexture(v->native[tex], &(SDL_Rect){0, 0, w, h}, px, w * 4);
    if (SETTING(SET_FILTER) == FILTER_SMOOTH) {
        scale2x(px, w, h, scale2x_tmp);
        scale2x(scale2x_tmp, w * 2, h * 2, scaled);
        SDL_UpdateTexture(v->smooth[tex], &(SDL_Rect){0, 0, w * SMOOTH_SCALE, h * SMOOTH_SCALE}, scaled,
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

static SDL_Texture *lcd_grid(View *v, int n) {
    if (v->lcd_mask && v->lcd_mask_n == n) return v->lcd_mask;
    SDL_DestroyTexture(v->lcd_mask);
    SDL_Texture *lcd_mask = v->lcd_mask = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, n, n);
    v->lcd_mask_n = n;
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

/* *t as a w x h render target of ren, made again when its size differs. */
static SDL_Texture *sized_target(SDL_Renderer *ren, SDL_Texture **t, int w, int h) {
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
    SDL_Renderer *ren = primary.ren;
    float kx = dest->w / cw, ky = dest->h / ch;
    SDL_FRect at[2];
    for (int i = 0; i < nshown; i++) {
        const SDL_FRect *r = &shown[i];
        at[i] = (SDL_FRect){SDL_roundf(dest->x + r->x * kx), SDL_roundf(dest->y + r->y * ky), SDL_roundf(r->w * kx),
                            SDL_roundf(r->h * ky)};
        int s = screens[i];
        if (!sized_target(ren, &shader_in[s], (int)r->w, (int)r->h) ||
            !sized_target(ren, &shader_out[s], (int)at[i].w, (int)at[i].h)) {
            return 0;
        }
        SDL_SetRenderTarget(ren, shader_in[s]);
        SDL_RenderTexture(ren, primary.internal, r, NULL);
        SDL_SetRenderTarget(ren, NULL);
        if (!shader_run(s, SETTING(SET_SHADER) - 1, shader_in[s], shader_out[s], shader_frames)) return 0;
    }
    for (int i = 0; i < nshown; i++) SDL_RenderTexture(ren, shader_out[screens[i]], NULL, &at[i]);
    shader_frames++;
    return 1;
}

/* Zoomed out, the UI layer over the main screen (at x, y, cw x ch native pixels), not slid with camera smoothing.
 * Smaller: at the picture's scale, on the screen's own 192 rows in its middle (on the picture's pixels, so its text
 * scales as evenly). Original size: at the scale and in the place it has when not zoomed out, its 192 rows over the
 * whole height (between the picture's pixels: the text scales less evenly, the more so at 77%). */
static void draw_ui(const View *v, int x, int y, int n, int cw, int ch) {
    if (!ui_on) return;
    int k = !view_shader(v) && SETTING(SET_FILTER) == FILTER_SMOOTH ? SMOOTH_SCALE : 1;
    SDL_Texture *t = k > 1 ? v->smooth[5] : v->native[5];
    if (SETTING(SET_ZOOM_UI)) {
        float columns = (float)cw * SCREEN_H / ch;
        SDL_RenderTexture(v->ren, t, &(SDL_FRect){((float)main_w - columns) / 2 * k, 0, columns * k, (float)SCREEN_H * k},
                          &(SDL_FRect){(float)x * n, (float)y * n, (float)cw * n, (float)ch * n});
        return;
    }
    if (ui_tall()) { /* the picture's rows, its boxes at the view's top and bottom (video.c ui_to_edges) */
        SDL_RenderTexture(v->ren, t, &(SDL_FRect){(float)main_guard * k, (float)main_guard_rows * k, (float)cw * k, (float)ch * k},
                          &(SDL_FRect){(float)x * n, (float)y * n, (float)cw * n, (float)ch * n});
        return;
    }
    float top = (float)(ch - SCREEN_H) / 2;
    SDL_RenderTexture(v->ren, t, &(SDL_FRect){(float)main_guard * k, 0, (float)cw * k, (float)SCREEN_H * k},
                      &(SDL_FRect){(float)x * n, ((float)y + top) * n, (float)cw * n, (float)SCREEN_H * n});
}

/* Draws the view's shown screens into its internal target, then the target into its window (through the shader
 * if one is chosen: then the target is the native size). */
static void draw_screens(View *v) {
    SDL_Renderer *ren = v->ren;
    SDL_Texture **native = v->native, **smooth = v->smooth, **hd_tex = v->hd_tex;
    const int *hd_k = v->hd_k;
    int cw, ch;
    content_size(v, &cw, &ch);
    SDL_FRect dest = dest_rect(v);
    int shader = view_shader(v);
    int n = shader ? 1 : internal_scale(v, &dest);
    SDL_Texture *internal = sized_target(ren, &v->internal, cw * n, ch * n);
    if (!internal) return;
    SDL_SetRenderTarget(ren, internal);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_FRect shown[2];
    int screens[2], nshown = 0;
    for (int screen = 0; screen < 2; screen++) {
        int x, y;
        if (!screen_slot(v, screen, &x, &y)) continue;
        int tex = screen == 0 && main_shown(v) ? 2 : screen; /* the main screen alone */
        int k = hd_k[tex] ? hd_k[tex] : !shader && SETTING(SET_FILTER) == FILTER_SMOOTH ? SMOOTH_SCALE : 1;
        SDL_Texture *t = hd_k[tex] ? hd_tex[tex] : k > 1 ? smooth[tex] : native[tex];
        if (tex != 2 || !main_guard) {
            int w = screen_width(tex), h = screen_height(tex);
            SDL_FRect src = {0, 0, (float)w * k, (float)h * k};
            SDL_FRect r = {(float)x * n, (float)y * n, (float)w * n, (float)h * n};
            SDL_RenderTexture(ren, t, &src, &r);
            screens[nshown] = screen;
            shown[nshown++] = r;
            if (tex == 2) draw_ui(v, x, y, n, cw, ch);
            continue;
        }
        /* The field with its hidden columns and rows, slid (camera smoothing); rows beyond the picture repeat its edge. */
        float vw = (float)(main_w - 2 * main_guard), sx = ((float)main_guard + slide_x) * k, vh = (float)ch, gy = (float)main_guard_rows;
        float hidden = SDL_clamp(slide_y, -gy, gy), o = slide_y - hidden; /* the slide from the hidden rows, and beyond them */
        screens[nshown] = screen;
        shown[nshown++] = (SDL_FRect){(float)x * n, (float)y * n, vw * n, vh * n};
        if (still_on) { /* what holds still on screen (the prison bridge's sky): under the slid picture, not slid */
            SDL_RenderTexture(ren, hd_k[2] ? (hd_k[4] == k ? hd_tex[4] : native[4]) : k > 1 ? smooth[4] : native[4],
                              &(SDL_FRect){(float)main_guard * k, gy * k, vw * k, vh * k},
                              &(SDL_FRect){(float)x * n, (float)y * n, vw * n, vh * n});
        }
        float rows = vh - SDL_fabsf(o), top = o < 0 ? -o : 0;
        SDL_RenderTexture(ren, t, &(SDL_FRect){sx, (gy + hidden + (o > 0 ? o : 0)) * k, vw * k, rows * k},
                          &(SDL_FRect){(float)x * n, (top + y) * n, vw * n, rows * n});
        if (o != 0) {
            float edge = o > 0 ? main_h - 1 : 0, at = o > 0 ? rows : 0;
            SDL_RenderTexture(ren, t, &(SDL_FRect){sx, edge * k, vw * k, (float)k},
                              &(SDL_FRect){(float)x * n, (at + y) * n, vw * n, SDL_fabsf(o) * n});
        }
        if (party_on) { /* the sprites the camera follows: not slid (drawn at the main screen's scale) */
            SDL_RenderTexture(ren, hd_k[2] ? (hd_k[3] == k ? hd_tex[3] : native[3]) : k > 1 ? smooth[3] : native[3],
                              &(SDL_FRect){(float)main_guard * k, gy * k, vw * k, vh * k},
                              &(SDL_FRect){(float)x * n, (float)y * n, vw * n, vh * n});
        }
        draw_ui(v, x, y, n, cw, ch);
    }
    if (minimap_shown(v)) draw_minimap(ren, (float)n * (main_shown(v) ? (float)ch / SCREEN_H : 1));
    SDL_Texture *grid = !shader && SETTING(SET_FILTER) == FILTER_LCD ? lcd_grid(v, n) : NULL;
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

/* The view's screen textures (the first count of top, bottom, main, party, still, UI). The main screen is opaque
 * except where the still layer shows through it; the party, still and UI layers are transparent elsewhere. */
static int create_textures(View *v, int count) {
    for (int i = 0; i < count; i++) {
        int w = i >= 2 ? VIDEO_LINE_MAX : SCREEN_W, h = i >= 2 ? VIDEO_ROWS_MAX : SCREEN_H;
        v->native[i] = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        v->smooth[i] = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                         w * SMOOTH_SCALE, h * SMOOTH_SCALE);
        if (!v->native[i] || !v->smooth[i]) return 0;
        SDL_SetTextureScaleMode(v->native[i], SDL_SCALEMODE_NEAREST);
        SDL_SetTextureScaleMode(v->smooth[i], SDL_SCALEMODE_LINEAR);
    }
    for (int i = 2; i < count; i++) {
        SDL_SetTextureBlendMode(v->native[i], SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(v->smooth[i], SDL_BLENDMODE_BLEND);
    }
    return 1;
}

/* ---- second screen window ---- */

/* r is mostly on a monitor (a saved place whose monitor is gone is not). */
static int on_a_display(const SDL_Rect *r) {
    SDL_Rect b, in;
    SDL_DisplayID d = SDL_GetDisplayForRect(r);
    return d && SDL_GetDisplayUsableBounds(d, &b) && SDL_GetRectIntersection(r, &b, &in) && in.w * 2 >= r->w &&
           in.h * 2 >= r->h;
}

/* First time: centred on a monitor other than the main window's; with none, beside the main window where it
 * fits. */
static void default_second_place(SDL_Rect *r) {
    SDL_DisplayID mine = SDL_GetDisplayForWindow(primary.win), other = 0;
    int n = 0;
    SDL_DisplayID *ds = SDL_GetDisplays(&n);
    for (int i = 0; ds && i < n && !other; i++) {
        if (ds[i] != mine) other = ds[i];
    }
    SDL_free(ds);
    SDL_Rect b;
    if (!SDL_GetDisplayUsableBounds(other ? other : mine, &b)) b = (SDL_Rect){0, 0, 1280, 720};
    r->w = SDL_min(SCREEN_W * WINDOW_SCALE, b.w);
    r->h = SDL_min(SCREEN_H * WINDOW_SCALE, b.h);
    r->x = b.x + (b.w - r->w) / 2;
    r->y = b.y + (b.h - r->h) / 2;
    int mx, my, mw, mh;
    if (other || !SDL_GetWindowPosition(primary.win, &mx, &my) || !SDL_GetWindowSize(primary.win, &mw, &mh)) return;
    if (mx + mw + r->w <= b.x + b.w) r->x = mx + mw;
    else if (mx - r->w >= b.x) r->x = mx - r->w;
    r->y = SDL_clamp(my, b.y, b.y + b.h - r->h);
}

/* Keeps the second window's place, while it is a plain window. */
static void remember_second_place(void) {
    if (!secondary.win ||
        (SDL_GetWindowFlags(secondary.win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
        return;
    }
    SDL_Rect r;
    if (SDL_GetWindowPosition(secondary.win, &r.x, &r.y) && SDL_GetWindowSize(secondary.win, &r.w, &r.h)) second_rect = r;
}

static void close_second(void) {
    remember_second_place();
    SDL_DestroyRenderer(secondary.ren); /* and its textures */
    SDL_DestroyWindow(secondary.win);
    secondary = (View){0};
    SDL_ResetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
    ss_set_enabled(1);
}

/* Opens the second window where it was last (or default_second_place), in Window mode on a monitor of its own.
 * 0 if it cannot. */
static int open_second(void) {
    /* Exclusive fullscreen would otherwise minimize the main window whenever the other one takes the focus. */
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_Rect r = second_rect;
    if (!r.w || !on_a_display(&r)) default_second_place(&r);
    secondary.win = SDL_CreateWindow("Egg of Time - Second screen", r.w, r.h,
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!secondary.win || !(secondary.ren = SDL_CreateRenderer(secondary.win, NULL)) || !create_textures(&secondary, 2)) {
        fprintf(stderr, "Second screen window: %s\n", SDL_GetError());
        close_second();
        return 0;
    }
    SDL_SetWindowPosition(secondary.win, r.x, r.y);
    SDL_ShowWindow(secondary.win);
    apply_second_mode();
    ss_set_enabled(0); /* both DS screens as they are, not laid out for one */
    return 1;
}

void display_event(const SDL_Event *e) {
    if (e->type < SDL_EVENT_WINDOW_FIRST || e->type > SDL_EVENT_WINDOW_LAST || !secondary.win) return;
    int second = e->window.windowID == SDL_GetWindowID(secondary.win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (second) {
            display_setting_set(SET_SECOND, 0);
        } else { /* SDL quits by itself only when the last window closes */
            SDL_PushEvent(&(SDL_Event){.type = SDL_EVENT_QUIT});
        }
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: apply_second_mode(); break;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
        if (second) second_moved = SDL_GetTicks();
        break;
    }
}

int display_init(void) {
    snprintf(ini_path, sizeof ini_path, "%seggoftime.ini", rt_data_dir());
    load_settings();
    shader_scan();
    force_windowed = SDL_getenv("EGG_WINDOWED") != NULL;
    int cw, ch;
    content_size(&primary, &cw, &ch);
    SDL_Window *win = primary.win = SDL_CreateWindow("Egg of Time", cw * WINDOW_SCALE, ch * WINDOW_SCALE,
                           SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | (shader_count() ? SDL_WINDOW_OPENGL : 0));
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    /* Shaders run on the OpenGL renderer; without them (or OpenGL 3.3), SDL's default renderer. */
    SDL_Renderer *ren = shader_count() ? SDL_CreateRenderer(win, "opengl") : NULL;
    if (ren && !shader_init(ren)) {
        SDL_DestroyRenderer(ren);
        ren = NULL;
    }
    if (!ren && !(ren = SDL_CreateRenderer(win, NULL))) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 0;
    }
    primary.ren = ren;
    list_shaders();
    SDL_SetRenderVSync(ren, SETTING(SET_VSYNC));
    if (SETTING(SET_WINDOW) != WINDOW_WINDOWED) last_full = SETTING(SET_WINDOW);
    if (!force_windowed) apply_window_mode(win, SETTING(SET_WINDOW));
    if (!create_textures(&primary, 6)) return 0;
    if (SETTING(SET_SECOND)) {
        if (!open_second()) SETTING(SET_SECOND) = 0;
        resize_window();
    }
    return 1;
}

void display_frame(const uint32_t *const screens[3], const uint32_t *party, const uint32_t *still, const uint32_t *ui,
                   int width, int height, int new_frame, float slide_dx, float slide_dy, const VideoHdFrame *hd) {
    /* A main screen of the margin asked for (plus guard) is the field: its guard columns are hidden. */
    main_guard = asked_guard && (width == SCREEN_W + 2 * asked_margin || (asked_zoom_rows && width == SCREEN_W + 2 * asked_zoom_margin)) ? asked_guard : 0;
    main_guard_rows = main_guard && asked_zoom_rows && height == SCREEN_H + 2 * asked_zoom_rows ? VIDEO_SLIDE_ROWS : 0;
    slide_x = main_guard ? slide_dx : 0;
    slide_y = main_guard ? slide_dy : 0;
    asked_guard = camera_smoothing() && wide_view() ? VIDEO_SLIDE_MAX : 0;
    asked_margin = SDL_min(wide_margin() + asked_guard, WIDE_MARGIN_MAX);
    video_set_wide_margin(asked_margin);
    zoom_view(asked_guard, &asked_zoom_rows, &asked_zoom_margin);
    video_set_zoom(asked_zoom_rows, asked_zoom_margin);
    video_set_fill_sides(SETTING(SET_FILL));
    video_set_track_bottom(SETTING(SET_MINIMAP) && minimap_allowed(&primary));
    video_set_camera_smoothing(asked_guard != 0);
    video_set_ui_edges(ui_tall());
    main_w = width;
    main_h = height;
    ui_on = ui != NULL;
    party_on = main_guard && party;
    still_on = main_guard && still;
    if (new_frame || reupload) {
        hd_frame = hd;
        int shown[3] = {0};
        View *at[3] = {&primary, &primary, &primary}; /* the window each screen is uploaded to */
        View *views[2] = {&primary, &secondary};
        for (int i = 0; i < 2; i++) {
            for (int screen = 0, x, y; screen < 2; screen++) {
                if (!screen_slot(views[i], screen, &x, &y)) continue;
                int tex = screen == 0 && main_shown(views[i]) ? 2 : screen;
                shown[tex] = 1;
                at[tex] = views[i];
            }
        }
        for (int i = 0; i < 3; i++) upload(at[i], i, screens[i], shown[i]);
        if (minimap_shown(&primary)) upload_minimap();
        if (party_on) upload(&primary, 3, party, shown[2]);
        if (still_on) upload(&primary, 4, still, shown[2]);
        if (ui_on) upload(&primary, 5, ui, 0);
        hd_frame = NULL;
        reupload = 0;
    }
    SDL_Renderer *ren = primary.ren;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    int movie = movie_draw(ren);
    if (!movie) draw_screens(&primary);
    draw_message();
    draw_popup();
    if (capture_path[0]) {
        SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
        if (shot) {
            SDL_SaveBMP(shot, capture_path);
            SDL_DestroySurface(shot);
        }
        capture_path[0] = 0;
    }
    SDL_RenderPresent(ren);
    if (secondary.win) { /* black while a cutscene plays in the main window */
        SDL_SetRenderDrawColor(secondary.ren, 0, 0, 0, 255);
        SDL_RenderClear(secondary.ren);
        if (!movie) draw_screens(&secondary);
        SDL_RenderPresent(secondary.ren);
    }
    if (second_moved && SDL_GetTicks() - second_moved >= 500) {
        second_moved = 0;
        remember_second_place();
        save_settings();
    }
}

int display_key(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_F2:
        if (secondary.win) {
            show_message("%s", "Layout: one screen per window");
            return 1;
        }
        display_setting_set(SET_LAYOUT, (SETTING(SET_LAYOUT) + 1) % LAYOUT_COUNT);
        show_message("Layout: %s", layout_labels[SETTING(SET_LAYOUT)]);
        return 1;
    case SDL_SCANCODE_F3:
        swapped = !swapped;
        reupload = 1; /* with the second window, the screens change windows */
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
    case SET_LAYOUT: return !secondary.win; /* one screen per window */
    case SET_LANGUAGE: return !RT_JAPAN;
    case SET_LANGUAGE_JP: return RT_JAPAN;
    case SET_ZOOM_UI: return SETTING(SET_ZOOM) != 0;
    case SET_FILL: return SETTING(SET_ASPECT) != ASPECT_ORIGINAL || SETTING(SET_ZOOM);
    case SET_MINIMAP: return minimap_allowed(&primary);
    case SET_MINIMAP_SIZE:
    case SET_MINIMAP_BG: return SETTING(SET_MINIMAP) && minimap_allowed(&primary);
    case SET_SHADER: return settings[SET_SHADER].count > 1;
    case SET_RESOLUTION:
    case SET_FILTER: return !SETTING(SET_SHADER); /* a shader replaces them */
    default: return 1;
    }
}

int display_language(void) {
    if (RT_JAPAN) return SETTING(SET_LANGUAGE_JP) == LANGUAGE_JP_ENGLISH ? LANGUAGE_ID_ENGLISH : LANGUAGE_ID_JAPANESE;
    return SETTING(SET_LANGUAGE) == LANGUAGE_FRENCH ? LANGUAGE_ID_FRENCH : LANGUAGE_ID_ENGLISH;
}

const char *display_shader_path(void) { return SETTING(SET_SHADER) ? settings[SET_SHADER].names[SETTING(SET_SHADER)] : NULL; }

const char *display_setting_label(int id, int v) {
    if (id == SET_SHADER) return v ? shader_label(v - 1) : "Off";
    if (id == SET_ZOOM && v == 3 && camera_smoothing()) return "80%"; /* the guard rows take 8 of its 248 (zoom_view) */
    if (id != SET_RESOLUTION) return settings[id].labels[v];
    if (v == 0) return "Auto";
    static char buf[32];
    int cw, ch;
    content_size(&primary, &cw, &ch);
    snprintf(buf, sizeof buf, "%dx (%dx%d)", v, cw * v, ch * v);
    return buf;
}

void display_setting_set(int id, int v) {
    settings[id].value = v;
    switch (id) {
    case SET_WINDOW:
        if (v != WINDOW_WINDOWED) last_full = v;
        apply_window_mode(primary.win, v);
        apply_second_mode();
        break;
    case SET_LAYOUT: resize_window(); break;
    case SET_SECOND:
        if (v && !secondary.win && !open_second()) settings[id].value = 0;
        if (!v && secondary.win) close_second();
        resize_window();
        reupload = 1;
        break;
    case SET_VSYNC: SDL_SetRenderVSync(primary.ren, v); break;
    case SET_FILTER:
    case SET_COLOR: reupload = 1; break;
    }
    save_settings();
}

int display_camera_smoothing(void) { return camera_smoothing(); }

int display_follows_game(void) { return fps_values[SETTING(SET_FRAMERATE)] == 60; }

uint64_t display_frame_ns(void) {
    int fps = fps_values[SETTING(SET_FRAMERATE)];
    return fps ? SDL_NS_PER_SECOND / (uint64_t)fps : 0;
}

int display_touch(float wx, float wy, int *x, int *y) {
    int cw, ch, sx, sy;
    float px, py;
    View *v = secondary.win && SDL_GetMouseFocus() == secondary.win ? &secondary : &primary; /* the pointer's window */
    if (!screen_slot(v, 1, &sx, &sy)) return 0;
    content_size(v, &cw, &ch);
    SDL_RenderCoordinatesFromWindow(v->ren, wx, wy, &px, &py);
    SDL_FRect d = dest_rect(v);
    float cx = (px - d.x) * cw / d.w - sx, cy = (py - d.y) * ch / d.h - sy; /* native pixels in the screen */
    if (cx < 0 || cy < 0 || cx >= SCREEN_W || cy >= SCREEN_H) return 0;
    *x = (int)cx;
    *y = (int)cy;
    return 1;
}

void display_capture(const char *path) { snprintf(capture_path, sizeof capture_path, "%s", path); }
