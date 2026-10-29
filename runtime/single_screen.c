/* Single screen mode: the boot, title and Game Settings screens, which the DS spreads over both screens, laid
 * out on the main (top) screen, for devices with one screen. Off (EGG_SINGLE_SCREEN=0) the main screen is the
 * top screen.
 *
 * The running scene is the game's own: Scene::run (main 0x0202514c), which every scene runs in, stores the
 * scene's secondary base (scene + 8) at 0x0207088c (func 0x02025d30), where the VBlank handler finds it. That
 * base's vtable names the scene class. Being game memory, it is right after loading a save state too.
 *   - Logos (ov007): first "Licensed by Nintendo" on the bottom screen alone, then Square Enix in the middle
 *     of the top screen and Mobiclip in the middle of the bottom one, on the same plain backdrop. Shown: the
 *     bottom screen while the top one is blank, then both logos stacked.
 *   - Title (ov007): logo and pendulum on the top screen. The bottom screen shows "Touch to Start" (title
 *     state 8), with the copyright once the logo has formed; a touch or START opens the menu (phase 7), the
 *     buttons (New Game...) on the bottom screen. The main screen gets the copyright, "Press any button to
 *     start" (blinking with the game's own prompt) and then the menu buttons, left of the pendulum and
 *     right above the copyright; any button stands in for the touch. In the menu, SELECT also opens the
 *     runtime's menu (not shown on screen).
 *   - Game Settings (ov007, after New Game): the options on the bottom screen, the highlighted one's
 *     description on the top screen. The main screen shows Battle Mode and Movies with the description under
 *     the highlighted one; Game Mode is left out (single screen mode always plays Classic) and SELECT reads
 *     "System settings" instead of the game's "Defaults": it opens the runtime's menu (system_menu.c).
 *   - Name entry (ov012): the character on a patch of grass on the top screen, the keyboard, name and
 *     portrait filling the bottom screen. The main screen shows the top screen while the character appears
 *     and after the name is accepted (the character's animation), the bottom screen while the name is entered.
 *   - Field menu (ov012, X), its Main Menu page: the party, location, time and gold on the top screen, the
 *     command icons and the description on the bottom one. The main screen shows the party as three compact
 *     cards (portrait, name and LV, HP, MP) built from the game's panels, the icons, then one bar with the time,
 *     the location and the gold, and the description; the "Main Menu" title is left out.
 *   - Its Equipment page: the member's portrait, stats and EXP on the top screen, the members (L/R), the four
 *     slots (or, while an item is chosen, the slot and the item list) and the description on the bottom one.
 *     The main screen shows the members, the name, LV, HP and MP, the stats packed closer, the slots in a 2x2
 *     grid (the slot being changed and two rows of the list while choosing) and the description; the portrait
 *     shows as a strip across the eyes beside the element; the title and the items' power are left out.
 *   - Its Inventory page: the party's HP and MP on the top screen, the items (and, once one is used, the
 *     member cards to use it on) on the bottom one. The main screen shows the bottom screen; while choosing
 *     who uses the item, with the item's bar moved to the top and a line per member (name, HP, MP) under it.
 *   - Its Techs page: the member's HP, MP and the tech's cost on the top screen, the techs on the bottom one, then
 *     the cards as in the Inventory. The main screen shows the bottom screen with the member's HP and MP added
 *     under the category; choosing who a tech is used on, as in the Inventory.
 *   - Its Party page: the party's sprites and their Dual and Triple techs (four boxes) on the top screen, all
 *     members' cards and the description on the bottom one. The main screen shows the bottom screen with the
 *     sprites under the party's cards, or with Y (and back) the top screen; a hint names the other view.
 *   - Its Save page: the highlighted file's party, location, time and gold on the top screen, the three files on
 *     the bottom one. The main screen shows the files narrowed at the left, the file's members (portrait, LV, HP
 *     and MP) at the right, then the time, location and gold in one bar as on the Main Menu page.
 *   - Its Settings page: every option's value on the top screen, the tabs, four options and "Defaults" on
 *     the bottom one. The main screen shows the bottom screen; on the first tab without Game Mode (single
 *     screen mode always plays Classic), the other options moved up. SELECT reads "System settings" instead
 *     of "Defaults": it opens the runtime's menu, as in Game Settings.
 *   - Its Bestiary page: the monster and its panel on the top screen, the tabs, a list of four (number, name) and
 *     the count discovered on the bottom one. The main screen shows the top screen with the tabs over its title,
 *     and under the monster the list as lines of text, the selected one framed, then the count.
 *   - Shops (ov012): each member's sprite, name and attack and defense (now and with the item highlighted) on the
 *     top screen; Buy / Sell / Equip, the categories, the gold, the item's panel, the list or the quantity on the
 *     bottom one, whose help bar stays empty. The main screen shows the bottom screen moved up into the gap
 *     under the tabs and, in place of the help bar, every member's name and the stat the item changes as lines
 *     of text, three to a line. Equip opens the Equipment page.
 *   - Widescreen (the top screen with its margins): the field, and the warp through a time gate (ov010): an
 *     affine BG with wraparound, whose margins the renderer draws as they are, under a star of sprites that
 *     covers the DS screen exactly, stretched across the margins (video.c); and the world map (ov002), whose
 *     margins are filled from its own map data (widescreen.c).
 *   - Everything else, the intro movie included, shows the top screen as it is; while the field is paused,
 *     with "Press SELECT to open System Settings" at the bottom (the menu opens from there too). */
#include <SDL3/SDL.h>

#include "dispatch.h"
#include "rt.h"
#include "single_screen.h"
#include "ui_font.h"
#include "video.h"

enum { SCENE_OTHER, SCENE_LOGOS, SCENE_TITLE, SCENE_SETTINGS, SCENE_NAMING, SCENE_FIELD, SCENE_WARP, SCENE_OVERWORLD, SCENE_MENU, SCENE_EQUIP, SCENE_ITEMS, SCENE_TECHS, SCENE_BESTIARY, SCENE_OPTIONS, SCENE_PARTY, SCENE_SAVE, SCENE_SHOP };
typedef struct Scene { int kind; uint32_t obj; } Scene;
static uint32_t title_held; /* keys held since before the title started: not a press */
#define TITLE_STATE 0x4a  /* title scene: current state (halfword); +0x48 holds the requested one */
#define TITLE_WAITING 8   /* "Touch to Start", until the phase is the menu's */
#define TITLE_PHASE 0x38b6 /* title scene: phase (halfword): 0 and 3 the logo forming, 5 formed, 7 menu, 0x10 leaving */
#define TITLE_MENU 7      /* a touch before the logo has formed opens it through states 0-0x1e, one after in state 8 */
#define OBJ 0x1000u       /* DISPCNT: sprites on */
#define SETTINGS_ROW 0x3f00       /* Game Settings: cursor row (byte); +0x3f01 the row shown, +0x3f02 the column */
#define SETTINGS_ROW_SHOWN 0x3f01
#define NAMING_STATE 0x4e48 /* name entry: state (word): 0 the character and keyboard fading in, 1 entering, 2 accepted */
#define NAMING_ENTERING 1

