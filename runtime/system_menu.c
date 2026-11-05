/* System Settings menu: language, window mode, layout, internal resolution, aspect ratio, framerate, V-Sync, graphics
 * filter, shader (listed when mods/shaders has some; the footer shows the chosen one's path), mods (display.c holds and applies them), and the Controls page (controls.c). SELECT opens it from the game's Game Settings screen (the button
 * single screen mode relabels "System settings"), the title screen's menu or while the field is paused; B or
 * SELECT closes it, and so does the game leaving that place (the title idling into the intro, say). Closed
 * from the pause, it also presses START for the game, which ends the pause. Up/Down pick a setting,
 * Left/Right change it (held: repeats); changes apply at once. Choosing a language other than the one the game
 * runs in asks whether to restart the game now (Left/Right pick Yes or No, A confirms, B is No): Yes powers the
 * game on again in it (RT_STATE_REBOOT), No leaves it for the next start. Up to ROWS_SHOWN rows fit; beyond them the list
 * scrolls with the cursor, and arrows in the header show which way more rows lie.
 *
 * A on Dump graphics asks whether to save every sprite and background as PNG files (gfx_dump.c), which takes a
 * while; Yes starts the dump on its own thread. While it runs, the menu shows its progress and the time left (from
 * the pace so far); the menu can be closed meanwhile, and the message box says when the dump is done.
 *
 * A on RetroAchievements opens its login page (cheevos.c): the player's name and password, typed with the keyboard
 * (or the system's on-screen keyboard) after A on their row, Enter or Esc ending the typing, and Log in; once logged
 * in, the name, Log out and the game's achievements, unlocked ones first, each with its badge (cheevos_badge, blank
 * while it downloads), title, points and description; Left/Right turn a page. The password shows as stars and is
 * forgotten once sent. B or SELECT goes back.
 *
 * A on Controls opens the button mapping: a row per DS button, fast forward and the save state slots, with its
 * keyboard key and gamepad button. Left/Right pick the key or the button, A waits for the new one (controls_capture), a
 * last row restores the defaults; B or SELECT goes back to the settings. After a rebinding, input is kept from
 * the menu until every key is released, so the new key does not act on the menu.
 *
 * The look follows the game's Game Settings screen, drawn by the runtime so it is the same wherever the menu
 * opens: a striped dark backdrop, light bevelled bars (header, footer), dark bevelled buttons, the
 * highlighted row's label lit and its value framed in the game's cursor teal. */
#include <SDL3/SDL.h>

#include "cheevos.h"
#include "controls.h"
#include "display.h"
#include "gfx_dump.h"
#include "rt.h"
#include "single_screen.h"
#include "system_menu.h"
#include "ui_font.h"
#include "ui_text.h"
#include "video.h"

#define REPEAT_DELAY_MS 400
#define REPEAT_MS 110
#define RESUME_MS 100 /* START held for the game after closing from the pause */

#define ROW_CONTROLS SET_COUNT /* after the display settings */
#define ROW_CHEEVOS (SET_COUNT + 1)
#define ROW_DUMP (SET_COUNT + 2)
#define ROW_COUNT (SET_COUNT + 3)
#define ROW_RESET CTL_COUNT /* the Controls page's last row */
#define ROWS_SHOWN 8        /* between the header and the footer */
#define CH_SLOT 2           /* rows an achievement takes on the RetroAchievements page */
#define CH_PAGE (ROWS_SHOWN / CH_SLOT - 1)

