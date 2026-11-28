# Override music (Steam phase 5): design

## Intent

With the player's Steam copy connected, Override music (`steammusic`) plays the Steam release's music in place of
the DS's sequenced tracks. Like every Steam feature, it reads the player's own copy and writes nothing of it
anywhere. The game must not run slower for it (the performance budget: keep game-thread work small).

## What the copy has (checked 2026-11-28)

- `Sound/BGM/BGM_xx.sab` in `resources.bin` (read with `se_read`): 85 files, `xx` hex, 01 ... 53 and 61, 62.
  0.15 to 15 MB each once unpacked. Their numbers are the DS sequence names (`BGM_00` ... `BGM_60` in
  `/sound/sound_data.sdat`): the title screen's BGM_30 recorded from the game (`EGG_WAV`) correlates 0.91 with
  Steam's BGM_30 by loudness envelope, 0.15 and 0.48 with two other tracks.
- DS tracks with no Steam file: BGM_00 and BGM_54 ... BGM_60 (with the DS-only Music Box tracks 5C-5F). They
  keep the DS music. Steam's 61 and 62 replace nothing.
- A `.sab` is a SEAD bank, little-endian: `sabf` at 0, a chunk table at 0x20 (16 bytes an entry: tag, u16, u16,
  u32 offset, u32), with `snd `, `seq `, `trk `, `mtrl`. The `mtrl` section: u16 material count at +0x04, then
  u32 offsets (from the section's start) at +0x10. A material entry:
  - +0x04 channels (u8), +0x05 codec (u8, 7 = HCA), +0x08 sample rate (u32, 48000)
  - +0x0c loop start, +0x10 loop end (u32, samples; both 0: no loop)
  - +0x14 extradata size, +0x18 stream size (u32), +0x1c extradata id (u16)
  - extradata at +0x20: +0x02 HCA header size (u16), +0x0d encryption (u8, nonzero: masked)
  - the HCA file at +0x30, `stream size + extradata size - 0x10` bytes.
- HCA (CRI), 48 kHz stereo, `ciph` type 0. Masked: every byte from the HCA header's end on is XORed with
  `key[(extradata_id + i) & 255]`, i counted from the header's end, with a fixed 256-byte table (the one vgmstream
  uses for SEAD, `sqex_streamfile.h`). Unmasked, the blocks start with the HCA sync `FF FF` and FFmpeg's `hca`
  demuxer and decoder play it (checked on BGM_01).
- BGM_01: loop 351670 ... 5305959. The HCA `loop` chunk gives the same points plus the encoder delay (1738
  samples in its `fmt` chunk): FFmpeg's output counts from before the delay, so the delay is added to the
  material's points. 64 of the 85 loop; the rest (fanfares, some scene tracks, ambience) play once.
- BGM_12 has 2 materials (3 s ambience each); every other file has 1. The first material is used.
- Level: Steam's BGM_30 is 5.7 dB quieter than the DS title screen's output (mean -23.8 dB against -18.1 dB).

## Goals

- Order for a music sequence: the player's `mods/music/BGM_xx.*` (Mods On), else Steam's `BGM_xx` (Override music
  On, a copy connected, the file exists), else the DS sequence.
- Plays like a music mod: the DS sequence runs muted, the game's fades and volume apply, the replacement stops
  with the player, decoding on the game thread in step with the mixer.
- Steam's own loop points; a track without them plays once (silent after its end while the sequence runs on),
  as the Steam release does.
- A fixed gain bringing Steam's music to the DS's level (5.7 dB from the title; confirmed on two more tracks).
- No hitch on track changes: unpacking and unmasking (tens of ms for a large file) happen on a worker thread. The
  replacement starts when its data is ready; until then the player is silent, as it is muted. The last 4 tracks
  stay unpacked in memory (a few tens of MB at most), so field and battle music switch at once.
- Nothing of the copy written to disk: FFmpeg reads the unmasked HCA from memory through an `AVIOContext`.
- The setting's footer: "Steam release's music." in the menu's 9 languages. README and DESIGN describe it.

## Non-goals

- Sound effects (phase 6, Override sounds).
- Steam's 61 and 62; the DS tracks Steam lacks.
- The banks' `snd`/`seq`/`trk` parts (volume curves, layering): the material's audio alone.

## Approach (chosen)

`runtime/music.c` gets a second kind of replacement source: memory instead of a path. A new small file
(`runtime/steam_music.c`, `steam_music.h`) owns the Steam side: a worker thread that unpacks `BGM_xx.sab` with
`se_read`, parses the bank, unmasks the first material's HCA in place, and keeps the results in a 4-entry cache;
the game thread asks for a track (non-blocking) and gets the HCA bytes and loop points once ready. `music.c` opens
them through an `AVIOContext` over the buffer (format `hca`), sets the loop from the material, and otherwise plays
it as today.

Alternatives not taken: unpacking on the game thread (a 20-80 ms hitch at each track change); unpacking every
track at connect (about 300 MB of memory, and a long start).

## Left to the builder

Thread signalling; cache eviction details; log lines; translations' wording; the exact gain after measuring.
