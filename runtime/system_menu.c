/* System Settings menu: language, window mode, internal resolution, aspect ratio, framerate, V-Sync, graphics
 * filter, shader (listed when mods/shaders has some; the footer shows the chosen one's path), mods (display.c holds and applies them), and the Controls page (controls.c). SELECT opens it from the game's Game Settings screen (the button
 * single screen mode relabels "System settings"), the title screen's menu or while the field is paused; B or
 * SELECT closes it, and so does the game leaving that place (the title idling into the intro, say). Closed
 * from the pause, it also presses START for the game, which ends the pause. Up/Down pick a setting,
 * Left/Right change it (held: repeats); changes apply at once. Choosing a language other than the one the game
 * runs in asks whether to restart the game now (Left/Right pick Yes or No, A confirms, B is No): Yes powers the
 * game on again in it (RT_STATE_REBOOT), No leaves it for the next start. Up to ROWS_SHOWN rows fit; beyond them the list
 * scrolls with the cursor, and arrows in the header show which way more rows lie.
 *
 * A on the last row, Controls, opens the button mapping: a row per DS button and fast forward, with its keyboard
 * key and gamepad button. Left/Right pick the key or the button, A waits for the new one (controls_capture), a
 * last row restores the defaults; B or SELECT goes back to the settings. After a rebinding, input is kept from
 * the menu until every key is released, so the new key does not act on the menu.
 *
 * The look follows the game's Game Settings screen, drawn by the runtime so it is the same wherever the menu
 * opens: a striped dark backdrop, light bevelled bars (header, footer), dark bevelled buttons, the
 * highlighted row's label lit and its value framed in the game's cursor teal. */
#include <SDL3/SDL.h>

#include "controls.h"
#include "display.h"
#include "rt.h"
#include "single_screen.h"
#include "system_menu.h"
#include "ui_font.h"
#include "video.h"

#define REPEAT_DELAY_MS 400
#define REPEAT_MS 110
#define RESUME_MS 100 /* START held for the game after closing from the pause */

#define ROW_CONTROLS SET_COUNT /* after the display settings */
#define ROW_COUNT (SET_COUNT + 1)
#define ROW_RESET CTL_COUNT /* the Controls page's last row */

static const char *const descriptions[ROW_COUNT] = {
    [SET_LANGUAGE] = "Game text. Applies at the next start.",
    [SET_WINDOW] = "How the game window is shown.",
    [SET_RESOLUTION] = "Size the screens are drawn at.",
    [SET_ASPECT] = "Wider shows more of the world.",
    [SET_FILL] = "Repeat small rooms' tiles beside them.",
    [SET_FRAMERATE] = "Frames shown per second. Game: 60.",
    [SET_VSYNC] = "Waits for the display: no tearing.",
    [SET_FILTER] = "How the pixels are scaled up.",
    [SET_SHADER] = "RetroArch shaders in mods/shaders.",
    [SET_COLOR] = "Deepens colors toward the SNES.",
    [SET_MODS] = "Replacement movies and music (mods).",
    [SET_FF_MUSIC] = "Normal speed may delay music cues.",
    [ROW_CONTROLS] = "Keyboard keys and gamepad buttons.",
};

static int place, cursor; /* place: where it was opened (SS_MENU_*), SS_MENU_NONE when closed */
static SDL_AtomicInt open_now; /* place != SS_MENU_NONE, for the game thread */
static int top;           /* the first row shown (rows: the shown settings, in order) */
static int confirm, yes;  /* the restart question is open; Yes is chosen */
static int controls_page, ctl_cursor, ctl_pad, ctl_top; /* ctl_pad: the gamepad column is chosen */
static int released_wait; /* a rebinding ended: the menu ignores input until every key is released */
static uint32_t prev_keys, blocked; /* blocked: held since the menu closed, kept from the game until released */
static Uint64 repeat_at, resume_until;

static int row_shown(int i) { return i == ROW_CONTROLS || display_setting_shown(i); }

static void controls_input(uint32_t act) {
    if (act & (KEY_B | KEY_SELECT)) controls_page = 0;
    if (act & KEY_UP) ctl_cursor = (ctl_cursor + ROW_RESET) % (ROW_RESET + 1);
    if (act & KEY_DOWN) ctl_cursor = (ctl_cursor + 1) % (ROW_RESET + 1);
    if (act & (KEY_LEFT | KEY_RIGHT)) ctl_pad = !ctl_pad;
    if (act & KEY_A) {
        if (ctl_cursor == ROW_RESET) {
            controls_reset();
            display_message("Controls: defaults restored", 2000);
        } else {
            controls_capture_start(ctl_cursor, ctl_pad);
        }
    }
}

