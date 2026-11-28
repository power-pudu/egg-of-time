/* Egg of Time's own achievements: 31, for the story (10), side quests (8) and the 13 endings, written for Egg of
 * Time and kept on this computer. They are checked only while the callback given to la_init returns 0 (main.c: no
 * other achievement service is in use), so one scene never shows two popups; meanwhile they wait, and what was
 * done then unlocks once it returns 0 again.
 *
 * Each achievement is a check on the game's memory, run once per game frame at VBlank on the game thread: a flag or
 * value the game sets when the milestone is done and keeps in its save. So a save that already did something
 * unlocks it once it loads (the game's memory holds zeros until a save loads, never anything else). Unlocks are kept
 * in local_achievements.ini in the data folder, one "<id> = <unix time>" line each; unknown lines are dropped.
 *
 * The host thread shows the popups and writes the file. An unlock alone gets its own popup (badge, title, points);
 * several in the same frame, as when a save loads, one popup saying how many.
 *
 * Badges are cut from the player's own ROM at start-up: the Extras gallery's illustrations for the story and side
 * quests, the Ending list's pictures for the endings (graphics mods applied later do not change them). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "display.h"
#include "gfx_rom.h"
#include "local_achievements.h"
#include "rt.h"
#include "ui_text.h"

#define POPUP_MS 5000
#define QUEUE_LEN 8

#define ENDINGS_SEEN GAME_ADDR(0x02074558u, 0x02074578u) /* u16, bit n: ending n + 1 seen (system data + 0x24d0) */

typedef struct Entry {
    const char *id; /* kept in the unlocks file: never change one */
    const char *title, *description;
    unsigned points;
    int (*done)(void);
    int art; /* its badge: Extras illustration ILLUST(n) or ending picture ENDING_ART(n) (see badge_paths) */
} Entry;
#define ILLUST(n) (n)
#define ENDING_ART(n) (100 + (n))

static int ending_seen(int n) { return (rt_r16(ENDINGS_SEEN) >> (n - 1)) & 1; }
#define ENDING(n) static int ending##n(void) { return ending_seen(n); }
ENDING(1) ENDING(2) ENDING(3) ENDING(4) ENDING(5) ENDING(6) ENDING(7) ENDING(8) ENDING(9) ENDING(10) ENDING(11)
ENDING(12) ENDING(13)

/* The story's progress: the SNES game's storyline counter ($7F0000), which the DS game keeps at the start of its
 * copy of that memory bank; the system data object (ENDINGS_SEEN's) points to it in its first word. The counter
 * only grows along the story; 0 until a save loads. Values (the event that sets them):
 * 0x1B Queen Leene rescued (Yakra defeated), 0x30 escaped Guardia Castle, 0x3C left for the Factory Ruins (Robo
 * repaired), 0x81 Masamune reforged, 0x8A Magus defeated, 0x99 Lavos falls to Earth (Azala defeated), 0xB7 Mt. Woe
 * falls, 0xCB Crono falls in the Ocean Palace, 0xD2 the Blackbird crashes (the Epoch gets its wings), 0xD5 Crono
 * revived, 0xD6 Black Omen destroyed. */
#define SNES_BANK_7F GAME_ADDR(0x02072088u, 0x020720a8u)
static unsigned storyline(void) {
    uint32_t bank = rt_r32(SNES_BANK_7F);
    return bank >= 0x02000000u && bank < 0x02400000u ? rt_r8(bank) : 0;
}
/* A quest's flag in the same bank ($7F0000 + offset, the byte a save slot keeps at slot offset 0x228 + offset). Each
 * is set by the quest's last scene in the game's event scripts, and only there:
 * Fiona's Shrine (Robo met again in 1000 AD), Guardia's throne room (the Rainbow Shell shown in 1000 AD), Sun Keep
 * 2300 AD (the charged Sun Stone taken), the Hero's Grave (Cyrus at rest), Ozzie's Fort (Ozzie gone), the Geno
 * Dome mainframe (Mother Brain defeated), Lucca's workshop (Lara saved; the scene's other ending leaves it clear). */
static int bank_bit(unsigned offset, unsigned bit) {
    uint32_t bank = rt_r32(SNES_BANK_7F);
    return bank >= 0x02000000u && bank < 0x02400000u && (rt_r8(bank + offset) & bit) != 0;
}
#define QUEST(name, offset, bit) static int name(void) { return bank_bit((offset), (bit)); }
QUEST(fiona, 0x19E, 0x02) QUEST(rainbow_shell, 0x0A7, 0x40) QUEST(sun_stone, 0x13A, 0x40)
QUEST(northern_ruins, 0x1F4, 0x40) QUEST(ozzie, 0x1F4, 0x20) QUEST(geno_dome, 0x13B, 0x10) QUEST(lucca_mother, 0x07C, 0x80)