static const char *const descriptions[ROW_COUNT] = {
    [SET_LANGUAGE] = "Game text. Applies at the next start.",
    [SET_LANGUAGE_JP] = "Game text. Applies at the next start.",
    [SET_WINDOW] = "How the game window is shown.",
    [SET_LAYOUT] = "Which screens are shown, and how.",
    [SET_SECOND] = "The other screen in its own window.",
    [SET_RESOLUTION] = "Size the screens are drawn at.",
    [SET_ASPECT] = "Wider shows more of the world.",
    [SET_ZOOM] = "More of the world; text keeps its size.",
    [SET_FILL] = "Repeat small rooms' tiles beside them.",
    [SET_MINIMAP] = "The bottom screen's map, at the top left.",
    [SET_MINIMAP_SIZE] = "How much of the screen the map takes.",
    [SET_MINIMAP_BG] = "The parchment behind the map.",
    [SET_FRAMERATE] = "Frames shown per second. Game: 60.",
    [SET_VSYNC] = "Waits for the display: no tearing.",
    [SET_FILTER] = "How the pixels are scaled up.",
    [SET_SHADER] = "RetroArch shaders in mods/shaders.",
    [SET_COLOR] = "Deepens colors toward the SNES.",
    [SET_MODS] = "Replacement movies and music (mods).",
    [SET_FF_MUSIC] = "Normal speed may delay music cues.",
    [SET_FF_SOUND] = "Music only mutes sound effects.",
    [ROW_CONTROLS] = "Keyboard keys and gamepad buttons.",
    [ROW_CHEEVOS] = "Achievements of retroachievements.org.",
    [ROW_DUMP] = "Saves the graphics for mods, as PNG.",
};

enum { CONFIRM_NONE, CONFIRM_LANGUAGE, CONFIRM_DUMP };

static int place, cursor; /* place: where it was opened (SS_MENU_*), SS_MENU_NONE when closed */
static SDL_AtomicInt open_now; /* place != SS_MENU_NONE, for the game thread */
static int top;           /* the first row shown (rows: the shown settings, in order) */
static int confirm, yes;  /* the question open (CONFIRM_*); Yes is chosen */
static Uint64 dump_since; /* when the graphics dump started */
static int controls_page, ctl_cursor, ctl_pad, ctl_top; /* ctl_pad: the gamepad column is chosen */
static int cheevos_page, ch_cursor, ch_top; /* the RetroAchievements page, its row and the first row shown */
static int typing;                  /* the text field typed in (CH_NAME, CH_PASSWORD; 0: none) */
static char ch_name[64], ch_password[256];
static int released_wait; /* a rebinding or typing ended: the menu ignores input until every key is released */
static uint32_t prev_keys, blocked; /* blocked: held since the menu closed, kept from the game until released */
static Uint64 repeat_at, resume_until;

static int row_shown(int i) { return i >= SET_COUNT || display_setting_shown(i); }

static void controls_input(uint32_t act) {
    if (act & (KEY_B | KEY_SELECT)) controls_page = 0;
    if (act & KEY_UP) ctl_cursor = (ctl_cursor + ROW_RESET) % (ROW_RESET + 1);
    if (act & KEY_DOWN) ctl_cursor = (ctl_cursor + 1) % (ROW_RESET + 1);
    if (act & (KEY_LEFT | KEY_RIGHT)) ctl_pad = !ctl_pad;
    if (act & KEY_A) {
        if (ctl_cursor == ROW_RESET) {
            controls_reset();
            display_message(ui_text("Controls: defaults restored"), 2000);
        } else {
            controls_capture_start(ctl_cursor, ctl_pad);
        }
    }
}

/* The RetroAchievements page's rows: name, password and Log in, or, logged in, the name and Log out. */
enum { CH_NAME = 1, CH_PASSWORD, CH_LOGIN, CH_LOGOUT };
static int ch_rows(int *rows) {
    int n = 0;
    rows[n++] = CH_NAME;
    if (cheevos_state() == CHEEVOS_LOGGED_IN) {
        rows[n++] = CH_LOGOUT;
    } else {
        rows[n++] = CH_PASSWORD;
        rows[n++] = CH_LOGIN;
    }
    return n;
}

/* The page's rows: ch_rows', then, logged in, the game's achievements (*items, from row *fixed on). */
static int ch_total(int *rows, int *fixed, const CheevosItem **items) {
    *fixed = ch_rows(rows);
    *items = NULL;
    return *fixed + (cheevos_state() == CHEEVOS_LOGGED_IN ? cheevos_list(items) : 0);
}

