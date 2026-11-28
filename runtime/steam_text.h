/* The game's text from the player's Steam copy, in the language chosen (steam_text.c). */
#pragma once

/* EGG_STEAMTEXT_CHECK=1: checks the text codec against the ROM (and the Steam copy, when found), prints the counts
 * and exits. Called once the ROM is loaded, before the game starts. */
void st_check(void);

/* Before the game starts, once the ROM and the graphics mods are in: notes the game's text files and serves the
 * language the settings ask for (the rebuilt files replace the ROM's in its file table). A change of the settings
 * applies when the game boots again (st_power_on, or the next start): it reads its text once, at boot. */
void st_init(void);
/* Powering the game on again (System Settings' restart), the game stopped: serves the language now chosen. */
void st_power_on(void);
/* The Steam language served (a code of the copy's Localize folders), "" the game's own text. */
const char *st_served(void);