static uint32_t input(uint32_t keys) {
    uint32_t pressed = keys & ~prev_keys;
    Uint64 now = SDL_GetTicks();
    prev_keys = keys;
    if (place == SS_MENU_NONE) {
        if ((pressed & KEY_SELECT) && (place = ss_menu_place()) != SS_MENU_NONE) {
            cursor = top = confirm = controls_page = 0;
            return 0;
        }
        blocked &= keys;
        return (keys & ~blocked) | (now < resume_until ? KEY_START : 0);
    }
    uint32_t dirs = keys & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT), act = pressed;
    if (pressed & dirs) {
        repeat_at = now + REPEAT_DELAY_MS;
    } else if (dirs && now >= repeat_at) {
        act |= dirs;
        repeat_at = now + REPEAT_MS;
    }
    int left = ss_menu_place() != place; /* the game moved on */
    if (controls_capturing()) {
        if (!left) {
            released_wait = 1;
            return 0;
        }
        controls_capture_cancel();
    }
    if (released_wait) {
        if (keys && !left) return 0;
        released_wait = 0;
        act = 0;
    }
    if (confirm && !left) {
        if (act & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) yes = !yes;
        if (act & (KEY_A | KEY_B)) {
            confirm = 0;
            if ((act & KEY_A) && yes) {
                rt_state_request = RT_STATE_REBOOT;
                place = SS_MENU_NONE;
                blocked = keys;
            } else {
                display_message("Language: applies at the next start", 2500);
            }
        }
        return 0;
    }
    if (controls_page && !left) {
        controls_input(act);
        return 0;
    }
    if ((act & (KEY_B | KEY_SELECT)) || left) {
        if (place == SS_MENU_PAUSE && !left) resume_until = now + RESUME_MS;
        place = SS_MENU_NONE;
        blocked = keys;
        return 0;
    }
    if (act & KEY_UP) do cursor = (cursor + ROW_COUNT - 1) % ROW_COUNT; while (!row_shown(cursor));
    if (act & KEY_DOWN) do cursor = (cursor + 1) % ROW_COUNT; while (!row_shown(cursor));
    if (cursor == ROW_CONTROLS) {
        if (act & KEY_A) controls_page = 1, ctl_cursor = ctl_pad = ctl_top = 0;
    } else if (act & (KEY_LEFT | KEY_RIGHT)) {
        int n = display_setting_count(cursor);
        display_setting_set(cursor, (display_setting_get(cursor) + ((act & KEY_RIGHT) ? 1 : n - 1)) % n);
        if (cursor == SET_LANGUAGE && (display_setting_get(SET_LANGUAGE) == LANGUAGE_FRENCH) != rt_french()) {
            confirm = 1;
            yes = 0;
        }
    }
    return 0;
}

uint32_t sysmenu_input(uint32_t keys) {
    uint32_t game_keys = input(keys);
    SDL_SetAtomicInt(&open_now, place != SS_MENU_NONE);
    return game_keys;
}

int sysmenu_is_open(void) { return SDL_GetAtomicInt(&open_now); }

/* ---- drawing ---- */

typedef struct Rect { int x, y, w, h; } Rect;
typedef struct Rgb { int r, g, b; } Rgb;
/* A bevelled panel as the game draws them: outer edge, bright then dark rim, inner line, and a noisy fill
 * shading from top to bottom (colours measured on Game Settings). */
typedef struct Style { Rgb edge, rim_lit, rim_shade, inner_lit, inner_shade, fill_top, fill_bottom; int noise; } Style;
static const Style BAR = {{57, 66, 74}, {206, 206, 214}, {57, 66, 74}, {165, 165, 173}, {99, 107, 115},
                          {123, 132, 140}, {57, 66, 74}, 16};
static const Style BUTTON = {{8, 16, 16}, {99, 99, 99}, {8, 16, 16}, {82, 82, 82}, {49, 49, 57},
                             {57, 62, 66}, {28, 34, 36}, 9};
static const Style BUTTON_LIT = {{49, 49, 49}, {214, 214, 214}, {49, 49, 49}, {181, 181, 181}, {140, 140, 140},
                                 {140, 140, 148}, {90, 99, 107}, 16};

static uint32_t rgb(int r, int g, int b) {
    r = SDL_clamp(r, 0, 255), g = SDL_clamp(g, 0, 255), b = SDL_clamp(b, 0, 255);
    return 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
}

