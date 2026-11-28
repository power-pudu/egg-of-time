# Local Achievements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Egg of Time's own 31 achievements (10 story, 8 side quests, 13 endings), active only while RetroAchievements is logged out, with popups, ROM-cut badges, a list and a reset on the RetroAchievements page.

**Architecture:** A new `runtime/local_achievements.c` holds a fixed C table; each entry's check reads game memory once per frame at VBlank on the game thread. Unlocks go to the host thread, which writes `local_achievements.ini` in the data folder and shows popups through `display_popup`. The System Settings RetroAchievements page lists them while logged out.

**Tech Stack:** C11, SDL3, the runtime's memory accessors (`rt_r8/16/32`, `GAME_ADDR`).

**Spec:** `docs/superpowers/specs/2026-11-27-local-achievements-design.md`

## Global Constraints

- The word "cheevos" appears nowhere in new code: file names, identifiers, comments, ini keys. New prefix: `la_`. Calling existing `cheevos_state()` from `system_menu.c` is fine; the new module reads RA's state only through a function pointer or a call made from existing code (see Task 1).
- Nothing of RA's set (titles, conditions, badges) and nothing of the game (graphics) is shipped.
- Never mention outside projects as inspiration in code or docs.
- Addresses through `GAME_ADDR(usa, japan)`.
- UI text through `ui_text()`, with FR and JA; JA only with glyphs in `runtime/ui_font_ja.inc` (check with a script).
- Edit C files with the Edit tool, not Python/sed heredocs: escapes like `'\n'` get mangled by the shell.
- Badges are `CHEEVOS_BADGE` (29) px square ABGR8888, since `display_popup` and the list draw that size; define `LA_BADGE 29` in the new header instead of using the old name.
- Restore `build/eggoftime.ini` after every test run; copy states to the scratchpad, never modify the user's files.
- Commit only when the user asks.

## Review Focus

1. RA in the "logging in" state at start-up: nothing may unlock or pop up until the login settles (logged in or failed). Test in Task 1.
2. Title screen / fresh boot before a save loads: zero unlocks (memory reads 0). Test in Task 1 with `rom/CT.sav` at the title.
3. Missing or corrupt `local_achievements.ini` (unknown ids, garbage lines): treated as no unlocks for those lines, never a crash, rewritten clean on the next unlock. Test in Task 1.
4. Reset while a finished save is loaded: everything clears, then the save's milestones unlock again next frame with one summary popup. Test in Task 2.
5. JP ROM: every address has its JP value; checks run on the JP build without false unlocks. Test in Task 4 (and endings in Task 1) with the JP build in `build/JPBuild`.

---

### Task 1: Core module, endings live

**Files:**
- Create: `runtime/local_achievements.c`, `runtime/local_achievements.h`
- Modify: `CMakeLists.txt` (add the source), `runtime/sched.c` (call `la_frame` after `sp_frame`), `runtime/main.c` (call `la_init` after `cheevos_init`, `la_poll` after `cheevos_poll`), `runtime/boot.c` / `runtime/state.c` (nothing: unlocks are state-based, no reset hook needed)

**Interfaces:**
- Produces:
  - `void la_init(int (*ra_active)(void));` host thread, before the game starts: reads `<rt_data_dir()>local_achievements.ini`. `ra_active` returns 1 while RA is logged in or logging in; main.c passes a small static wrapper around `cheevos_state() != CHEEVOS_LOGGED_OUT`.
  - `void la_frame(void);` game thread, VBlank: if `ra_active()` returns nonzero, does nothing; else runs every locked entry's check, marks new unlocks, and hands this frame's new unlocks to the host as one batch.
  - `void la_poll(void);` host thread, every window frame: writes the ini when unlocks changed; shows the next queued popup: a batch of 1 as `display_popup(badge, LA_BADGE, title, "<points> points", 5000)`, a batch of 2+ as one popup `"<N> achievements unlocked"` / `"from your save"` (via `ui_text`) with no badge.
  - `typedef struct LaItem { const char *title, *description; unsigned points; int unlocked; const uint32_t *badge; } LaItem;` and `int la_list(const LaItem **items);` (31 entries, unlocked first, then table order; `badge` may be NULL until Task 3).
  - `void la_reset(void);` host thread: clears every unlock and rewrites the ini (used by Task 2).
  - `#define LA_COUNT 31`, `#define LA_BADGE 29`.
