# Steam translations (Connect Steam version, phase 3)

## Intent

The player's own Steam copy of the game holds the game's text in 13 languages. With **Use Steam translation**
on, Egg of Time shows the game's text in the **Translation language** chosen, read from that copy at run time.
Nothing of the Steam release is stored or shipped. The two settings exist on the Connect Steam version page
already and do nothing yet.

## Goals

- Every Steam language: en, ja, fr, de, it, es, ko, zh-Hans, zh-Hant and any other `Localize/<code>/msg/` the
  copy has (the setting already lists them).
- All the game's text the Steam copy has: dialogue, menus, items, techs, monsters, Extras.
- The game keeps laying text out itself: windows, pauses, names, timing and choices work as with its own text.
- Letters the DS font lacks (Korean, Chinese, Japanese on the USA/EU ROM, rare accents) are drawn from a TrueType
  font: the system's by default, a font mod's when one is loaded.

## Non-goals

- Translating text the Steam release has no line for. It stays the DS text.
- Steam-only text (cloud saves, resolution, the gamepad `_pad` variants, the PC manual).
- Fixing menus too narrow for a longer translation.
- Linux/macOS Steam copies are out of scope: Connect Steam version is Windows-first already. The text reading
  itself has no Windows code, so a copy found elsewhere works too.

## Anti-goals

- A changed ROM file on disk, or Steam text written anywhere on disk.
- A text that looks right in English and breaks the game's control codes (a lost wait, a wrong name).
- A translation mod (`mods/translation`) overridden: user mods win over Steam content.

## What the formats are

- Steam: `resources.bin` (ARC1, read by `runtime/steam_edges.c` already). `Localize/<code>/msg/<name>.txt`, one
  line per string: `<KEY>,<text>`, with `\` a line break and tags such as `<CT>`, `<PAGE>`, `<NAME_LUC>`,
  `<NUMBER>`, `<WAIT>0a</WAIT>`, `<AUTO_END>`. Key `FLD_KMES0_002` is string 2 of `kmes0`.
- DS: `msg/big/<name>.msg` and `msg/small/<name>.msg`, the same names as the Steam files. Header `"\0\0\0\0TEXT"`,
  u16 slots per string (4 on USA/EU, 2 on the Japanese ROM), u16 1, u32 file size; then one u32 offset per slot of
  every string, interleaved (string 0 slot 0, slot 1, ...), plus one end offset. A slot is bytes ending in `02`.
  USA/EU fill slot 1 (English) and slot 3 (French); slots 0 and 2 hold just `02`.
- DS text bytes: one byte per common letter (`e` 00, space 01, `a` 03, `t` 04 ...), `11` a line break, two-byte
  control codes such as `c5 af` (a number) and `c6 98` (auto end).

## Approach (chosen: swap the files as the game reads them)

1. **Swap the files (chosen).** When Use Steam translation is on, each `.msg` the Steam language has a file for is
   rebuilt in memory: every string the Steam file has a line for gets that line, encoded, in every slot (so the
   game's own language setting doesn't matter); the rest keep their DS bytes. The rebuilt files are served in
   place of the ROM's. The game's code is untouched.
2. Hook the game's text decoder and swap each string as it is decoded. Same result, more reverse-engineering, more
   fragile. Not chosen.
3. Draw Steam text over the game. Loses the game's windows and timing. Dropped.

## Decided

- **Every Steam language** (user).
- **Approach 1** (user).
- **Missing letters from a system font by default** (user): Windows, Malgun Gothic (ko), Microsoft YaHei
  (zh-Hans), Microsoft JhengHei (zh-Hant), Yu Gothic (ja, on the USA/EU ROM), Segoe UI for anything else; Linux,
  Noto Sans CJK if installed. A loaded font mod's TrueType font comes first for letters it has.
- **The encoding is learned, not guessed:** letters and tags are mapped by lining up the Steam English (and French)
  lines with the ROM's own English (and French) slots, string by string, at run time. No table copied from either
  release is shipped.
- Mod order: a `.msg` a translation mod changed keeps the mod's text.

## Calls I made (the user may correct)

- The rebuilt files are served by giving the game's file table new entries past the ROM's end in `rt_rom.data`
  (the in-memory ROM), so the card emulation serves them like any file. The ROM file on disk is never touched.
- The setting applies at the next file the game opens if the game reads its file table from the card each time;
  otherwise it applies at the next start, and the page says so. The builder checks which.
- A letter the DS font lacks gets a new glyph number past the font's last glyph (big 366, small 359), drawn by
  `gfx_font.c` from TrueType (draw and advance hooks wrapped as for font mods, Mods on or off). The builder finds
  how the game's text decoder turns bytes into glyph numbers and which byte sequences reach numbers that high; if
  none can, the decoder is hooked for those codes only.
- A Steam line with a tag the alignment never saw is left as DS text and logged, not guessed.
- File coverage: the USA ROM has 75 `.msg` files (69 in `msg/big`, 6 in `msg/small`, `system` in both); every
  one has a Steam `.txt` of the same name. Items, techs, monsters, menus and Extras are among them (`item`, `tech`,
  `monster`, `menu`, `ex_*`). No mapping layer is needed; a DS file with no Steam file stays as it is.
- Alignment needs only enough strings per tag, not every string: lines whose wording differs between the releases
  just don't vote. A tag table written in our own code (tag name to DS code, worked out from the alignment and
  checked by it) is fine; what may not be shipped is either release's text or font data. Success: every tag the
  Steam files use is mapped, and the round-trip test matches the ROM's English for at least 95% of strings.
- Sizes: a rebuilt file may be larger than the ROM's; `rt_rom.data` is grown to hold the new files. The builder
  finds the game's per-string and per-file buffer limits; a line that would exceed them stays DS text and is
  logged, so a translation can never overflow the game's memory.
- Glyphs are rasterised by our own code from TrueType and cached by us; nothing about them goes to the game's font
  data, so the game's font size doesn't limit how many there are, only the decoder's codes do (left to builder).
- Translation mods: per file. A `.msg` whose bytes in the patched ROM differ from the verified ROM's keeps them.
- Live or restart: live is preferred (the next text file the game opens); "applies at the next start" is
  acceptable if live isn't safe, and the page says so.
- The Japanese ROM: working there is wanted but not required for this phase.
- The Japanese ROM: same file format, 2 slots; the learning pass aligns its Japanese slot with Steam's Japanese. If its decoder differs,
  Steam text is off there and the page says it needs the USA or European ROM.

## Left to the builder

- The control-code table: which tags map to which DS codes, from the alignment.
- Where in the runtime the swap happens (card read or FAT) and whether it can be live.
- How much code space the extra glyphs get and how glyph numbers are allocated (per language, at build time of
  the files).
- CJK glyph size: the game's Big / Small font size settings apply as for font mods.
- Tests: an encoder round trip (Steam English re-encoded equals the ROM's English slot for every string the two
  agree on), and in-game screenshots per script family (Latin, Japanese, Korean, Chinese).

## Constraints

- Never mention inspiring projects in code or docs.
- Fix by general cause, not per string or per file.
- Never write Steam text or a changed ROM to disk.
