/* The game's text from the player's Steam copy (Connect Steam version > Use Steam translation): nothing of it is
 * stored or shipped.
 *
 * The game's text files (msg/big/<name>.msg, msg/small/<name>.msg): "\0\0\0\0TEXT", a byte: slots per string (4 in the
 * USA and European games: English in slot 1, French in 3; 2 in the Japanese one), 3 bytes: strings, u32 the file's
 * size; then
 * a u32 offset for each slot of each string, interleaved (string 0's slots, then string 1's...); the last slot ends
 * at the file's end. Each slot's bytes end with code 2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx_font.h"
#include "gfx_rom.h"
#include "display.h"
#include "rt.h"
#include "steam_edges.h"
#include "steam_text.h"

typedef struct Msg {
    int slots, strings, end; /* end: the code ending each slot (2 in msg/big, 0 in msg/small) */
    const uint8_t *data;
    size_t size;
} Msg;

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

/* 1 when p..p+n is a text file whose offsets all lie in it, in order. */
static int msg_parse(const uint8_t *p, size_t n, Msg *m) {
    if (n < 20 || memcmp(p, "\0\0\0\0TEXT", 8)) return 0;
    int slots = p[8];
    uint32_t strings = p[9] | (uint32_t)p[10] << 8 | (uint32_t)p[11] << 16, count = (uint32_t)slots * strings;
    if (!count || le32(p + 12) != n || 16 + 4 * (uint64_t)count > n) return 0;
    uint32_t prev = 16 + 4 * count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t o = le32(p + 16 + 4 * i);
        if (o < prev || o > n) return 0;
        prev = o;
    }
    int end = -1; /* the last byte of the first slot that has bytes */
    for (uint32_t i = 0; i < count && end < 0; i++) {
        uint32_t a = le32(p + 16 + 4 * i), b = i + 1 < count ? le32(p + 20 + 4 * i) : (uint32_t)n;
        if (b > a) end = p[b - 1];
    }
    if (end < 0) return 0;
    *m = (Msg){slots, (int)strings, end, p, n};
    return 1;
}

/* String s's bytes in a slot (its end code included). */
static const uint8_t *msg_slot(const Msg *m, int s, int slot, size_t *len) {
    uint32_t i = (uint32_t)(s * m->slots + slot), a = le32(m->data + 16 + 4 * i);
    uint32_t b = i + 1 < (uint32_t)(m->strings * m->slots) ? le32(m->data + 20 + 4 * i) : (uint32_t)m->size;
    *len = b - a;
    return m->data + a;
}

/* A text file with m's header and these slots' bytes (strings * slots of them, in the file's order); malloc'd. */
static uint8_t *msg_build(const Msg *m, uint8_t *const *bytes, const size_t *lens, size_t *out_len) {
    int total = m->strings * m->slots;
    size_t size = 16 + 4 * (size_t)total;
    for (int k = 0; k < total; k++) size += lens[k];
    uint8_t *out = malloc(size);
    if (!out || size > 0xFFFFFFFFu) return free(out), NULL;
    memcpy(out, m->data, 12);
    put32(out + 12, (uint32_t)size);
    uint32_t at = 16 + 4 * (uint32_t)total;
    for (int k = 0; k < total; k++) {
        put32(out + 16 + 4 * k, at);
        memcpy(out + at, bytes[k], lens[k]);
        at += (uint32_t)lens[k];
    }
    *out_len = size;
    return out;
}

/* ---- codes ---- */

/* A slot's bytes are UTF-8 over the game's own code numbers (main 0x0203d800 reads them: 1, 2 or 3 bytes, a byte
 * from 0xF0 up standing alone). A code below the font's glyph count is that glyph (in msg/big: e 0, space 1, a 3...,
 * 0x11 a line break; a slot's last byte ends it: 2 in msg/big, 0 in msg/small); the control codes (a name, a number, a wait...) lie above, from about 0x16E to 0x19C;
 * any other code is drawn as glyph number code, so the codes from CODE_EXTRA up are free for letters the game's
 * fonts lack (gfx_font.c draws them). */
#define CODE_EXTRA 0x1000u

static uint32_t code_get(const uint8_t *s, size_t n, size_t *i) {
    uint32_t b = s[*i];
    if (b < 0x80 || b >= 0xF0) return (*i)++, b;
    if (b < 0xE0 && *i + 1 < n) {
        uint32_t c = (b & 0x1F) << 6 | (s[*i + 1] & 0x3F);
        *i += 2;
        return c;
    }
    if (b >= 0xE0 && *i + 2 < n) {
        uint32_t c = (b & 0x0F) << 12 | (uint32_t)(s[*i + 1] & 0x3F) << 6 | (s[*i + 2] & 0x3F);
        *i += 3;
        return c;
    }
    return (*i)++, b;
}

