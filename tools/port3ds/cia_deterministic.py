#!/usr/bin/env python3
"""Make a makerom CIA reproducible (in place).

makerom (target t, no common key) writes three random values per run:
  ticket title key (16 bytes, unused: the content is not encrypted),
  ticket ID (8 bytes), and the TMD content ID (4 bytes).
The TMD hashes and both RSA signatures depend on them, so two runs on the same
ELF differ in about 600 bytes. This script sets the three values to constants,
recomputes the TMD info-record and content-info hashes, and replaces the two
signatures with SHA-256(signed body) tiled over the signature field.
The makerom signatures never verify (test keys against production certs); AM
accepts the CIA only with signature checks patched out, with the old or the new
signature alike. Nothing else in the file changes.
"""
import hashlib
import struct
import sys

TITLE_KEY = bytes(16)
TICKET_ID = bytes.fromhex("0000000000000001")
CONTENT_ID = bytes.fromhex("00000000")

SIG_SIZES = {0x00010004: 0x100 + 0x3C, 0x00010003: 0x200 + 0x3C, 0x00010005: 0x60 + 0x3C,
             0x00010000: 0x200 + 0x3C, 0x00010001: 0x100 + 0x3C, 0x00010002: 0x3C + 0x3C}
SIG_BODY = {0x00010004: 0x100, 0x00010003: 0x200, 0x00010005: 0x3C, 0x00010000: 0x200,
            0x00010001: 0x100, 0x00010002: 0x3C}


def align(n, a=0x40):
    return (n + a - 1) // a * a


def resign(buf, off, body_len):
    stype = struct.unpack_from(">I", buf, off)[0]
    sig_len = SIG_BODY[stype]
    body = off + 4 + SIG_SIZES[stype]
    digest = hashlib.sha256(bytes(buf[body:body + body_len])).digest()
    buf[off + 4:off + 4 + sig_len] = (digest * (sig_len // 32 + 1))[:sig_len]
    return body


def main(path):
    buf = bytearray(open(path, "rb").read())
    hdr, _t, _v, cert, tik, tmd = struct.unpack_from("<IHHIII", buf, 0)
    tik_off = align(hdr) + align(cert)
    tmd_off = tik_off + align(tik)

    # Ticket: body = issuer(0x40) key(0x10 at 0x7F rel. to body+0x3F) ... ID at 0x90.
    tb = tik_off + 4 + SIG_SIZES[struct.unpack_from(">I", buf, tik_off)[0]]
    buf[tb + 0x7F:tb + 0x8F] = TITLE_KEY
    buf[tb + 0x90:tb + 0x98] = TICKET_ID
    resign(buf, tik_off, tik - (tb - tik_off))

    # TMD: header 0xC4 bytes, then 64 info records (0x900), then chunk records (0x30 each).
    mb = tmd_off + 4 + SIG_SIZES[struct.unpack_from(">I", buf, tmd_off)[0]]
    count = struct.unpack_from(">H", buf, mb + 0x9E)[0]
    info = mb + 0xC4
    chunks = info + 0x900
    for i in range(count):
        buf[chunks + 0x30 * i:chunks + 0x30 * i + 4] = CONTENT_ID
    # Info record 0 covers all chunk records (makerom: one group when count <= 0x...).
    n0 = struct.unpack_from(">H", buf, info + 2)[0]
    buf[info + 4:info + 36] = hashlib.sha256(bytes(buf[chunks:chunks + 0x30 * n0])).digest()
    buf[mb + 0xA4:mb + 0xC4] = hashlib.sha256(bytes(buf[info:info + 0x900])).digest()
    resign(buf, tmd_off, 0xC4)
    open(path, "wb").write(buf)


if __name__ == "__main__":
    main(sys.argv[1])
