# Override music Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With a Steam copy connected and Override music On, each DS music sequence that has a Steam version plays
it (after the player's own music mod, before the DS sequence).

**Architecture:** A new `runtime/steam_music.c` unpacks `Sound/BGM/BGM_xx.sab` on a worker thread, parses the
SEAD bank, unmasks the first material's HCA in memory and keeps 4 tracks cached. `runtime/music.c` gets a memory
source (FFmpeg `hca` through an `AVIOContext` over the buffer) that waits, muted, until its track is ready, then
plays like a music mod with the material's loop points.

**Tech Stack:** C11, SDL3 (threads, mutex, condition), FFmpeg (libavformat AVIO, `hca` demuxer and decoder).

**Spec:** `docs/superpowers/specs/2026-11-28-steam-music-design.md`

## Global Constraints

- Nothing of the Steam copy written to disk (the scratchpad's analysis copies are deleted at the end).
- Order: `mods/music/BGM_xx.*` (Mods On) > Steam (`SET_STEAM_MUSIC` On, `se_available()`, the file exists) > DS.
  Override music does not depend on the Mods setting.
- The game thread does no unpacking or unmasking; per frame it adds at most an atomic/mutex check per pending
  source.
- Mask: byte i after the HCA header (i from 0) XOR `key[(extradata_id + i) & 255]`, the 256-byte SEAD table.
- Loop: FFmpeg samples = material points + the HCA `fmt` encoder delay; end exclusive (`loop_end + 1`); both
  points 0: plays once.
- Footer text: "Steam release's music." in the 9 menu languages.
- Never mention inspiring projects (or other tools' names) in code or docs; the table is described as the SEAD
  banks' key.

## Review Focus

- A `.sab` that is truncated, has codec != 7, 0 materials, or offsets past its end: no crash, the DS plays.
- Track change while the previous track is still unpacking (rapid field/battle switches): the right track plays,
  no buffer freed while FFmpeg reads it (refcount).
- Save state load / `music_reset` with a pending source: nothing leaks, the cache survives.
- A once-only track (BGM_30, fanfares) ends silent while its sequence runs on, as music mods without loop do.
- Quitting the game with the worker mid-unpack: clean exit (no hang on join).

---

### Task 1: The Steam tracks (`steam_music.c`)

**Files:**
- Create: `runtime/steam_music.h`, `runtime/steam_music.c`
- Modify: `runtime/steam_edges.h`, `runtime/steam_edges.c` (`se_has`), `CMakeLists.txt` (source list beside
  `steam_edges.c`), `runtime/main.c` (the check hook beside `EGG_STEAMTEXT_CHECK`)

**Interfaces:**
- Consumes: `se_read(const char *name, size_t *len)`, `se_available()`.
- Produces:
  ```c
  int se_has(const char *name);          /* steam_edges.h: the archive holds the file (0 without a copy) */
  typedef struct SteamTrack {
      const uint8_t *hca; size_t len;    /* an HCA file, unmasked */
      int64_t loop_start, loop_end;      /* FFmpeg output samples, end exclusive; loop_end < 0: plays once */
  } SteamTrack;
  int steam_music_has(const char *seq);  /* "BGM_30": Sound/BGM/BGM_30.sab exists */
  /* Non-blocking: 1 and *out (a reference) when ready; 0 while it unpacks (queued); -1 none or damaged. */
  int steam_music_get(const char *seq, SteamTrack **out);
  void steam_music_release(SteamTrack *t);
  void steam_music_quit(void);           /* stops the worker; at exit */
  ```

- [ ] **Step 1:** `se_has`: the existing `bsearch` over `entries`, no unpack.
- [ ] **Step 2:** `steam_music.c`: `static int parse(uint8_t *d, size_t n, SteamTrack *t)` following the spec's
  layout (bounds-checked: chunk table, `mtrl` count >= 1, first material in range, codec 7, HCA magic at +0x30,
  header size < length), unmasks in place when encryption is nonzero, reads the encoder delay from the HCA `fmt`
  chunk (+0x0c, big-endian u16) and sets the loop as the Global Constraints say.
- [ ] **Step 3:** Cache of 4 entries (name, data, track, refs, state: loading / ready / failed), one lazily
  created worker thread fed by a small request queue under an `SDL_Mutex` + `SDL_Condition`; least recently used,
  unreferenced entries are evicted; a failed entry answers -1 without retrying.
- [ ] **Step 4:** `EGG_STEAMMUSIC_CHECK=1` (in `main.c`, after `se_init`, like `EGG_STEAMTEXT_CHECK`): loads every
  `BGM_00` ... `BGM_62` the copy has through `steam_music_get` (waiting), logs
  `steam music: BGM_xx <len> bytes, loop <s>-<e>` and checks the first block after the header starts `FF FF`, then
  exits. Build. Run it. Expected: 85 lines, no failure; BGM_01 `loop 353408-5307698`; BGM_30 `loop 0--1`.
- [ ] **Step 5:** Offline check of the loop arithmetic (scratchpad, `ffmpeg` on BGM_01 unmasked by the scratchpad's
  `sab.py`): the 2048 samples ending at `loop_end` match those ending at `loop_start` better than with the delay
  left out. Expected: correlation with the delay > without it. Delete the scratchpad's audio copies afterwards.

### Task 2: Playing them (`music.c`)

**Files:**
- Modify: `runtime/music.c` (header comment, `Source`, `open_source`, `close_source`, `music_prepare`,
  `music_mix`)

**Interfaces:**
- Consumes: Task 1's `steam_music_has`, `steam_music_get`, `steam_music_release`; `display_setting_get(SET_STEAM_MUSIC)`.
- Produces: no new public function; `music_replaced` is true for a pending Steam source (the DS stays muted).

- [ ] **Step 1:** `Source` gains `SteamTrack *track; char seq[16]; int pending; AVIOContext *avio; int64_t mem_pos;`
  and a memory read/seek pair (`AVSEEK_SIZE` answers `len`). `open_source` takes either a path or a track; a track
  opens with `av_find_input_format("hca")` through the AVIO context (`AVFMT_FLAG_CUSTOM_IO`), loop points from
  the track, `once` when `loop_end < 0`. `close_source` frees the AVIO buffer and context and releases the track.
- [ ] **Step 2:** `music_prepare`: after the mod lookup finds nothing, a BGM with `SET_STEAM_MUSIC` On and
  `steam_music_has` gets a pending source (`ref_db` from the sequence's volume, as mods). `music_mix`: a pending
  source polls `steam_music_get` once per call: 1 opens it, -1 drops it (logs once), 0 keeps waiting (silent).
- [ ] **Step 3:** Gain: `STEAM_MUSIC_GAIN` (applied beside `MUSIC_GAIN` for track sources), first 1.93 (5.7 dB).
  Build. Expected: no errors or warnings.
- [ ] **Step 4:** Test in game (back up `build/eggoftime.ini`, windowed, `steammusic = yes`, `EGG_LOG_FPS=1`,
  `EGG_WAV`): the title logs `sound mod: ... (hca, 48000 Hz, loop 0--1)` and plays Steam's BGM_30; a field save
  state (copied to the scratchpad) plays its Steam track; entering a battle and back switches without a hitch
  (fps log stays 60). Measure the field and battle tracks against the DS's (Override music off) with `volumedetect`
  and set the gain to their mean difference. With a test mod file `mods/music/BGM_30.wav` (made from silence by
  ffmpeg in the scratchpad, removed after), the mod wins. Restore the ini.

### Task 3: Footer, docs, memory

**Files:**
- Modify: `runtime/system_menu.c`, `runtime/ui_text.c`, `runtime/ui_font_cjk.inc` (regenerated only if a
  Korean/Chinese glyph is new: `tools/font/ui_font_cjk.py <fusion dir>`), `README.md`, `DESIGN.md`,
  `res/mods/music/readme.txt`

- [ ] **Step 1:** `[SET_STEAM_MUSIC] = "Steam release's music."` and its row in `ui_text.c` (en, fr, ja, de, it,
  es, ko, zh-Hans, zh-Hant); Japanese only from glyphs already in `ui_font_ja.inc`.
- [ ] **Step 2:** README: a "Steam music" bullet beside "Steam cutscenes". DESIGN: the music mods paragraph covers
  the Steam source (bank layout, mask, loop arithmetic, worker and cache); the Connect Steam bullet says only
  sounds are still to come. The music mods readme says a mod file comes before Override music.
- [ ] **Step 3:** Build; open System Settings (`EGG_AUTOKEYS=MENU`) in en and ko and capture the footer. Expected:
  the text shows, no missing glyph boxes. Restore the ini.
- [ ] **Step 4:** Memory: `steam-version-phases.md` marks phase 5 done; index line "phases 1-5 done".