static size_t code_put(uint32_t c, uint8_t *o) {
    if (c < 0x80) return o[0] = (uint8_t)c, 1;
    if (c < 0x800) return o[0] = (uint8_t)(0xC0 | c >> 6), o[1] = (uint8_t)(0x80 | (c & 0x3F)), 2;
    o[0] = (uint8_t)(0xE0 | (c >> 12 & 0x0F)), o[1] = (uint8_t)(0x80 | (c >> 6 & 0x3F)), o[2] = (uint8_t)(0x80 | (c & 0x3F));
    return 3;
}

/* ---- the Steam text ---- */

/* A Steam text file (Localize/<lang>/msg/<name>.txt) as its lines, one a string, without the key some files start
 * each line with ("FLD_KMES0_002,"). Most files have a line for each string of the game's file of the same name, in
 * its order; a few have lines more or fewer (map_lines pairs those). */
typedef struct Lines { char *buf; char **at, **key; int count; } Lines; /* key: NULL in a file without them */

static void lines_free(Lines *l) { free(l->buf), free(l->at), free(l->key), *l = (Lines){0}; }

static int keyed(const char *s) { /* MSG_..., FLD_KMES0_002,: never a line of text ("NO, ...") */
    int under = 0;
    if (*s < 'A' || *s > 'Z') return 0;
    for (; (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'; s++) under |= *s == '_';
    return under && *s == ',';
}

static int lines_read(const char *lang, const char *base, Lines *l) {
    char name[160];
    size_t n;
    snprintf(name, sizeof name, "Localize/%s/msg/%s.txt", lang, base);
    uint8_t *p = se_read(name, &n);
    *l = (Lines){0};
    if (!p) return 0;
    l->buf = malloc(n + 1);
    l->at = malloc((n + 1) * sizeof *l->at);
    if (!l->buf || !l->at) return free(p), lines_free(l), 0;
    memcpy(l->buf, p, n), l->buf[n] = 0;
    free(p);
    char *s = l->buf + (n >= 3 && !memcmp(l->buf, "\xEF\xBB\xBF", 3) ? 3 : 0);
    while (*s) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        size_t len = strlen(s);
        if (len && s[len - 1] == '\r') s[len - 1] = 0;
        l->at[l->count++] = s;
        if (!e) break;
        s = e + 1;
    }
    int with_keys = 0; /* a keyed file: most of its lines (one odd line keeps the others' keys out of the text) */
    for (int i = 0; i < l->count; i++) with_keys += keyed(l->at[i]);
    if (with_keys * 2 > l->count && (l->key = malloc((size_t)l->count * sizeof *l->key))) {
        for (int i = 0; i < l->count; i++) {
            char *comma = keyed(l->at[i]) ? strchr(l->at[i], ',') : NULL;
            l->key[i] = "";
            if (comma) *comma = 0, l->key[i] = l->at[i], l->at[i] = comma + 1;
        }
    }
    return 1;
}

/* A line as tokens: a letter (its code point) or a tag: <NAME>, </NAME>, and \ (a line break), by its index in
 * tags[]. */
typedef struct Tok { uint32_t v; uint8_t tag; } Tok;
#define TOKENS 2048
#define TAGS 256
static char tags[TAGS][32];
static int tag_count;

static int tag_id(const char *s, size_t n) {
    for (int i = 0; i < tag_count; i++) {
        if (strlen(tags[i]) == n && !memcmp(tags[i], s, n)) return i;
    }
    if (tag_count == TAGS || n >= sizeof tags[0]) return -1;
    memcpy(tags[tag_count], s, n), tags[tag_count][n] = 0;
    return tag_count++;
}

static int tokenize(const char *s, Tok *t) {
    int n = 0;
    size_t left = strlen(s);
    while (left) {
        if (n == TOKENS) return -1;
        const char *close = *s == '<' ? memchr(s, '>', left) : NULL;
        if (*s == '\\' || close) {
            size_t len = close ? (size_t)(close - s) - 1 : 1;
            int id = tag_id(close ? s + 1 : s, len);
            if (id < 0) return -1;
            t[n++] = (Tok){(uint32_t)id, 1};
            size_t step = close ? len + 2 : 1;
            s += step, left -= step;
            continue;
        }
        uint32_t c = SDL_StepUTF8(&s, &left);
        if (c == SDL_INVALID_UNICODE_CODEPOINT || c > 0xFFFF) return -1;
        t[n++] = (Tok){c, 0};
    }
    return n;
}

/* A slot as codes, without its end code (its last byte). */
static int slot_codes(const uint8_t *b, size_t len, uint16_t *out) {
    size_t at = 0;
    int n = 0;
    while (at + 1 < len && n < TOKENS) out[n++] = (uint16_t)code_get(b, len - 1, &at);
    return n;
}

/* ---- learning the codes ---- */

/* What code each letter and tag is, for each of the game's two fonts (msg/big and msg/small number their glyphs
 * apart), learned by lining up the Steam text with the game's own in the same language (English and French;
 * Japanese in the Japanese game), string by string: where both have as many tokens as codes, each token votes for
 * the code at its place. A letter or tag takes the code that won at least 90% of its votes, a letter at least 2 (one
 * line could be off by a code). Codes are kept + 1 (0: none). font: the one the file being worked on uses. */
#define KEYS (0x10000 + TAGS) /* a font's letters, then its tags */
static uint16_t codes[2][KEYS];
static int font, end_code; /* the font and end code of the file being worked on */
static int extras; /* letters without a code get extra ones (gfxfont_extra) */

static int font_of(const char *path) { return strstr(path, "/small/") != NULL; }

typedef struct Vote { uint64_t key; uint32_t n; } Vote; /* key: font bit 41, tag bit 40, letter or tag 16-39, code + 1 */
#define VOTES (1u << 19)
static Vote *votes;
static uint32_t vote_count; /* keys in votes: no new one past 3/4 full */

static void vote(uint64_t key) {
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 45) & (VOTES - 1);
    while (votes[h].key && votes[h].key != key) h = (h + 1) & (VOTES - 1);
    if (!votes[h].key && vote_count >= VOTES / 4 * 3) return;
    vote_count += !votes[h].key;
    votes[h].key = key, votes[h].n++;
}

