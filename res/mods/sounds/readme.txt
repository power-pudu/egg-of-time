Egg of Time - sound mods
========================

Put audio files in this folder to replace the game's sound effects, or single
samples of its instruments.

Start with System Settings > Dump audio: it saves every sound effect and every
sample of the game as WAV files in mods\sounds\dump, named as this folder takes
them. Edit or record a file, then put it here (not in dump) under the same name.

Sound effects
-------------
A file named after an effect replaces it as heard:

    SE_0A3.wav      replaces sound effect SE_0A3 (dump\SE_0A3.wav)

There are 512 effects, SE_000 to SE_1FF. dump\effects.txt lists their lengths,
and which sample folders each one plays from.

- The file plays once, to its end, even past the end of the game's own effect.
  With LOOPSTART / LOOPEND tags it loops instead, until the game stops the effect.
- The game's volume changes for the effect apply to it.
- An effect without a file here plays as usual.

Samples
-------
A file in a folder named after a wave archive replaces one of its samples,
wherever the game plays it, in sound effects and music alike:

    WAVE_SE_01\003.wav     replaces sample 3 of WAVE_SE_01 (dump\WAVE_SE_01\003.wav)

The game still plays the notes: each note sets the sample's pitch, envelope,
volume and pan, as it did for the original. Record the sample at the original's
pitch; any sample rate works (the file's own rate is kept). It loops as the
original did (dump\waves.txt): from the same point in time, to the file's end.
LOOPSTART / LOOPEND tags set a loop of the file's own. Stereo is mixed to mono.

An effect file wins over sample files: the effect's samples are not heard then.
Sample files are read once, the first time a sample plays; restart the game after
adding some.

Formats
-------
The same as music mods (see mods\music\readme.txt): OGG Vorbis, Opus, FLAC, MP3,
WAV, M4A/AAC, or MSU-1 PCM (.pcm). LOOPSTART, LOOPEND and LOOPLENGTH tags are
counted in samples. If there are several files for one name, the first in this
order is used: .ogg, .opus, .flac, .mp3, .wav, .m4a, .aac, .pcm.

Playing
-------
- Replacements play while System Settings > Mods is On (the default).
- Fast forward plays them faster; Fast forward sound "Music only" mutes the
  effects' replacements too.
- After loading a save state, an effect's replacement that was playing stops.