#define STORY(name, value) static int name(void) { return storyline() >= (value); }
STORY(yakra, 0x1B) STORY(prison, 0x30) STORY(robo, 0x3C) STORY(masamune, 0x81) STORY(magus, 0x8A)
STORY(azala, 0x99) STORY(mt_woe, 0xB7) STORY(ocean_palace, 0xCB) STORY(epoch_wings, 0xD2) STORY(crono, 0xD5)
STORY(black_omen, 0xD6)

static const Entry set[LA_COUNT] = {
    /* Story */
    {"story_yakra", "A Queen Returned", "Defeat Yakra and bring Marle back from 600 AD.", 5, yakra, ILLUST(12)},
    {"story_prison", "Breakout", "Escape Guardia's prison.", 5, prison, ILLUST(0)},
    {"story_robo", "Spare Parts", "Repair Robo.", 5, robo, ILLUST(3)},
    {"story_masamune", "Blade Reborn", "Reforge the Masamune.", 10, masamune, ILLUST(4)},
    {"story_magus", "Castle Siege", "Defeat Magus at his castle.", 10, magus, ILLUST(13)},
    {"story_azala", "Age of Reptiles", "Defeat Azala in the Tyrano Lair.", 10, azala, ILLUST(10)},
    {"story_mt_woe", "Floating Mountain", "Defeat Giga Gaia on Mt. Woe.", 10, mt_woe, ILLUST(27)},
    {"story_ocean_palace", "Into the Depths", "Face Lavos in the Ocean Palace.", 10, ocean_palace, ILLUST(7)},
    {"story_crono", "A Moment Restored", "Bring Crono back at Death Peak.", 10, crono, ILLUST(14)},
    {"story_epoch_wings", "Wings of Time", "Give the Epoch wings.", 10, epoch_wings, ILLUST(11)},
    /* Side quests */
    {"side_fiona", "Green Thumb", "Help Fiona's forest grow back.", 10, fiona, ILLUST(19)},
    {"side_rainbow_shell", "Prism of the Sea", "Bring the Rainbow Shell to Guardia Castle.", 10, rainbow_shell, ILLUST(17)},
    {"side_sun_stone", "Charged by Ages", "Charge the Sun Stone.", 10, sun_stone, ILLUST(18)},
    {"side_northern_ruins", "Rest in Peace", "Lay Cyrus to rest in the Northern Ruins.", 10, northern_ruins, ILLUST(20)},
    {"side_ozzie", "Fort Fall", "Drive Ozzie out of his fort.", 10, ozzie, ILLUST(29)},
    {"side_geno_dome", "Machine Heart", "Defeat Mother Brain in the Geno Dome.", 10, geno_dome, ILLUST(24)},
    {"side_lucca_mother", "Second Chance", "Save Lucca's mother.", 10, lucca_mother, ILLUST(2)},
    {"side_black_omen", "Omen's End", "Conquer the Black Omen.", 25, black_omen, ILLUST(8)},
    /* Endings, in the order of the Extras' Ending list */
    {"ending01", "Past the End", "See the ending \"Beyond Time\".", 10, ending1, ENDING_ART(0)},
    {"ending02", "Together Again", "See the ending \"Reunion\".", 10, ending2, ENDING_ART(1)},
    {"ending03", "Behind the Scenes", "See the ending \"The Dream Project\".", 10, ending3, ENDING_ART(2)},
    {"ending04", "Heir to the Throne", "See the ending \"The Successor of Guardia\".", 10, ending4, ENDING_ART(3)},
    {"ending05", "Sweet Dreams", "See the ending \"Good Night\".", 10, ending5, ENDING_ART(4)},
    {"ending06", "Hero of Legend", "See the ending \"The Legendary Hero\".", 10, ending6, ENDING_ART(5)},
    {"ending07", "Forgotten Days", "See the ending \"The Unknown Past\".", 10, ending7, ENDING_ART(6)},
    {"ending08", "Faces of the Ages", "See the ending \"People of the Times\".", 10, ending8, ENDING_ART(7)},
    {"ending09", "A Promise Kept", "See the ending \"The Oath\".", 10, ending9, ENDING_ART(8)},
    {"ending10", "Rule of Reptites", "See the ending \"Dino Age\".", 10, ending10, ENDING_ART(9)},
    {"ending11", "The Prophet's Wish", "See the ending \"What the Prophet Seeks\".", 10, ending11, ENDING_ART(10)},
    {"ending12", "Looking Back", "See the ending \"Memory Lane\".", 10, ending12, ENDING_ART(11)},
    {"ending13", "After the Dream", "See the ending \"Dream's Epilogue\".", 25, ending13, ENDING_ART(12)},
};

