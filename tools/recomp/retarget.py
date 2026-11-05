"""Carries a translation made for another version of Chrono Trigger DS over to the player's USA or European ROM.

(Europe's files are the USA's, so what is said of the USA ROM here holds for both; "USA" below is the ROM being
built.) A patch made for the Japanese ROM cannot be applied to a USA ROM. What such a
translation changes is data: runtime/translation.c applies the patches to the player's dump of the version they
were made for, and this script moves what they changed into the USA ROM:

- a file the patches changed whose original is identical in both versions (graphics, usually) is copied;
- text (msg/<font>/*.msg): every string the patches changed goes into the same entry and language slot of the USA
  file. Characters are glyph numbers of the folder's font (msgcmn.fnt), so they are converted by matching glyph
  bitmaps between the patched font and the USA one; control codes come after the font's last glyph and keep their
  distance from it. Strings the patches left alone keep the USA text;
- glyphs the translation added (accented letters drawn over unused characters) are written over USA glyphs no text
  uses, blank ones first, so the USA font keeps its glyph count and the control codes stay where the game expects;
- the ARM9's program and overlays are carried over the same way, when the patches changed one that is identical
  in both versions (the European overlays are the USA ones, and hold some text); a change to code that differs
  between the versions, or a changed file of another kind, cannot be carried over: the script says which. ARM7
  changes (a flash cart fix, say) are left out: the port emulates the ARM7 and runs none of its code.

File formats (all little-endian):
  .msg  "\\0\\0\\0\\0TEXT", languages (u8), entries (u16), a byte, file size (u32), then a u32 offset per entry and
        language (entry * languages + language), each to a string ending in the font's character for ASCII 0.
        A character is a glyph or control number, written like UTF-8 (1 to 3 bytes).
  .fnt  "\\0\\0\\0\\0FONT", height (u8), bits per pixel (u8), glyph count (u16), an ASCII-to-glyph table (128 u16),
        a u32 offset per glyph (0: none), then each glyph: advance width (u8), bytes per row (u8), the rows.

Changed files are written past the last file in the ROM and the file allocation table points at them.

Usage: python -I retarget.py <USA ROM> <original ROM> <patched ROM> <output ROM>
Exit code 0 on success; otherwise the reason is printed and the exit code is 1.
"""

import struct
import sys

ALIGN = 0x200


