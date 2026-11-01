/* ROM patch decoders for translation mods (runtime/translation.c): IPS, and BPS (byuu's format). Both are checked
 * before they are applied: BPS carries CRC-32s of its source, its result and itself, so a patch meant for another
 * ROM is refused instead of corrupting it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rompatch.h"

static int fail(char *err, size_t err_size, const char *msg) {
    snprintf(err, err_size, "%s", msg);
    return 0;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

RomPatchFormat rompatch_format(const uint8_t *data, size_t size) {
    if (size >= 5 && !memcmp(data, "PATCH", 5)) return ROM_PATCH_IPS;
    if (size >= 4 && !memcmp(data, "BPS1", 4)) return ROM_PATCH_BPS;
    if (size >= 4 && data[0] == 0xD6 && data[1] == 0xC3 && data[2] == 0xC4 && data[3] == 0) return ROM_PATCH_XDELTA;
    if (size >= 4 && !memcmp(data, "%XD", 3)) return ROM_PATCH_XDELTA1; /* %XDZ004% and older xdelta 1 versions */
    return ROM_PATCH_UNKNOWN;
}

/* ---- IPS ---- */

/* Grows buf to hold at least need bytes, zeroing the new space: an IPS record can reach past the ROM's end. */
static int grow(uint8_t **buf, size_t *cap, size_t need, char *err, size_t err_size) {
    if (need <= *cap) return 1;
    size_t ncap = *cap ? *cap : 256;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    uint8_t *p = realloc(*buf, ncap);
    if (!p) return fail(err, err_size, "out of memory");
    memset(p + *cap, 0, ncap - *cap);
    *buf = p, *cap = ncap;
    return 1;
}

static int apply_ips(const uint8_t *patch, size_t n, const uint8_t *src, size_t src_size, uint8_t **out,
                     size_t *out_size, char *err, size_t err_size) {
    size_t cap = src_size ? src_size : 1, size = src_size, pos = 5;
    uint8_t *buf = malloc(cap);
    if (!buf) return fail(err, err_size, "out of memory");
    if (src_size) memcpy(buf, src, src_size);
    for (;;) {
        if (pos + 3 > n) goto bad;
        uint32_t off = be24(patch + pos);
        pos += 3;
        if (off == 0x454F46) break; /* "EOF" */
        if (pos + 2 > n) goto bad;
        size_t len = be16(patch + pos);
        pos += 2;
        if (len == 0) { /* run-length record: length, then the byte */
            if (pos + 3 > n) goto bad;
            len = be16(patch + pos);
            pos += 2;
            uint8_t value = patch[pos++];
            if (!grow(&buf, &cap, (size_t)off + len, err, err_size)) goto oom;
            memset(buf + off, value, len);
        } else { /* literal record */
            if (len > n - pos) goto bad;
            if (!grow(&buf, &cap, (size_t)off + len, err, err_size)) goto oom;
            memcpy(buf + off, patch + pos, len);
            pos += len;
        }
        if ((size_t)off + len > size) size = (size_t)off + len;
    }
    *out = buf;
    *out_size = size;
    return 1;
bad:
    free(buf);
    return fail(err, err_size, "malformed IPS patch");
oom:
    free(buf);
    return 0;
}

/* ---- BPS ---- */

/* BPS integers are bijective base-128, little endian, the last byte marked with bit 7. */
static int bps_varint(const uint8_t *p, size_t n, size_t *pos, uint64_t *value) {
    uint64_t v = 0, shift = 1;
    for (int groups = 0;; groups++) {
        if (*pos >= n || groups == 10) return 0;
        uint8_t b = p[(*pos)++];
        v += (uint64_t)(b & 0x7F) * shift;
        if (b & 0x80) break;
        shift <<= 7;
        v += shift;
    }
    *value = v;
    return 1;
}