static void typing_end(void) {
    if (!typing) return;
    if (typing == CH_NAME) ch_cursor = 1; /* on to the password */
    typing = 0;
    display_text_input(0);
    released_wait = 1;
}

int sysmenu_text_event(const SDL_Event *e) {
    if (!typing) return 0;
    char *text = typing == CH_NAME ? ch_name : ch_password;
    size_t size = typing == CH_NAME ? sizeof ch_name : sizeof ch_password, len = SDL_strlen(text);
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT:
        if (len + SDL_strlen(e->text.text) < size) SDL_strlcat(text, e->text.text, size);
        return 1;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_BACKSPACE) {
            while (len && (text[--len] & 0xC0) == 0x80) {} /* a whole UTF-8 character */
            text[len] = 0;
        } else if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER ||
                   e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_TAB) {
            typing_end();
        }
        return 1;
    case SDL_EVENT_KEY_UP: return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: typing_end(); return 1;
    default: return 0;
    }
}

static void cheevos_input(uint32_t act) {
    int rows[3], fixed;
    const CheevosItem *items;
    int n = ch_total(rows, &fixed, &items);
    ch_cursor = SDL_min(ch_cursor, n - 1); /* the rows change with logging in and out */
    if (act & (KEY_B | KEY_SELECT)) cheevos_page = 0;
    if (act & KEY_UP) ch_cursor = (ch_cursor + n - 1) % n;
    if (act & KEY_DOWN) ch_cursor = (ch_cursor + 1) % n;
    if (act & KEY_LEFT) ch_cursor = SDL_max(ch_cursor - CH_PAGE, 0); /* a page up or down the list */
    if (act & KEY_RIGHT) ch_cursor = SDL_min(ch_cursor + CH_PAGE, n - 1);
    if (!(act & KEY_A) || ch_cursor >= fixed || cheevos_state() == CHEEVOS_LOGGING_IN) return;
    switch (rows[ch_cursor]) {
    case CH_NAME:
        if (cheevos_state() == CHEEVOS_LOGGED_OUT) typing = CH_NAME, display_text_input(1);
        break;
    case CH_PASSWORD: typing = CH_PASSWORD, display_text_input(1); break;
    case CH_LOGIN:
        if (!ch_name[0] || !ch_password[0]) {
            display_message(ui_text("RetroAchievements: type your name and password"), 2500);
            break;
        }
        cheevos_login(ch_name, ch_password);
        SDL_memset(ch_password, 0, sizeof ch_password);
        break;
    case CH_LOGOUT:
        cheevos_logout();
        ch_cursor = 0;
        break;
    }
}

