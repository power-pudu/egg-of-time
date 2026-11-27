/* Steam showing the player in the game while Egg of Time runs (steam_presence.c). */
#pragma once

/* Main thread: starts the Steam helper once a Steam copy is connected (at start-up, or when the menu connects one);
 * again later does nothing. */
void sp_start(void);

/* Game thread, each frame: with Trigger Steam achievements on, has the helper unlock the achievement of each ending
 * the game's system data shows as seen and not yet sent. */
void sp_frame(void);
