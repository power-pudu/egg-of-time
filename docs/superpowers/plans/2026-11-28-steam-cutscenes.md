# Override cutscenes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With a Steam copy connected and Override cutscenes On, the DS movies that have a Steam version play it.

**Architecture:** `movie.c` picks a source per movie (mod file, else Steam `.dat`, else none) and opens a masked
source through a custom `AVIOContext` that XORs while reading. The rest of the player is unchanged.

**Tech Stack:** C11, SDL3 (`SDL_IOStream`), FFmpeg (libavformat AVIO).

**Spec:** `docs/superpowers/specs/2026-11-28-steam-cutscenes-design.md`

## Global Constraints

- Nothing of the Steam copy written to disk.
- Order: `mods/cutscenes/<name>.mp4` (Mods On) > Steam (`steammovies` On, `se_available()`) > DS.
- Key: byte at offset `o` XOR `0xFF - (o & 0xFF)`.
- Map: op_ds 001, m2_ds..m7_ds 002..007, enosp1_ds 008; m7_ds `007-en` unless `ui_game_language() == LANGUAGE_ID_JAPANESE`.
- Game thread stays at 60 fps outside movies; nothing added per frame.
- Never mention inspiring projects in code or docs.

## Review Focus

- A Steam file missing or unreadable: the DS movie plays (play_file returns 0).
- Seeking past the end / `AVSEEK_SIZE`: the AVIO seek callback answers the file size.
- Skip (START, touch, intro any button) and the skipped byte behave as with movie mods.
- Mods Off with Override cutscenes On still plays Steam; a mod file still wins with both On.
- Save state / power-on during a movie: unchanged path (`cpu->resume_pc`).

---

### Task 1: Steam source in movie.c

**Files:**
- Modify: `runtime/movie.c` (`replacement`, `play_file`, header comment)

**Interfaces:**
- Consumes: `se_available()`, `se_dir()` (`steam_edges.h`), `display_setting_get(SET_STEAM_MOVIES)`,
  `ui_game_language()` (`ui_text.h`).
- Produces: `static int replacement(uint32_t path, char *out, size_t n, int *masked)`;
  `static int play_file(const char *path, int masked, uint32_t skip_keys)`.

- [ ] **Step 1:** Add the masked reader: `typedef struct Masked { SDL_IOStream *io; int64_t pos; } Masked;`,
  `read_masked(void *, uint8_t *, int)` (reads, XORs each byte with `0xFF - ((pos + i) & 0xFF)`, returns
  `AVERROR_EOF` at the end), `seek_masked(void *, int64_t, int whence)` (`AVSEEK_SIZE` returns the size;
  SEEK_SET/CUR/END ignoring `AVSEEK_FORCE`).
- [ ] **Step 2:** In `play_file`, a masked path opens with `avformat_alloc_context`, `avio_alloc_context` (64 KiB
  buffer, the callbacks) as `pb`, `AVFMT_FLAG_CUSTOM_IO`; `close_player` frees the AVIO context and closes the
  stream.
- [ ] **Step 3:** `replacement` returns the mod file (Mods On, exists), else the Steam file
  (`<se_dir()>/<nnn>.dat` from the map, exists, `masked = 1`), else 0.
- [ ] **Step 4:** Build. Expected: no errors or warnings.
- [ ] **Step 5:** Test in game (backup ini, windowed, `steammovies = yes`): the opening from the title (idle) and
  an in-game movie from a save state play the Steam version (log `movie: playing ...001.dat (1920x1080 h264)`),
  START skips, game resumes; a missing map entry (m1_ds) plays the DS movie. Restore the ini.

### Task 2: Footer text, docs

**Files:**
- Modify: `runtime/system_menu.c` (footer), `runtime/ui_text.c` (a row), `runtime/ui_font_cjk.inc`
  (regenerated with `tools/font/ui_font_cjk.py` and the Fusion Pixel fonts), `README.md`, `DESIGN.md`.

- [ ] **Step 1:** Footer `[SET_STEAM_MOVIES] = "Steam release's cutscenes, in HD."`, with its ui_text row in en, fr,
  ja (glyphs already in ui_font_ja.inc), de, it, es, ko, zh-Hans, zh-Hant.
- [ ] **Step 2:** Regenerate `ui_font_cjk.inc`; check the ja characters are all in `ui_font_ja.inc`.
- [ ] **Step 3:** README Steam bullet and DESIGN Movie mods / Connect Steam version: what plays, the map, the mask,
  the order.
- [ ] **Step 4:** Build; System Settings > Connect Steam version shows the footer in English and Korean.