#define MENU_PAGE 0x19c4      /* field menu (ov012, X): its current page object (Main Menu, Equipment...) */
#define MENU_MAIN 0x02159bf8u /* the Main Menu page's vtable */
#define MENU_EQUIP 0x021595fcu /* the Equipment page's: +4 its state (EQUIP_PICKING: choosing an item), +0x32c the slot */
#define MENU_ITEMS 0x02159774u /* the Inventory page's vtable */
#define MENU_TECHS 0x02159b30u /* the Techs page's */
#define MENU_PARTY 0x021596ccu /* the Party page's vtable */
#define MENU_SAVE 0x02159a10u  /* the Save page's */
#define MENU_OPTIONS 0x02159510u /* the Settings page's: +0xa the tab, +0xc the row, +0xe the option (tab * 4 + row) */
#define OPTIONS_TAB 0xa
#define OPTIONS_ROW 0xc
#define OPTIONS_INDEX 0xe
#define CONFIG 0x020708d0u /* points to the save configuration: Game Mode byte 2 bit 7 (1 = Classic) */
#define EQUIP_STATE 0x4
#define EQUIP_PICKING 5
#define EQUIP_SLOT 0x32c
#define EQUIP_CHARACTER 0x6688 /* field menu scene: the Equipment page's character (id, byte), set as its data is
                                * shown; the page's own +0x332 is a place on the members bar, in the menu's order */

#define CURRENT_SCENE 0x0207088cu /* the running scene's secondary base (scene + 8), set by Scene::run */

static Scene current(void) {
    Scene sc = {SCENE_OTHER, 0};
    uint32_t base = rt_r32(CURRENT_SCENE);
    if (!EGG_SINGLE_SCREEN || base >> 24 != 0x02) return sc;
    sc.obj = base - 8;
    uint32_t vt = rt_r32(base); /* the secondary base's vtable */
    if (rt_overlay_resident(7)) {
        if (vt == 0x0212ad68u) sc.kind = SCENE_LOGOS;
        else if (vt == 0x0212aed4u) sc.kind = SCENE_TITLE;
        else if (vt == 0x0212b300u) sc.kind = SCENE_SETTINGS;
    }
    if (vt == 0x02156880u && rt_overlay_resident(12)) sc.kind = SCENE_NAMING;
    if (vt == 0x02158b24u && rt_overlay_resident(12)) {
        uint32_t page = rt_r32(sc.obj + MENU_PAGE);
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_MAIN) sc.kind = SCENE_MENU;
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_EQUIP) sc.kind = SCENE_EQUIP;
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_ITEMS) sc.kind = SCENE_ITEMS;
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_TECHS) sc.kind = SCENE_TECHS;
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_OPTIONS) sc.kind = SCENE_OPTIONS;
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_PARTY) sc.kind = SCENE_PARTY;
        if (page >> 24 == 0x02 && rt_r32(page) == MENU_SAVE) sc.kind = SCENE_SAVE;
    }
    if (vt == 0x021575f0u && rt_overlay_resident(12)) sc.kind = SCENE_SHOP; /* ov012: a shop */
    if (vt == 0x0219fc8cu && rt_overlay_resident(18)) sc.kind = SCENE_BESTIARY; /* ov018: the menu's Bestiary */
    if (vt == 0x02115558u && rt_overlay_resident(0)) sc.kind = SCENE_FIELD; /* ov000: towns, maps, battles */
    if (vt == 0x0212535cu && rt_overlay_resident(10)) sc.kind = SCENE_WARP;  /* ov010: travel through a time gate */
    if (vt == 0x02120a68u && rt_overlay_resident(2)) sc.kind = SCENE_OVERWORLD; /* ov002: the world map */
    return sc;
}

/* The bottom screen's engine registers (POWCNT1 bit 15: engine A on the top screen). */
static uint32_t bottom_io(void) { return (io_get16(0x304) & 0x8000) ? 0x1000u : 0; }

static int title_waiting(Scene sc) {
    return sc.kind == SCENE_TITLE && rt_r16(sc.obj + TITLE_STATE) == TITLE_WAITING &&
           rt_r16(sc.obj + TITLE_PHASE) < TITLE_MENU;
}

/* Up/Down in Game Settings: the direction the game gets for the press being held (0: not held). */
static uint32_t settings_up, settings_down;

static uint32_t settings_dir(uint32_t keys, uint32_t key, uint32_t *held_as, int wraps, uint32_t other) {
    if (!(keys & key)) {
        *held_as = 0;
        return keys;
    }
    if (!*held_as) *held_as = wraps ? other : key; /* decided when the press starts, kept while it is held */
    return (keys & ~key) | *held_as;
}

/* Game Settings without its Game Mode row (rows 0 Game Mode, 1 Battle Mode, 2 Movies): the cursor leaves
 * row 0 (a pulsed Down, so the scene sees a fresh press once it takes input), and Up/Down wrap between the
 * two other rows. SELECT, the game's "Defaults", is kept for "System settings" (not implemented yet). */
static uint32_t settings_keys(Scene sc, uint32_t keys) {
    keys &= ~KEY_SELECT;
    int row = rt_r8(sc.obj + SETTINGS_ROW);
    if (row == 0) {
        keys &= ~(KEY_UP | KEY_DOWN);
        return rt_frame_count & 2 ? keys | KEY_DOWN : keys;
    }
    keys = settings_dir(keys, KEY_UP, &settings_up, row == 1, KEY_DOWN);
    return settings_dir(keys, KEY_DOWN, &settings_down, row == 2, KEY_UP);
}

/* Equipment page, browsing: the slots form a 2x2 grid on the main screen (Weapon, Helm / Armor, Accessory), so the
 * D-pad moves in it. The game moves through them in a column with Up/Down (Right chooses an item, like A): a
 * press becomes the one Up or Down that reaches the slot in that direction, if there is one. Down and Up within
 * a column skip a slot: the cursor is put on the slot between first (unseen: the game draws it only after
 * moving on), so the game's own move lands on the target. While choosing an item, the list is beside the slot
 * as on the DS, but Left (the game's way back to the slots, like B) is left out: only B goes back. */
static uint32_t equip_held, equip_key; /* the key held for the game until the cursor is on the slot */
static int equip_to = -1, equip_wait;

static uint32_t equip_keys(Scene sc, uint32_t keys) {
    uint32_t page = rt_r32(sc.obj + MENU_PAGE), dirs = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
    uint32_t pressed = keys & ~equip_held;
    equip_held = keys;
    uint32_t state = rt_r32(page + EQUIP_STATE);
    if (state != 0) {
        equip_to = -1;
        return state == EQUIP_PICKING ? keys & ~KEY_LEFT : keys; /* the list: Left would leave it, as B does */
    }
    uint32_t v = rt_r32(page + EQUIP_SLOT);
    int slot = (int)(v & 3), to = -1;
    if (equip_to >= 0) {
        if (equip_to == slot || ++equip_wait > 30) equip_to = -1; /* there, or the game took none */
        return equip_to >= 0 ? (keys & ~dirs) | equip_key : keys & ~dirs;
    }
    if ((pressed & KEY_RIGHT) && !(slot & 1)) to = slot + 1;
    if ((pressed & KEY_LEFT) && (slot & 1)) to = slot - 1;
    if ((pressed & KEY_DOWN) && slot < 2) to = slot + 2;
    if ((pressed & KEY_UP) && slot >= 2) to = slot - 2;
    keys &= ~dirs;
    if (to < 0) return keys;
    if (to - slot == 2 || slot - to == 2) rt_w32(page + EQUIP_SLOT, (v & ~3u) | (uint32_t)(slot + to) / 2);
    equip_to = to, equip_wait = 0, equip_key = to > slot ? KEY_DOWN : KEY_UP;
    return keys | equip_key;
}

/* Field menu, Settings page, without Game Mode (tab 0, row 0: single screen mode always plays Classic, kept
 * so whatever sets it). The cursor leaves row 0 with a pulsed Down, as in Game Settings; Up from Battle
 * Mode puts the cursor on row 0 first (unseen), so the game's own Up wraps to the last option. */
static int options_wrap; /* row 0 stands in for Battle Mode until the game takes the Up */

