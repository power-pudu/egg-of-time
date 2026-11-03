Egg of Time
===========

A native PC port of Chrono Trigger (Nintendo DS), made by static recompilation.

This download contains no part of the game. You need your own dump of Chrono Trigger (USA) for the
Nintendo DS (game code YQUE, SHA-1 11f9e8204e484eab42181b210c9f214584be75bf).

Getting started
---------------
1. Unzip this folder anywhere you can write to (not Program Files).
2. Start "Egg of Time.exe" and choose your ROM file (.nds). It is copied to rom\CT.nds.
3. The first start builds the game from your ROM: a few minutes, with a progress bar. Later starts
   begin at once. A new version of Egg of Time builds it again when it needs to.

The build uses the tools in kit\ (dsd, Python, LLVM/clang); nothing is installed and nothing is
downloaded. What it makes stays in game\. If it fails, game\build.log has the details.

Your saves (rom\CT.sav), save states (rom\CT.state, F5 / F7) and settings (eggoftime.ini) stay in
this folder. To update, unzip the new version over this one.

Controls: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
F2 layout, F3 swap screens, F4 filter, F11 fullscreen, hold Left Shift to fast forward, hold Esc to quit.
Gamepads work too (hold the right trigger to fast forward). Remap keys and gamepad buttons in
System Settings > Controls.
Mods: replacement cutscenes and music go in mods\cutscenes and mods\music, and translation patches
(.ips, .bps, .xdelta, .delta) in mods\translation (see the readme files there). A translation
patch is applied to your ROM when the game is built, in memory: rom\CT.nds stays the unmodified
dump. Patches made for Chrono Trigger (Japan) or (Europe) work too, with your dump of that version
beside them. RetroArch shaders go in mods\shaders\glsl (.glslp, .glsl) and mods\shaders\slang
(.slangp, .slang), and are chosen in System Settings > Shader.
System Settings > Dump graphics saves the game's sprites and backgrounds as PNG files in
mods\graphics\dump, for making graphics mods: edited copies in mods\graphics replace them, at the
original size or in HD (see the readme there).

This is an unofficial fan project, not affiliated with or endorsed by Square Enix or Nintendo.
Chrono Trigger is a trademark of Square Enix. Egg of Time is MIT-licensed (LICENSE.md); third-party
licenses are in kit\licenses.