static int (*in_use)(void);
static SDL_Mutex *lock;      /* everything below */
static int64_t when[LA_COUNT]; /* unix time of the unlock; 0 = locked */
static int dirty;            /* the file needs writing */
static uint8_t held[LA_COUNT]; /* game thread only: frames in a row each locked check has held */
#define HOLD_FRAMES 30 /* half a second: memory read while a save or state is still being laid out never counts */
typedef struct Batch { int count, first; } Batch; /* one frame's unlocks: how many, and the first one's index */
static Batch queue[QUEUE_LEN];
static int queue_head, queue_count;
static Uint64 popup_until;
static char path[1024];
static LaItem items[LA_COUNT];

/* ---- badges: cut from the player's ROM ---- */

static uint32_t badges[LA_COUNT][LA_BADGE * LA_BADGE]; /* ABGR8888 */

/* Entry i's badge: its picture (a 256x192 screen of 8-bit tiles: the Extras gallery's illustrations and ending
 * pictures), the centred square of it shrunk to LA_BADGE by averaging. 0 when the ROM's files are not there. */
static int build_badge(int i) {
    char ncg[96], ncl[96];
    int art = set[i].art, ending = art >= ENDING_ART(0);
    const char *dir = ending ? "menu/Extra/ending/" : "menu/Extra/illust/", *tag = ending ? "_NEW" : "";
    snprintf(ncg, sizeof ncg, "%s%03d%s_ncg.bin", dir, ending ? art - ENDING_ART(0) : art, tag);
    snprintf(ncl, sizeof ncl, "%s%03d%s_ncl.bin", dir, ending ? art - ENDING_ART(0) : art, tag);
    const GfxFile *fg = gfx_find(ncg), *fl = gfx_find(ncl);
    const GfxFile *fs = gfx_find(ending ? "menu/Extra/ending/bg_pct_end_nsc.bin" : "menu/Extra/illust/bg_pct_illust_nsc.bin");
    if (!fg || !fl || !fs) return 0;
    GfxBlob gb = gfx_open(fg), sb = gfx_open(fs);
    GfxTiles t;
    GfxLayout l;
    GfxPalette p;
    int ok = gfx_read_tiles(&gb, &t) && gfx_read_palette(fl, &p) && gfx_screen_layout(&sb, t.bpp8, &l);
    if (ok) {
        uint8_t *px = SDL_calloc((size_t)l.w, (size_t)l.h);
        ok = px != NULL;
        if (ok) {
            gfx_draw(&l, &t, px);
            int side = SDL_min(l.w, l.h), x0 = (l.w - side) / 2, y0 = (l.h - side) / 2;
            for (int by = 0; by < LA_BADGE; by++) {
                for (int bx = 0; bx < LA_BADGE; bx++) {
                    int sx0 = x0 + bx * side / LA_BADGE, sx1 = x0 + (bx + 1) * side / LA_BADGE;
                    int sy0 = y0 + by * side / LA_BADGE, sy1 = y0 + (by + 1) * side / LA_BADGE;
                    unsigned r = 0, g = 0, b = 0, n = 0;
                    for (int y = sy0; y < sy1; y++) {
                        for (int x = sx0; x < sx1; x++, n++) {
                            SDL_Color c = p.c[px[y * l.w + x]];
                            r += c.r, g += c.g, b += c.b;
                        }
                    }
                    if (n) badges[i][by * LA_BADGE + bx] = 0xFF000000u | (b / n) << 16 | (g / n) << 8 | r / n;
                }
            }
            SDL_free(px);
        }
        free(l.tiles); /* gfx_rom.c allocates with malloc */
    }
    gfx_close(&sb);
    gfx_close(&gb);
    return ok;
}

static void build_badges(void) {
    if (!gfx_find("menu/Extra/illust/bg_pct_illust_nsc.bin")) gfx_list_files();
    int made = 0;
    for (int i = 0; i < LA_COUNT; i++) {
        if (build_badge(i)) items[i].badge = badges[i], made++;
    }
    if (made < LA_COUNT) rt_log("local achievements: %d of %d badges read from the ROM", made, LA_COUNT);
}

static void load(void) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char id[64];
        long long t;
        if (sscanf(line, " %63[a-z0-9_] = %lld", id, &t) != 2 || t <= 0) continue;
        for (int i = 0; i < LA_COUNT; i++) {
            if (!strcmp(id, set[i].id)) when[i] = t;
        }
    }
    fclose(f);
}