static uint32_t options_keys(Scene sc, uint32_t keys) {
    uint32_t cfg = rt_r32(CONFIG), page = rt_r32(sc.obj + MENU_PAGE);
    rt_w8(cfg + 2, rt_r8(cfg + 2) | 0x80u);
    int row = rt_r8(page + OPTIONS_ROW);
    if (rt_r8(page + OPTIONS_TAB) != 0) row = -1;
    if (row == 1 && (keys & KEY_UP)) {
        rt_w8(page + OPTIONS_ROW, 0);
        rt_w8(page + OPTIONS_INDEX, 0);
        options_wrap = 1;
        return keys;
    }
    if (row != 0) {
        options_wrap = 0;
        return keys;
    }
    if (options_wrap && (keys & KEY_UP)) return keys;
    options_wrap = 0;
    keys &= ~(KEY_UP | KEY_DOWN | KEY_A);
    return rt_frame_count & 2 ? keys | KEY_DOWN : keys;
}

/* Party page: Y (unused by the game there) switches the main screen between the members and the Dual and
 * Triple techs. The techs view hides the cursor, so it keeps the D-pad and A from the game. */
static int party_techs;
static uint32_t party_held;

static uint32_t party_keys(uint32_t keys) {
    if (keys & ~party_held & KEY_Y) party_techs = !party_techs;
    party_held = keys;
    keys &= ~KEY_Y;
    return party_techs ? keys & ~(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A) : keys;
}

uint32_t ss_game_keys(uint32_t keys) {
    static int seen = SCENE_OTHER;
    Scene sc = current();
    if (sc.kind != seen) { /* a scene started */
        seen = sc.kind;
        title_held = keys;
        settings_up = settings_down = 0;
        equip_to = -1;
        options_wrap = 0;
        party_techs = 0;
        party_held = keys;
    }
    if (sc.kind == SCENE_EQUIP) return equip_keys(sc, keys);
    if (sc.kind == SCENE_SETTINGS) return settings_keys(sc, keys);
    if (sc.kind == SCENE_OPTIONS) return options_keys(sc, keys);
    if (sc.kind == SCENE_PARTY) return party_keys(keys);
    if (!title_waiting(sc)) return keys;
    title_held &= keys;
    return keys & ~title_held ? keys | KEY_START : keys;
}

/* ---- composition ---- */

#define BLACK 0xFF000000u

/* Copies the non-black pixels of rows [y0, y1) of src into dst, moved by (dx, dy). */
static void overlay(uint32_t *dst, const uint32_t *src, int y0, int y1, int dx, int dy) {
    for (int y = y0; y < y1; y++) {
        int ty = y + dy;
        if (ty < 0 || ty >= SCREEN_H) continue;
        for (int x = 0; x < SCREEN_W; x++) {
            int tx = x + dx;
            uint32_t p = src[y * SCREEN_W + x];
            if (p != BLACK && tx >= 0 && tx < SCREEN_W) dst[ty * SCREEN_W + tx] = p;
        }
    }
}

/* Title layout on the main screen: the free area left of the resting pendulum. */
#define FREE_CX 95       /* centre column */
#define FREE_CY 130      /* centre row */
#define MENU_DX (-32)    /* bottom-screen buttons (centred at column 127) moved into it... */
#define MENU_Y1 165      /* ...ending right above the copyright (main rows 166-188), over the logo's lower part */
#define COPYRIGHT_Y0 156 /* bottom-screen rows holding the copyright (BG2)... */
#define COPYRIGHT_Y1 186
#define COPYRIGHT_DY 6   /* ...moved down to the main screen's last rows */

static void compose_title(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, top, SCREEN_W * SCREEN_H * 4);
    overlay(main, bottom, COPYRIGHT_Y0, COPYRIGHT_Y1, 0, COPYRIGHT_DY);
    Scene sc = current();
    /* States before the waiting one set the bottom screen up, "Touch to Start" included: no menu yet. */
    if (sc.kind == SCENE_TITLE && rt_r16(sc.obj + TITLE_STATE) < TITLE_WAITING) return;
    if (title_waiting(sc)) {
        uint32_t io = bottom_io();
        if (!(io_get32(io) & OBJ)) return; /* the game's prompt blinks by turning its sprites off */
        /* Follow the bottom screen's fade (master brightness, darken mode). */
        uint16_t mb = io_get16(io + 0x6C);
        int f = (mb >> 14) == 2 ? (mb & 31) : 0;
        if (f > 16) f = 16;
        uint32_t v = 255u * (uint32_t)(16 - f) / 16;
        const char *prompt = "Press any button to start";
        ui_draw_text(main, prompt, FREE_CX - ui_text_width(prompt) / 2, FREE_CY - 4, BLACK | v << 16 | v << 8 | v);
        return;
    }
    /* Menu: the buttons, wherever the game puts them above the copyright, in the free area. They don't fit
     * between the logo and the copyright, so they end above the copyright and cover part of the logo. */
    int first = -1, last = -1;
    for (int y = 0; y < COPYRIGHT_Y0; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            if (bottom[y * SCREEN_W + x] != BLACK) {
                if (first < 0) first = y;
                last = y;
                break;
            }
        }
    }
    /* Each row from its first to its last lit pixel, black included: the black outlines of the labels must
     * hide the logo under them. */
    for (int y = first; first >= 0 && y <= last; y++) {
        const uint32_t *src = &bottom[y * SCREEN_W];
        int x0 = 0, x1 = SCREEN_W - 1;
        while (x0 <= x1 && src[x0] == BLACK) x0++;
        while (x1 >= x0 && src[x1] == BLACK) x1--;
        int ty = y + MENU_Y1 - (last + 1);
        if (ty < 0) continue;
        for (int x = x0; x <= x1; x++) {
            if (x + MENU_DX >= 0 && x + MENU_DX < SCREEN_W) main[ty * SCREEN_W + x + MENU_DX] = src[x];
        }
    }
}

/* Game Settings: top screen, the highlighted option's title and description box; bottom screen, the options
 * (Game Mode, Battle Mode, Movies), SELECT "Defaults" / START "Accept" and the footer. The main screen stacks
 * full-width bands of both: header, Battle Mode, Movies with the description box under the highlighted one,
 * the buttons with SELECT relabelled, footer (26 + 27 + 27 + 66 + 20 + 26 = 192 rows). */
typedef struct Band { int screen, y0, h; } Band;
static const Band SET_HEADER = {0, 0, 26}, SET_BATTLE = {1, 58, 27}, SET_MOVIES = {1, 98, 27},
                  SET_DESC = {0, 119, 66}, SET_BUTTONS = {1, 142, 20}, SET_FOOTER = {1, 166, 26};
#define SELECT_LABEL_X0 46 /* the "Defaults" label (x 48-84 of the SELECT button, x 16-119)... */
#define SELECT_LABEL_X1 117
#define SELECT_LABEL_Y0 5  /* ...rows 147-156 of the bottom screen, painted over with... */
#define SELECT_LABEL_Y1 15
#define SELECT_FILL_X 86   /* ...the plain button texture at x 86-115 */
#define SELECT_FILL_W 30

static int put_band(uint32_t *main, int y, Band b, const uint32_t *top, const uint32_t *bottom) {
    memcpy(&main[y * SCREEN_W], &(b.screen ? bottom : top)[b.y0 * SCREEN_W], (size_t)b.h * SCREEN_W * 4);
    return y + b.h;
}

/* The SELECT button's "Defaults" relabelled "System settings" (the runtime's menu), the button's top at row y. */
static void relabel_select(uint32_t *main, int y) {
    for (int r = SELECT_LABEL_Y0; r < SELECT_LABEL_Y1; r++) {
        uint32_t *row = &main[(y + r) * SCREEN_W];
        for (int x = SELECT_LABEL_X0; x < SELECT_LABEL_X1; x++) {
            row[x] = row[SELECT_FILL_X + (x - SELECT_LABEL_X0) % SELECT_FILL_W];
        }
    }
    ui_draw_text(main, "System settings", SELECT_LABEL_X0 + 2, y + 6, 0xFFFFFFFFu);
}