static uint32_t noise(int x, int y) { /* fixed per pixel, so the texture does not shimmer */
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void panel(uint32_t *s, Rect r, const Style *st) {
    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x++) {
            int left = x, top = y, right = r.w - 1 - x, bottom = r.h - 1 - y;
            int d = SDL_min(SDL_min(left, top), SDL_min(right, bottom));
            int lit = SDL_min(left, top) <= SDL_min(right, bottom);
            Rgb c;
            if (d == 0) c = st->edge;
            else if (d == 1) c = lit ? st->rim_lit : st->rim_shade;
            else if (d == 2) c = lit ? st->inner_lit : st->inner_shade;
            else {
                int n = (int)(noise(r.x + x, r.y + y) % (uint32_t)(2 * st->noise + 1)) - st->noise;
                c.r = st->fill_top.r + (st->fill_bottom.r - st->fill_top.r) * y / r.h + n;
                c.g = st->fill_top.g + (st->fill_bottom.g - st->fill_top.g) * y / r.h + n;
                c.b = st->fill_top.b + (st->fill_bottom.b - st->fill_top.b) * y / r.h + n;
            }
            s[(r.y + y) * SCREEN_W + r.x + x] = rgb(c.r, c.g, c.b);
        }
    }
}

static void frame(uint32_t *s, Rect r, uint32_t color) {
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            if (x - r.x < 2 || r.x + r.w - x <= 2 || y - r.y < 2 || r.y + r.h - y <= 2) s[y * SCREEN_W + x] = color;
        }
    }
}

/* The game's B button sign: a white disc with a dark B. */
static void b_sign(uint32_t *s, int cx, int cy) {
    for (int y = -8; y <= 8; y++) {
        for (int x = -8; x <= 8; x++) {
            int d = x * x + y * y;
            if (d <= 64) s[(cy + y) * SCREEN_W + cx + x] = d > 49 ? rgb(16, 16, 16) : rgb(240, 240, 240);
        }
    }
    ui_draw_text_flat(s, "B", cx - 1, cy - 3, rgb(24, 24, 24));
}

#define WHITE rgb(255, 255, 255)
#define GREY rgb(176, 176, 176)
#define YELLOW rgb(248, 240, 88) /* the game's chosen value */
#define TEAL rgb(24, 198, 173)   /* the game's cursor frame */

#define ROW_Y 27
#define ROW_PITCH 17
#define ROW_H 16
#define ROWS_SHOWN 8 /* between the header and the footer */

/* A small arrow in the header's right end: up (dir -1) or down (+1), lit when more rows lie that way. */
static void scroll_arrow(uint32_t *s, int dir, int lit) {
    int cy = dir < 0 ? 7 : 16;
    for (int k = 0; k < 4; k++) {
        int y = cy + (dir < 0 ? k - 2 : 2 - k);
        for (int x = 241 - k; x <= 241 + k; x++) s[y * SCREEN_W + x] = lit ? WHITE : rgb(90, 99, 107);
    }
}

/* Keeps row `at` of n in view of ROWS_SHOWN rows from *top, and lights the header's arrows. */
static void scroll_to(uint32_t *s, int *top, int at, int n) {
    *top = SDL_clamp(*top, at - ROWS_SHOWN + 1, at);
    *top = SDL_clamp(*top, 0, SDL_max(n - ROWS_SHOWN, 0));
    if (n > ROWS_SHOWN) scroll_arrow(s, -1, *top > 0), scroll_arrow(s, 1, *top + ROWS_SHOWN < n);
}

/* text cut to at most w pixels wide, ending in ".." when cut (a buffer shared by every call). */
static const char *fit(const char *text, int w) {
    static char buf[256];
    if (ui_text_width(text) <= w) return text;
    for (size_t n = SDL_min(SDL_strlen(text), sizeof buf - 3);; n--) {
        SDL_memcpy(buf, text, n);
        SDL_memcpy(buf + n, "..", 3);
        if (n == 0 || ui_text_width(buf) <= w) break;
    }
    return buf;
}

static void centered(uint32_t *s, const char *text, Rect r, uint32_t color) {
    ui_draw_text(s, text, r.x + (r.w - ui_text_width(text)) / 2, r.y + 4, color);
}

static void draw_controls(uint32_t *s) {
    const char *title = "Controls";
    ui_draw_text(s, title, 12, 8, WHITE);
    centered(s, "Keyboard", (Rect){92, 4, 78, 16}, GREY);
    centered(s, "Gamepad", (Rect){174, 4, 62, 16}, GREY);
    scroll_to(s, &ctl_top, ctl_cursor, ROW_RESET + 1);
    int capturing = controls_capturing();
    for (int r = ctl_top; r <= ROW_RESET && r < ctl_top + ROWS_SHOWN; r++) {
        int y = ROW_Y + (r - ctl_top) * ROW_PITCH, sel = r == ctl_cursor;
        if (r == ROW_RESET) {
            Rect b = {6, y, 244, ROW_H};
            panel(s, b, sel ? &BUTTON_LIT : &BUTTON);
            if (sel) frame(s, b, TEAL);
            centered(s, "Reset to defaults", b, sel ? WHITE : GREY);
            continue;
        }
        Rect lr = {6, y, 82, ROW_H}, cells[2] = {{92, y, 78, ROW_H}, {174, y, 76, ROW_H}};
        panel(s, lr, sel ? &BUTTON_LIT : &BUTTON);
        ui_draw_text(s, controls_name(r), lr.x + 6, y + 4, sel ? WHITE : GREY);
        for (int c = 0; c < 2; c++) {
            int on = sel && c == ctl_pad;
            panel(s, cells[c], &BUTTON);
            if (on) frame(s, cells[c], TEAL);
            const char *v = on && capturing ? "Press..." : c ? controls_pad_label(r) : controls_key_label(r);
            centered(s, v, cells[c], on ? YELLOW : WHITE);
        }
    }
}

