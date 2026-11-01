"""Applies an xdelta3 (VCDIFF, RFC 3284) patch to a Nintendo DS ROM that xdelta3 refuses.

Many translation patches were not made from a clean dump but from the dump unpacked and repacked by ndstool, as the
patchers they came with do before applying them (ndstool -x, then ndstool -c). The repacked ROM holds the same files
laid out differently, so the patch's window checksums fail on the dump itself. This script applies the patch to the
dump, and when that fails, to the dump rebuilt the way that ndstool lays a ROM out:

- the header's first 0x200 bytes, zeros up to 0x4000, then the ARM9 program (its size from the header);
- the ARM9 overlay table (4-byte aligned), the ARM7 program and the banner (0x200-aligned each), the file name table
  (4-byte aligned) rebuilt from the folders, then the file allocation table;
- the files right after the table, 4-byte aligned and zero-padded: the overlays first, then each folder's files
  followed by its subfolders. ndstool reads the folders back from the disk, so names come in Windows' order
  (compared upper-cased), not the ROM's, and the files get new IDs in that order;
- the ROM ends 4-byte aligned.

A dump whose secure area is decrypted (the first ARM9 bytes read E7FFDEFF) cannot rebuild the secure area of a dump
that kept it encrypted, and the header of such a dump carries that area's CRC: the checksum of the patch window
holding the header and secure area then fails while every other one matches. That window copies those bytes from the
source unchanged, so its mismatch is accepted in that case alone, and the script says so.

Only the default code table and uncompressed sections are supported (what xdelta3 writes with -S none or for
already-compressed data); other patches need xdelta3 itself.

Usage: python -I vcdiff.py <patch> <source ROM> <output ROM>
Exit code 0 on success; otherwise the reason is printed and the exit code is 1.
"""

import struct
import sys
import zlib

SECURE_AREA_END = 0x8000
DECRYPTED_SECURE_AREA = 0xE7FFDEFF


def _code_table():
    """RFC 3284 section 5.6: (type, size, mode) pairs; type 0 none, 1 ADD, 2 RUN, 3 COPY."""
    t = [((2, 0, 0), (0, 0, 0)), ((1, 0, 0), (0, 0, 0))]
    t += [((1, s, 0), (0, 0, 0)) for s in range(1, 18)]
    for m in range(9):
        t.append(((3, 0, m), (0, 0, 0)))
        t += [((3, s, m), (0, 0, 0)) for s in range(4, 19)]
    for m in range(6):
        t += [((1, a, 0), (3, c, m)) for a in range(1, 5) for c in range(4, 7)]
    for m in range(6, 9):
        t += [((1, a, 0), (3, 4, m)) for a in range(1, 5)]
    t += [((3, 4, m), (1, 1, 0)) for m in range(9)]
    return t


CODE_TABLE = _code_table()


def _integer(b, i):
    v = 0
    while True:
        x = b[i]
        i += 1
        v = v << 7 | x & 0x7F
        if not x & 0x80:
            return v, i