static void compose_settings(Scene sc, uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    int movies = rt_r8(sc.obj + SETTINGS_ROW_SHOWN) == 2;
    int y = put_band(main, 0, SET_HEADER, top, bottom);
    y = put_band(main, y, SET_BATTLE, top, bottom);
    if (!movies) y = put_band(main, y, SET_DESC, top, bottom);
    y = put_band(main, y, SET_MOVIES, top, bottom);
    if (movies) y = put_band(main, y, SET_DESC, top, bottom);
    int buttons = y;
    y = put_band(main, y, SET_BUTTONS, top, bottom);
    put_band(main, y, SET_FOOTER, top, bottom);
    relabel_select(main, buttons);
}

/* Field menu, Main Menu page (see the top). Top screen: title rows 0-22, party panels 127x47 at x 0, y 24 + 48 n
 * (absent members: no panel), location panel x 128-255, y 136-166 (two text lines, either may be empty), time
 * and gold bar rows 168-190. Bottom screen: icon rows 36-83 and 92-139 (the cursor included), description
 * rows 168-191, backdrop elsewhere. Main screen: cards rows 1-42, icons 44-91 and 92-139, bar 141-163,
 * description 168-191 (as on the bottom screen). Text glyphs: 6 rows (7 with descenders), outlined. */
#define CARD_W 85
#define CARD_H 42
#define BAR_Y 168    /* the bar on the top screen... */
#define BAR_H 23
#define BAR_TO 141   /* ...and on the main screen */
#define BAR_TEXT 8   /* bar rows of the text crops below (glyphs from row 9) */

/* Copies a w x h rectangle between two 256-wide screens, clipped to dst. */
static void blit(uint32_t *dst, int dx, int dy, const uint32_t *src, int sx, int sy, int w, int h) {
    for (int y = 0; y < h; y++) {
        if (dy + y < 0 || dy + y >= SCREEN_H) continue;
        for (int x = 0; x < w; x++) {
            if (dx + x >= 0 && dx + x < SCREEN_W) dst[(dy + y) * SCREEN_W + dx + x] = src[(sy + y) * SCREEN_W + sx + x];
        }
    }
}

static int green(uint32_t p) { return (p >> 8) & 0xFF; }

/* A member's panel: its top bevel (a light row over a dark one) under the black gap; the backdrop where there
 * is none. Relative, so it holds while the menu fades. */
static int panel_at(const uint32_t *top, int py) {
    return green(top[(py + 1) * SCREEN_W + 60]) > 2 * green(top[(py + 2) * SCREEN_W + 60]) + 4;
}

/* Text on the panels is white with a black shadow, neutral grey while the menu fades in (the text fades from
 * black, the stone does not): the stone (a dithered blue-grey gradient, lighter at the top) is never neutral. */
static int text_px(uint32_t p) { return (p & 0xFF) == ((p >> 8) & 0xFF) && (p & 0xFF) == ((p >> 16) & 0xFF); }

/* Copies the text pixels of a w x h rectangle (see text_px) between two 256-wide screens. */
static void blit_text(uint32_t *dst, int dx, int dy, const uint32_t *src, int sx, int sy, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = src[(sy + y) * SCREEN_W + sx + x];
            if (text_px(p)) dst[(dy + y) * SCREEN_W + dx + x] = p;
        }
    }
}

/* The member's panel at (0, py) rebuilt as a card at (cx, cy): its frame, its stone with the text taken out
 * (card row y shows panel row y + 2, so the gradient matches), the portrait, then the text: name and LV on one
 * line over HP and MP. */
static void menu_card(uint32_t *main, const uint32_t *top, int py, int cx, int cy) {
    enum { SX = 34, IN = CARD_W - 8 }; /* the stone shown: panel columns SX.., under the text */
    uint32_t stone[IN];
    for (int y = 4; y < CARD_H - 4; y++) {
        const uint32_t *row = &top[(py + y + 2) * SCREEN_W + SX];
        for (int x = 0; x < IN; x++) { /* a text pixel takes the stone 8 columns left (glyphs are narrower) */
            stone[x] = text_px(row[x]) ? (x >= 8 ? stone[x - 8] : row[x - 8]) : row[x];
        }
        memcpy(&main[(cy + y) * SCREEN_W + cx + 4], stone, sizeof stone);
    }
    blit(main, cx, cy, top, 0, py, CARD_W - 4, 4);                                  /* top edge */
    blit(main, cx + CARD_W - 4, cy, top, 123, py, 4, 4);
    blit(main, cx, cy + CARD_H - 4, top, 0, py + 43, CARD_W - 4, 4);                /* bottom edge */
    blit(main, cx + CARD_W - 4, cy + CARD_H - 4, top, 123, py + 43, 4, 4);
    for (int y = 4; y < CARD_H - 4; y++) {                                            /* sides, row for row */
        blit(main, cx, cy + y, top, 0, py + y + 2, 4, 1);
        blit(main, cx + CARD_W - 4, cy + y, top, 123, py + y + 2, 4, 1);
    }
    blit(main, cx + 3, cy + 4, top, 12, py + 6, 16, 34);       /* portrait, on its own rows */
    blit_text(main, cx + 20, cy + 5, top, 33, py + 9, 33, 7);  /* name */
    blit_text(main, cx + 55, cy + 5, top, 33, py + 21, 26, 7); /* LV n */
    blit_text(main, cx + 20, cy + 16, top, 60, py + 21, 61, 7); /* HP n/ n */
    blit_text(main, cx + 20, cy + 26, top, 60, py + 31, 61, 7); /* MP n/ n */
}

/* Columns of the outlined text in rows [y0, y0 + h) of the top screen between x0 and x1 (lit glyph pixels: text
 * other than the shadow), with the outline; 0 when there is none. */
static int text_span(const uint32_t *top, int y0, int h, int x0, int x1, int *from) {
    int a = x1, b = x0 - 1;
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x1; x++) {
            uint32_t p = top[y * SCREEN_W + x];
            if (text_px(p) && p != BLACK) a = SDL_min(a, x), b = SDL_max(b, x);
        }
    }
    if (b < a) return 0;
    *from = a - 1;
    return b - a + 3;
}

/* The top screen's time and gold bar at row y of the main screen (Main Menu and Save pages): emptied with its own
 * text-free middle (x 95-184: the time ends by x 92, 7 digits of gold start from x 191), then TIME and the time
 * at the left (5 columns apart, as the gold and its G), the location (the panel above the bar, x 128-255 rows
 * 136-167, when there: the Save page's empty files have none) centred, the gold at the right. */
