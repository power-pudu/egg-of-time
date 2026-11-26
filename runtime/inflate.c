/* DEFLATE (RFC 1951) and gzip (RFC 1952) decoding. Canonical Huffman codes are decoded a bit at a time, from the
 * count of codes of each length: small and slow, fine for the few hundred kilobytes a room needs. */
#include <string.h>

#include "inflate.h"

typedef struct Inflate {
    const uint8_t *in;
    size_t n, pos;
    uint32_t bits;
    int count;
    uint8_t *out;
    size_t out_n, op;
    int err;
} Inflate;

typedef struct Huffman {
    uint16_t count[16]; /* codes of each length */
    uint16_t symbol[320]; /* the symbols by code */
} Huffman;

static uint32_t take(Inflate *s, int need) {
    while (s->count < need) {
        if (s->pos >= s->n) {
            s->err = 1;
            return 0;
        }
        s->bits |= (uint32_t)s->in[s->pos++] << s->count;
        s->count += 8;
    }
    uint32_t v = s->bits & ((1u << need) - 1);
    s->bits >>= need;
    s->count -= need;
    return v;
}

/* The canonical code of the lengths; 0 when they over-subscribe it. */
static int build(Huffman *h, const uint8_t *length, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left = left * 2 - h->count[len];
        if (left < 0) return 0;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (uint16_t)i;
    return 1;
}

static int decode(Inflate *s, const Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)take(s, 1);
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1;
    return -1;
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inflate *s, const Huffman *lit, const Huffman *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0 || s->err) return 0;
        if (sym < 256) {
            if (s->op >= s->out_n) return 0;
            s->out[s->op++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) return 0;
            size_t len = len_base[sym] + take(s, len_extra[sym]);
            int d = decode(s, dist);
            if (d < 0 || d >= 30) return 0;
            size_t back = dist_base[d] + take(s, dist_extra[d]);
            if (s->err || back > s->op || len > s->out_n - s->op) return 0;
            for (size_t i = 0; i < len; i++, s->op++) s->out[s->op] = s->out[s->op - back];
        }
    }
}

static int fixed(Inflate *s) {
    static Huffman lit, dist;
    static int built;
    if (!built) {
        uint8_t l[288];
        memset(l, 8, 144), memset(l + 144, 9, 112), memset(l + 256, 7, 24), memset(l + 280, 8, 8);
        build(&lit, l, 288);
        memset(l, 5, 30);
        build(&dist, l, 30);
        built = 1;
    }
    return codes(s, &lit, &dist);
}

static int dynamic(Inflate *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = (int)take(s, 5) + 257, ndist = (int)take(s, 5) + 1, ncode = (int)take(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return 0;
    uint8_t l[320] = {0};
    for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)take(s, 3);
    Huffman lencode, lit, dist;
    if (!build(&lencode, l, 19)) return 0;
    memset(l, 0, sizeof l);
    for (int i = 0; i < nlen + ndist;) {
        int sym = decode(s, &lencode);
        if (sym < 0 || s->err) return 0;
        if (sym < 16) {
            l[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t v = 0;
        int rep;
        if (sym == 16) {
            if (!i) return 0;
            v = l[i - 1], rep = 3 + (int)take(s, 2);
        } else {
            rep = sym == 17 ? 3 + (int)take(s, 3) : 11 + (int)take(s, 7);
        }
        if (i + rep > nlen + ndist) return 0;
        while (rep--) l[i++] = v;
    }
    if (!l[256] || !build(&lit, l, nlen) || !build(&dist, l + nlen, ndist)) return 0;
    return codes(s, &lit, &dist);
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

long rt_gunzip(const uint8_t *in, size_t n, uint8_t *out, size_t out_n) {
    if (n < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return -1;
    int flags = in[3];
    size_t pos = 10;
    if (flags & 4) pos += 2 + (in[10] | (size_t)in[11] << 8); /* FEXTRA */
    for (int f = 8; f <= 16; f <<= 1) /* FNAME, FCOMMENT: zero-terminated */
        if (flags & f) {
            while (pos < n && in[pos]) pos++;
            pos++;
        }
    if (flags & 2) pos += 2; /* FHCRC */
    if (pos + 8 > n) return -1;
    Inflate s = {.in = in, .n = n, .pos = pos, .out = out, .out_n = out_n};
    int last;
    do {
        last = (int)take(&s, 1);
        int type = (int)take(&s, 2);
        if (s.err) return -1;
        if (type == 0) { /* stored */
            s.bits = 0, s.count = 0;
            if (s.pos + 4 > s.n) return -1;
            size_t len = s.in[s.pos] | (size_t)s.in[s.pos + 1] << 8;
            if ((len ^ 0xFFFF) != (s.in[s.pos + 2] | (size_t)s.in[s.pos + 3] << 8)) return -1;
            s.pos += 4;
            if (len > s.n - s.pos || len > s.out_n - s.op) return -1;
            memcpy(s.out + s.op, s.in + s.pos, len);
            s.pos += len, s.op += len;
        } else if (type == 1) {
            if (!fixed(&s)) return -1;
        } else if (type == 2) {
            if (!dynamic(&s)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    /* The trailer follows the last block's byte (an archive may pad the member after it). */
    if (s.pos + 8 > n) return -1;
    const uint8_t *t = in + s.pos;
    uint32_t crc = t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
    uint32_t size = t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
    if (size != (uint32_t)s.op || crc != crc32_of(out, s.op)) return -1;
    return (long)s.op;
}
