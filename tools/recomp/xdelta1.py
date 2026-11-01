"""Applies an xdelta 1.x patch (%XDZ004%, the format of xdelta 1.1, often named .delta): runtime/translation.c runs
it for translation mods, because xdelta3 reads only VCDIFF.

Layout: a 32-byte header (magic, flags, name lengths), the two file names, the patch's data segments, the control
(XdeltaControl, serialized by EDSIO), the control's offset (4 bytes, big-endian) and the magic again. With
FLAG_PATCH_COMPRESSED each segment and the control are gzip members. The control lists the sources (data segments
of the patch, and the file it was made from, with its MD5) and the instructions: copy length bytes at offset of
source index, or, from a sequential source, its next length bytes (the offset is then 0). EDSIO numbers are
little-endian base-128 varints.

Usage: python -I xdelta1.py <patch> <source ROM> <output ROM>
Exit code 0 on success; otherwise the reason is printed and the exit code is 1.
"""

import hashlib
import struct
import sys
import zlib

MAGIC = b"%XDZ004%"
FLAG_PATCH_COMPRESSED = 8
CONTROL_TYPE = 0x8003


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError("truncated control")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def uint(self):
        value = shift = 0
        while True:
            b = self.take(1)[0]
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return value

    def boolean(self):
        return self.take(1)[0] != 0


def segment(patch, pos, compressed):
    """A gzip member (or the rest of the patch, uncompressed) at pos: its bytes and where the next one starts."""
    if not compressed:
        return patch[pos:], len(patch)
    z = zlib.decompressobj(31)
    data = z.decompress(patch[pos:])
    if not z.eof:
        raise ValueError("truncated gzip data")
    return data, len(patch) - len(z.unused_data)


def apply(patch, source):
    if len(patch) < 44 or patch[:8] != MAGIC or patch[-8:] != MAGIC:
        raise ValueError("not an xdelta 1.1 patch (%XDZ004%)" if patch[:4] == b"%XDZ" else "not an xdelta 1 patch")
    flags, names = struct.unpack(">II", patch[8:16])
    compressed = bool(flags & FLAG_PATCH_COMPRESSED)
    pos = 32 + (names >> 16) + (names & 0xFFFF)
    control_at = struct.unpack(">I", patch[-12:-8])[0]

    c = Reader(segment(patch, control_at, compressed)[0])
    if struct.unpack(">I", c.take(4))[0] != CONTROL_TYPE:
        raise ValueError("unknown control record")
    c.take(4)
    to_md5, to_len = c.take(16), c.uint()
    c.boolean()  # has_data
    sources = []
    for _ in range(c.uint()):
        name = c.take(c.uint()).decode("latin-1")
        md5, length, isdata, sequential = c.take(16), c.uint(), c.boolean(), c.boolean()
        sources.append((name, md5, length, isdata, sequential))

    # Another dump of the same game can differ in bytes the patch never reads (a 2009 patch made from a scene dump
    # applies to the No-Intro one): a source that does not match is used anyway, and only the result decides.
    blobs, other = [], None
    for name, md5, length, isdata, _ in sources:
        if isdata:
            data, pos = segment(patch, pos, compressed)
            if len(data) != length or hashlib.md5(data).digest() != md5:
                raise ValueError("damaged patch data")
            blobs.append(data)
        else:
            if len(source) != length or hashlib.md5(source).digest() != md5:
                other = (f"the patch was made for another ROM ({name}, {length} bytes, MD5 {md5.hex()}); "
                         f"this one is {len(source)} bytes, MD5 {hashlib.md5(source).hexdigest()}")
            blobs.append(source)

    out = bytearray()
    read = [0] * len(blobs)  # how much of each sequential source the instructions have used
    for _ in range(c.uint()):
        index, offset, length = c.uint(), c.uint(), c.uint()
        if index < len(blobs) and sources[index][4]:
            offset, read[index] = read[index], read[index] + length
        if index >= len(blobs) or offset + length > len(blobs[index]):
            raise ValueError(other or "instruction out of range")
        out += blobs[index][offset:offset + length]
    if len(out) != to_len or hashlib.md5(out).digest() != to_md5:
        raise ValueError(other or "the patched ROM does not match the patch's checksum")
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    try:
        with open(sys.argv[1], "rb") as f:
            patch = f.read()
        with open(sys.argv[2], "rb") as f:
            source = f.read()
        out = apply(patch, source)
    except (OSError, ValueError, zlib.error) as e:
        print(e)
        sys.exit(1)
    with open(sys.argv[3], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