static void learn_pair(const char *line, const uint8_t *slot, size_t len) {
    static Tok t[TOKENS];
    static uint16_t c[TOKENS];
    int nt = tokenize(line, t), nc = slot_codes(slot, len, c);
    int skip = nc - nt; /* codes in front the Steam text has no token for (an item's icon) */
    if (nt <= 0 || skip < 0 || skip > 2) return;
    for (int i = 0; i < nt; i++) {
        if (c[skip + i] == 0xFFFF) continue; /* + 1 would not fit its 16 bits */
        vote((uint64_t)font << 41 | (uint64_t)t[i].tag << 40 | (uint64_t)t[i].v << 16 | ((uint64_t)c[skip + i] + 1));
    }
}

static void decide(void) {
    static uint32_t best[2][KEYS], total[2][KEYS];
    static uint16_t code[2][KEYS];
    memset(best, 0, sizeof best), memset(total, 0, sizeof total);
    for (uint32_t i = 0; i < VOTES; i++) {
        if (!votes[i].key) continue;
        uint64_t key = votes[i].key;
        uint32_t f = (uint32_t)(key >> 41) & 1, from = (uint32_t)(key >> 16 & 0xFFFFFF), k = key >> 40 & 1 ? 0x10000 + from : from;
        if (k >= KEYS) continue;
        total[f][k] += votes[i].n;
        if (votes[i].n > best[f][k]) best[f][k] = votes[i].n, code[f][k] = (uint16_t)(key & 0xFFFF);
    }
    for (int f = 0; f < 2; f++) {
        for (uint32_t k = 0; k < KEYS; k++) codes[f][k] = best[f][k] >= (k < 0x10000 ? 2u : 1u) && best[f][k] * 10 >= total[f][k] * 9 ? code[f][k] : 0;
    }
}

/* The game's code + 1 for letter v of the current font (0 none). A fullwidth form the font lacks (Japanese and Chinese
 * text write digits and Latin letters so, as in the tables the game builds numbers and names from) is its plain
 * letter. */
static uint16_t letter_code(uint32_t v) {
    if (codes[font][v]) return codes[font][v];
    if (v >= 0xFF01 && v <= 0xFF5E && codes[font][v - 0xFEE0]) return codes[font][v - 0xFEE0];
    if (v == 0x3000) return codes[font][' '];
    return 0;
}

/* A line as the game's bytes, its end code included; -1 when a letter or tag has no code (*why says which, in
 * part), or it doesn't fit. */