static void menu_bar(uint32_t *main, const uint32_t *top, int y) {
    blit(main, 0, y, top, 0, BAR_Y, SCREEN_W, BAR_H);
    for (int x = 4; x < SCREEN_W - 4; x += 90) blit(main, x, y, top, 95, BAR_Y, SDL_min(90, SCREEN_W - 4 - x), BAR_H);
    int ft = 0, fv = 0, fg = 0, ty = BAR_Y + BAR_TEXT + 1;
    int wt = text_span(top, ty, 7, 4, 50, &ft), wv = text_span(top, ty, 7, 50, 128, &fv), wg = text_span(top, ty, 7, 128, 252, &fg);
    blit(main, 6, y + BAR_TEXT, top, ft, BAR_Y + BAR_TEXT, wt, 9);                    /* TIME */
    blit(main, 6 + wt + 3, y + BAR_TEXT, top, fv, BAR_Y + BAR_TEXT, wv, 9);           /* the time */
    blit(main, SCREEN_W - 6 - wg, y + BAR_TEXT, top, fg, BAR_Y + BAR_TEXT, wg, 9);   /* the gold and G */
    if (text_px(top[140 * SCREEN_W + 200])) return; /* backdrop, not the panel's stone */
    int f1 = 0, f2 = 0, w1 = text_span(top, 143, 7, 132, 252, &f1), w2 = text_span(top, 154, 7, 132, 252, &f2);
    int gap = w1 && w2 ? 4 : 0, x = (SCREEN_W - w1 - gap - w2) / 2;
    if (w1) blit_text(main, x, y + BAR_TEXT, top, f1, 142, w1, 9); /* the text alone: the bar's stone around it */
    if (w2) blit_text(main, x + w1 + gap, y + BAR_TEXT, top, f2, 153, w2, 9);
}

static void compose_menu(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);                    /* backdrop, second icon row, description */
    for (int y = 83; y >= 36; y--) memcpy(&main[(y + 8) * SCREEN_W], &bottom[y * SCREEN_W], SCREEN_W * 4); /* first row */
    memcpy(&main[36 * SCREEN_W], &bottom[28 * SCREEN_W], 8 * SCREEN_W * 4); /* backdrop where it was */
    for (int i = 0; i < 3; i++) {
        if (panel_at(top, 24 + 48 * i)) menu_card(main, top, 24 + 48 * i, i * CARD_W, 1);
    }
    menu_bar(main, top, BAR_TO);
}

/* Field menu, Equipment page (see the top). Top screen: title rows 0-22, portrait x 8-50 and element rows 31-73,
 * element icon 16x16 at (64, 31), name and HP rows 51-60, LV and MP rows 65-74 (from x 62), stat lines (glyphs) at rows 89, 106, 122, 138, 154,
 * EXP at 177. Bottom screen: members bar rows 0-22, slots 120x28 at x 4, y 36 + 28 n (label and button), the
 * item list while choosing at x 128, y 36 + 28 n (button rows, the cursor framed in cyan), description rows
 * 168-191. Main screen: members 0-22, the two info rows 23-42 (left of them the portrait's eye strip, portrait
 * rows 43-62, and the element icon), stat lines
 * 43-108 (11 rows each), slots 110-165, description. Whole rows are moved where possible: the backdrop is
 * brushed along the rows. */
#define EQ_ROW 28
/* The portrait's rows shown (20 from this one), by character (0 Crono, 1 Marle, 2 Lucca, 3 Robo, 4 Frog, 5 Ayla,
 * 6 Magus): the eyes (Frog's are at the top). */
static const int8_t EYES[7] = {14, 17, 13, 10, 0, 17, 12};
#define EQ_GRID 110

static int cyan(uint32_t p) {
    int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    return g > 2 * r + 60 && b > 2 * r + 60;
}

/* Slot n of the bottom screen at (dx, dy) of the main screen, without the backdrop around it (it is shaded
 * across the screen, darker at the sides): the button (cursor frame included, x 8-119, from row 4) and the
 * label's text (white with a black shadow; grey while fading in, the backdrop stays below 80). */
static void equip_slot(uint32_t *main, const uint32_t *bottom, int n, int dx, int dy) {
    int sy = 36 + EQ_ROW * n;
    blit(main, dx + 4, dy + 4, bottom, 8, sy + 4, 112, EQ_ROW - 4);
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 64; x++) {
            uint32_t p = bottom[(sy + y) * SCREEN_W + 4 + x];
            if (text_px(p) && ((p & 0xFF) == 0 || (p & 0xFF) >= 80)) main[(dy + y) * SCREEN_W + dx + x] = p;
        }
    }
}

static int equip_portrait = -1, equip_portrait_wait; /* the character whose portrait is on screen (-1: unknown) */
static int equip_row; /* choosing: the list row (0-3) the cursor was last seen on */

/* Whether the item list is on the bottom screen: its buttons (blue-grey) across x 200, where the slots leave
 * the backdrop (greys). The page's state leaves EQUIP_PICKING for a few frames while the list scrolls. */
static int equip_list_shown(const uint32_t *bottom) {
    for (int y = 36; y < 36 + 4 * EQ_ROW; y += 4) {
        if (!text_px(bottom[y * SCREEN_W + 200])) return 1;
    }
    return 0;
}

static void compose_equip(Scene sc, uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    uint32_t page = rt_r32(sc.obj + MENU_PAGE);
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4); /* members bar, the grid's backdrop, description */
    memcpy(&main[23 * SCREEN_W], &top[51 * SCREEN_W], 10 * SCREEN_W * 4); /* name, HP */
    memcpy(&main[33 * SCREEN_W], &top[65 * SCREEN_W], 10 * SCREEN_W * 4); /* LV, MP */
    for (int y = 23; y < 43; y++) { /* the portrait's columns: the row's empty end (x 216-255) */
        blit(main, 0, y, main, 216, y, 40, 1);
        blit(main, 40, y, main, 216, y, 22, 1);
    }
    /* The member changes at once on L/R, the portrait only after fading out to black: the strip follows the
     * portrait on screen (or the member, if no black frame came within 30 frames). */
    int member = rt_r8(sc.obj + EQUIP_CHARACTER);
    if (member != equip_portrait) {
        int black = 1;
        for (int y = 31; y < 74 && black; y += 3) {
            for (int x = 8; x < 56 && black; x += 3) black = top[y * SCREEN_W + x] == BLACK;
        }
        if (equip_portrait < 0 || black || ++equip_portrait_wait > 30) equip_portrait = member, equip_portrait_wait = 0;
    }
    int eyes = equip_portrait < 7 ? EYES[equip_portrait] : 12;
    blit(main, 2, 23, top, 11, 31 + eyes, 42, 20); /* a full-size strip of the portrait across the eyes */
    blit(main, 46, 25, top, 64, 31, 16, 16); /* the element icon */
    static const int lines[] = {89, 106, 122, 138, 154, 177};
    for (int i = 0; i < 6; i++) memcpy(&main[(43 + 11 * i) * SCREEN_W], &top[(lines[i] - 2) * SCREEN_W], 11 * SCREEN_W * 4);
    memcpy(&main[(EQ_GRID - 1) * SCREEN_W], &top[187 * SCREEN_W], SCREEN_W * 4); /* backdrop down to the grid */
    for (int y = EQ_GRID; y < EQ_GRID + 2 * EQ_ROW; y += 20) { /* the grid's backdrop: clean rows 148-167 */
        memcpy(&main[y * SCREEN_W], &bottom[148 * SCREEN_W], (size_t)SDL_min(20, EQ_GRID + 2 * EQ_ROW - y) * SCREEN_W * 4);
    }
    int slot = (int)(rt_r32(page + EQUIP_SLOT) & 3);
    if (rt_r32(page + EQUIP_STATE) != EQUIP_PICKING && !equip_list_shown(bottom)) {
        for (int i = 0; i < 4; i++) equip_slot(main, bottom, i, (i & 1) * 128, EQ_GRID + (i >> 1) * EQ_ROW);
        equip_row = 0;
        return;
    }
    /* Choosing: the slot at the left; the list's two rows around the cursor at the right (its own columns). The
     * cursor is hidden while the list scrolls: the rows stay those it was last on, the items sliding through. */
    equip_slot(main, bottom, slot, 0, EQ_GRID);
    for (int i = 0; i < 4; i++) {
        for (int y = 36 + EQ_ROW * i; y < 36 + EQ_ROW * (i + 1); y++) {
            if (cyan(bottom[y * SCREEN_W + 129]) || cyan(bottom[y * SCREEN_W + 130])) equip_row = i;
        }
    }
    int first = equip_row & ~1;
    blit(main, 128, EQ_GRID, bottom, 128, 36 + EQ_ROW * first, 128, 2 * EQ_ROW);
}

