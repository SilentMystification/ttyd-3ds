"""Helpers shared by the geometry disc-data tools: the per-entry byte swap, reading main.dol out of a disc image.

The swap here is the host copy of port_disc_data_fill's swap in port3ds/game/disc_data.c; keep them in step.
Formats (the fourth field of an entry in tools/port3ds/disc_geometry.txt):
  raw      bytes, no swap (u8/s8/GXColor arrays, GX display lists written as bytes)
  s16      swap every 2 bytes
  swap32   swap every 4 bytes (f32, u32, Vec)
  n64vtx   effN64Vtx (stride 14): swap the five leading s16 of each vertex, keep the last 4 bytes
  dl32     a u32[] display list: the disc bytes are already GX byte order and the port keeps them as they are
           (no swap); the sources used to write the words big-endian and byte-swap them at start-up
"""

import struct

FORMATS = ("raw", "s16", "swap32", "n64vtx", "dl32")
FORMAT_ID = {f: i for i, f in enumerate(FORMATS)}  # the numbers port_disc_data_register takes


def swap(data, fmt):
    """The bytes as the port holds them, from the disc's big-endian bytes."""
    b = bytearray(data)
    if fmt in ("raw", "dl32"):
        return bytes(b)
    if fmt == "s16":
        step = 2
    elif fmt == "swap32":
        step = 4
    elif fmt == "n64vtx":
        if len(b) % 14:
            raise ValueError("n64vtx size is not a multiple of 14")
        for v in range(0, len(b), 14):
            for k in range(5):
                i = v + k * 2
                b[i], b[i + 1] = b[i + 1], b[i]
        return bytes(b)
    else:
        raise ValueError(f"unknown format {fmt}")
    if len(b) % step:
        raise ValueError(f"size {len(b)} is not a multiple of {step}")
    for i in range(0, len(b), step):
        b[i:i + step] = b[i:i + step][::-1]
    return bytes(b)


def compiled_to_memory(data, fmt):
    """What memory holds after start-up, from the bytes the old compiled array held (little-endian values).

    Every format but dl32 keeps the compiled bytes as they were; the u32 display lists were written as
    big-endian words and byte-swapped once by a constructor, so memory holds each word reversed."""
    if fmt == "dl32":
        b = bytearray(data)
        for i in range(0, len(b), 4):
            b[i:i + 4] = b[i:i + 4][::-1]
        return bytes(b)
    return bytes(data)


class Dol:
    """main.dol of a GameCube disc image, read by GameCube address."""

    def __init__(self, iso_path):
        self.f = open(iso_path, "rb")
        self.f.seek(0)
        head = self.f.read(0x440)
        if len(head) < 0x440 or struct.unpack(">I", head[0x1C:0x20])[0] != 0xC2339F3D:
            raise ValueError(f"{iso_path} is not a GameCube disc image")
        self.game_id = head[:6].decode("ascii", "replace")
        self.base = struct.unpack(">I", head[0x420:0x424])[0]
        self.f.seek(self.base)
        h = self.f.read(0x100)
        self.sections = []
        for i in range(18):
            off, addr, size = struct.unpack(">III", (h[i * 4:i * 4 + 4] + h[0x48 + i * 4:0x48 + i * 4 + 4]
                                                      + h[0x90 + i * 4:0x90 + i * 4 + 4]))
            if size:
                self.sections.append((off, addr, size))

    def read(self, addr, size):
        for off, a, sz in self.sections:
            if a <= addr and addr + size <= a + sz:
                self.f.seek(self.base + off + (addr - a))
                data = self.f.read(size)
                if len(data) != size:
                    raise ValueError("short read")
                return data
        raise ValueError(f"0x{addr:08X}+0x{size:X} is not inside one DOL section")


    def file(self, path):
        """(offset, size) of a file in the image's file system, e.g. "rel/mri.rel"."""
        f = self.f
        f.seek(0x424)
        fst_off, = struct.unpack(">I", f.read(4))
        f.seek(fst_off)
        root = f.read(12)
        n, = struct.unpack(">I", root[8:12])
        f.seek(fst_off)
        ents = f.read(12 * n)
        f.seek(fst_off + 12 * n)
        # the name table runs up to the first file's data; read generously
        table = f.read(0x40000)

        def name_at(off):
            return table[off:table.index(b"\0", off)].decode("ascii", "replace")

        def walk(i, dir_end, prefix, want):
            while i < dir_end:
                flag_name, a, b = struct.unpack(">III", ents[i * 12:i * 12 + 12])
                is_dir = flag_name >> 24
                nm = name_at(flag_name & 0xFFFFFF)
                if is_dir:
                    if want[0] == nm:
                        return walk(i + 1, b, prefix + nm + "/", want[1:])
                    i = b
                else:
                    if len(want) == 1 and want[0] == nm:
                        return a, b
                    i += 1
            return None

        r = walk(1, n, "", path.split("/"))
        if r is None:
            raise ValueError(f"{path} is not in the image")
        return r

    def read_rel_section(self, rel, section, offset, size):
        """Bytes of a REL's section (index as in the REL's section table) at an offset."""
        base, rel_size = self.file(f"rel/{rel}.rel")
        self.f.seek(base)
        h = self.f.read(0x40)
        nsec, secinfo = struct.unpack(">II", h[0x0C:0x14])
        self.f.seek(base + secinfo + section * 8)
        soff, ssize = struct.unpack(">II", self.f.read(8))
        soff &= ~1
        if section <= 0 or section >= nsec or soff == 0 or offset + size > ssize:
            raise ValueError("REL section out of range")
        self.f.seek(base + soff + offset)
        return self.f.read(size)