/* Written to a temporary file, then renamed over the old one, so a failed or interrupted write never loses the
 * unlocks already kept. Returns 0 on failure: the caller tries again later. */
static int save(const int64_t *times) {
    char tmp[sizeof path + 4];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    int ok = f != NULL;
    for (int i = 0; ok && i < LA_COUNT; i++) {
        if (times[i]) ok = fprintf(f, "%s = %lld\n", set[i].id, (long long)times[i]) > 0;
    }
    if (f && fclose(f) != 0) ok = 0;
    if (ok) ok = SDL_RenamePath(tmp, path);
    if (!ok) {
        remove(tmp);
        rt_log("local achievements: cannot write %s", path);
    }
    return ok;
}

void la_init(int (*active)(void)) {
    in_use = active;
    snprintf(path, sizeof path, "%slocal_achievements.ini", rt_data_dir());
    load();
    build_badges();
    lock = SDL_CreateMutex();
}

void la_frame(void) {
    if (!lock || !in_use || in_use()) return;
    int locked[LA_COUNT], found[LA_COUNT], n = 0;
    SDL_LockMutex(lock);
    for (int i = 0; i < LA_COUNT; i++) locked[i] = !when[i];
    SDL_UnlockMutex(lock);
    for (int i = 0; i < LA_COUNT; i++) { /* the checks read only the game's memory: outside the lock */
        if (!locked[i] || !set[i].done()) held[i] = 0;
        else if (++held[i] >= HOLD_FRAMES) held[i] = 0, found[n++] = i;
    }
    if (!n) return;
    int64_t now = (int64_t)time(NULL);
    SDL_LockMutex(lock);
    for (int k = 0; k < n; k++) {
        when[found[k]] = now;
        rt_log("local achievements: unlocked %s", set[found[k]].id);
    }
    dirty = 1;
    if (queue_count == QUEUE_LEN) queue_head = (queue_head + 1) % QUEUE_LEN, queue_count--; /* the oldest popup goes */
    queue[(queue_head + queue_count++) % QUEUE_LEN] = (Batch){n, found[0]};
    SDL_UnlockMutex(lock);
}

void la_poll(void) {
    if (!lock) return;
    Uint64 now = SDL_GetTicks();
    int64_t times[LA_COUNT];
    Batch b = {0, 0};
    static Uint64 retry_at; /* after a failed write: the next try, so a lasting failure doesn't retry every frame */
    SDL_LockMutex(lock);
    int write = dirty && now >= retry_at;
    if (write) dirty = 0;
    memcpy(times, when, sizeof times);
    if (queue_count && now >= popup_until) {
        b = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_LEN;
        queue_count--;
        popup_until = now + POPUP_MS;
    }
    SDL_UnlockMutex(lock);
    if (write && !save(times)) {
        SDL_LockMutex(lock);
        dirty = 1;
        SDL_UnlockMutex(lock);
        retry_at = now + 5000;
    }
    if (b.count == 1) {
        const Entry *e = &set[b.first];
        char line2[64];
        snprintf(line2, sizeof line2, ui_text("Achievement unlocked  %u pts"), e->points);
        display_popup(items[b.first].badge, LA_BADGE, e->title, line2, POPUP_MS);
    } else if (b.count > 1) {
        char line1[64];
        snprintf(line1, sizeof line1, ui_text("%d achievements unlocked"), b.count);
        display_popup(NULL, LA_BADGE, line1, ui_text("from your save"), POPUP_MS);
    }
}

int la_list(const LaItem **out) {
    static LaItem sorted[LA_COUNT];
    int64_t times[LA_COUNT];
    SDL_LockMutex(lock);
    memcpy(times, when, sizeof times);
    SDL_UnlockMutex(lock);
    int n = 0;
    for (int pass = 1; pass >= 0; pass--) {
        for (int i = 0; i < LA_COUNT; i++) {
            if (!times[i] != !pass) continue;
            LaItem it = {set[i].title, set[i].description, set[i].points, pass, items[i].badge};
            sorted[n++] = it;
        }
    }
    *out = sorted;
    return n;
}

int la_unlocked_count(void) {
    int n = 0;
    SDL_LockMutex(lock);
    for (int i = 0; i < LA_COUNT; i++) n += when[i] != 0;
    SDL_UnlockMutex(lock);
    return n;
}

void la_reset(void) {
    SDL_LockMutex(lock);
    memset(when, 0, sizeof when);
    queue_count = 0;
    dirty = 1;
    SDL_UnlockMutex(lock);
    rt_log("local achievements: reset");
}