static int apply_bps(const uint8_t *patch, size_t n, const uint8_t *src, size_t src_size, uint8_t **out,
                     size_t *out_size, char *err, size_t err_size) {
    if (n < 16) return fail(err, err_size, "malformed BPS patch");
    size_t pos = 4;
    uint64_t stated_src, target_size, metadata;
    if (!bps_varint(patch, n, &pos, &stated_src) || !bps_varint(patch, n, &pos, &target_size) ||
        !bps_varint(patch, n, &pos, &metadata))
        return fail(err, err_size, "malformed BPS patch");
    if (stated_src != src_size) return fail(err, err_size, "BPS patch is for another ROM");
    if (metadata > n - pos) return fail(err, err_size, "malformed BPS patch");
    pos += (size_t)metadata;
    if (target_size > SIZE_MAX || n - pos < 12) return fail(err, err_size, "malformed BPS patch");
    size_t size = (size_t)target_size, at = 0, src_rel = 0, tgt_rel = 0;
    uint8_t *dst = malloc(size ? size : 1);
    if (!dst) return fail(err, err_size, "out of memory");
    while (n - pos > 12) { /* the last 12 bytes are the three CRC-32s */
        uint64_t action;
        if (!bps_varint(patch, n - 12, &pos, &action)) goto bad;
        size_t len = (size_t)(action >> 2) + 1;
        switch (action & 3) {
        case 0: { /* source read: length bytes at an absolute source offset */
            uint64_t offset;
            if (!bps_varint(patch, n - 12, &pos, &offset)) goto bad;
            if (offset > src_size || len > src_size - (size_t)offset || len > size - at) goto bad;
            memcpy(dst + at, src + (size_t)offset, len);
            at += len;
            break;
        }
        case 1: /* target read: length literal bytes from the patch */
            if (len > n - 12 - pos || len > size - at) goto bad;
            memcpy(dst + at, patch + pos, len);
            pos += len;
            at += len;
            break;
        case 2: { /* source copy: relative to the last source position */
            uint64_t data;
            if (!bps_varint(patch, n - 12, &pos, &data)) goto bad;
            int64_t rel = (int64_t)(data >> 1);
            if (data & 1) rel = -rel;
            int64_t next = (int64_t)src_rel + rel;
            if (next < 0 || (uint64_t)next > src_size || len > src_size - (size_t)next || len > size - at) goto bad;
            src_rel = (size_t)next;
            memcpy(dst + at, src + src_rel, len);
            src_rel += len;
            at += len;
            break;
        }
        case 3: { /* target copy: relative to the last target position; may overlap, like a run */
            uint64_t data;
            if (!bps_varint(patch, n - 12, &pos, &data)) goto bad;
            int64_t rel = (int64_t)(data >> 1);
            if (data & 1) rel = -rel;
            int64_t next = (int64_t)tgt_rel + rel;
            if (next < 0 || (uint64_t)next >= at || len > size - at) goto bad;
            size_t from = (size_t)next;
            for (size_t i = 0; i < len; i++) dst[at + i] = dst[from + i];
            at += len;
            tgt_rel = from + len;
            break;
        }
        }
    }
    if (at != size || n - pos != 12) goto bad;
    if (crc32(patch, n - 4) != le32(patch + n - 4)) {
        free(dst);
        return fail(err, err_size, "BPS patch checksum mismatch");
    }
    if (crc32(src, src_size) != le32(patch + n - 12)) {
        free(dst);
        return fail(err, err_size, "BPS patch is for another ROM (source checksum mismatch)");
    }
    if (crc32(dst, size) != le32(patch + n - 8)) {
        free(dst);
        return fail(err, err_size, "BPS patch result checksum mismatch");
    }
    *out = dst;
    *out_size = size;
    return 1;
bad:
    free(dst);
    return fail(err, err_size, "malformed BPS patch");
}

int rompatch_apply(RomPatchFormat format, const uint8_t *patch, size_t patch_size, const uint8_t *src,
                   size_t src_size, uint8_t **out, size_t *out_size, char *err, size_t err_size) {
    switch (format) {
    case ROM_PATCH_IPS: return apply_ips(patch, patch_size, src, src_size, out, out_size, err, err_size);
    case ROM_PATCH_BPS: return apply_bps(patch, patch_size, src, src_size, out, out_size, err, err_size);
    default: return fail(err, err_size, "not an IPS or BPS patch");
    }
}