/* Field menu, Inventory and Techs pages (see the top). Top screen: a member every 56 rows from row 24, name (x 54-97) and
 * HP (x 150-233) at rows 32-41, MP at 46-55. Bottom screen, choosing who uses the item: the item's bar rows
 * 38-63, the member cards from row 72, backdrop rows 23-39. Text is white with a black shadow on a backdrop
 * that stays below 80 (neutral grey while fading in). */
static void blit_light_text(uint32_t *dst, int dx, int dy, const uint32_t *src, int sx, int sy, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = src[(sy + y) * SCREEN_W + sx + x];
            if (text_px(p) && ((p & 0xFF) == 0 || (p & 0xFF) >= 80)) dst[(dy + y) * SCREEN_W + dx + x] = p;
        }
    }
}

/* Choosing who uses an item or tech (Inventory and Techs alike): the item's or tech's bar moved to the top, a
 * line per member under it, the cards as they are. Returns 0 when not choosing. The page's state (8-9 in the
 * Inventory) starts a frame before the cards and ends a frame before they go, so this goes by the bar itself:
 * stone across the middle (x 128) where the lists have backdrop. The Techs list's buttons pass there while it
 * scrolls, under its categories, which leave a black gap at (128, 40) the bar covers. */
static int compose_targets(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    if (text_px(bottom[50 * SCREEN_W + 128]) || bottom[40 * SCREEN_W + 128] == BLACK) return 0;
    for (int y = 0; y < 72; y += 17) memcpy(&main[y * SCREEN_W], &bottom[23 * SCREEN_W], (size_t)SDL_min(17, 72 - y) * SCREEN_W * 4);
    memcpy(main, &bottom[38 * SCREEN_W], 26 * SCREEN_W * 4); /* the bar */
    for (int i = 0; i < 3; i++) {
        int sy = 56 * i, y = 29 + 11 * i;
        blit_light_text(main, 8, y, top, 54, sy + 32, 44, 10);   /* name */
        blit_light_text(main, 70, y, top, 150, sy + 32, 84, 10); /* HP n/ n */
        blit_light_text(main, 162, y, top, 150, sy + 46, 84, 10); /* MP n/ n */
    }
    return 1;
}

static void compose_items(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);
    compose_targets(main, top, bottom);
}

/* Techs page, the list: the bottom screen, with the member's HP and MP (top screen, first member's lines) in the
 * empty rows between the category and the list; the tech's name and cost on the top screen are on its row.
 * While the list scrolls its buttons pass through those rows (hidden only from row 47 up): the rows are kept
 * from the last frame at rest (backdrop at x 8, where the buttons start) and put back under the line. */
#define TECHS_BAND_Y 48
#define TECHS_BAND_H 16
static uint32_t techs_band[TECHS_BAND_H * SCREEN_W];
static int techs_band_kept;

static void compose_techs(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);
    if (compose_targets(main, top, bottom)) return;
    int rest = 1;
    for (int y = TECHS_BAND_Y; y < TECHS_BAND_Y + TECHS_BAND_H; y++) rest &= text_px(bottom[y * SCREEN_W + 8]);
    if (rest) {
        memcpy(techs_band, &bottom[TECHS_BAND_Y * SCREEN_W], sizeof techs_band);
        techs_band_kept = 1;
    } else if (techs_band_kept) {
        memcpy(&main[TECHS_BAND_Y * SCREEN_W], techs_band, sizeof techs_band);
    }
    blit_light_text(main, 30, 51, top, 150, 32, 84, 10);  /* HP n/ n */
    blit_light_text(main, 142, 51, top, 150, 46, 84, 10); /* MP n/ n */
}

/* Field menu, Save page (see the top). Top screen: the highlighted file's party as the Main Menu's (cards every 48
 * rows from row 24, x 0-127, LV and HP at card row 20, MP at 31, from x 33), its reserve every 28 rows from row 25
 * (x 128-255, LV and HP at row 3, MP at 14, from x 160), the location and the bar as the Main Menu's. Bottom
 * screen: the files every 48 rows from row 16 (40 rows, x 8-247, the cursor framed in cyan), a used one with its
 * number at x 21-28 rows 15-23 and the members' portraits (16x16 at row 20: three, a gap, the others), backdrop
 * rows 152-167, the description from row 168. Main screen: the files narrowed to x 2-125 from row 4 (a used one's
 * number beside its name, the portraits packed), the highlighted file's members on the right a line each (its
 * portrait, LV and HP, MP), the bar at row 144, the description. Text: white with a black shadow. While a file is
 * confirmed (Yes / No in place of the two others) the main screen shows the bottom screen. */
#define SAVE_FILE_Y 16
#define SAVE_FILE_H 40
#define SAVE_ROW 48
#define SAVE_TO 4
#define SAVE_LINE_H 20
#define SAVE_BAR_TO 144
static const uint8_t SAVE_PORTRAIT_X[7] = {40, 56, 72, 92, 108, 124, 140};

static void save_text(uint32_t *main, int dx, int dy, const uint32_t *top, int sx, int sy, int w) {
    for (int y = 0; y < 9; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = top[(sy + y) * SCREEN_W + sx + x];
            if (text_px(p) && ((p & 0xFF) == 0 || (p & 0xFF) >= 200)) main[(dy + y) * SCREEN_W + dx + x] = p;
        }
    }
}

static int save_portrait(const uint32_t *bottom, int sy, int k) { /* whether the file at row sy has member k */
    for (int y = sy + 22; y < sy + 34; y += 2) {
        for (int x = SAVE_PORTRAIT_X[k] + 2; x < SAVE_PORTRAIT_X[k] + 14; x += 2) {
            uint32_t p = bottom[y * SCREEN_W + x];
            int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
            if (SDL_abs(r - b) > 40 || SDL_abs(r - g) > 40) return 1; /* the stone is grey-blue */
        }
    }
    return 0;
}

static int save_shown = -1; /* the row of the file last seen highlighted (the cursor pulses up to near white) */

static void compose_save(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);
    uint32_t stone = bottom[(SAVE_FILE_Y + SAVE_ROW + 20) * SCREEN_W + 128]; /* the second file's bluish stone... */
    if (((stone >> 16) & 0xFF) < (stone & 0xFF) + 8) {                       /* ...or the confirmation's grey */
        save_shown = -1;
        return;
    }
    for (int y = 0; y < SAVE_BAR_TO; y += 16) blit(main, 0, y, bottom, 0, 152, SCREEN_W, SDL_min(16, SAVE_BAR_TO - y));
    for (int i = 0; i < 3; i++) {
        int sy = SAVE_FILE_Y + SAVE_ROW * i, dy = SAVE_TO + SAVE_ROW * i;
        uint32_t c = bottom[(sy + 20) * SCREEN_W + 9];
        int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        if (g > r + 40 && b > r + 40) save_shown = sy;
        blit(main, 2, dy, bottom, 8, sy, 116, SAVE_FILE_H);
        blit(main, 118, dy, bottom, 240, sy, 8, SAVE_FILE_H); /* its right end */
        int used = 0;
        for (int k = 0; k < 7; k++) used |= save_portrait(bottom, sy, k);
        if (!used) continue;
        blit(main, 8, dy + 5, bottom, 160, sy + 5, 24, 31); /* stone over the number's place... */
        blit(main, 15, dy + 5, bottom, 21, sy + 15, 8, 9);  /* ...the number up beside the name */
        for (int k = 0; k < 7; k++) blit(main, 10 + 16 * k, dy + 20, bottom, SAVE_PORTRAIT_X[k], sy + 20, 16, 16);
    }
    for (int k = 0; save_shown >= 0 && k < 7; k++) {
        if (!save_portrait(bottom, save_shown, k)) continue;
        int y = 2 + SAVE_LINE_H * k;
        blit(main, 132, y + 2, bottom, SAVE_PORTRAIT_X[k], save_shown + 20, 16, 16);
        int ty = k < 3 ? 24 + 48 * k + 20 : 25 + 28 * (k - 3) + 3, tx = k < 3 ? 33 : 160;
        save_text(main, 160, y + 1, top, tx, ty, 90);       /* LV, HP */
        save_text(main, 160, y + 11, top, tx, ty + 11, 90); /* MP */
    }
    menu_bar(main, top, SAVE_BAR_TO);
}

