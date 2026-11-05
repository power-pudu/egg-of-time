/* The Minimap setting without its background (minimap.c). */
#pragma once

#include <stdint.h>

#include "video.h"

/* Which pixels of the map panel (single_screen.h: SS_MAP_W x SS_MAP_H) are the map and not its parchment: keep[] 1
 * for those. 0 if the plain parchment could not be read from the ROM: keep is not set. Host thread. */
int minimap_cutout(const VideoBottomTop *top, uint8_t *keep);
