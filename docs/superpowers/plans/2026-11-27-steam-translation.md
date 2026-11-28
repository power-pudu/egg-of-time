# Steam translations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With Use Steam translation on, the game's text comes from the player's Steam copy in the chosen
Translation language, every Steam language included.

**Architecture:** A new module `runtime/steam_text.c` reads `Localize/<lang>/msg/*.txt` through `steam_edges.c`,
learns the DS text encoding by aligning Steam English/French with the ROM's own slots, rebuilds each `.msg` in
memory and points the ROM's file table (FAT) in `rt_rom.data` at the rebuilt copies appended past the ROM's end.
Letters the DS fonts lack get glyph numbers past the font's last glyph, drawn from a TrueType font by
`gfx_font.c`.

**Tech Stack:** C11, SDL3, stb_truetype (already used by `gfx_font.c`).

**Spec:** `docs/superpowers/specs/2026-11-27-steam-translation-design.md`

## Global Constraints

- Nothing of the Steam release or the ROM written to disk; no Steam/ROM text or font data in the source.
- Translation mods win per file: a `.msg` whose bytes differ from the verified ROM's keeps them.
- A Steam line that cannot be encoded safely (unknown tag, over the game's buffer limit) stays DS text and is logged.
- Never mention inspiring projects in code or docs. Fix by general cause.
- Build: `cmake --build build --parallel` with the PATH from memory; zero warnings.
- Test runs restore `build/eggoftime.ini`; states are copied to the scratchpad.
- Commit only when the user asks (work stays uncommitted on main until then).

## Review Focus

1. Live switching while the game has a `.msg` open or is mid-read: no torn file, no crash.
2. A line whose Steam text needs more bytes than the game's text buffer: stays DS text, no overflow.
3. Steam copy disconnected, or language missing, while Use Steam translation is on: the ROM's text, no crash.
4. Name tags (`<NAME_CRO>` etc.) and choice/wait codes survive encoding: a renamed hero shows his own name.
5. The Japanese ROM (2 slots, other font): either works or is cleanly off with the page saying so.

---

### Task 1: The `.msg` codec and the decoder's rules

**Files:**
- Create: `runtime/steam_text.c`, `runtime/steam_text.h`
- Modify: `runtime/steam_edges.c`, `runtime/steam_edges.h` (export `se_read`), `CMakeLists.txt`

**Interfaces:**
- Produces: `uint8_t *se_read(const char *name, size_t *len)` (malloc'd, NULL if absent) in `steam_edges.h`.
- Produces (internal to steam_text.c):
  `typedef struct Msg { int slots, strings; uint32_t *off; const uint8_t *data; size_t size; } Msg;`
  `int msg_parse(const uint8_t *p, size_t n, Msg *m);` `const uint8_t *msg_slot(const Msg *m, int s, int slot, size_t *len);`
  `uint8_t *msg_build(const Msg *m, uint8_t *const *slot_bytes, const size_t *slot_lens, size_t *out_len);`
- Produces: a written description in `DESIGN.md` of how the game's decoder reads a slot: which bytes are letters
  (and how a letter byte becomes a font glyph: the `.fnt` char table at data + 12), which are control codes and how
  long each is, and which byte sequences can reach glyph numbers past 366 (big) / 359 (small).

- [ ] **Step 1:** Add `EGG_STEAMTEXT_CHECK=1` (handled in `steam_text.c`, called from `main.c` after the ROM loads,
  exits after printing): parse every ROM `msg/**.msg`, rebuild each with `msg_build` from its own slots, and print
  `msgcheck: <n> files, <k> identical`. Run it before `msg_build` exists. Expected: build or link failure.
- [ ] **Step 2:** Implement `msg_parse`, `msg_slot`, `msg_build` per the spec's header layout. Expected:
  `msgcheck: 75 files, 75 identical` on the USA ROM.
- [ ] **Step 3:** Find the game's slot decoder (start from the draw-glyph caller of main 0x0203d518, which gets
  glyph numbers) and record its byte rules in DESIGN.md. Extend the check to decode every slot into tokens
  (`{kind: letter|control, value, bytes}`) and re-encode it; expected `tokens: <n> slots round trip`, all of them.

### Task 2: Learning the encoding from the two English texts

**Files:** Modify `runtime/steam_text.c`

**Interfaces:**
- Produces: an opaque `Codec` (codepoint -> DS byte sequence, tag name -> DS control bytes; the builder picks the
  map type) with `int codec_learn(Codec *c, const char *lang_steam, int rom_slot);` and
  `int codec_encode(const Codec *c, const char *steam_line, uint8_t *out, size_t cap, int *unknown_tag);`
  returning bytes written or -1.

- [ ] **Step 1:** Extend `EGG_STEAMTEXT_CHECK`: learn from Steam `en` vs slot 1 and Steam `fr` vs slot 3, then
  re-encode every Steam English line and compare with the ROM slot. Print
  `align: tags <mapped>/<used>, english <match>/<total>`. Run before `codec_learn` exists: link failure.
- [ ] **Step 2:** Implement alignment: tokenize the Steam line (letters, `\`, `<TAG>`, `<TAG>arg</TAG>`), pair
  with the ROM slot's tokens; when both sequences have the same letter count, letters vote char-to-code and tags
  vote tag-to-control-code. Keep a vote only if it wins at least 90% for that char/tag. Expected: every tag used is
  mapped; english match >= 95%.
- [ ] **Step 3:** Strings whose ROM slot is the 1-byte empty slot and whose Steam line is empty stay as is.
  Expected unchanged counts.

### Task 3: Extra glyphs drawn from TrueType

**Files:** Modify `runtime/gfx_font.c`, `runtime/gfx_font.h`, `runtime/steam_text.c`

**Interfaces:**
- Produces: `int gfxfont_extra(int small, uint32_t codepoint);` returning the glyph number (past the font's
  count) for a letter the font lacks, allocating it; `void gfxfont_extra_reset(void);`
- Produces: system font selection by language, in `steam_text.c`:
  `const char *st_system_font(const char *lang);` (Windows: `%WINDIR%\Fonts\malgun.ttf`, `msyh.ttc`,
  `msjh.ttc`, `YuGothM.ttc`, `segoeui.ttf`; Linux: `NotoSansCJK-Regular.ttc` under `/usr/share/fonts`). A loaded
  font mod's TrueType font is tried first for each letter.

- [ ] **Step 1:** Extend the check: encode every line of Steam `ko`; print `extra: <n> letters, <k> drawable`.
  Expected before implementation: link failure.
- [ ] **Step 2:** Implement: the codec gives a letter it has no code for the byte sequence the decoder maps to
  glyph `count + i` (Task 1's rules), or, if no sequence can, hook the decoder for an escape the game never uses.
  The draw and advance hooks are on whenever Steam text is on. Expected: all `ko` letters drawable with
  Malgun Gothic.
- [ ] **Step 3:** TTC fonts: stb_truetype offset of face 0 (`stbtt_GetFontOffsetForIndex`). Expected: YaHei
  letters drawable.

### Task 4: Serving the rebuilt files

**Files:** Modify `runtime/steam_text.c`, `runtime/main.c` (init), `runtime/display.c` (settings),
`runtime/system_menu.c` (page notes)

**Interfaces:**
- Produces: `void st_init(void);` (after the ROM and Steam copy load, before the game starts) and
  `void st_apply(int on, const char *lang);` (called by display.c when either setting changes; safe on the host
  thread, takes effect between game frames).
- Consumes: `se_available`, `se_language`, `se_read` (Task 1), `codec_*` (Task 2), `gfxfont_extra` (Task 3).

- [ ] **Step 1:** Locate the FAT through the ROM header (0x48 offset, 0x4C size) and the FNT (0x40) to find each
  `.msg`'s file id. Rebuilt files are appended past the original ROM size (4-aligned) in a grown `rt_rom.data`;
  their FAT start/end point there; turning off restores the original FAT entries. Swap only at VBlank on the game
  thread (a request flag set by `st_apply`, consumed in `sched.c` next to `la_frame`) so no card read sees a
  half-written FAT.
- [ ] **Step 2:** Find whether the game reads FAT entries from the card per open or caches them in RAM (watch the
  FAT range with the existing card read path). Cached: apply at start only, and the page footer for the two
  settings says "Applies the next time you start the game." Per open: live.
- [ ] **Step 3:** Buffer limits: find the game's slot buffer size (the decoder's destination) and refuse longer
  lines (DS text kept, `steam text: <file> <n>: too long` logged). Translation mods: skip files whose bytes differ
  from the verified ROM's.
- [ ] **Step 4:** Run with Steam translation on for de, ja, ko, zh-Hans and en from a prison save state; take
  windowed screenshots of the same dialogue. Expected: each language shows, names and waits work, en looks like
  the DS text. Also: Steam copy path bad -> ROM text, no crash; language switched mid-dialogue -> no crash.

### Task 5: Docs and the Japanese ROM

**Files:** `README.md`, `DESIGN.md`, `runtime/ui_text.c`, memory `steam-version-phases.md`

- [ ] **Step 1:** Run the check on the Japanese ROM (2 slots: learn from Steam `ja` vs slot 0 and `en` vs slot 1).
  Works: test one dialogue in `en` and `ko`. Fails: Steam text off on that ROM, the page note says it needs the
  USA or European ROM.
- [ ] **Step 2:** README: what the two settings do, system fonts used, font mods first. DESIGN.md: the module,
  formats, alignment, FAT swap, extra glyphs. FR/JA for any new UI string. Memory: phase 3 done.