static int encode(const char *line, uint8_t *out, size_t cap, const char **why) {
    static Tok t[TOKENS];
    int nt = tokenize(line, t);
    size_t o = 0;
    if (nt < 0) return *why = "too long", -1;
    for (int i = 0; i < nt; i++) {
        uint16_t c = t[i].tag ? codes[font][0x10000 + t[i].v] : letter_code(t[i].v);
        if (!c && !t[i].tag && extras) { /* a letter the game's font lacks: one drawn from TrueType */
            int g = gfxfont_extra(font, t[i].v);
            if (g >= 0) c = (uint16_t)(g + 1);
        }
        if (!c) return *why = t[i].tag ? tags[t[i].v] : "letter", -1;
        if (o + 4 > cap) return *why = "too long", -1;
        o += code_put(c - 1u, out + o);
    }
    if (o + 1 > cap) return *why = "too long", -1;
    out[o++] = end_code;
    return (int)o;
}

/* String s of file m in a Steam line: the codes the game's own text puts before the words that the Steam text has
 * no token for (an item's icon: those its slot ref_slot has in front of what ref_line, the Steam line in that
 * slot's language, encodes to) come first, then the line. 0 when the line is empty and the game's slot isn't (that
 * string keeps its text), -1 when it can't be encoded. */
static int encode_string(const Msg *m, int s, int ref_slot, const char *ref_line, const char *line, uint8_t *out, size_t cap,
                         const char **why) {
    static uint8_t ref[TOKENS * 3 + 1];
    size_t rl;
    const uint8_t *r = msg_slot(m, s, ref_slot, &rl);
    if (!*line && rl > 1) return *why = "empty", 0;
    const char *ignored;
    int el = ref_line ? encode(ref_line, ref, sizeof ref, &ignored) : -1;
    size_t keep = el > 0 && (size_t)el < rl && !memcmp(r + rl - (size_t)el, ref, (size_t)el) ? rl - (size_t)el : 0;
    if (keep >= cap) return *why = "too long", -1;
    memcpy(out, r, keep);
    int n = encode(line, out + keep, cap - keep, why);
    return n < 0 ? -1 : (int)keep + n;
}

/* Which Steam line each string of a file is (-1: none), from the language learned in (its slot): line i is
 * string i when the counts agree; else the lines whose bytes equal a string's pair up in order (the longest such
 * sequence), and between two pairs as many lines as strings pair up in order too. map: m->strings entries. */
static void map_lines(const Msg *m, const Lines *l, int slot, int *map) {
    for (int s = 0; s < m->strings; s++) map[s] = s < l->count && l->count == m->strings ? s : -1;
    if (l->count == m->strings) return;
    int n = m->strings, k = l->count;
    uint16_t *dp = calloc((size_t)(n + 1) * (size_t)(k + 1), sizeof *dp);
    uint8_t *eq = calloc((size_t)n * (size_t)(k ? k : 1), 1);
    if (!dp || !eq) return free(dp), free(eq);
    static uint8_t buf[TOKENS * 3 + 1];
    for (int j = 0; j < k; j++) {
        const char *why;
        int len = encode(l->at[j], buf, sizeof buf, &why);
        for (int s = 0; len > 0 && s < n; s++) {
            size_t sl;
            const uint8_t *b = msg_slot(m, s, slot, &sl);
            eq[(size_t)s * (size_t)k + (size_t)j] = len > 1 && sl >= (size_t)len && sl <= (size_t)len + 6 && !memcmp(b + sl - len, buf, (size_t)len);
        }
    }
#define DP(a, b) dp[(size_t)(a) * (size_t)(k + 1) + (size_t)(b)]
    for (int s = n - 1; s >= 0; s--) {
        for (int j = k - 1; j >= 0; j--) {
            DP(s, j) = eq[(size_t)s * (size_t)k + (size_t)j] ? (uint16_t)(DP(s + 1, j + 1) + 1) : SDL_max(DP(s + 1, j), DP(s, j + 1));
        }
    }
    int s = 0, j = 0, ps = 0, pj = 0;
    while (s <= n && j <= k) {
        int anchor = s < n && j < k && eq[(size_t)s * (size_t)k + (size_t)j];
        if (anchor || s == n || j == k) {
            if (s - ps == j - pj) {
                for (int i = 0; i < s - ps; i++) map[ps + i] = pj + i; /* a gap of as many lines as strings */
            }
            if (!anchor) break;
            map[s] = j;
            ps = ++s, pj = ++j;
        } else if (DP(s + 1, j) >= DP(s, j + 1)) {
            s++;
        } else {
            j++;
        }
    }
#undef DP
    free(dp), free(eq);
}