static uint32_t input(uint32_t keys) {
    uint32_t pressed = keys & ~prev_keys;
    Uint64 now = SDL_GetTicks();
    prev_keys = keys;
    if (place == SS_MENU_NONE) {
        if ((pressed & KEY_SELECT) && (place = ss_menu_place()) != SS_MENU_NONE) {
            cursor = top = confirm = controls_page = cheevos_page = 0;
            while (!row_shown(cursor)) cursor++;
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
    if (typing) {
        if (!left) return 0;
        typing_end();
    }
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
            int asked = confirm;
            confirm = CONFIRM_NONE;
            if (asked == CONFIRM_DUMP) {
                if ((act & KEY_A) && yes) gfxdump_start(), dump_since = now;
            } else if ((act & KEY_A) && yes) {
                rt_state_request = RT_STATE_REBOOT;
                place = SS_MENU_NONE;
                blocked = keys;
            } else {
                display_message(ui_text("Language: applies at the next start"), 2500);
            }
        }
        return 0;
    }
    if (controls_page && !left) {
        controls_input(act);
        return 0;
    }
    if (cheevos_page && !left) {
        cheevos_input(act);
        return 0;
    }
    if ((act & (KEY_B | KEY_SELECT)) || left) {
        if (place == SS_MENU_PAUSE && !left) resume_until = now + RESUME_MS;
        place = SS_MENU_NONE;
        blocked = keys;
        return 0;
    }
    if (gfxdump_running()) return 0; /* the progress covers the rows */
    if (act & KEY_UP) do cursor = (cursor + ROW_COUNT - 1) % ROW_COUNT; while (!row_shown(cursor));
    if (act & KEY_DOWN) do cursor = (cursor + 1) % ROW_COUNT; while (!row_shown(cursor));
    if (cursor == ROW_CONTROLS) {
        if (act & KEY_A) controls_page = 1, ctl_cursor = ctl_pad = ctl_top = 0;
    } else if (cursor == ROW_CHEEVOS) {
        if (act & KEY_A) cheevos_page = 1, ch_cursor = ch_top = 0;
    } else if (cursor == ROW_DUMP) {
        if ((act & KEY_A) && !gfxdump_running()) confirm = CONFIRM_DUMP, yes = 0;
    } else if (act & (KEY_LEFT | KEY_RIGHT)) {
        int n = display_setting_count(cursor);
        display_setting_set(cursor, (display_setting_get(cursor) + ((act & KEY_RIGHT) ? 1 : n - 1)) % n);
        if ((cursor == SET_LANGUAGE || cursor == SET_LANGUAGE_JP) && display_language() != rt_language()) {
            confirm = CONFIRM_LANGUAGE;
            yes = 0;
        }
    }
    return 0;
}

uint32_t sysmenu_input(uint32_t keys) {
    int images;
    if (gfxdump_finished(&images)) {
        char text[96];
        SDL_snprintf(text, sizeof text, ui_text("Graphics: %d images in mods/graphics/dump"), images);
        display_message(text, 4000);
    }
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
        if ((text[n] & 0xC0) == 0x80) continue; /* a whole UTF-8 character */
        SDL_memcpy(buf, text, n);
        SDL_memcpy(buf + n, "..", 3);
        if (n == 0 || ui_text_width(buf) <= w) break;
    }
    return buf;
}

static void centered(uint32_t *s, const char *text, Rect r, uint32_t color) {
    ui_draw_text(s, text, r.x + (r.w - ui_text_width(text)) / 2, r.y + 4, color);
}

/* A row's label and value: a display setting's, or an action's (Controls, RetroAchievements, Dump graphics). */
static const char *row_title(int i) {
    if (i == ROW_CONTROLS) return ui_text("Controls");
    if (i == ROW_CHEEVOS) return "RetroAchievements";
    return ui_text(i == ROW_DUMP ? "Dump graphics" : display_setting_title(i));
}

static const char *row_value(int i, int w) {
    static char buf[16];
    int done, total;
    float share;
    if (i == ROW_CONTROLS) return ui_text("A: change");
    if (i == ROW_CHEEVOS) {
        int st = cheevos_state();
        return st == CHEEVOS_LOGGED_IN ? fit(cheevos_user(), w) : ui_text(st == CHEEVOS_LOGGING_IN ? "Logging in..." : "A: log in");
    }
    if (i != ROW_DUMP) return fit(ui_text(display_setting_label(i, display_setting_get(i))), w);
    if (!gfxdump_running()) return ui_text("A: dump");
    gfxdump_progress(&done, &total, &share);
    SDL_snprintf(buf, sizeof buf, "%d%%", (int)(share * 100));
    return buf;
}

/* A bar panel in the middle of the screen with centred lines of text from its top. */
static void dialog(uint32_t *s, Rect r, const char *const *lines, int n) {
    panel(s, r, &BAR);
    for (int i = 0; i < n; i++) ui_draw_text(s, lines[i], (SCREEN_W - ui_text_width(lines[i])) / 2, r.y + 10 + 12 * i, WHITE);
}

