/* The runtime's own text in the player's language (ui_text.c). */
#pragma once

/* The language ids are the DS firmware's (boot.c). */
enum { LANGUAGE_ID_JAPANESE, LANGUAGE_ID_ENGLISH, LANGUAGE_ID_FRENCH };

/* en translated to the language chosen in System Settings, for its menu. */
const char *ui_text(const char *en);
/* en translated to a language (the one the game runs in, for text on the game's screens). */
const char *ui_text_in(int language, const char *en);