/* Field menu, Party page (see the top). Top screen: title rows 0-23, the party's sprites x 80-175 rows 24-62 on a
 * backdrop of greys up to 33, the tech boxes below. Bottom screen: the cards in two columns every 42 rows (the
 * party's three on the left, so x 0-127 rows 126-167 stays free), the help bar from row 168 (text rows 177-183).
 * The backdrop is greys only (lighter towards the middle); the sprites' greys and outlines count as the sprite
 * next to one of its coloured pixels. */
#define PARTY_SPRITES_X 80
#define PARTY_SPRITES_Y 24
#define PARTY_SPRITES_W 96
#define PARTY_SPRITES_H 39 /* row 63 borders the boxes' bevel */
#define PARTY_SLOT_X 16
#define PARTY_SLOT_Y 126
#define PARTY_HINT_RIGHT 214 /* the help bar's text area ends here (the B button follows) */
#define PARTY_HINT_Y 176

static void compose_party(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    const char *hint = party_techs ? "Y: Party" : "Y: Techs";
    if (party_techs) {
        memcpy(main, top, SCREEN_W * SCREEN_H * 4);
        ui_draw_text(main, hint, SCREEN_W - 8 - ui_text_width(hint), 8, 0xFFFFFFFFu);
        return;
    }
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);
    for (int y = 0; y < PARTY_SPRITES_H; y++) {
        for (int x = 0; x < PARTY_SPRITES_W; x++) {
            const uint32_t *q = &top[(PARTY_SPRITES_Y + y) * SCREEN_W + PARTY_SPRITES_X + x];
            int sprite = !text_px(*q) || !text_px(q[-1]) || !text_px(q[1]) || !text_px(q[-SCREEN_W]) ||
                         !text_px(q[SCREEN_W]);
            if (sprite) main[(PARTY_SLOT_Y + y) * SCREEN_W + PARTY_SLOT_X + x] = *q;
        }
    }
    ui_draw_text(main, hint, PARTY_HINT_RIGHT - ui_text_width(hint), PARTY_HINT_Y, 0xFFFFFFFFu);
}

/* Field menu, Settings page (see the top). Bottom screen: tab bar rows 0-31, the four options every 28 rows
 * from row 32 (24 tall), plain backdrop rows 140-147, "Defaults" and the description below. On tab 0 the main
 * screen leaves out Game Mode: the three other options move up a row, the backdrop fills the row freed. */
#define OPTION_Y 32
#define OPTION_H 28
#define OPTIONS_END 144
#define BACKDROP_Y 140
#define OPTIONS_SELECT_Y 146 /* the SELECT button, as relabel_select places it (label rows 151-160) */

static void compose_options(Scene sc, uint32_t *main, const uint32_t *bottom) {
    memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);
    relabel_select(main, OPTIONS_SELECT_Y);
    if (rt_r8(rt_r32(sc.obj + MENU_PAGE) + OPTIONS_TAB) != 0) return;
    blit(main, 0, OPTION_Y, bottom, 0, OPTION_Y + OPTION_H, SCREEN_W, OPTIONS_END - OPTION_Y - OPTION_H);
    for (int y = OPTIONS_END - OPTION_H; y < OPTIONS_END; y++)
        blit(main, 0, y, bottom, 0, BACKDROP_Y + (y - OPTIONS_END + OPTION_H) % (OPTIONS_END - BACKDROP_Y), SCREEN_W, 1);
}

/* Field menu, Bestiary page (see the top). Top screen: title rows 0-23, the monster in x 0-119, the panel from
 * x 120. Bottom screen: tabs rows 0-23, list entries every 28 rows from row 40 (number x 18-47, name from x 78,
 * glyphs from row 49 + 28 n, the cursor framed in cyan from x 16), "Discovered" and the percentage at rows
 * 176-183. Text: white, grey (165) or yellow (the selected known name) with a black shadow; the buttons' bevels
 * are greys below 160. Main screen: tabs, the monster rows 24-133, the list lines 138 + 11 n, the count at 182. */
static int bestiary_text(uint32_t p) {
    int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    return (text_px(p) && (r == 0 || r >= 160)) || (r > b + 60 && g > b + 40);
}

static void blit_bestiary_text(uint32_t *main, int dx, int dy, const uint32_t *bottom, int sx, int sy, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = bottom[(sy + y) * SCREEN_W + sx + x];
            if (bestiary_text(p)) main[(dy + y) * SCREEN_W + dx + x] = p;
        }
    }
}

static void compose_bestiary(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, top, SCREEN_W * SCREEN_H * 4);
    memcpy(main, bottom, 24 * SCREEN_W * 4); /* the tabs over the title */
    for (int y = 134; y < SCREEN_H; y++) blit(main, 0, y, top, 0, 24 + (y - 134) % 10, 120, 1); /* backdrop under the monster */
    for (int i = 0; i < 4; i++) {
        int sy = 49 + 28 * i, y = 138 + 11 * i;
        blit_bestiary_text(main, 6, y, bottom, 22, sy, 26, 8);  /* number */
        blit_bestiary_text(main, 30, y, bottom, 78, sy, 88, 8); /* name */
        uint32_t c = bottom[(40 + 28 * i + 10) * SCREEN_W + 16];
        if (cyan(c)) { /* the cursor: a frame round the line, in its colour (it pulses) */
            for (int x = 1; x <= 118; x++) main[(y - 2) * SCREEN_W + x] = main[(y + 8) * SCREEN_W + x] = c;
            for (int v = y - 2; v <= y + 8; v++) main[v * SCREEN_W + 1] = main[v * SCREEN_W + 118] = c;
        }
    }
    blit_bestiary_text(main, 4, 182, bottom, 4, 176, 86, 8);    /* Discovered: n */
    blit_bestiary_text(main, 84, 182, bottom, 176, 176, 36, 8); /* n % */
}

/* Shops (see the top). Top screen: title rows 0-23, the members in two columns of 128 (the party at the left, the
 * others at the right), a member every 40 rows from row 24: the sprite x 16-31, the name x 40-67 at rows 10-17 of
 * the member's 40, then two lines of text (9 rows from row 21: attack, from row 31: defense): the icon x 40-48,
 * the value right-aligned at x 80, and for a member who can use the item highlighted an arrow x 91-97 and the
 * value with it from x 104 (blue: higher, red: lower). Text and icons are white, black or coloured on a dark
 * backdrop (never black, no channel above 80; its tint follows the menu colour). Bottom screen: tabs rows 0-23,
 * backdrop rows 24-37, everything else in rows 38-167, the help bar (empty) from row 168. Main screen: tabs, the
 * bottom screen from row 38 at row 24, then the members as lines of text in a 3 x 3 grid (the party on the first
 * line): the name and the stat the item changes (defense if only defense values are coloured, else attack), these
 * spans of its line packed. */