/* The graphics dump's progress: a bar filling with the work done, and the time left at the pace so far. */
static void draw_dump_progress(uint32_t *s) {
    int done, total;
    float share;
    gfxdump_progress(&done, &total, &share);
    char count[80], left[80];
    if (!total) {
        SDL_strlcpy(count, ui_text("Listing the game's files..."), sizeof count);
        left[0] = 0;
    } else {
        SDL_snprintf(count, sizeof count, ui_text("%d of %d files"), done, total);
        Uint64 spent = SDL_GetTicks() - dump_since, rest = share > 0 ? (Uint64)((double)spent * (1 - share) / share) : 0;
        if (share < 0.02f || spent < 2000) SDL_strlcpy(left, ui_text("Working out the time left..."), sizeof left);
        else if (rest < 60000) SDL_snprintf(left, sizeof left, ui_text("About %d s left"), (int)(rest / 1000) + 1);
        else SDL_snprintf(left, sizeof left, ui_text("About %d min left"), (int)((rest + 30000) / 60000));
    }
    const char *lines[] = {ui_text("Dumping graphics...")};
    Rect r = {28, 52, 200, 86}, bar = {44, 78, 168, 12};
    dialog(s, r, lines, 1);
    panel(s, bar, &BUTTON);
    int fill = (int)((float)(bar.w - 6) * share);
    for (int y = bar.y + 3; y < bar.y + bar.h - 3; y++) {
        for (int x = bar.x + 3; x < bar.x + 3 + fill; x++) s[y * SCREEN_W + x] = TEAL;
    }
    ui_draw_text(s, count, (SCREEN_W - ui_text_width(count)) / 2, 98, WHITE);
    ui_draw_text(s, left, (SCREEN_W - ui_text_width(left)) / 2, 112, GREY);
}

static void draw_controls(uint32_t *s) {
    const char *title = ui_text("Controls");
    ui_draw_text(s, title, 12, 8, WHITE);
    centered(s, ui_text("Keyboard"), (Rect){92, 4, 78, 16}, GREY);
    centered(s, ui_text("Gamepad"), (Rect){174, 4, 62, 16}, GREY);
    scroll_to(s, &ctl_top, ctl_cursor, ROW_RESET + 1);
    int capturing = controls_capturing();
    for (int r = ctl_top; r <= ROW_RESET && r < ctl_top + ROWS_SHOWN; r++) {
        int y = ROW_Y + (r - ctl_top) * ROW_PITCH, sel = r == ctl_cursor;
        if (r == ROW_RESET) {
            Rect b = {6, y, 244, ROW_H};
            panel(s, b, sel ? &BUTTON_LIT : &BUTTON);
            if (sel) frame(s, b, TEAL);
            centered(s, ui_text("Reset to defaults"), b, sel ? WHITE : GREY);
            continue;
        }
        Rect lr = {6, y, 82, ROW_H}, cells[2] = {{92, y, 78, ROW_H}, {174, y, 76, ROW_H}};
        panel(s, lr, sel ? &BUTTON_LIT : &BUTTON);
        ui_draw_text(s, ui_text(controls_name(r)), lr.x + 6, y + 4, sel ? WHITE : GREY);
        for (int c = 0; c < 2; c++) {
            int on = sel && c == ctl_pad;
            panel(s, cells[c], &BUTTON);
            if (on) frame(s, cells[c], TEAL);
            const char *v = on && capturing ? ui_text("Press...") : c ? ui_text(controls_pad_label(r)) : controls_key_label(r);
            centered(s, v, cells[c], on ? YELLOW : WHITE);
        }
    }
}

