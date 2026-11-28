# Override cutscenes (Steam phase 4): design

## Intent

With the player's Steam copy connected, Override cutscenes (`steammovies`) plays the Steam release's HD
cutscenes in place of the DS's 256x192 Mobiclip movies. Like every Steam feature, it reads the player's own copy
and writes nothing of it anywhere.

## What the copy has (checked 2026-11-28)

- `001.dat` ... `008.dat` and `007-en.dat` in the copy's folder (beside `resources.bin`): MP4 (1920x1080 H.264,
  29.97 fps, AAC 48 kHz stereo), each byte XORed with `0xFF - (offset % 256)`. Decoded with the key, they
  start `ftyp mp42`; the index (`moov`) is not at the front, so reading needs seeking.
- Matched by picture against the DS movies (`/movie/<name>.mods`):

  | DS | Steam |
  |---|---|
  | op_ds (opening) | 001 |
  | m2_ds ... m7_ds | 002 ... 007 |
  | enosp1_ds (ending) | 008 |
  | m1_ds, enosp2_ds | none: the DS's play |

- `007-en.dat` is `007.dat` with English subtitles under its Japanese text cards (same audio, frames re-encoded).
- The Steam movies run 1-6 s longer than the DS's; a replacement plays to its end and returns, as movie mods do.

## Goals

- Steam version of each matched movie, when Override cutscenes is On and a copy is connected.
- Order: the player's `mods/cutscenes/<name>.mp4` (Mods On), else the Steam version (Override cutscenes On), else
  the DS movie. Override cutscenes does not depend on the Mods setting.
- m7: `007.dat` when the game's screens are Japanese (`ui_game_language()`: the Steam translation served, else
  the game's language), else `007-en.dat`.
- Nothing of the copy written to disk: the file is unmasked as FFmpeg reads it.
- Performance: the existing movie player (hardware decoding); unmasking is one XOR per byte read.

## Non-goals

- No Steam version for m1_ds or enosp2_ds.
- No change to how movie mods play, pace or skip.

## Approach (chosen)

`runtime/movie.c`: `replacement()` gives a source (a path, and whether it is masked). A masked source opens through
an `AVIOContext` whose read and seek callbacks use an `SDL_IOStream` on the `.dat` and XOR the bytes with the key at
their offset. Everything after `avformat_open_input` is unchanged. The setting's footer becomes "Steam release's
cutscenes, in HD." in the menu's 9 languages. README and DESIGN describe it.

Alternatives not taken: unmasking to a temporary file (writes the copy's data to disk); a table of the Steam
files inside `steam_edges.c` (the movies are not in `resources.bin`, only the folder is shared).

## Left to the builder

AVIO buffer size; log lines; the exact wording of the translations.