/* The base of a text file's name: msg/big/kmes0.msg -> kmes0. */
static void base_name(const char *path, char *out, size_t cap) {
    const char *b = strrchr(path, '/');
    snprintf(out, cap, "%s", b ? b + 1 : path);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

/* The languages to learn from: the Steam language and the game's slot holding the same one. */
static const struct { const char *lang; int slot; } ref_usa[] = {{"en", 1}, {"fr", 3}}, ref_jp[] = {{"ja", 0}, {"en", 1}};

/* Learns the codes from the game's text files (rt_rom.data as the game first had it) and the Steam copy. 1 when the
 * copy has text to learn from. */
static int learn(void) {
    votes = calloc(VOTES, sizeof *votes);
    if (!votes) return 0;
    int files = 0;
    for (int pass = 0; pass < 2; pass++) { /* the files whose counts agree, then the others, paired by those */
        for (int i = 0, n = gfx_list_files(); i < n; i++) {
            const GfxFile *f = gfx_file(i);
            Msg m;
            char base[64];
            if (!gfx_ends_with(f->path, ".msg") || !msg_parse(rt_rom.data + f->offset, f->size, &m)) continue;
            base_name(f->path, base, sizeof base);
            font = font_of(f->path), end_code = m.end;
            for (int r = 0; r < 2; r++) {
                const char *lang = RT_JAPAN ? ref_jp[r].lang : ref_usa[r].lang;
                int slot = RT_JAPAN ? ref_jp[r].slot : ref_usa[r].slot;
                Lines l;
                if (slot >= m.slots || !lines_read(lang, base, &l)) continue;
                if ((l.count == m.strings) == !pass) {
                    int *map = malloc((size_t)m.strings * sizeof *map);
                    if (map) map_lines(&m, &l, slot, map);
                    for (int s = 0; map && s < m.strings; s++) {
                        size_t len;
                        const uint8_t *b = msg_slot(&m, s, slot, &len);
                        if (map[s] >= 0) learn_pair(l.at[map[s]], b, len);
                    }
                    free(map);
                    files += !pass && !r;
                }
                lines_free(&l);
            }
        }
        decide();
    }
    free(votes), votes = NULL;
    return files > 0;
}

/* ---- the fonts for letters the game's lack ---- */

/* The system's fonts that have the language's letters (Windows: its Fonts folder; elsewhere Noto CJK where it's
 * usually installed), then one for anything else. A font mod's own .ttf comes first (gfx_font.c). */
static void load_fonts(const char *lang) {
#ifdef _WIN32
    static const struct { const char *lang, *file; } by_lang[] = {
        {"ja", "YuGothM.ttc"}, {"ja", "msgothic.ttc"}, {"ko", "malgun.ttf"}, {"zh-Hans", "msyh.ttc"},
        {"zh-Hant", "msjh.ttc"}, {"", "segoeui.ttf"}, {"", "msyh.ttc"}, {"", "malgun.ttf"},
    };
    const char *win = getenv("WINDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s\\Fonts\\", win ? win : "C:\\Windows");
#else
    static const struct { const char *lang, *file; } by_lang[] = {
        {"", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"}, {"", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"},
        {"", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"}, {"", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"},
    };
    const char *dir = "";
#endif
    gfxfont_extra_reset();
    for (int pass = 0; pass < 2; pass++) { /* the language's own, then the others */
        for (size_t i = 0; i < SDL_arraysize(by_lang); i++) {
            if ((*by_lang[i].lang == 0) != pass || (pass == 0 && strcmp(by_lang[i].lang, lang))) continue;
            int again = 0; /* already loaded for the language */
            for (size_t k = 0; k < i; k++) again |= !strcmp(by_lang[k].file, by_lang[i].file) && !strcmp(by_lang[k].lang, lang);
            if (pass && again) continue;
            char path[600];
            snprintf(path, sizeof path, "%s%s", dir, by_lang[i].file);
            gfxfont_fallback(path);
        }
    }
}

/* ---- serving files ---- */

/* Files rebuilt, waiting to be served. */
typedef struct Built { uint32_t id; uint8_t *data; size_t size; } Built;

/* The files serve these bytes from now on: copied to a buffer of their own that card reads find past the ROM's end
 * (rt_rom.more: rt_rom.data never moves, sdat.c keeps pointers into it), their FAT entries pointed there. Returns 0
 * when out of memory (the files keep their bytes). With the game stopped (before it starts, or powering on again). */
static uint8_t *overlay;
static int serve(const Built *b, int n) {
    uint32_t base = (uint32_t)((rt_rom.size + 0x1FF) & ~(size_t)0x1FF);
    size_t end = 0;
    for (int i = 0; i < n; i++) end = ((end + 3) & ~(size_t)3) + b[i].size;
    if (!n || (uint64_t)base + end > 0xFFFFFFFFu) return 0;
    uint8_t *o = malloc(end);
    if (!o) return 0;
    memset(o, 0xFF, end);
    uint32_t fat = le32(rt_rom.data + 0x48);
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        at = (at + 3) & ~(size_t)3;
        memcpy(o + at, b[i].data, b[i].size);
        put32(rt_rom.data + fat + b[i].id * 8, (uint32_t)(base + at));
        put32(rt_rom.data + fat + b[i].id * 8 + 4, (uint32_t)(base + at + b[i].size));
        at += b[i].size;
    }
    overlay = o, rt_rom.more = o, rt_rom.more_at = base, rt_rom.more_size = (uint32_t)end;
    return 1;
}

