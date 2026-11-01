/* ROM patch formats for translation mods: IPS and BPS (translation.c also runs xdelta3 for VCDIFF patches and
 * kit/recomp/xdelta1.py for xdelta 1 ones). */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum RomPatchFormat {
    ROM_PATCH_UNKNOWN = 0,
    ROM_PATCH_IPS,
    ROM_PATCH_BPS,
    ROM_PATCH_XDELTA,
    ROM_PATCH_XDELTA1,
} RomPatchFormat;

/* The format a patch file holds, judged by its first bytes. */
RomPatchFormat rompatch_format(const uint8_t *data, size_t size);

/* Applies an IPS or BPS patch to the source ROM. On success returns 1 and stores the patched ROM (malloc'd, free it
 * with free) in *out and *out_size; on failure returns 0 with a message in err. */
int rompatch_apply(RomPatchFormat format, const uint8_t *patch, size_t patch_size, const uint8_t *src,
                   size_t src_size, uint8_t **out, size_t *out_size, char *err, size_t err_size);
