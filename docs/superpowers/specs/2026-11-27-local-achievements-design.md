# Local achievements: design

Date: 2026-11-27. Status: draft for approval.

## Intent

RetroAchievements (RA) will not let Egg of Time ship its set for offline play: the set must come from RA's server,
which needs an account. Egg of Time gets its own achievement set instead, written from scratch and shipped with the
game, so players without an RA account still have goals. It should feel like an RA set: story progress and side-quest
completion, with popups, badges and points.

## Goals

- 31 achievements: 10 story, 8 side quests, 13 endings (list below).
- Active only while RA is not logged in. Logged in, RA's set runs and ours is silent, so one scene never shows two
  popups. Steam achievements (`runtime/steam_presence.c`) are separate and keep working either way.
- Look like RA's: the same top-right popup (badge, title, points) and a list in System Settings.
- Unlocks persist across sessions, in a file in the data folder.
- Old saves count: a milestone the loaded game already shows as done unlocks when it is seen in memory, as the Steam
  ending sync does.
- Several unlocks in the same frame (2 or more, as when an old save loads) show one summary popup ("N achievements
  unlocked from your save"); the list shows which. A single unlock gets its own popup with its badge.
- A "Reset Egg of Time achievements" row on the achievements page, with a Yes/No confirm, clears every local unlock
  (for a flag that fired wrongly, or a fresh playthrough). Milestones the loaded game still shows as done unlock
  again on the next frame, after the reset; that is expected.

## Non-goals

- No DS-only side quests (Lost Sanctum, Dimensional Vortex): none now, none planned.
- No syncing to RA: local unlocks are never sent to RA (not allowed). They stay if the player logs in later; our set
  only pauses while logged in.
- No condition editing without a rebuild: the set is fixed.

## Anti-goals

- Nothing of RA's: no titles, descriptions, conditions or badges copied from RA's set for this game. Every entry is
  our own.
- Nothing of the game shipped: badges are cut from the player's own ROM at start-up, never stored in the release.
- The word "cheevos" appears nowhere in the new code (files, identifiers, comments, ini keys). The module is
  `runtime/local_achievements.c` / `.h`.
- Never mention outside projects as inspiration in code or docs.

## The set

Titles below are working descriptions; final titles and descriptions are the builder's to write, in our own words
(EN, FR and JA, see Constraints).

Story (10):
1. Defeat Yakra and bring Marle back from 600 AD
2. Escape Guardia's prison
3. Repair Robo
4. Reforge the Masamune
5. Defeat Magus at his castle
6. Defeat Azala in the Tyrano Lair
7. Defeat Giga Gaia on Mt. Woe
8. Face Lavos in the Ocean Palace
9. Bring Crono back at Death Peak
10. Give the Epoch wings

Side quests (8):
11. Fiona's forest (Robo's quest)
12. Rainbow Shell
13. Sun Stone
14. Northern Ruins and Cyrus (Frog's quest)
15. Ozzie's Fort
16. Geno Dome
17. Lucca's mother
18. Black Omen

Endings (13): endings 1-13 of the Extras' Ending list, Beyond Time through Dream's Epilogue. Detected from the
ending-seen bits already found for Steam: u16 at 0x02074558 (JP 0x02074578); bit 0 is ending 1, bit 12 ending 13
(as in `runtime/steam_presence.c`). No "loaded" guard is needed: the field is in BSS, zeroed at boot (write from
main 0x020008a0), and written only by the system-data load (memcpy at main 0x02029980) and the game itself, so before
a save loads it reads 0, never garbage.

Points, scaled by effort as on RA: story 5-10, side quests 10, endings 10, 25 for the hardest (Black Omen, Dream's
Epilogue). Exact values are the builder's call within that scale.

## Approaches

- **A. C table in `runtime/local_achievements.c` (picked).** Each entry: title, description, points, badge source,
  and a check function that reads game memory (story flags, items, ending bits). All checked once per frame at
  VBlank on the game thread, like `sp_frame`. Simple, typed, next to the existing Steam code; changing a condition
  means a rebuild, fine for a fixed set.
- B. Conditions in rcheevos' condition language in a shipped text file, run offline by the linked rcheevos engine.
  Reuses a tested engine and edits need no rebuild, but 31 hand-written conditions in that syntax are hard to review,
  and the engine expects RA's server data, so running it offline needs extra glue.
- C. Hooks on game functions (boss fight end, event scripts). Exact timing, but one fragile override per achievement,
  much more reverse engineering, and milestones an old save already finished could never unlock.

The user picked A.

## Decided by the user

- Set content as above; DS-only quests excluded.
- Only while RA is logged out.
- Badges from game art: each badge cut from the player's ROM (a portrait, sprite or item icon fitting the
  milestone), 29 px square like RA's (`CHEEVOS_BADGE`), greyed while locked.