/* ---- the files served ---- */

/* The game's text files as the ROM has them (their FAT entries), and whether a translation mod changed one (its
 * bytes differ from the dump's: it keeps them). */
typedef struct File { char path[128]; uint32_t id, start, end; int modded; } File;
static File *files;
static int file_count, listed, learned = -1; /* learned: -1 not tried yet */
static char applied[16]; /* the language served, "" none */

/* The dump's bytes of file id (rt_rom.path: never patched); NULL if unreadable. */
static uint8_t *dump_file(SDL_IOStream *io, uint32_t id, uint32_t *size) {
    uint8_t h[8], e[8];
    if (!io || SDL_SeekIO(io, 0x48, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, h, 4) != 4) return NULL;
    if (SDL_SeekIO(io, (Sint64)le32(h) + id * 8, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, e, 8) != 8) return NULL;
    uint32_t a = le32(e), b = le32(e + 4);
    uint8_t *p = b > a && b - a < (64u << 20) ? malloc(b - a) : NULL;
    if (p && (SDL_SeekIO(io, a, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, p, b - a) != b - a)) free(p), p = NULL;
    *size = b - a;
    return p;
}

static void list_files(void) {
    if (listed) return;
    listed = 1;
    SDL_IOStream *io = SDL_IOFromFile(rt_rom.path, "rb");
    if (!io) rt_log("steam text: cannot read %s, so a translation mod's text files are not told apart", rt_rom.path);
    for (int i = 0, n = gfx_list_files(); i < n; i++) {
        const GfxFile *f = gfx_file(i);
        if (!gfx_ends_with(f->path, ".msg")) continue;
        File *nf = realloc(files, (size_t)(file_count + 1) * sizeof *nf);
        if (!nf) break;
        files = nf;
        uint32_t size = 0;
        uint8_t *dump = dump_file(io, f->id, &size);
        int same = !io || (dump && size == f->size && !memcmp(dump, rt_rom.data + f->offset, size));
        free(dump);
        File *nf2 = &files[file_count++];
        snprintf(nf2->path, sizeof nf2->path, "%s", f->path);
        nf2->id = f->id, nf2->start = f->offset, nf2->end = f->offset + f->size, nf2->modded = !same;
        if (!same) rt_log("steam text: %s keeps a translation mod's text", f->path);
    }
    SDL_CloseIO(io);
}

/* File m rebuilt with the Steam lines of `lang`: each slot holding text (the game's languages) gets the line of its
 * string, where it can be encoded; *kept counts the strings that keep the game's text, and the first few are
 * logged. NULL when the copy has no such file or its lines can't be paired. */
static uint8_t *rebuild(const Msg *m, const char *path, const char *lang, size_t *out_len, int *kept) {
    char base[64];
    base_name(path, base, sizeof base);
    const char *ref_lang = RT_JAPAN ? "ja" : "en";
    int ref_slot = RT_JAPAN ? 0 : 1;
    Lines l, ref;
    if (!lines_read(lang, base, &l)) return NULL;
    int *pick = NULL; /* the line of `lang` for each line of the reference language: by key, else by place */
    if (lines_read(ref_lang, base, &ref) && ref_slot < m->slots && (pick = malloc((size_t)(ref.count + 1) * sizeof *pick))) {
        for (int j = 0, next = 0; j < ref.count; j++) {
            pick[j] = !ref.key && !l.key && ref.count == l.count ? j : -1;
            for (int t = 0; ref.key && l.key && t < l.count && pick[j] < 0; t++) {
                int at = (next + t) % l.count; /* usually the next one */
                if (*ref.key[j] && !strcmp(ref.key[j], l.key[at])) pick[j] = at, next = at + 1;
            }
        }
    }
    if (!pick) {
        lines_free(&l), lines_free(&ref);
        return NULL;
    }
    int total = m->strings * m->slots, *map = malloc((size_t)m->strings * sizeof *map);
    uint8_t **bytes = calloc((size_t)total, sizeof *bytes), *out = NULL;
    size_t *lens = calloc((size_t)total, sizeof *lens);
    char *owned = calloc((size_t)total, 1);
    if (map && bytes && lens && owned) {
        map_lines(m, &ref, ref_slot, map);
        static uint8_t buf[TOKENS * 3 + 8];
        for (int s = 0; s < m->strings; s++) {
            int n = -2;
            const char *why = "no line";
            if (map[s] >= 0 && pick[map[s]] >= 0) n = encode_string(m, s, ref_slot, ref.at[map[s]], l.at[pick[map[s]]], buf, sizeof buf, &why);
            if (n < 0 && (*kept)++ < 8) rt_log("steam text: %s %d keeps its text (%s)", base, s, why);
            for (int k = 0; k < m->slots; k++) {
                int i = s * m->slots + k;
                const uint8_t *b = msg_slot(m, s, k, &lens[i]);
                bytes[i] = (uint8_t *)b;
                if (n <= 0 || lens[i] <= 1 || !(bytes[i] = malloc((size_t)n))) {
                    bytes[i] = (uint8_t *)b;
                    continue;
                }
                memcpy(bytes[i], buf, (size_t)n), lens[i] = (size_t)n, owned[i] = 1;
            }
        }
        out = msg_build(m, bytes, lens, out_len);
    }
    for (int i = 0; owned && bytes && i < total; i++) {
        if (owned[i]) free(bytes[i]);
    }
    free(map), free(bytes), free(lens), free(owned), free(pick);
    lines_free(&l), lines_free(&ref);
    return out;
}

