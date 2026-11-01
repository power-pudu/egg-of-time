/* Translation mods: ROM patches in mods/translation applied to the verified ROM before the game is built. */
#pragma once

#include <stddef.h>

/* Applies every .ips, .bps, .xdelta, .vcdiff, .xdelta3 or .delta file in mods/translation (in the data folder, rt.h),
 * in file-name order, to the loaded ROM (rt_rom.data, replaced with a malloc'd buffer). Returns 1 when a patch was
 * applied, 0 when the folder has none, or -1 on failure with a message in err. */
int translation_apply(char *err, size_t err_size);