- Table entry (internal): `{ const char *id; const char *title, *description; unsigned points; int (*done)(void); }`. Ids are stable strings, written to the ini: `ending01`..`ending13`, and for Task 4 `story_yakra`, `story_prison`, `story_robo`, `story_masamune`, `story_magus`, `story_azala`, `story_mt_woe`, `story_ocean_palace`, `story_crono`, `story_epoch_wings`, `side_fiona`, `side_rainbow_shell`, `side_sun_stone`, `side_northern_ruins`, `side_ozzie`, `side_geno_dome`, `side_lucca_mother`, `side_black_omen`.
- Ini format: one line per unlock, `<id> = <unix seconds>`; lines with unknown ids are dropped.

- [ ] **Step 1:** Write the module with the 13 ending entries live (bit `n-1` of u16 `GAME_ADDR(0x02074558u, 0x02074578u)`; titles from the Ending list in our words, 10 points, Dream's Epilogue 25) and the 18 others present with a `done` that returns 0 (filled in Task 4). Thread hand-off under an `SDL_Mutex`, popup queue of 8 batches (drop the oldest if full; the ini still records every unlock). Log each unlock: `local achievements: unlocked <id>`.
- [ ] **Step 2:** Build. Run: `cmake --build build --parallel`. Expected: no warnings from the new files.
- [ ] **Step 3: Completed state, RA logged out.** Move `<rt_data_dir()>retroachievements.ini` aside to the scratchpad (RA logged out), run with `EGG_LOAD_STATE=<scratch>/c.state` for 12 s. Expected: 13 `unlocked ending..` log lines in one frame, `local_achievements.ini` holding 13 lines, one summary popup in a window capture. Restore the RA ini.
- [ ] **Step 4: RA logged in.** Delete the test `local_achievements.ini`, run the same state with RA's ini in place. Expected: no `local achievements:` lines.
- [ ] **Step 5: Title screen.** Logged out, no state, `rom/CT.sav`, 12 s. Expected: no unlock lines.
- [ ] **Step 6: Corrupt file.** Write `garbage\nending01 = x\nnot_an_id = 5\n` as the ini, run step 3. Expected: no crash; the ini afterwards holds exactly the 13 valid ending lines.
- [ ] **Step 7:** Commit only if the user asks.

### Task 2: List and reset on the RetroAchievements page

**Files:**
- Modify: `runtime/system_menu.c` (`ch_total`, `draw_achievement`, the page's input handling), `runtime/ui_text.c`

**Interfaces:**
- Consumes: `la_list`, `la_reset`, `LaItem`, `LA_BADGE` (Task 1).

- [ ] **Step 1:** While `cheevos_state() != CHEEVOS_LOGGED_IN`, the page's rows are: name, password, Log in, a heading row `Egg of Time achievements  N / 31`, a `Reset Egg of Time achievements` row, then the 31 items. Draw items with the existing `draw_achievement` layout, taking the badge from `LaItem.badge` (blank square when NULL, greyed when locked, as RA's list does). A on Reset asks with the menu's existing Yes/No confirm pattern (look for one in `system_menu.c`; if none exists, two rows "Yes, reset" / "No"), default No; Yes calls `la_reset()`.
- [ ] **Step 2:** `ui_text` entries with FR and JA for: the heading, the Reset row, its confirm question, the summary popup's two lines from Task 1, the Reset row's footer. JA glyphs checked with a script against `ui_font_ja.inc`.
- [ ] **Step 3:** Build and run logged out with the completed state; open System Settings > RetroAchievements (EGG_AUTOKEYS), capture. Expected: heading `13 / 31`, endings first in colour, locked ones greyed.
- [ ] **Step 4: Reset with the save loaded.** Navigate to Reset, Yes. Expected: the log shows the reset, then 13 unlock lines again on the next frame and one summary popup; the ini holds 13 lines.
- [ ] **Step 5:** Commit only if the user asks.

### Task 3: Badges from the ROM

**Files:**
- Modify: `runtime/local_achievements.c` (badge build at `la_init` or lazily on the host thread), possibly a reader in `runtime/gfx_*.c` if one is missing

**Interfaces:**
- Produces: `LaItem.badge` non-NULL for every entry whose art reads; table entry gains `badge_source` (a ROM file path plus a cell/rect, as the builder decides).

- [ ] **Step 1:** Endings: the Extras Ending list thumbnails, `/menu/Extra/ending/<NNN>_NEW_ncg.bin` + `_ncl.bin` (000..012), scaled to 29x29. Story and side quests: a character portrait, sprite or item icon fitting each milestone, cut from the ROM's files with the existing graphics readers. Locked = greyscale of the same picture, computed at runtime. Unreadable art = NULL (Task 2 draws a blank square).
- [ ] **Step 2:** Run the completed state logged out, capture the list. Expected: 31 badges recognisable, none corrupt; the summary popup unchanged (no badge).
- [ ] **Step 3:** Commit only if the user asks.

### Task 4: The 18 story and side-quest checks

**Files:**
- Modify: `runtime/local_achievements.c` (the 18 `done` functions), `DESIGN.md` (each address and its meaning)

**Interfaces:**
- Consumes: the table and ids from Task 1.

- [ ] **Step 1: Find the game's SNES-layout progress data.** The DS game keeps the SNES game's memory layout for game state (see `SNES_FLAGS` in `runtime/widescreen.c`). Locate the DS address of the storyline counter and the event-flag area by comparing RAM dumps (`EGG_DUMP_RAM`, `EGG_DUMP_AT`) of the user's states in progress order: `CT_gato_millenial_fare.state` (start), `CT_bug_futureMetalThing.state`, `CT_blackOmen_MegaMutant.state`, `CT_lavos_core.state`, `CT.complete_state`. A single byte that only grows across them is the storyline counter.
Points: story 5 (early: yakra, prison, robo) to 10 (the rest); side quests 10, `side_black_omen` 25.

- [ ] **Step 2: Story checks** (`story_*`): storyline counter `>=` the value reached right after each milestone. Write each threshold and how it was found in a comment. Where the counter cannot separate a milestone, use its event flag, a party member (Robo repaired = Robo in the party data) or a key item (Masamune).
- [ ] **Step 3: Side-quest checks** (`side_*`): the quest's own completion flag, or the reward item / state it leaves (Rainbow Shell given, Sun Stone charged, Fiona's forest grown, Black Omen's boss beaten). Use `EGG_WATCH_WRITE` on candidate addresses while playing a state through the quest's end when one is available.
- [ ] **Step 4: Verify.** Each state, logged out, fresh ini: the unlock lines match the milestones that state has passed (gato: none; complete: all 31). Write the table of expected vs got in the task report. Also the JP build (`build/JPBuild`), with the JP `GAME_ADDR` values: completed state there if one exists, else the title screen with no false unlocks.
- [ ] **Step 5:** If a milestone cannot be detected reliably, stop and ask the user to drop or replace it (spec rule; the set stays at 31).
- [ ] **Step 6:** Commit only if the user asks.

### Task 5: Docs

**Files:**
- Modify: `README.md` (a "Local achievements" bullet: 31, only while RA logged out, reset), `DESIGN.md` (module, threading, ini, summary popup rule, badge sources), memory `steam-version-phases.md` untouched; add a project memory `local-achievements.md` with status and the found addresses.

- [ ] **Step 1:** Write them.
- [ ] **Step 2:** Commit only if the user asks.