#define SHOP_SHIFT 14
#define SHOP_GRID_Y 154
#define SHOP_LINE_H 11
static const struct { uint8_t x, w; } SHOP_SPANS[] = {{40, 9}, {66, 15}, {91, 7}, {104, 16}};

static int shop_fg(uint32_t p) {
    return p == BLACK || (p & 0xFF) > 80 || ((p >> 8) & 0xFF) > 80 || ((p >> 16) & 0xFF) > 80;
}

static void blit_shop_fg(uint32_t *main, int dx, int dy, const uint32_t *top, int sx, int sy, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = top[(sy + y) * SCREEN_W + sx + x];
            if (shop_fg(p)) main[(dy + y) * SCREEN_W + dx + x] = p;
        }
    }
}

/* Whether any member's line (from row 21: attack, 31: defense) has a value with the item in colour. */
static int shop_changed(const uint32_t *top, int line) {
    for (int m = 0; m < 8; m++) {
        for (int y = 0; y < 7; y++) {
            const uint32_t *row = &top[(24 + 40 * (m & 3) + line + y) * SCREEN_W + 128 * (m >> 2)];
            for (int x = 104; x < 120; x++) {
                int r = row[x] & 0xFF, g = (row[x] >> 8) & 0xFF, b = (row[x] >> 16) & 0xFF;
                if (SDL_max(r, b) >= 200 && SDL_min(r, g) <= 100) return 1; /* red or blue text, not the backdrop */
            }
        }
    }
    return 0;
}

static void compose_shop(uint32_t *main, const uint32_t *top, const uint32_t *bottom) {
    memcpy(main, bottom, 24 * SCREEN_W * 4);
    memcpy(&main[24 * SCREEN_W], &bottom[(24 + SHOP_SHIFT) * SCREEN_W], (SHOP_GRID_Y - 24) * SCREEN_W * 4);
    for (int y = SHOP_GRID_Y; y < SCREEN_H; y += SHOP_SHIFT) /* the backdrop the shift left out */
        memcpy(&main[y * SCREEN_W], &bottom[24 * SCREEN_W], (size_t)SDL_min(SHOP_SHIFT, SCREEN_H - y) * SCREEN_W * 4);
    int line = shop_changed(top, 21) || !shop_changed(top, 31) ? 21 : 31;
    for (int m = 0, n = 0; m < 8; m++) { /* the party (three at most), then the others */
        int sx = 128 * (m >> 2), sy = 24 + 40 * (m & 3);
        int named = 0; /* a name: text in its place */
        for (int y = sy + 10; y < sy + 17 && m != 3; y++) {
            for (int x = sx + 40; x < sx + 68; x++) named |= top[y * SCREEN_W + x] == 0xFFFFFFFFu;
        }
        if (!named) {
            if (m < 3) n = 3; /* the party ends: the others from the second line */
            continue;
        }
        int x = 85 * (n % 3) + 2, y = SHOP_GRID_Y + 3 + SHOP_LINE_H * (n / 3);
        n++;
        blit_shop_fg(main, x, y, top, sx + 40, sy + 9, 28, 9); /* the name */
        x += 29;
        for (size_t k = 0; k < SDL_arraysize(SHOP_SPANS); k++) {
            blit_shop_fg(main, x, y, top, sx + SHOP_SPANS[k].x, sy + line, SHOP_SPANS[k].w, 9);
            x += SHOP_SPANS[k].w + 1;
        }
    }
}

/* Whether a screen is one plain colour. */
static int blank(const uint32_t *px) {
    for (int i = 1; i < SCREEN_W * SCREEN_H; i++) {
        if (px[i] != px[0]) return 0;
    }
    return 1;
}

/* The field's pause (ov000 func 0x0210155c, START): dims both screens and loops until START, with the byte
 * at *(0x0207b520) + 0x407 set to 0xff meanwhile. */
static int field_paused(void) {
    if (!rt_overlay_resident(0)) return 0;
    uint32_t base = rt_r32(0x0207b520u);
    return base >> 24 == 0x02 && rt_r8(base + 0x407) == 0xff;
}

int ss_in_field(void) { return current().kind == SCENE_FIELD; }
int ss_in_warp(void) { return current().kind == SCENE_WARP; }
int ss_in_overworld(void) { return current().kind == SCENE_OVERWORLD; }

static SDL_AtomicInt menu_place;

int ss_menu_place(void) { return SDL_GetAtomicInt(&menu_place); }

int ss_compose(uint32_t *main, const uint32_t *top, const uint32_t *bottom, const uint32_t *wide, int wide_w) {
    Scene sc = current();
    if (sc.kind != SCENE_EQUIP) equip_portrait = -1; /* the page opens with its member's portrait */
    int paused = EGG_SINGLE_SCREEN && sc.kind == SCENE_FIELD && field_paused();
    int title_menu = sc.kind == SCENE_TITLE && rt_r16(sc.obj + TITLE_PHASE) == TITLE_MENU;
    int settings = sc.kind == SCENE_SETTINGS || sc.kind == SCENE_OPTIONS; /* SELECT relabelled "System settings" */
    SDL_SetAtomicInt(&menu_place, settings     ? SS_MENU_GAME_SETTINGS
                                  : paused     ? SS_MENU_PAUSE
                                  : title_menu ? SS_MENU_TITLE
                                               : SS_MENU_NONE);
    if (sc.kind == SCENE_FIELD || sc.kind == SCENE_WARP || sc.kind == SCENE_OVERWORLD) { /* widescreen: the top screen with its margins */
        memcpy(main, wide, (size_t)wide_w * SCREEN_H * 4);
        if (paused) {
            const char *hint = "Press SELECT to open System Settings";
            ui_draw_text_in(main, wide_w, hint, (wide_w - ui_text_width(hint)) / 2, SCREEN_H - UI_FONT_H - 6, 0xFFFFFFFFu);
        }
        return wide_w;
    }
    switch (sc.kind) {
    case SCENE_LOGOS:
        if (blank(top)) {
            memcpy(main, bottom, SCREEN_W * SCREEN_H * 4);
            break;
        }
        /* Square Enix (top rows 85-107) above Mobiclip (bottom rows 50-141), centred together. */
        for (int y = 0; y < SCREEN_H; y++) {
            const uint32_t *src = y < 60 ? &top[(y + 57) * SCREEN_W] : &bottom[(y - 21) * SCREEN_W];
            memcpy(&main[y * SCREEN_W], src, SCREEN_W * 4);
        }
        break;
    case SCENE_TITLE:
        compose_title(main, top, bottom);
        break;
    case SCENE_SETTINGS:
        compose_settings(sc, main, top, bottom);
        break;
    case SCENE_MENU:
        compose_menu(main, top, bottom);
        break;
    case SCENE_EQUIP:
        compose_equip(sc, main, top, bottom);
        break;
    case SCENE_ITEMS:
        compose_items(main, top, bottom);
        break;
    case SCENE_TECHS:
        compose_techs(main, top, bottom);
        break;
    case SCENE_OPTIONS:
        compose_options(sc, main, bottom);
        break;
    case SCENE_PARTY:
        compose_party(main, top, bottom);
        break;
    case SCENE_SAVE:
        compose_save(main, top, bottom);
        break;
    case SCENE_SHOP:
        compose_shop(main, top, bottom);
        break;
    case SCENE_BESTIARY:
        compose_bestiary(main, top, bottom);
        break;
    case SCENE_NAMING:
        memcpy(main, rt_r32(sc.obj + NAMING_STATE) == NAMING_ENTERING ? bottom : top, SCREEN_W * SCREEN_H * 4);
        break;
    default:
        memcpy(main, top, SCREEN_W * SCREEN_H * 4);
        break;
    }
    return SCREEN_W;
}
