/* RetroAchievements (cheevos.c, rcheevos + libcurl): achievements (softcore) and rich presence for the player logged in
 * with the System Settings menu or retroachievements.ini. */
#pragma once
#include <stdint.h>

/* Host thread, before the game starts: logs in with retroachievements.ini, if it holds a login. */
void cheevos_init(void);
/* Game thread, once per game frame (VBlank): checks the achievements against the game's memory. */
void cheevos_frame(void);
/* Game thread: the game was powered on again or a save state loaded; achievements start watching afresh. */
void cheevos_reset(void);
/* Host thread, every window frame: shows the next queued notification (login, server) in the message box, and the
 * next unlock as a popup with the achievement's badge, title and points. */
void cheevos_poll(void);

/* System Settings menu (host thread). */
enum { CHEEVOS_LOGGED_OUT, CHEEVOS_LOGGING_IN, CHEEVOS_LOGGED_IN };
int cheevos_state(void);
const char *cheevos_user(void); /* the logged-in player's name, "" when not logged in */
void cheevos_login(const char *user, const char *password);
void cheevos_logout(void);

/* One achievement of the loaded game's set. */
typedef struct CheevosItem {
    char title[64], description[160];
    char progress[24]; /* how far along a locked one is, e.g. "3/10"; "" when the set does not measure it */
    char image_url[192]; /* its badge: in colour once unlocked, else greyed */
    unsigned points;
    int unlocked;
} CheevosItem;
/* The loaded game's achievements, unlocked ones first (refreshed at most twice a second); 0 until the set loads. */
int cheevos_list(const CheevosItem **items);
/* An achievement's badge, CHEEVOS_BADGE pixels square (ABGR8888, alpha included), or NULL while it downloads (asked
 * for here, and kept in the data folder's cache/badges) or when it cannot be had. */
#define CHEEVOS_BADGE 29
const uint32_t *cheevos_badge(const char *image_url);
