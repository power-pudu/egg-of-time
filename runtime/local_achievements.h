/* Egg of Time's own achievements (local_achievements.c): 31 for the story, side quests and endings, kept on this
 * computer, active only while no other achievement service is in use. */
#pragma once
#include <stdint.h>

#define LA_COUNT 31
#define LA_BADGE 29 /* badge size in pixels, square: the size display_popup and the System Settings list draw */

/* Host thread, before the game starts: reads the unlocks file in the data folder. active returns nonzero while
 * another achievement service is in use: the set is checked only while it returns 0. */
void la_init(int (*active)(void));
/* Game thread, once per game frame (VBlank): checks the locked achievements against the game's memory. */
void la_frame(void);
/* Host thread, every window frame: saves new unlocks and shows the next unlock popup. */
void la_poll(void);

/* One achievement, for the System Settings list. */
typedef struct LaItem {
    const char *title, *description;
    unsigned points;
    int unlocked;
    const uint32_t *badge; /* LA_BADGE pixels square, ABGR8888, in colour (greyed by the list while locked), or NULL */
} LaItem;
/* Host thread: the achievements, unlocked ones first, each group in the set's order. Always LA_COUNT. */
int la_list(const LaItem **items);
/* Host thread: how many are unlocked. */
int la_unlocked_count(void);
/* Host thread: forgets every unlock (System Settings' reset). Achievements the loaded game already shows as done
 * unlock again on the next frame. */
void la_reset(void);