- Points as above.
- List shown on the RetroAchievements page of System Settings: while logged out, the login rows come first, then a
  heading "Egg of Time achievements  N / 31" and the list (badge, title, points, description), like RA's list.
- Unlock popup: the existing top-right popup (`display_popup`).
- Unlocks kept in a file in the data folder (`rt_data_dir()`), never sent to RA.
- Old saves unlock what they already show as done.
- File name `local_achievements`, no "cheevos" in the new code.

## Constraints

- Detection reads game memory only: main RAM / DTCM through `rt_r8`/`rt_r16`/`rt_r32`, addresses through
  `GAME_ADDR(usa, japan)` so the JP ROM works too.
- Runs on the game thread at VBlank (beside `cheevos_frame` and `sp_frame` in `runtime/sched.c`); popups and file
  writes must not stall that thread (hand them to the host thread, as `cheevos.c` does with its popup queue).
- "RA logged in" means `cheevos_state() == CHEEVOS_LOGGED_IN` (existing code; calling it is fine, only the new code
  must not use the word). While logged in nothing is checked, unlocked or shown as a popup; while logging in, wait.
- A check must not fire on a title screen or a fresh game where memory holds no save yet (flags cleared, or system
  data not yet loaded), and must not fire on garbage: each condition should read a flag that is only set by the
  milestone itself.
- UI text through `ui_text()` with FR and JA versions; JA text may only use glyphs in `runtime/ui_font_ja.inc`
  (check with a script, as was done for the Steam footer).
- Main RAM can change abruptly with save states (F5/F9) and resets; an unlock is permanent once written, so a state
  from a completed game unlocks its milestones (accepted: same as the old-save rule).

## Calls made while writing this (check these)

- Any save slot or New Game+ file that shows a milestone done unlocks it for good (global, not per slot): the same
  rule as old saves. A fresh NG+ file carrying story flags unlocks those at once.
- A milestone with no single clean flag may be detected from a combination (flags plus inventory or party). An item
  check that holds for one frame is enough, since unlocks are permanent. If a milestone cannot be detected reliably
  at all, the builder stops and asks the user to drop or replace it; the set is meant to stay at 31.
- After RA logs out mid-session, checks resume and anything already done in memory unlocks then.
- Badge fallback: if a badge's graphics cannot be read from the ROM, the entry shows a plain placeholder square
  (greyed while locked); the list still shows.
- The RetroAchievements page always exists (rcheevos is always linked), so the list has a home in every build.
- Verification is manual, with the user's save states and RAM dumps; there is no automated harness for memory
  conditions.

## Left to the builder

- Finding the memory flag behind each of the 18 non-ending milestones (event flags, inventory, party). Tools:
  `EGG_DUMP_RAM`/`EGG_DUMP_AT` dumps around each milestone in the user's save states, `EGG_WATCH_WRITE`, and the
  completed state `build/rom/CT.complete_state` (every milestone done) against earlier states. Each found address
  and its meaning goes in a comment and in DESIGN.md.
- Final titles, descriptions and point values.
- Which sprite, portrait or icon each badge is cut from, and how (the game's graphics files in the ROM's file
  system, through the existing graphics readers).
- The unlocks file's name and format (a small ini-like text list of unlocked ids with timestamps is enough).
- How the menu shares row drawing with RA's list without the new module using the word "cheevos".
- The popup queue: single unlocks in different frames queue, like RA's popup queue, so none is lost; a frame with 2
  or more becomes one summary popup.