def apply(patch, source):
    """Decodes the patch against source; returns the target and the windows whose checksum failed, as
    (target offset, length)."""
    if patch[:3] != b"\xd6\xc3\xc4" or patch[3] != 0:
        raise ValueError("not an xdelta3 (VCDIFF) patch")
    indicator, pos = patch[4], 5
    if indicator & 3:
        raise ValueError("the patch uses secondary compression or its own code table; xdelta3 is needed for it")
    if indicator & 4:
        n, pos = _integer(patch, pos)
        pos += n
    out, bad = bytearray(), []
    while pos < len(patch):
        wi = patch[pos]
        pos += 1
        seg_len = seg_pos = 0
        if wi & 3:
            seg_len, pos = _integer(patch, pos)
            seg_pos, pos = _integer(patch, pos)
        _, pos = _integer(patch, pos)  # length of the delta encoding
        target_len, pos = _integer(patch, pos)
        if patch[pos]:
            raise ValueError("the patch compresses its sections; xdelta3 is needed for it")
        pos += 1
        data_len, pos = _integer(patch, pos)
        inst_len, pos = _integer(patch, pos)
        addr_len, pos = _integer(patch, pos)
        checksum = None
        if wi & 4:
            checksum = int.from_bytes(patch[pos:pos + 4], "big")
            pos += 4
        data = patch[pos:pos + data_len]
        inst = patch[pos + data_len:pos + data_len + inst_len]
        addr = patch[pos + data_len + inst_len:pos + data_len + inst_len + addr_len]
        pos += data_len + inst_len + addr_len
        if wi & 1:
            if seg_pos + seg_len > len(source):
                raise ValueError("the patch reads past the end of the ROM: it was made for another ROM")
            seg = source[seg_pos:seg_pos + seg_len]
        else:
            seg = bytes(out[seg_pos:seg_pos + seg_len])
        tgt = bytearray()
        near, next_near, same = [0] * 4, 0, [0] * 768
        d = i = a = 0
        while i < len(inst):
            code = inst[i]
            i += 1
            for typ, size, mode in CODE_TABLE[code]:
                if not typ:
                    continue
                if not size:
                    size, i = _integer(inst, i)
                if typ == 1:
                    tgt += data[d:d + size]
                    d += size
                elif typ == 2:
                    tgt += bytes((data[d],)) * size
                    d += 1
                else:
                    here = seg_len + len(tgt)
                    if mode == 0:
                        x, a = _integer(addr, a)
                    elif mode == 1:
                        v, a = _integer(addr, a)
                        x = here - v
                    elif mode < 6:
                        v, a = _integer(addr, a)
                        x = near[mode - 2] + v
                    else:
                        x = same[(mode - 6) * 256 + addr[a]]
                        a += 1
                    near[next_near] = x
                    next_near = (next_near + 1) % 4
                    same[x % 768] = x
                    if x + size <= seg_len:
                        tgt += seg[x:x + size]
                    else:
                        for k in range(x, x + size):  # may overlap what it writes
                            tgt.append(seg[k] if k < seg_len else tgt[k - seg_len])
        if len(tgt) != target_len:
            raise ValueError("the patch is damaged (a window decodes to the wrong size)")
        if checksum is not None and zlib.adler32(bytes(tgt)) != checksum:
            bad.append((len(out), len(tgt)))
        out += tgt
    return bytes(out), bad


def _align(x, a):
    return (x + a - 1) // a * a