/* Serves the game's text in the language the settings ask for, before the game starts: the game reads its text
 * files once, at boot, and keeps them. */
/* The ROM's own text files served again (their FAT entries), and the extra letters forgotten. */
static void restore(void) {
    uint32_t fat = le32(rt_rom.data + 0x48);
    for (int i = 0; i < file_count; i++) {
        put32(rt_rom.data + fat + files[i].id * 8, files[i].start);
        put32(rt_rom.data + fat + files[i].id * 8 + 4, files[i].end);
    }
    rt_rom.more = NULL, rt_rom.more_at = rt_rom.more_size = 0;
    free(overlay), overlay = NULL;
    gfxfont_extra_reset();
    applied[0] = 0;
}

static void apply(void) {
    const char *lang = display_steam_text_language();
    if (!strcmp(lang, applied)) return;
    if (*applied) restore();
    if (!*lang) return;
    Uint64 t0 = SDL_GetTicks();
    list_files();
    if (learned < 0) learned = learn(); /* from the game's own files, once */
    if (!learned) {
        rt_log("steam text: nothing to learn the codes from");
        return;
    }
    load_fonts(lang);
    extras = 1;
    int served = 0, kept = 0;
    long grown = 0; /* bytes the text files grew by */
    Built *built = calloc((size_t)file_count + 1, sizeof *built);
    for (int i = 0; built && i < file_count; i++) {
        const File *f = &files[i];
        Msg m;
        if (f->modded || !msg_parse(rt_rom.data + f->start, f->end - f->start, &m)) continue;
        font = font_of(f->path), end_code = m.end;
        size_t len;
        uint8_t *out = rebuild(&m, f->path, lang, &len, &kept);
        if (out) built[served++] = (Built){f->id, out, len}, grown += (long)len - (long)(f->end - f->start);
    }
    if (!built || !serve(built, served)) served = 0;
    if (served) snprintf(applied, sizeof applied, "%s", lang);
    if (!served) gfxfont_extra_reset(); /* no text uses them: the game's glyph routines as they were */
    for (int i = 0; built && i < file_count; i++) free(built[i].data);
    free(built);
    extras = 0;
    rt_log("steam text: %s, %d files (%+ld KB) in %u ms; %d strings keep the game's text", lang, served, grown / 1024,
           (unsigned)(SDL_GetTicks() - t0), kept);
}

void st_init(void) { apply(); }

void st_power_on(void) { apply(); }

const char *st_served(void) { return applied; }

/* ---- the check ---- */