void sysmenu_draw(uint32_t *s) {
    for (int y = 0; y < SCREEN_H; y++) { /* backdrop: the game's brushed dark stripes */
        for (int x = 0; x < SCREEN_W; x++) s[y * SCREEN_W + x] = (y & 1) ? rgb(24, 24, 24) : rgb(16, 16, 16);
    }
    panel(s, (Rect){0, 0, 256, 24}, &BAR);
    if (controls_page) {
        draw_controls(s);
    } else {
        const char *title = "System Settings";
        ui_draw_text(s, title, (SCREEN_W - ui_text_width(title)) / 2, 8, WHITE);

        int rows[ROW_COUNT], n = 0, at = 0;
        for (int i = 0; i < ROW_COUNT; i++) {
            if (row_shown(i)) at = i == cursor ? n : at, rows[n++] = i;
        }
        scroll_to(s, &top, at, n);
        for (int r = top; r < n && r < top + ROWS_SHOWN; r++) {
            int i = rows[r], y = ROW_Y + (r - top) * ROW_PITCH, ty = y + 4, sel = i == cursor;
            Rect lr = {6, y, 112, ROW_H}, vr = {122, y, 128, ROW_H};
            panel(s, lr, sel ? &BUTTON_LIT : &BUTTON);
            panel(s, vr, &BUTTON);
            if (sel) frame(s, vr, TEAL);
            ui_draw_text(s, i == ROW_CONTROLS ? "Controls" : display_setting_title(i), lr.x + 6, ty, sel ? WHITE : GREY);
            const char *v = i == ROW_CONTROLS ? "A: change" : fit(display_setting_label(i, display_setting_get(i)), vr.w - 24);
            ui_draw_text(s, v, vr.x + (vr.w - ui_text_width(v)) / 2, ty, sel ? YELLOW : WHITE);
            if (sel && i != ROW_CONTROLS) {
                ui_draw_text(s, "<", vr.x + 5, ty, WHITE);
                ui_draw_text(s, ">", vr.x + vr.w - 8, ty, WHITE);
            }
        }
    }

    const char *footer = descriptions[cursor];
    if (cursor == SET_SHADER && display_shader_path()) footer = fit(display_shader_path(), 208);
    if (confirm) footer = "A: confirm.  B: not now.";
    else if (controls_page && controls_capturing()) footer = ctl_pad ? "Press a gamepad button. Esc: cancel." : "Press a key. Esc: cancel.";
    else if (controls_page) footer = ctl_cursor == ROW_RESET ? "A: restore the default controls." : "A: change.  Left/Right: key or pad.";
    panel(s, (Rect){0, 166, 222, 26}, &BAR);
    ui_draw_text(s, footer, 8, 175, WHITE);
    panel(s, (Rect){224, 166, 32, 26}, &BAR);
    b_sign(s, 240, 179);
    if (confirm) {
        static const char *const lines[] = {"Restart the game now?", "The new language needs a restart.",
                                            "Unsaved progress will be lost."};
        panel(s, (Rect){28, 52, 200, 86}, &BAR);
        for (int i = 0; i < 3; i++) ui_draw_text(s, lines[i], (SCREEN_W - ui_text_width(lines[i])) / 2, 62 + 12 * i, WHITE);
        for (int i = 0; i < 2; i++) {
            Rect b = {64 + 72 * i, 106, 56, 20};
            int sel = (i == 0) == yes;
            panel(s, b, sel ? &BUTTON_LIT : &BUTTON);
            if (sel) frame(s, b, TEAL);
            const char *label = i == 0 ? "Yes" : "No";
            ui_draw_text(s, label, b.x + (b.w - ui_text_width(label)) / 2, b.y + 6, sel ? WHITE : GREY);
        }
    }
}

void sysmenu_draw_select_button(uint32_t *s, int x1, int y1) {
    Rect r = {x1 - SELECT_BUTTON_W, y1 - SELECT_BUTTON_H, SELECT_BUTTON_W, SELECT_BUTTON_H};
    panel(s, r, &BUTTON);
    ui_draw_text(s, "SELECT", r.x + (r.w - ui_text_width("SELECT")) / 2, r.y + 3, WHITE);
}
