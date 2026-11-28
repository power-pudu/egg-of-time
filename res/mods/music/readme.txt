Egg of Time - music mods
========================

Put an audio file in this folder to replace one of the game's music tracks. Name it
after the track it replaces, with the file's own extension:

    BGM_18.ogg      replaces "Chrono Trigger"
    BGM_45.flac     replaces "Battle"

A track without a file here plays as usual.

Tracks
------
Titles and numbers are the game's own, from Extras > Music Box. The game does not
list the tracks marked "not in the Music Box"; to find where one plays, run the game
from a terminal: its log names every music track as it starts ("music: BGM_xx").

    BGM_00   (not in the Music Box)
    BGM_01   Green Memories                           Music Box #5
    BGM_02   Yearnings of the Wind                    Music Box #9
    BGM_03   Corridor of Time                         Music Box #51
    BGM_04   Rhythm of Earth, Wind, and Sky           Music Box #43
    BGM_05   A Desolate World                         Music Box #24
    BGM_06   Guardia's Millennial Fair                Music Box #6
    BGM_07   Crono & Marle - A Distant Promise        Music Box #56
    BGM_08   Secret of the Forest                     Music Box #11
    BGM_09   Zeal Palace                              Music Box #52
    BGM_0A   Derelict Factory                         Music Box #33
    BGM_0B   Ayla's Theme                             Music Box #42
    BGM_0C   Guardia Castle - Pride & Glory           Music Box #13
    BGM_0D   Lavos's Theme                            Music Box #28
    BGM_0E   Robo's Theme                             Music Box #32
    BGM_0F   Morning Glow                             Music Box #3
    BGM_10   The Cathedral                            Music Box #15
    BGM_11   (not in the Music Box)
    BGM_12   (not in the Music Box)
    BGM_13   The Epoch - Wings of Time                Music Box #57
    BGM_14   Schala's Theme                           Music Box #53
    BGM_15   Jolly Ol' Spekkio                        Music Box #37
    BGM_16   Critical Moment                          Music Box #23
    BGM_17   The Trial                                Music Box #21
    BGM_18   Chrono Trigger                           Music Box #2
    BGM_19   (not in the Music Box)
    BGM_1A   (not in the Music Box)
    BGM_1B   Fanfare 1                                Music Box #20
    BGM_1C   Fanfare 2                                Music Box #35
    BGM_1D   Depths of the Night                      Music Box #50
    BGM_1E   Peaceful Days                            Music Box #4
    BGM_1F   Strange Occurrences                      Music Box #8
    BGM_20   (not in the Music Box)
    BGM_21   (not in the Music Box)
    BGM_22   (not in the Music Box)
    BGM_23   The Hidden Truth                         Music Box #22
    BGM_24   A Prayer for the Wayfarer                Music Box #16
    BGM_25   Huh!?                                    Music Box #14
    BGM_26   The Last Day of the World                Music Box #29
    BGM_27   Johnny of the Robo Gang                  Music Box #30
    BGM_28   Magus Confronted                         Music Box #47
    BGM_29   Boss Battle 1                            Music Box #18
    BGM_2A   Frog's Theme                             Music Box #19
    BGM_2B   Good Night                               Music Box #10
    BGM_2C   Bike Chase                               Music Box #31
    BGM_2D   Those Without the Will to Live           Music Box #27
    BGM_2E   Mystery from the Past                    Music Box #25
    BGM_2F   Creeping through the Sewers              Music Box #39
    BGM_30   A Premonition                            Music Box #1
    BGM_31   Ocean Palace                             Music Box #55
    BGM_32   The Final Battle                         Music Box #61
    BGM_33   Site 16                                  Music Box #26
    BGM_34   (not in the Music Box)
    BGM_35   (not in the Music Box)
    BGM_36   Burn! Bobonga! Burn!                     Music Box #44
    BGM_37   (not in the Music Box)
    BGM_38   Primeval Mountain                        Music Box #41
    BGM_39   World Revolution                         Music Box #60
    BGM_3A   (not in the Music Box)
    BGM_3B   Sealed Door                              Music Box #54
    BGM_3C   Light of Silence                         Music Box #17
    BGM_3D   Fanfare 3                                Music Box #38
    BGM_3E   At the End of Time                       Music Box #36
    BGM_3F   Outskirts of Time                        Music Box #64
    BGM_40   Strains of Insanity                      Music Box #46
    BGM_41   (not in the Music Box)
    BGM_42   Gato's Song                              Music Box #7
    BGM_43   (not in the Music Box)
    BGM_44   Black Omen                               Music Box #58
    BGM_45   Battle                                   Music Box #12
    BGM_46   Tyranno Lair                             Music Box #49
    BGM_47   (not in the Music Box)
    BGM_48   The Fiendlord's Keep                     Music Box #45
    BGM_49   Festival of Stars                        Music Box #62
    BGM_4A   (not in the Music Box)
    BGM_4B   (not in the Music Box)
    BGM_4C   (not in the Music Box)
    BGM_4D   Epilogue - To My Dear Friends            Music Box #63
    BGM_4E   Boss Battle 2                            Music Box #40
    BGM_4F   Rat-a-tat-tat, it's...Mitsuda            Music Box #65
    BGM_50   Determination                            Music Box #59
    BGM_51   Battle 2                                 Music Box #34
    BGM_52   Singing Mountain                         Music Box #48
    BGM_53   (not in the Music Box)
    BGM_54   (not in the Music Box)
    BGM_55   (not in the Music Box)
    BGM_56   (not in the Music Box)
    BGM_57   (not in the Music Box)
    BGM_58   (not in the Music Box)
    BGM_59   (not in the Music Box)
    BGM_5A   (not in the Music Box)
    BGM_5B   (not in the Music Box)
    BGM_5C   Time to Rest - After the Battle          Music Box #69
    BGM_5D   One Sunny Day When We Met                Music Box #66
    BGM_5E   Scattering Blossoms                      Music Box #67
    BGM_5F   A Meeting with Destiny                   Music Box #68
    BGM_60   (not in the Music Box)

Formats
-------
1. Any common audio file: .ogg (Vorbis), .opus, .flac, .mp3, .wav, .m4a, .aac.
   Any sample rate, mono or stereo (more channels are mixed down to stereo).
   Looping: the track loops from the LOOPSTART tag to the LOOPEND tag, or to
   LOOPSTART + LOOPLENGTH, all counted in samples (the tags vgmstream rips and
   RPG Maker use). Without these tags the whole file loops.

2. MSU-1 PCM (.pcm), the SNES MSU-1 format: "MSU1", a 4-byte loop point
   (samples, little-endian), then 44.1 kHz 16-bit stereo little-endian samples.
   It plays to its end, then continues from the loop point.

If there are several files for one track, the first in the order above is used
(.ogg, .opus, .flac, .mp3, .wav, .m4a, .aac, .pcm).

Playing
-------
- Replacements play while System Settings > Mods is On (the default). With a Steam
  copy connected and Override music on, a file here still comes first.
- The game's fades and volume changes apply to them. A jingle (fanfare) stops when
  the game's own jingle would have ended.
- Leave some headroom: a file mastered at full loudness will sound louder than the
  game's sound effects.
- After loading a save state, a replaced track starts again from its beginning.