class Rom:
    def __init__(self, data):
        self.data = bytearray(data)
        self.code = bytes(data[0xC:0x10]).decode("latin-1")
        self.fnt, _, self.fat, fat_size = struct.unpack("<4I", data[0x40:0x50])
        self.count = fat_size // 8
        self.paths = {}
        self._walk(0xF000, "")

    def _walk(self, dir_id, prefix):
        d = self.data
        at = self.fnt + 8 * (dir_id & 0xFFF)
        o, file_id, _ = struct.unpack("<IHH", d[at:at + 8])
        o += self.fnt
        while d[o]:
            t = d[o]
            name = bytes(d[o + 1:o + 1 + (t & 0x7F)]).decode("latin-1")
            o += 1 + (t & 0x7F)
            if t & 0x80:
                self._walk(struct.unpack("<H", d[o:o + 2])[0], prefix + name + "/")
                o += 2
            else:
                self.paths[prefix + name] = file_id
                file_id += 1

    def extent(self, file_id):
        return struct.unpack("<II", self.data[self.fat + 8 * file_id:self.fat + 8 * file_id + 8])

    def file(self, file_id):
        a, b = self.extent(file_id)
        return bytes(self.data[a:b])

    def code_files(self, cpu):
        """The code of one CPU ("arm9" or "arm7"): its program and its overlays, as (bytes, file id); the
        program is no file (None)."""
        d = self.data
        o, _, _, size = struct.unpack("<4I", d[0x20 if cpu == "arm9" else 0x30:][:16])
        out = {cpu: (bytes(d[o:o + size]), None)}
        table, size = struct.unpack("<II", d[0x50 if cpu == "arm9" else 0x58:][:8])
        for k in range(size // 32):
            file_id = struct.unpack("<I", d[table + 32 * k + 24:table + 32 * k + 28])[0]
            out[f"{cpu} overlay {k}"] = (self.file(file_id), file_id)
        return out

    def replace(self, files):
        """Writes the changed files past the last one and points the allocation table at them."""
        end = max(self.extent(i)[1] for i in range(self.count))
        for file_id, data in files.items():
            end = (end + ALIGN - 1) // ALIGN * ALIGN
            if end + len(data) > len(self.data):
                raise ValueError("the translated files do not fit in the ROM")
            self.data[end:end + len(data)] = data
            struct.pack_into("<II", self.data, self.fat + 8 * file_id, end, end + len(data))
            end += len(data)


class Font:
    def __init__(self, data):
        if data[4:8] != b"FONT":
            raise ValueError("not a font")
        self.head = data[:0x10C]
        self.height = data[8]
        n = struct.unpack("<H", data[10:12])[0]
        self.ascii = struct.unpack("<128H", data[12:12 + 256])
        offsets = struct.unpack(f"<{n}I", data[0x10C:0x10C + 4 * n])
        self.glyphs = [bytes(data[o:o + 2 + self.height * data[o + 1]]) if o else None for o in offsets]

    @property
    def count(self):
        return len(self.glyphs)

    def build(self):
        out = bytearray(self.head)
        out += bytes(4 * self.count)
        for i, g in enumerate(self.glyphs):
            if g:
                struct.pack_into("<I", out, 0x10C + 4 * i, len(out))
                out += g
        return bytes(out)


def blank(glyph):
    return not any(glyph[2:])


def decode(b):
    out, i = [], 0
    while i < len(b):
        x = b[i]
        if x < 0x80:
            out.append(x)
            i += 1
        elif x < 0xE0:
            out.append((x & 0x1F) << 6 | b[i + 1] & 0x3F)
            i += 2
        else:
            out.append((x & 0x0F) << 12 | (b[i + 1] & 0x3F) << 6 | b[i + 2] & 0x3F)
            i += 3
    return out


def encode(chars):
    out = bytearray()
    for c in chars:
        if c < 0x80:
            out.append(c)
        elif c < 0x800:
            out += bytes((0xC0 | c >> 6, 0x80 | c & 0x3F))
        else:
            out += bytes((0xE0 | c >> 12, 0x80 | c >> 6 & 0x3F, 0x80 | c & 0x3F))
    return bytes(out)


class Text:
    def __init__(self, data, end):
        if data[4:8] != b"TEXT":
            raise ValueError("not a text file")
        self.head = data[:12]
        self.languages, self.entries = data[8], data[9] | data[10] << 8
        n = self.languages * self.entries
        self.strings = []
        for o in struct.unpack(f"<{n}I", data[16:16 + 4 * n]):
            e = o
            while data[e] != end:
                e += 1
            self.strings.append(bytes(data[o:e]))

    def get(self, entry, language):
        return self.strings[entry * self.languages + language]

    def build(self, end):
        n = len(self.strings)
        out = bytearray(self.head) + bytes(4 + 4 * n)
        for i, s in enumerate(self.strings):
            struct.pack_into("<I", out, 16 + 4 * i, len(out))
            out += s + bytes((end,))
        struct.pack_into("<I", out, 12, len(out))
        return bytes(out)


def retarget(target, original, patched):
    t, o, p = Rom(target), Rom(original), Rom(patched)
    if o.code != p.code:
        raise ValueError(f"the patched ROM is {p.code}, the original {o.code}")
    log, out = [], {}
    for cpu in ("arm9", "arm7"):
        code_o, code_p, code_t = o.code_files(cpu), p.code_files(cpu), t.code_files(cpu)
        changed_code = [k for k in code_o if code_o[k][0] != code_p.get(k, (None,))[0]]
        if changed_code and cpu == "arm7":
            log.append(f"left out: the ARM7 code changes ({', '.join(changed_code[:3])})")
            continue
        for k in changed_code:
            new, (old, file_id) = code_p[k][0], code_t.get(k, (None, None))
            if old != code_o[k][0] or (file_id is None and len(new) != len(old)):
                raise ValueError(f"the translation changes the game's code ({k}), which cannot be carried over "
                                 f"from {o.code} to {t.code}")
            if file_id is None:
                at = struct.unpack("<I", t.data[0x20:0x24])[0]
                t.data[at:at + len(new)] = new
            else:
                out[file_id] = new
            log.append(f"{k}: carried over (identical in both versions)")
    if set(o.paths) != set(p.paths):  # file IDs may differ: vcdiff.py patches a repacked ROM
        raise ValueError("the translation adds, removes or renames files")

    # Overlays are files too: they are compared above, not carried.
    overlays = {struct.unpack("<I", o.data[table + 32 * k + 24:table + 32 * k + 28])[0]
                for table, size in (struct.unpack("<II", o.data[at:at + 8]) for at in (0x50, 0x58))
                for k in range(size // 32)}
    changed = [path for path, i in p.paths.items()
               if i not in overlays and o.file(o.paths[path]) != p.file(i)]
    fonts, texts = {}, {}
    for path in changed:
        if path not in t.paths:
            raise ValueError(f"{path} is not in {t.code}: it cannot be carried over")
        if t.file(t.paths[path]) == o.file(o.paths[path]):
            out[t.paths[path]] = p.file(p.paths[path])
        elif path.startswith("msg/") and path.endswith(".msg"):
            texts.setdefault(path.rsplit("/", 1)[0], []).append(path)
        elif path.startswith("msg/") and path.endswith("/msgcmn.fnt"):
            fonts[path.rsplit("/", 1)[0]] = path
        else:
            raise ValueError(f"{path} differs between {o.code} and {t.code}: it cannot be carried over")

    for folder in sorted(set(texts) | set(fonts)):
        font_path = folder + "/msgcmn.fnt"
        if font_path not in p.paths or font_path not in t.paths:
            raise ValueError(f"{folder} has no font")
        pf, tf = Font(p.file(p.paths[font_path])), Font(t.file(t.paths[font_path]))
        p_end, t_end = pf.ascii[0], tf.ascii[0]

        # USA glyphs no text in this folder uses and no ASCII character maps to: room for the translation's own,
        # blank ones first, then from the end of the font.
        used = set(tf.ascii)
        folder_texts = {path: Text(t.file(i), t_end) for path, i in t.paths.items()
                        if path.startswith(folder + "/") and path.endswith(".msg")}
        for text in folder_texts.values():
            for s in text.strings:
                used.update(decode(s))
        free = [i for i, g in enumerate(tf.glyphs) if g and i not in used]
        free.sort(key=lambda i: (not blank(tf.glyphs[i]), -i))

        # A glyph is the same character when its whole record (width, row size, bitmap) is the same; blank ones
        # are spaces of their width. Glyphs the USA text uses come first, then the ASCII tables.
        by_record = {}
        for i in sorted(range(tf.count), key=lambda i: i not in used):
            if tf.glyphs[i]:
                by_record.setdefault(tf.glyphs[i], i)
        ascii_map = {pf.ascii[c]: tf.ascii[c] for c in range(128) if pf.ascii[c] != 0xFFFF and tf.ascii[c] != 0xFFFF}
        glyph_map = {}

        def glyph(c):
            if c in glyph_map:
                return glyph_map[c]
            g = pf.glyphs[c]
            slot = by_record.get(g, ascii_map.get(c))
            if slot is None:
                if not g:
                    raise ValueError(f"{font_path}: the translation uses character {c}, which has no glyph")
                if not free:
                    raise ValueError(f"{font_path}: the translation needs more new characters than the "
                                     f"{t.code} font has room for")
                slot = free.pop(0)
                if by_record.get(tf.glyphs[slot]) == slot:
                    del by_record[tf.glyphs[slot]]
                tf.glyphs[slot] = g
                by_record[g] = slot
                log.append(f"{font_path}: character {c} drawn over glyph {slot}")
            if slot in free:
                free.remove(slot)  # mapped to as it is: no longer free to draw over
            glyph_map[c] = slot
            return slot

        def convert(s):
            return encode(c - pf.count + tf.count if c >= pf.count else glyph(c) for c in decode(s))

        for path in texts.get(folder, []):
            pt, ot = Text(p.file(p.paths[path]), p_end), Text(o.file(o.paths[path]), p_end)
            tt = folder_texts[path]
            if pt.entries != tt.entries or pt.entries != ot.entries:
                raise ValueError(f"{path} has {pt.entries} entries, {tt.entries} in {t.code}")
            for entry in range(pt.entries):
                for language in range(min(pt.languages, tt.languages)):
                    s = pt.get(entry, language)
                    if s != ot.get(entry, language):
                        tt.strings[entry * tt.languages + language] = convert(s)
            out[t.paths[path]] = tt.build(t_end)
        out[t.paths[font_path]] = tf.build()

    t.replace(out)
    return bytes(t.data), log


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    try:
        roms = []
        for path in sys.argv[1:4]:
            with open(path, "rb") as f:
                roms.append(f.read())
        data, log = retarget(*roms)
    except (OSError, ValueError, struct.error, IndexError) as e:
        print(e)
        sys.exit(1)
    for line in log:
        print(line)
    with open(sys.argv[4], "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
