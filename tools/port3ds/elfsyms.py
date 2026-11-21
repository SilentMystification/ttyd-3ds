"""Read the symbol table of an ELF32 file (big- or little-endian).

Used for the decomp's GameCube ELF (build/<version>/MarioSt.elf, PowerPC,
big-endian) and for the 3DS port's ELF (ARM, little-endian), so tools can
read the same game variables by name from Dolphin, paper-cut and Azahar.
"""

import struct
from pathlib import Path


class Symbol:
    __slots__ = ("name", "addr", "size", "kind")

    def __init__(self, name, addr, size, kind):
        self.name, self.addr, self.size, self.kind = name, addr, size, kind

    def __repr__(self):
        return f"Symbol({self.name!r}, 0x{self.addr:08X}, size={self.size}, {self.kind})"


def load_symbols(path):
    """Returns {name: Symbol} for every named FUNC or OBJECT symbol."""
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise ValueError(f"{path}: not an ELF32 file")
    end = ">" if data[5] == 2 else "<"
    shoff = struct.unpack_from(end + "I", data, 0x20)[0]
    shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)

    sections = []
    for i in range(shnum):
        sections.append(struct.unpack_from(end + "IIIIIIIIII", data, shoff + i * shentsize))

    symbols = {}
    for sh in sections:
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9]
        if sh_type != 2:  # SHT_SYMTAB
            continue
        str_off = sections[sh_link][4]
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize or 16):
            st_name, st_value, st_size, st_info, _other, _shndx = struct.unpack_from(end + "IIIBBH", data, off)
            kind = {1: "object", 2: "func"}.get(st_info & 0xF)
            if kind is None or st_name == 0:
                continue
            name_end = data.index(b"\0", str_off + st_name)
            name = data[str_off + st_name:name_end].decode("ascii", "replace")
            # Prefer the first global definition of a name.
            if name not in symbols or (st_info >> 4) == 1:
                symbols[name] = Symbol(name, st_value, st_size, kind)
    return symbols