/* A text field's value: the text (the password as stars), with a cursor while typed in, else a hint. */
static const char *field_value(int row, int w) {
    static char buf[80];
    const char *text = row == CH_NAME ? ch_name : ch_password;
    if (row == CH_NAME && cheevos_state() == CHEEVOS_LOGGED_IN) return fit(cheevos_user(), w);
    if (!text[0] && typing != row) return ui_text("A: type");
    size_t n = 0;
    if (row == CH_PASSWORD) {
        for (const char *p = text; *p && n < 40; p++) {
            if ((*p & 0xC0) != 0x80) buf[n++] = '*';
        }
        buf[n] = 0;
    } else {
        SDL_strlcpy(buf, text, sizeof buf - 1);
        n = SDL_strlen(buf);
    }
    if (typing != row) return fit(buf, w);
    buf[n] = '_', buf[n + 1] = 0;
    const char *v = buf; /* while typing, the end of the text stays in view */
    while (*v && ui_text_width(v) > w) v++;
    return v;
}

/* text in at most two lines w pixels wide from (x, y), broken between words, the second cut with ".." if too long. */
static void draw_lines(uint32_t *s, const char *text, int x, int y, int w, uint32_t color) {
    if (ui_text_width(text) <= w) {
        ui_draw_text(s, text, x, y, color);
        return;
    }
    char line[256];
    size_t len = SDL_strlcpy(line, text, sizeof line), cut = 0;
    for (size_t i = 1; i < len && i < sizeof line - 1; i++) { /* the longest first line ending before a space */
        if (line[i] != ' ') continue;
        line[i] = 0;
        int fits = ui_text_width(line) <= w;
        line[i] = ' ';
        if (!fits) break;
        cut = i;
    }
    if (!cut) {
        ui_draw_text(s, fit(text, w), x, y, color);
        return;
    }
    line[cut] = 0;
    ui_draw_text(s, line, x, y, color);
    ui_draw_text(s, fit(line + cut + 1, w), x, y + 10, color);
}

/* An achievement's slot: its badge (coloured once unlocked, greyed before), the title with its points or, locked,
 * how far along it is, and the description below. */
static void draw_achievement(uint32_t *s, const CheevosItem *a, int y, int sel) {
    Rect b = {6, y, 244, CH_SLOT * ROW_PITCH - 1};
    panel(s, b, sel ? &BUTTON_LIT : &BUTTON);
    if (sel) frame(s, b, TEAL);
    Rect img = {b.x + 3, y + 2, CHEEVOS_BADGE, CHEEVOS_BADGE};
    const uint32_t *badge = cheevos_badge(a->image_url);
    for (int py = 0; py < img.h; py++) {
        for (int px = 0; px < img.w; px++) {
            uint32_t *d = &s[(img.y + py) * SCREEN_W + img.x + px], c = badge ? badge[py * img.w + px] : 0xFF101010u;
            uint32_t al = c >> 24, out = 0xFF000000u;
            for (int k = 0; k < 24; k += 8) { /* over the panel by the badge's alpha */
                out |= (((c >> k & 0xFF) * al + (*d >> k & 0xFF) * (255 - al)) / 255) << k;
            }
            *d = out;
        }
    }
    char right[32];
    if (a->progress[0]) SDL_snprintf(right, sizeof right, "%s", a->progress);
    else SDL_snprintf(right, sizeof right, "%u %s", a->points, a->points == 1 ? "pt" : "pts");
    int tx = img.x + img.w + 5, tw = b.x + b.w - 6 - tx, rw = ui_text_width(right);
    uint32_t color = a->unlocked ? (sel ? YELLOW : WHITE) : (sel ? WHITE : GREY);
    ui_draw_text(s, fit(a->title, tw - rw - 6), tx, y + 3, color);
    ui_draw_text(s, right, tx + tw - rw, y + 3, color);
    draw_lines(s, a->description, tx, y + 14, tw, sel ? WHITE : GREY);
}

/* Rows an entry of the RetroAchievements page takes. */
static int ch_height(int r, int fixed) { return r < fixed ? 1 : CH_SLOT; }

