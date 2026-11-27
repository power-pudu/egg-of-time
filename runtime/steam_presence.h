/* Steam showing the player in the game while Egg of Time runs (steam_presence.c). */
#pragma once

/* Main thread: starts the Steam helper once a Steam copy is connected (at start-up, or when the menu connects one);
 * again later does nothing. */
void sp_start(void);