def rebuild(rom):
    """The ROM laid out as ndstool -c lays out what ndstool -x extracted from it (see the module comment)."""
    u32 = lambda at: struct.unpack("<I", rom[at:at + 4])[0]
    fnt_at, fat_at, count = u32(0x40), u32(0x48), u32(0x4C) // 8
    extent = lambda i: struct.unpack("<II", rom[fat_at + 8 * i:fat_at + 8 * i + 8])

    # The folders as ndstool reads them back: names in Windows' order.
    def walk(dir_id):
        at = fnt_at + 8 * (dir_id & 0xFFF)
        o, file_id, _ = struct.unpack("<IHH", rom[at:at + 8])
        o += fnt_at
        entries = []
        while rom[o]:
            t = rom[o]
            name = bytes(rom[o + 1:o + 1 + (t & 0x7F)])
            o += 1 + (t & 0x7F)
            if t & 0x80:
                entries.append((name, walk(struct.unpack("<H", rom[o:o + 2])[0])))
                o += 2
            else:
                entries.append((name, file_id))
                file_id += 1
        return sorted(entries, key=lambda e: e[0].upper())

    root = walk(0xF000)
    dirs = []  # directory ids in depth-first order

    def number(entries):
        dirs.append(entries)
        for _, e in entries:
            if isinstance(e, list):
                number(e)

    number(root)
    dir_ids = {id(e): 0xF000 + k for k, e in enumerate(dirs)}
    overlays = []
    for table_at, size_at in ((0x50, 0x54), (0x58, 0x5C)):
        table = u32(table_at)
        overlays += [u32(table + 32 * k + 24) for k in range(u32(size_at) // 32)]
    if sorted(overlays) != list(range(len(overlays))):
        raise ValueError("the overlays are not the first files")
    order = list(overlays)  # file id -> old file id
    main, sub = bytearray(8 * len(dirs)), bytearray()

    def add(entries, parent):
        struct.pack_into("<IHH", main, 8 * (dir_ids[id(entries)] & 0xFFF), len(main) + len(sub), len(order), parent)
        for name, e in entries:
            if isinstance(e, list):
                sub.extend(bytes((len(name) | 0x80,)) + name + struct.pack("<H", dir_ids[id(e)]))
            else:
                sub.extend(bytes((len(name),)) + name)
        sub.append(0)
        order.extend(e for _, e in entries if not isinstance(e, list))
        for _, e in entries:
            if isinstance(e, list):
                add(e, dir_ids[id(entries)])

    add(root, len(dirs))
    if len(order) != count:
        raise ValueError("the file name table does not name every file")

    out = bytearray(rom[:0x200]) + bytes(0x3E00)
    out += rom[u32(0x20):u32(0x20) + u32(0x2C)]
    placed = {}
    for key, data, align in (("ov9", rom[u32(0x50):u32(0x50) + u32(0x54)], 4),
                             ("arm7", rom[u32(0x30):u32(0x30) + u32(0x3C)], 0x200),
                             ("banner", rom[u32(0x68):u32(0x68) + 0x840], 0x200),
                             ("fnt", main + sub, 4)):
        out += bytes(_align(len(out), align) - len(out))
        placed[key] = len(out)
        out += data
    out += bytes(_align(len(out), 4) - len(out))
    new_fat = len(out)
    out += bytes(8 * count)
    for new_id, old_id in enumerate(order):
        out += bytes(_align(len(out), 4) - len(out))
        a, b = extent(old_id)
        struct.pack_into("<II", out, new_fat + 8 * new_id, len(out), len(out) + b - a)
        out += rom[a:b]
    out += bytes(_align(len(out), 4) - len(out))
    for at, value in ((0x30, placed["arm7"]), (0x40, placed["fnt"]), (0x44, len(main) + len(sub)),
                      (0x48, new_fat), (0x50, placed["ov9"]), (0x68, placed["banner"]), (0x80, len(out))):
        struct.pack_into("<I", out, at, value)
    crc = 0xFFFF
    for x in out[:0x15E]:
        crc ^= x
        for _ in range(8):
            crc = crc >> 1 ^ 0xA001 if crc & 1 else crc >> 1
    struct.pack_into("<H", out, 0x15E, crc)
    return bytes(out)


def patch_rom(patch, rom):
    """The patched ROM and what to log; raises ValueError when the patch does not fit the ROM."""
    out, bad = apply(patch, rom)
    if not bad:
        return out, []
    try:
        rebuilt = rebuild(rom)
    except (ValueError, struct.error, IndexError):
        rebuilt = None
    if rebuilt is not None:
        out, bad = apply(patch, rebuilt)
        log = ["the patch was made from the ROM repacked by ndstool: applied to the ROM rebuilt that way"]
        if not bad:
            return out, log
        decrypted = struct.unpack("<I", rom[0x4000:0x4004])[0] == DECRYPTED_SECURE_AREA
        if decrypted and all(start < SECURE_AREA_END for start, _ in bad):
            log.append("the checksum of the patch's first window (header and secure area, copied as they are) "
                       "differs: your dump has the secure area decrypted, the one the patch was made from probably "
                       "not")
            return out, log
    raise ValueError(f"the patch was made for another ROM: {len(bad)} of its checksums do not match")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    try:
        with open(sys.argv[1], "rb") as f:
            patch = f.read()
        with open(sys.argv[2], "rb") as f:
            rom = f.read()
        out, log = patch_rom(patch, rom)
    except (OSError, ValueError, IndexError) as e:
        print(e)
        sys.exit(1)
    for line in log:
        print(line)
    with open(sys.argv[3], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