/* Keeps the chosen entry of n in view of ROWS_SHOWN rows from ch_top, and lights the header's arrows. */
static void ch_scroll(uint32_t *s, int n, int fixed) {
    int used = 0;
    ch_top = SDL_min(ch_top, ch_cursor);
    for (int r = ch_top; r <= ch_cursor; r++) used += ch_height(r, fixed);
    for (; used > ROWS_SHOWN; ch_top++) used -= ch_height(ch_top, fixed);
    for (int r = ch_cursor + 1; r < n; r++) used += ch_height(r, fixed);
    if (ch_top > 0 || used > ROWS_SHOWN) scroll_arrow(s, -1, ch_top > 0), scroll_arrow(s, 1, used > ROWS_SHOWN);
}

static void draw_cheevos(uint32_t *s) {
    int rows[3], fixed;
    const CheevosItem *items;
    int n = ch_total(rows, &fixed, &items), unlocked = 0;
    char title[64] = "RetroAchievements";
    for (int i = 0; i < n - fixed; i++) unlocked += items[i].unlocked;
    if (n > fixed) SDL_snprintf(title, sizeof title, ui_text("Achievements: %d of %d"), unlocked, n - fixed);
    ui_draw_text(s, title, (SCREEN_W - ui_text_width(title)) / 2, 8, WHITE);
    ch_cursor = SDL_min(ch_cursor, n - 1);
    ch_scroll(s, n, fixed);
    for (int r = ch_top, used = 0; r < n && used + ch_height(r, fixed) <= ROWS_SHOWN; used += ch_height(r++, fixed)) {
        int y = ROW_Y + used * ROW_PITCH, sel = r == ch_cursor;
        if (r >= fixed) {
            draw_achievement(s, &items[r - fixed], y, sel);
            continue;
        }
        if (rows[r] == CH_LOGIN || rows[r] == CH_LOGOUT) {
            Rect b = {6, y, 244, ROW_H};
            panel(s, b, sel ? &BUTTON_LIT : &BUTTON);
            if (sel) frame(s, b, TEAL);
            const char *label = rows[r] == CH_LOGOUT                       ? "Log out"
                                : cheevos_state() == CHEEVOS_LOGGING_IN ? "Logging in..."
                                                                        : "Log in";
            centered(s, ui_text(label), b, sel ? WHITE : GREY);
            continue;
        }
        Rect lr = {6, y, 112, ROW_H}, vr = {122, y, 128, ROW_H};
        panel(s, lr, sel ? &BUTTON_LIT : &BUTTON);
        panel(s, vr, &BUTTON);
        if (sel) frame(s, vr, TEAL);
        const char *label = rows[r] == CH_PASSWORD ? "Password" : cheevos_state() == CHEEVOS_LOGGED_IN ? "Player" : "Name";
        ui_draw_text(s, ui_text(label), lr.x + 6, y + 4, sel ? WHITE : GREY);
        const char *v = field_value(rows[r], vr.w - 12);
        ui_draw_text(s, v, vr.x + (vr.w - ui_text_width(v)) / 2, y + 4, sel ? YELLOW : WHITE);
    }
}

static const char *cheevos_footer(void) {
    int rows[3], fixed;
    const CheevosItem *items;
    ch_total(rows, &fixed, &items);
    if (typing) return ui_text("Type, then Enter.");
    if (cheevos_state() == CHEEVOS_LOGGING_IN) return ui_text("Logging in...");
    if (ch_cursor >= fixed) {
        const CheevosItem *a = &items[ch_cursor - fixed];
        static char buf[96];
        if (a->unlocked) return ui_text("Unlocked.");
        if (!a->progress[0]) return ui_text("Not unlocked yet.");
        SDL_snprintf(buf, sizeof buf, ui_text("Not unlocked yet: %s so far."), a->progress);
        return buf;
    }
    switch (rows[ch_cursor]) {
    case CH_NAME:
        return ui_text(cheevos_state() == CHEEVOS_LOGGED_IN ? "Achievements unlock as you play." : "Your retroachievements.org name.");
    case CH_PASSWORD: return ui_text("Only a login token is saved.");
    case CH_LOGIN: return ui_text("No keyboard? See retroachievements.ini");
    default: return ui_text("A: log out.");
    }
}

