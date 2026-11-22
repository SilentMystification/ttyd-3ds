"""Effective data addresses of ARM-mode (ARMv6/v7 + VFP) load and store instructions.

decode(insn, regs) -> list of (address, size, is_write), or None when the
instruction does not access memory (or is one this decoder does not know).
regs is r0..r15 as sampled with the PC being the address of the instruction;
a read of r15 gives PC+8 as the hardware does. Conditions are not evaluated
(the sample has no CPSR), so a conditional load that did not execute is still
counted; for the tool's statistics that is a small bias.

Covered: LDR/STR (immediate, register, scaled register, pre/post index, byte),
LDRH/LDRSH/LDRSB/LDRD/STRH/STRD, LDM/STM (the whole block is one access of
4*n bytes), LDREX/STREX family, VLDR/VSTR, VLDM/VSTM/VPUSH/VPOP.
Not covered: Thumb (the build has no Thumb code), NEON VLDn/VSTn (reported as
"neon" by is_neon_mem), coprocessor LDC/STC to other coprocessors.
"""

M32 = 0xFFFFFFFF


def _reg(regs, n, pc):
    return (pc + 8) & M32 if n == 15 else regs[n]


def _shifted(regs, insn, pc):
    rm = _reg(regs, insn & 15, pc)
    typ = (insn >> 5) & 3
    amt = (insn >> 7) & 31
    if typ == 0:
        return (rm << amt) & M32
    if typ == 1:
        return 0 if amt == 0 else rm >> amt
    if typ == 2:
        if amt == 0:
            amt = 32
        v = rm - (1 << 32) if rm & 0x80000000 else rm
        return (v >> min(amt, 31)) & M32
    if amt == 0:  # RRX: needs carry, approximate
        return rm >> 1
    return ((rm >> amt) | (rm << (32 - amt))) & M32


def is_neon_mem(insn):
    return (insn >> 24) == 0xF4


def decode(insn, regs, pc):
    cond = insn >> 28
    if cond == 0xF:
        return None
    top = (insn >> 25) & 7
    if top in (2, 3):  # single data transfer, 01xxxxxx
        if top == 3 and insn & 0x10:
            return None  # media instruction
        p, u, b, w, l = (insn >> 24) & 1, (insn >> 23) & 1, (insn >> 22) & 1, (insn >> 21) & 1, (insn >> 20) & 1
        base = _reg(regs, (insn >> 16) & 15, pc)
        off = _shifted(regs, insn, pc) if top == 3 else insn & 0xFFF
        addr = (base + off if u else base - off) & M32 if p else base
        return [(addr, 1 if b else 4, not l)]
    if top == 0:
        if (insn & 0x90) == 0x90:  # bit7 and bit4
            sh = (insn >> 5) & 3
            if sh == 0:
                if (insn & 0x0F800000) == 0x01800000:  # exclusive: LDREX/STREX family
                    size = {0: 4, 1: 8, 2: 1, 3: 2}[(insn >> 21) & 3]
                    return [(regs[(insn >> 16) & 15], size, not (insn >> 20) & 1)]
                if (insn & 0x0FB00000) == 0x01000000:  # SWP/SWPB
                    return [(regs[(insn >> 16) & 15], 1 if insn & 0x400000 else 4, True)]
                return None  # multiply
            p, u, i, l = (insn >> 24) & 1, (insn >> 23) & 1, (insn >> 22) & 1, (insn >> 20) & 1
            base = _reg(regs, (insn >> 16) & 15, pc)
            off = ((insn >> 4) & 0xF0) | (insn & 15) if i else _reg(regs, insn & 15, pc)
            addr = (base + off if u else base - off) & M32 if p else base
            if sh == 1:
                size, wr = 2, not l
            elif l:
                size, wr = (1 if sh == 2 else 2), False
            else:
                size, wr = 8, sh == 3  # LDRD (sh=2) reads, STRD (sh=3) writes
            return [(addr, size, wr)]
        return None
    if top == 4:  # LDM/STM
        p, u, l = (insn >> 24) & 1, (insn >> 23) & 1, (insn >> 20) & 1
        base = _reg(regs, (insn >> 16) & 15, pc)
        n = bin(insn & 0xFFFF).count("1")
        if u:
            start = base + (4 if p else 0)
        else:
            start = base - 4 * n + (0 if p else 4)
        return [(start & M32, 4 * n, not l)] if n else None
    if top == 6:  # coprocessor load/store; VFP when cp = 10/11
        if (insn >> 9) & 7 != 5:
            return None
        base = _reg(regs, (insn >> 16) & 15, pc)
        imm = (insn & 0xFF) * 4
        l = (insn >> 20) & 1
        if (insn >> 24) & 1 and not (insn >> 21) & 1:  # VLDR/VSTR: P=1, W=0
            u = (insn >> 23) & 1
            addr = (base + imm if u else base - imm) & M32
            return [(addr, 8 if (insn >> 8) & 1 else 4, not l)]
        if (insn >> 21) & 0x7F == 0b0000010 or (insn >> 21) & 0x7F == 0b0000011:
            return None  # VMOV two registers
        p, u = (insn >> 24) & 1, (insn >> 23) & 1
        if u and not p:  # IA
            addr = base
        elif p and not u:  # DB (VPUSH is this with sp)
            addr = base - imm
        else:
            return None
        return [(addr & M32, imm, not l)] if imm else None
    return None
