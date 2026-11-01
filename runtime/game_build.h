/* The game library (the recompiled code) and the ROM, ready before the game starts (game_build.c). */
#pragma once

/* Loads the ROM (rom_arg, else rom/CT.nds) and the game library from game/, both in the data folder. In a release
 * (kit/ next to the executable) it asks for the ROM when there is none, checks it, and builds the game library from it
 * on the first start, with its own screens in the window. 0 if the player quit or it failed (after telling them). */
int game_build_prepare(const char *rom_arg);