void sysmenu_draw(uint32_t *s) {
    for (int y = 0; y < SCREEN_H; y++) { /* backdrop: the game's brushed dark stripes */
        for (int x = 0; x < SCREEN_W; x++) s[y * SCREEN_W + x] = (y & 1) ? rgb(24, 24, 24) : rgb(16, 16, 16);
    }
    panel(s, (Rect){0, 0, 256, 24}, &BAR);
    if (controls_page) {
        draw_controls(s);
    } else if (cheevos_page) {
        draw_cheevos(s);
    } else {
        const char *title = ui_text("System Settings");
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
            ui_draw_text(s, row_title(i), lr.x + 6, ty, sel ? WHITE : GREY);
            const char *v = row_value(i, vr.w - 24);
            ui_draw_text(s, v, vr.x + (vr.w - ui_text_width(v)) / 2, ty, sel ? YELLOW : WHITE);
            if (sel && i < SET_COUNT) {
                ui_draw_text(s, "<", vr.x + 5, ty, WHITE);
                ui_draw_text(s, ">", vr.x + vr.w - 8, ty, WHITE);
            }
        }
    }

    const char *footer = ui_text(descriptions[cursor]);
    if (cursor == SET_SHADER && display_shader_path()) footer = fit(display_shader_path(), 208);
    if (confirm) footer = ui_text("A: confirm.  B: not now.");
    else if (gfxdump_running() && !controls_page && !cheevos_page) footer = ui_text("Closing the menu leaves it running.");
    else if (controls_page && controls_capturing() && controls_capture_conflict()) {
        static char used[96];
        SDL_snprintf(used, sizeof used, ui_text("%s uses that one. Press another."), ui_text(controls_capture_conflict()));
        footer = used;
    } else if (controls_page && controls_capturing()) footer = ui_text(ctl_pad ? "Press a gamepad button. Esc: cancel." : "Press a key. Esc: cancel.");
    else if (controls_page) footer = ui_text(ctl_cursor == ROW_RESET ? "A: restore the default controls." : "A: change.  Left/Right: key or pad.");
    else if (cheevos_page) footer = cheevos_footer();
    panel(s, (Rect){0, 166, 222, 26}, &BAR);
    draw_lines(s, footer, 8, ui_text_width(footer) <= 208 ? 175 : 170, 208, WHITE);
    panel(s, (Rect){224, 166, 32, 26}, &BAR);
    b_sign(s, 240, 179);
    if (confirm) {
        const char *const language[] = {ui_text("Restart the game now?"), ui_text("The new language needs a restart."),
                                        ui_text("Unsaved progress will be lost.")};
        const char *const dump[] = {ui_text("Dump all game graphics?"), ui_text("Every sprite and background, to"),
                                    ui_text("mods/graphics/dump. May take a while.")};
        dialog(s, (Rect){28, 52, 200, 86}, confirm == CONFIRM_DUMP ? dump : language, 3);
        for (int i = 0; i < 2; i++) {
            Rect b = {64 + 72 * i, 106, 56, 20};
            int sel = (i == 0) == yes;
            panel(s, b, sel ? &BUTTON_LIT : &BUTTON);
            if (sel) frame(s, b, TEAL);
            const char *label = ui_text(i == 0 ? "Yes" : "No");
            ui_draw_text(s, label, b.x + (b.w - ui_text_width(label)) / 2, b.y + 6, sel ? WHITE : GREY);
        }
    } else if (gfxdump_running() && !controls_page && !cheevos_page) {
        draw_dump_progress(s);
    }
}

void sysmenu_draw_select_button(uint32_t *s, int x1, int y1) {
    Rect r = {x1 - SELECT_BUTTON_W, y1 - SELECT_BUTTON_H, SELECT_BUTTON_W, SELECT_BUTTON_H};
    panel(s, r, &BUTTON);
    ui_draw_text(s, "SELECT", r.x + (r.w - ui_text_width("SELECT")) / 2, r.y + 3, WHITE);
}
