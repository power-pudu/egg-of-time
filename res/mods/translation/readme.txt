Egg of Time - translation mods
==============================

Put a translation patch in this folder to play the game in another language. Egg of
Time applies it to your own ROM when it builds the game, so rom/CT.nds stays the
unmodified dump. The patched game is not a new ROM file; it is only used in memory
(and for that one build).

Supported formats:

    .ips            IPS
    .bps            BPS
    .xdelta         xdelta3 / VCDIFF (.vcdiff and .xdelta3 also work)
    .delta          xdelta 1

Patches are applied in file-name order, each to the result of the one before. A
translation split into several files therefore works, and so does a base patch plus
a fix. To apply a patch after another, give it a name that sorts later, for example
01-base.xdelta and 02-fix.xdelta.

Patches for another version of the game
---------------------------------------

Egg of Time runs the USA version, but many translations were made for Chrono Trigger
(Japan) or (Europe). Put your own dump of the version the patches were made for in this folder
too (any file name ending in .nds). The patches are then applied to that ROM, and
what they translated (text, fonts, graphics) is carried over to the USA game. Text
the translation left untouched stays in English. The log (eggoftime.log) lists what
was carried over.

Only one .nds may be in this folder. A translation that changes the game's code
cannot be carried over, and the game says so; changes to the ARM7's code (a flash
cart fix, for example) are left out, because the port does not run that code.

Notes:

- rom/CT.nds must be the unmodified Chrono Trigger (USA) dump, as the game checks.
- A translation patch usually also translates the game's graphics and text in the
  ROM; the port plays them as the patch left them.
- Changing the patch files makes the next start build the game again (a few minutes).
- BPS and xdelta 1 patches that were made for another ROM are refused (an xdelta 1
  patch still applies to another dump of the same version when the result matches).
- Many xdelta patches were made from the ROM as ndstool unpacks and repacks it (the
  patchers they came with do that first). When a patch does not fit your dump, Egg of
  Time rebuilds the dump that way and applies the patch to the result.
- xdelta3 is needed for xdelta patches that compress their data: release builds
  include it in kit/xdelta3; a kit built without it uses one from PATH.