void st_check(void) {
    if (!getenv("EGG_STEAMTEXT_CHECK")) return;
    int files = 0, same = 0, slots = 0, trips = 0;
    for (int i = 0, n = gfx_list_files(); i < n; i++) {
        const GfxFile *f = gfx_file(i);
        if (!gfx_ends_with(f->path, ".msg")) continue;
        files++;
        Msg m;
        if (!msg_parse(rt_rom.data + f->offset, f->size, &m)) {
            printf("msgcheck: %s: not a text file\n", f->path);
            continue;
        }
        int total = m.strings * m.slots;
        uint8_t **bytes = calloc((size_t)total, sizeof *bytes);
        size_t *lens = calloc((size_t)total, sizeof *lens);
        for (int k = 0; bytes && lens && k < total; k++) bytes[k] = (uint8_t *)msg_slot(&m, k / m.slots, k % m.slots, &lens[k]);
        size_t len = 0;
        uint8_t *out = bytes && lens ? msg_build(&m, bytes, lens, &len) : NULL;
        if (out && len == f->size && !memcmp(out, rt_rom.data + f->offset, len)) same++;
        else printf("msgcheck: %s: rebuilt differs (%zu bytes, %u in the ROM)\n", f->path, len, f->size);
        for (int k = 0; bytes && lens && k < total; k++) { /* each slot as codes, and back */
            uint8_t again[4096];
            size_t at = 0, o = 0;
            while (at < lens[k] && o + 3 <= sizeof again) o += code_put(code_get(bytes[k], lens[k], &at), again + o);
            slots++;
            trips += o == lens[k] && !memcmp(again, bytes[k], o);
        }
        free(out), free(bytes), free(lens);
    }
    printf("msgcheck: %d files, %d identical\n", files, same);
    printf("tokens: %d slots, %d round trip\n", slots, trips);
    if (learn()) { /* the Steam text in the language learned from, against the game's */
        int match = 0, strings = 0, missing[TAGS] = {0};
        for (int i = 0, n = gfx_list_files(); i < n; i++) {
            const GfxFile *f = gfx_file(i);
            Msg m;
            char base[64];
            Lines l;
            int slot = 1, *map;
            if (!gfx_ends_with(f->path, ".msg") || !msg_parse(rt_rom.data + f->offset, f->size, &m)) continue;
            base_name(f->path, base, sizeof base);
            font = font_of(f->path), end_code = m.end;
            if (!lines_read(RT_JAPAN ? "ja" : "en", base, &l)) continue;
            if (RT_JAPAN) slot = 0;
            if ((map = malloc((size_t)m.strings * sizeof *map))) map_lines(&m, &l, slot, map);
            int fm = 0, fs = 0;
            for (int s = 0; map && s < m.strings; s++) {
                if (map[s] < 0) continue;
                static uint8_t buf[TOKENS * 3 + 1];
                const char *why = "";
                size_t len;
                const uint8_t *b = msg_slot(&m, s, slot, &len);
                int got = encode_string(&m, s, slot, l.at[map[s]], l.at[map[s]], buf, sizeof buf, &why);
                if (!got) continue; /* an empty line: the string keeps its text */
                fs++;
                fm += got == (int)len && !memcmp(buf, b, len);
                for (int t = 0; got < 0 && t < tag_count; t++) missing[t] += why == tags[t];
            }
            if (fs && fm * 100 < fs * 95) printf("align: %s: %d of %d strings match\n", base, fm, fs);
            match += fm, strings += fs;
            free(map);
            lines_free(&l);
        }
        int mapped = 0;
        for (int t = 0; t < tag_count; t++) {
            mapped += codes[0][0x10000 + t] || codes[1][0x10000 + t];
            if (!codes[0][0x10000 + t] && !codes[1][0x10000 + t]) printf("align: tag <%s> unmapped (%d lines)\n", tags[t], missing[t]);
        }
        printf("align: tags %d/%d, english %d/%d\n", mapped, tag_count, match, strings);
        const char *lang = getenv("EGG_STEAMTEXT_CHECK"); /* a language code: its letters the fonts lack */
        if (strlen(lang) > 1) {
            load_fonts(lang);
            int need = 0, drawable = 0, lines = 0, encoded = 0;
            static uint8_t seen[2][0x10000];
            for (int i = 0, n = gfx_list_files(); i < n; i++) {
                const GfxFile *f = gfx_file(i);
                char base[64];
                Lines l;
                if (!gfx_ends_with(f->path, ".msg")) continue;
                base_name(f->path, base, sizeof base);
                font = font_of(f->path);
                if (!lines_read(lang, base, &l)) continue;
                for (int j = 0; j < l.count; j++) {
                    static Tok t[TOKENS];
                    static uint8_t buf[TOKENS * 3 + 1];
                    const char *why;
                    int nt = tokenize(l.at[j], t);
                    for (int k = 0; k < nt; k++) {
                        if (t[k].tag || letter_code(t[k].v) || seen[font][t[k].v]) continue;
                        seen[font][t[k].v] = 1, need++;
                        extras = 1;
                        drawable += gfxfont_extra(font, t[k].v) >= 0;
                        extras = 0;
                    }
                    extras = 1;
                    lines++, encoded += encode(l.at[j], buf, sizeof buf, &why) > 0;
                    extras = 0;
                }
                lines_free(&l);
            }
            printf("extra: %s: %d letters, %d drawable; %d of %d lines encode\n", lang, need, drawable, encoded, lines);
        }
    }
    fflush(stdout);
    exit(0);
}
