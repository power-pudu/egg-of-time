/* The runtime's own text in the player's language (ui_text.c). */
#pragma once

/* The language ids are the DS firmware's (boot.c), the game's own three, then the Steam release's others (the menu's
 * and the game's screens' language with its translation: never the firmware's). */
enum {
    LANGUAGE_ID_JAPANESE, LANGUAGE_ID_ENGLISH, LANGUAGE_ID_FRENCH, LANGUAGE_ID_GERMAN, LANGUAGE_ID_ITALIAN,
    LANGUAGE_ID_SPANISH, LANGUAGE_ID_CHINESE_S, LANGUAGE_ID_KOREAN, LANGUAGE_ID_CHINESE_T
};

/* The language id of a Steam translation's code (its Localize folder: "de", "zh-Hans"...); -1 unknown. */
int ui_language_of(const char *code);
/* A Steam translation's name, in its own language ("Deutsch", "한국어"); the code itself when unknown. */
const char *ui_language_name(const char *code);
/* The language the game's screens show: the Steam translation served, else the game's own. */
int ui_game_language(void);

/* en translated to the language chosen in System Settings, for its menu. */
const char *ui_text(const char *en);
/* en translated to a language (the one the game runs in, for text on the game's screens). */
const char *ui_text_in(int language, const char *en);
