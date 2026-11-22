#!/usr/bin/env python3
"""Tests of the port's memory card files (GCI) on the host: docs/USER_GUIDE.md, "Saves".

    tools/port3ds/test_gci.py                 run the self test (needs gcc), then check the Azahar profile's saves
    tools/port3ds/test_gci.py check FILE...   validate GCI files; exit status 1 when one is invalid
    tools/port3ds/test_gci.py selftest        only the self test

A GCI file is one file of a GameCube memory card: a 0x40-byte directory entry (all fields big-endian), then
`blocks * 0x2000` bytes of data. Dolphin keeps its "GCI folder" memory cards as one such file per card file.

`check` validates, field by field:
  - the directory entry: game code, maker code, the 0xFF padding bytes, banner and icon formats, the
    banner/icon/comment offsets against the data, the permission bits, the block count against the file size;
  - the TTYD save inside (a file named mariost_save_file): the header block's byte-sum checksums, and for each of
    the four slots both copies (main at 0x2000, backup at 0x12000): version string, size word, byte-sum checksum
    and its complement. These are the game's own tests (src/manager/cardmgr.c: headerCheck, dataCheck).
  - the byte order of the save data: "BE" is the GameCube's (and what the port writes), "LE" is what earlier port
    builds wrote. The checksum value does not depend on the order (a byte sum), but the size word and the
    checksum words do.

`selftest` compiles the port's own card.c and save_swap.c (port3ds/game/) for the host, with stubs for the
platform layer (threads and log), and drives the CARD API (mount, create, write, read, get/set status) on a
temporary folder. It builds them for the US, JP and PAL releases. The swap tables are the ones the game build
generates from the compiler's struct layout (build/3ds/game/generated/port_swap_gen.c) when that exists for the
release (US and PAL); otherwise a stand-in with the same kinds of fields. It checks:
  - what the port writes is a valid GCI, with big-endian data that passes the game's checks as the GameCube
    would run them (the entry and the TTYD save checks above), and the port reads back the native data it wrote;
  - a GCI made as Dolphin makes them (another modification time, copy counter, permissions, comment offset,
    another file name, big-endian data) reads as the same native data;
  - a save from an earlier port build (little-endian) is read as it is and migrated: the log says so, and the
    next write stores it big-endian;
  - paper-cut's test GCIs (genuine big-endian saves, if ../paper-cut exists) read to the native data the
    tables give, and pass the game's check on the native bytes;
  - a save of another region, a truncated file and a copy of one save under two names are handled, with a log
    line, not a crash.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SECTOR = 0x2000
HDR = 0x40
SAVE_NAME = "mariost_save_file"
SAVE_BLOCKS = 17  # 0x22000 bytes: 0x2000 header block and eight slots of 0x4000

# Per release: game code, version string, size of CardSaveSlot.data (include/manager/cardmgr.h)
RELEASES = {
    b"G8ME": {"name": "US", "version": b"010\0", "size": 0x2260},
    b"G8MP": {"name": "PAL", "version": b"010\0", "size": 0x2260},
    b"G8MJ": {"name": "JP", "version": b"009\0", "size": 0x2140},
}
META_DATA = 0x1E46  # sizeof(CardMetadata.data) that the header checksum covers
SLOT_SIZE = 0x4000
EPOCH_2000 = 946684800  # Unix time of the GameCube clock's zero


# ---- The swap tables ----

GEN_C = os.path.join(REPO, "build", "3ds", "game", "generated", "port_swap_gen.c")
SLOT_BASE = 0x2000
META_TYPE, SLOT_TYPE = "CardMetadata", "CardSaveSlot"
_runs_cache = {}


def load_runs(game):
    """({type: [(offset, width, count)]}, source): the unit runs of CardMetadata and CardSaveSlot."""
    if game in _runs_cache:
        return _runs_cache[game]
    rel = RELEASES[game]
    out = None
    if game != b"G8MJ" and os.path.exists(GEN_C):  # the generated tables are for the build's release (US)
        text = open(GEN_C).read()
        out = {}
        for t in (META_TYPE, SLOT_TYPE):
            m = re.search(r"static const PortSwapRun runs_%s\[\] = \{(.*?)\n\};" % t, text, re.S)
            if m is None:
                out = None
                break
            out[t] = [tuple(int(x) for x in e) for e in re.findall(r"\{(\d+), (\d+), (\d+)\}", m.group(1))]
        src = "generated tables"
    if out is None:
        out = {META_TYPE: [(0x1E40, 2, 3), (0x1FF8, 4, 2)],
               SLOT_TYPE: [(0, 2, 1), (8, 4, (rel["size"] - 8) // 4), (0x3FF4, 4, 3)]}
        src = "stand-in tables"
    _runs_cache[game] = (out, src)
    return _runs_cache[game]


def swap_region(buf, base, runs):
    for off, w, n in runs:
        for k in range(n):
            a = base + off + k * w
            buf[a:a + w] = buf[a:a + w][::-1]


def swap_save(data, game, only=None):
    """The save's data with every region's units byte-reversed (native <-> big-endian). `only` limits it to
    the listed region numbers (0 = header block, 1..8 = slots)."""
    runs, _ = load_runs(game)
    d = bytearray(data)
    for r in range(9):
        if only is not None and r not in only:
            continue
        if r == 0:
            swap_region(d, 0, runs[META_TYPE])
        else:
            swap_region(d, SLOT_BASE + (r - 1) * SLOT_SIZE, runs[SLOT_TYPE])
    return bytes(d)


# ---- Directory entry ----

class Gci:
    pass


def parse(data):
    """Directory entry of a GCI file as a Gci object; `errors` lists what is wrong."""
    g = Gci()
    g.errors = []
    g.warnings = []
    g.size = len(data)
    if len(data) < HDR:
        g.errors.append("shorter than the 0x40-byte directory entry (%d bytes)" % len(data))
        return g
    g.game = data[0:4]
    g.maker = data[4:6]
    g.pad6 = data[6]
    g.banner_fmt = data[7]
    g.filename = data[8:0x28].split(b"\0")[0]
    g.time, g.icon_addr = struct.unpack(">II", data[0x28:0x30])
    g.icon_fmt, g.icon_speed = struct.unpack(">HH", data[0x30:0x34])
    g.perm, g.copies = data[0x34], data[0x35]
    g.first_block, g.blocks = struct.unpack(">HH", data[0x36:0x3A])
    g.pad3a = data[0x3A:0x3C]
    g.comment_addr = struct.unpack(">I", data[0x3C:0x40])[0]
    g.data = data[HDR:]
    return g


def check_entry(g):
    """Field by field checks of the directory entry (appends to g.errors and g.warnings)."""
    if g.size < HDR:
        return
    e, w = g.errors, g.warnings
    if not all(0x20 <= c < 0x7F for c in g.game):
        e.append("game code %r is not printable ASCII" % g.game)
    if not all(0x20 <= c < 0x7F for c in g.maker):
        e.append("maker code %r is not printable ASCII" % g.maker)
    if g.pad6 != 0xFF:
        e.append("byte 6 is 0x%02X, not the 0xFF padding" % g.pad6)
    if not g.filename:
        e.append("empty file name")
    # The 32-byte file name is NUL padded; the SDK compares the whole 32 bytes.
    if g.pad3a != b"\xff\xff":
        e.append("bytes 0x3A-0x3B are %s, not 0xFFFF" % g.pad3a.hex())
    if g.blocks == 0:
        e.append("block count is 0")
    elif g.size != HDR + g.blocks * SECTOR:
        e.append("file is %d bytes; %d blocks need %d" % (g.size, g.blocks, HDR + g.blocks * SECTOR))
    if g.perm & ~0x1C:
        e.append("permission byte 0x%02X has bits outside public(4), no-copy(8), no-move(0x10)" % g.perm)
    if not (g.perm & 0x04):
        w.append("permission byte 0x%02X lacks the public bit" % g.perm)
    if g.first_block < 5:
        w.append("first block %d is in the card's own system area (5 is the first data block); "
                 "importers ignore it" % g.first_block)
    now = time.time() - EPOCH_2000
    if g.time == 0 or g.time > now + 86400:
        w.append("modification time %d is 0 or in the future" % g.time)
    # Banner, icons and the offsets the SDK derives from the formats (CARDStat.c UpdateIconOffsets)
    size = g.blocks * SECTOR
    off = g.icon_addr if g.icon_addr != 0xFFFFFFFF else 0
    if g.icon_addr != 0xFFFFFFFF and g.icon_addr >= size:
        e.append("icon address 0x%X is outside the data" % g.icon_addr)
    banner = g.banner_fmt & 3
    if banner == 1:
        off += 96 * 32 + 512
    elif banner == 2:
        off += 96 * 32 * 2
    elif banner == 3:
        e.append("banner format 3 is not defined")
    tlut = False
    for i in range(8):
        fmt = (g.icon_fmt >> (2 * i)) & 3
        if fmt == 1:
            off += 32 * 32
            tlut = True
        elif fmt == 2:
            off += 32 * 32 * 2
        elif fmt == 3:
            e.append("icon %d format 3 is not defined" % i)
    if tlut:
        off += 512
    if g.icon_addr != 0xFFFFFFFF and off > size:
        e.append("banner and icons end at 0x%X, past the data (0x%X)" % (off, size))
    g.icons_end = off
    if g.comment_addr != 0xFFFFFFFF and g.comment_addr + 64 > size:
        e.append("comment address 0x%X leaves no room for the 64 comment bytes" % g.comment_addr)


# ---- TTYD save ----

def u32(b, o, le):
    return struct.unpack("<I" if le else ">I", b[o:o + 4])[0]


def u16(b, o, le):
    return struct.unpack("<H" if le else ">H", b[o:o + 2])[0]


def check_save(g):
    """TTYD's own checks of the save in a GCI; sets g.endian ('LE', 'BE' or None) and g.slots."""
    g.endian = None
    g.slots = []
    rel = RELEASES.get(g.game)
    if rel is None or g.filename != SAVE_NAME.encode():
        return
    d = g.data
    if len(d) != SAVE_BLOCKS * SECTOR:
        g.errors.append("a TTYD save is %d blocks (0x%X bytes of data), this has %d" %
                        (SAVE_BLOCKS, SAVE_BLOCKS * SECTOR, g.blocks))
        return
    # Byte order: the size word of any slot.
    votes = {"LE": 0, "BE": 0}
    for s in range(8):
        o = 0x2000 + s * SLOT_SIZE + 0x3FF4
        if u32(d, o, True) == rel["size"]:
            votes["LE"] += 1
        if u32(d, o, False) == rel["size"]:
            votes["BE"] += 1
    if votes["LE"] and not votes["BE"]:
        g.endian = "LE"
    elif votes["BE"] and not votes["LE"]:
        g.endian = "BE"
    elif votes["LE"] and votes["BE"]:
        g.endian = "LE"  # cannot happen: 0x2260 is not a palindrome
    else:
        g.endian = "LE"
        g.warnings.append("no slot has a size word of 0x%X in either byte order" % rel["size"])
    le = g.endian == "LE"
    if le:
        g.warnings.append("little-endian data: a save from an earlier port build (the port converts it on load and "
                          "writes it big-endian), not a GameCube save")
    # Header block: headerCheck()
    c1, c2 = u32(d, 0x1FF8, le), u32(d, 0x1FFC, le)
    g.header_ok = c2 == (~c1 & 0xFFFFFFFF) and c1 == (sum(d[0:META_DATA]) & 0xFFFFFFFF)
    if not g.header_ok:
        g.warnings.append("header block checksum is wrong (the game rewrites the header, not an error)")
    # Slots: dataCheck() on the main (0x2000) and backup (0x12000) copies
    for s in range(4):
        ok = []
        for base in (0x2000, 0x12000):
            o = base + s * SLOT_SIZE
            slot = d[o:o + SLOT_SIZE]
            c1, c2 = u32(slot, 0x3FFC - 4, le), u32(slot, 0x3FFC, le)
            good = (u32(slot, 0x3FF4, le) == rel["size"] and slot[0x3FF0:0x3FF4] == rel["version"] and
                    c2 == (~c1 & 0xFFFFFFFF) and c1 == (sum(slot[0:rel["size"]]) & 0xFFFFFFFF))
            ok.append(good)
        flags = u16(d, 0x2000 + s * SLOT_SIZE, le)
        g.slots.append((ok[0], ok[1], flags))
    if not any(a or b for a, b, _ in g.slots):
        g.warnings.append("no slot has a valid copy: the game lists every slot as broken or empty")
    elif not all(a or b for a, b, _ in g.slots):
        g.warnings.append("some slots have no valid copy: the game lists them as broken")


def describe(path, g):
    lines = ["%s: %d bytes" % (path, g.size)]
    if g.size >= HDR:
        lines.append("  entry: game %s maker %s file \"%s\" %d blocks (first %d) time %d (%s) copies %d perm 0x%02X"
                     % (g.game.decode("latin1"), g.maker.decode("latin1"), g.filename.decode("latin1"),
                        g.blocks, g.first_block, g.time,
                        time.strftime("%Y-%m-%d %H:%M", time.gmtime(g.time + EPOCH_2000)), g.copies, g.perm))
        lines.append("  banner fmt 0x%02X icon addr 0x%08X fmt 0x%04X speed 0x%04X comment 0x%08X" %
                     (g.banner_fmt, g.icon_addr, g.icon_fmt, g.icon_speed, g.comment_addr))
    if g.endian:
        lines.append("  TTYD save: data is %s; header %s; slots (main, backup, flags): %s" % (
            g.endian, "ok" if g.header_ok else "BAD",
            ", ".join("%d:%s/%s/0x%X" % (i, "ok" if a else "bad", "ok" if b else "bad", f)
                      for i, (a, b, f) in enumerate(g.slots))))
    for w in g.warnings:
        lines.append("  warning: " + w)
    for e in g.errors:
        lines.append("  ERROR: " + e)
    return "\n".join(lines)


def check_file(path):
    g = parse(open(path, "rb").read())
    check_entry(g)
    check_save(g)
    print(describe(path, g))
    return g


# ---- Building saves ----

def entry(game=b"G8ME", maker=b"01", name=SAVE_NAME, t=0x32942FBE, blocks=SAVE_BLOCKS, perm=4, copies=0,
          first=5, banner=2, icon_addr=0x40, icon_fmt=1, icon_speed=2, comment=0):
    n = name.encode() if isinstance(name, str) else name
    return (game + maker + b"\xff" + bytes([banner]) + n.ljust(32, b"\0") +
            struct.pack(">II", t, icon_addr) + struct.pack(">HH", icon_fmt, icon_speed) +
            bytes([perm, copies]) + struct.pack(">HH", first, blocks) + b"\xff\xff" + struct.pack(">I", comment))


def make_save_data(game=b"G8ME", le=True, empty_slots=(1, 2, 3), seed=1):
    """A 0x22000-byte TTYD save as the game lays it out: valid header block, four slots in both copies.
    Native (little-endian) data as the game holds it, or with le=False the same save as the GameCube
    stores it (every unit of the tables swapped)."""
    rel = RELEASES[game]
    pk = lambda f, v: struct.pack("<" + f, v)
    d = bytearray(SAVE_BLOCKS * SECTOR)
    meta = bytearray(SECTOR)
    meta[0:11] = b"Paper Mario"
    meta[0x20:0x20 + 15] = b"11/20 save data"
    meta[0x1E40:0x1E46] = pk("H", 1) + pk("H", 0) + pk("H", 0)
    meta[0x1FF8:0x2000] = b"\0" * 8
    s = sum(meta[0:META_DATA]) & 0xFFFFFFFF
    meta[0x1FF8:0x2000] = pk("I", s) + pk("I", ~s & 0xFFFFFFFF)
    d[0:SECTOR] = meta
    for slot in range(4):
        sl = bytearray(SLOT_SIZE)
        if slot in empty_slots:
            sl[0:2] = pk("H", 1)  # CARD_SAVE_FLAG_EMPTY
        else:
            for i in range(8, rel["size"]):  # stands for GlobalWork and the rest
                sl[i] = (i * 31 + seed) & 0xFF
            sl[8:12] = pk("I", 0x3C0004)
        sl[0x3FF0:0x3FF4] = rel["version"]
        sl[0x3FF4:0x3FF8] = pk("I", rel["size"])
        s = sum(sl[0:rel["size"]]) & 0xFFFFFFFF
        sl[0x3FF8:0x4000] = pk("I", s) + pk("I", ~s & 0xFFFFFFFF)
        for base in (0x2000, 0x12000):
            d[base + slot * SLOT_SIZE:base + (slot + 1) * SLOT_SIZE] = sl
    return bytes(d) if le else swap_save(d, game)


# ---- The port's card.c on the host ----

FAKE_OS_H = """#pragma once
typedef long long OSTime;
#define OS_TIMER_CLOCK 40500000ull
#define OSTicksToSeconds(t) ((t)/(OS_TIMER_CLOCK))
OSTime OSGetTime(void);
typedef struct { char x[0x50]; } DSPTaskInfo;
typedef struct { void* a; void* b; } OSThreadQueue;
typedef struct { char x[0x30]; } OSAlarm;
typedef struct { char x[0x20]; } DVDDiskID;
"""

STUBS_C = r"""
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
long long OSGetTime(void) { return 0x7FFFFFFFll * 0 + (long long)1000000000 * 40500000ll; } /* 2031-09-09 */
void plat_log(int l, const char* f, ...) {
    static const char* n[] = {"E", "W", "I", "D"};
    va_list a; va_start(a, f);
    printf("[%s] ", n[l]); vprintf(f, a); printf("\n"); va_end(a);
}
void plat_fatal(const char* f, ...) { (void)f; exit(99); }
void plat_wd_hold(int on) { (void)on; }
void plat_set_save_busy(int (*b)(void)) { (void)b; }
void port_irq_enter(void) {}
void port_irq_leave(void) {}
void port_thread_dispatch_if_idle(void) {}
typedef struct { pthread_t t; } PlatThread;
typedef struct { pthread_mutex_t m; pthread_cond_t c; int s; } PlatEvent;
typedef struct { pthread_mutex_t m; } PlatMutex;
struct targ { void (*f)(void*); void* a; };
static void* tr(void* p) { struct targ t = *(struct targ*)p; free(p); t.f(t.a); return 0; }
PlatThread* plat_thread_create(void (*f)(void*), void* a, uint32_t sz, int pr, int core, const char* n) {
    (void)sz; (void)pr; (void)core; (void)n;
    PlatThread* t = malloc(sizeof *t); struct targ* x = malloc(sizeof *x);
    x->f = f; x->a = a; pthread_create(&t->t, 0, tr, x); return t;
}
PlatEvent* plat_event_create(void) {
    PlatEvent* e = calloc(1, sizeof *e); pthread_mutex_init(&e->m, 0); pthread_cond_init(&e->c, 0); return e;
}
void plat_event_signal(PlatEvent* e) {
    pthread_mutex_lock(&e->m); e->s = 1; pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m);
}
void plat_event_wait(PlatEvent* e) {
    pthread_mutex_lock(&e->m); while (!e->s) pthread_cond_wait(&e->c, &e->m); e->s = 0; pthread_mutex_unlock(&e->m);
}
PlatMutex* plat_mutex_create(void) {
    PlatMutex* m = calloc(1, sizeof *m); pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&m->m, &a); return m;
}
void plat_mutex_lock(PlatMutex* m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex* m) { pthread_mutex_unlock(&m->m); }
"""

HARNESS_C = r"""
/* usage: harness list | read NAME OUT | rewrite NAME OUT | create NAME SIZE IN   (works in ./card) */
#include <dolphin/card.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void wait_done(void) { for (int i = 0; i < 5000 && CARDGetResultCode(0) == CARD_RESULT_BUSY; i++) usleep(1000); }
int main(int argc, char** argv) {
    static char work[40960];
    CARDInit();
    CARDMountAsync(0, work, NULL, NULL);
    wait_done();
    printf("mount %d\n", CARDGetResultCode(0));
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; i < 127; i++) {
            CARDStat st;
            if (CARDGetStatus(0, i, &st) == CARD_RESULT_READY)
                printf("file %.4s/%.2s \"%.32s\" length %u time %u icon %08x comment %08x\n", (char*)st.gameName,
                       (char*)st.company, (char*)st.fileName, (unsigned)st.length, (unsigned)st.time,
                       (unsigned)st.iconAddr, (unsigned)st.commentAddr);
        }
        s32 bytes, files;
        CARDFreeBlocks(0, &bytes, &files);
        printf("free %d blocks, %d files\n", bytes / 8192, files);
    } else if (!strcmp(argv[1], "read")) {
        CARDFileInfo fi;
        int o = CARDOpen(0, argv[2], &fi);
        printf("open %d\n", o);
        if (o != 0) return 3;
        char* buf = malloc(fi.length);
        CARDReadAsync(&fi, buf, fi.length, 0, NULL);
        wait_done();
        printf("read %d length %d\n", CARDGetResultCode(0), fi.length);
        FILE* f = fopen(argv[3], "wb"); fwrite(buf, 1, fi.length, f); fclose(f);
    } else if (!strcmp(argv[1], "rewrite")) {
        CARDFileInfo fi;
        if (CARDOpen(0, argv[2], &fi) != 0) return 3;
        char* buf = malloc(fi.length);
        CARDReadAsync(&fi, buf, fi.length, 0, NULL);
        wait_done();
        CARDWriteAsync(&fi, buf, fi.length, 0, NULL);
        wait_done();
        printf("rewrite %d\n", CARDGetResultCode(0));
    } else if (!strcmp(argv[1], "create")) {
        CARDFileInfo fi;
        unsigned size = (unsigned)atoi(argv[3]);
        CARDCreateAsync(0, argv[2], size, &fi, NULL);
        wait_done();
        printf("create %d\n", CARDGetResultCode(0));
        char* buf = calloc(1, size);
        FILE* f = fopen(argv[4], "rb"); fread(buf, 1, size, f); fclose(f);
        CARDWriteAsync(&fi, buf, size, 0, NULL);
        wait_done();
        printf("write %d\n", CARDGetResultCode(0));
        CARDStat st;
        CARDGetStatus(0, fi.fileNo, &st);
        st.iconAddr = 0x40; st.commentAddr = 0; st.bannerFormat = 2; st.iconFormat = 1; st.iconSpeed = 2;
        CARDSetStatusAsync(0, fi.fileNo, &st, NULL);
        wait_done();
        printf("setstatus %d\n", CARDGetResultCode(0));
    }
    return 0;
}
"""


class Host:
    """card.c built for the host for one release; run() drives it in a card folder."""

    def __init__(self, tmp, macro, game):
        self.game = game
        src = os.path.join(tmp, "src")
        os.makedirs(os.path.join(src, "inc", "dolphin"), exist_ok=True)
        for name, text in (("inc/dolphin/os.h", FAKE_OS_H), ("inc/dolphin/dsp.h", "#pragma once\n"),
                           ("inc/dolphin/dvd.h", "#pragma once\n"), ("stubs.c", STUBS_C), ("harness.c", HARNESS_C)):
            with open(os.path.join(src, name), "w") as f:
                f.write(text)
        runs, self.tables = load_runs(game)
        hdr = ['#pragma once', '#include "port3ds/port_swap.h"']
        for t, size in ((META_TYPE, 0x2000), (SLOT_TYPE, 0x4000)):
            hdr.append("static const PortSwapRun runs_%s[] = {%s};" % (
                t, ", ".join("{%d, %d, %d}" % r for r in runs[t])))
            hdr.append('const PortSwapType port_swap_type_%s = {"%s", %d, %d, runs_%s};' % (
                t, t, size, len(runs[t]), t))
        with open(os.path.join(src, "inc", "port_swap_gen.h"), "w") as f:
            f.write("\n".join(hdr) + "\n")
        self.exe = os.path.join(tmp, "harness_" + game.decode())
        cmd = ["gcc", "-std=gnu11", "-w", "-include", os.path.join(REPO, "tools/port3ds/host_decode/host_prefix.h")]
        if macro:
            cmd.append("-D" + macro)
        cmd += ["-DPORT_SAVE_HOST_TEST", "-DMETA_DATA_SIZE=0x1E46u", "-DSLOT_DATA_SIZE=0x%Xu" % RELEASES[game]["size"]]
        cmd += ["-I", os.path.join(src, "inc"), "-I", os.path.join(REPO, "libs/dolsdk2004/include"),
                "-I", os.path.join(REPO, "port3ds/include"), "-I", os.path.join(REPO, "include"),
                "-o", self.exe, os.path.join(src, "harness.c"), os.path.join(REPO, "port3ds/game/card.c"),
                os.path.join(REPO, "port3ds/game/save_swap.c"),
                os.path.join(src, "stubs.c"), "-lpthread"]
        subprocess.run(cmd, check=True, cwd=REPO)

    def run(self, folder, *args):
        r = subprocess.run([self.exe] + list(args), cwd=folder, capture_output=True, text=True, timeout=60)
        return r.returncode, r.stdout


class Results:
    def __init__(self):
        self.failed = 0
        self.passed = 0

    def check(self, ok, text):
        print("  %s %s" % ("PASS" if ok else "FAIL", text))
        if ok:
            self.passed += 1
        else:
            self.failed += 1


def fresh(tmp, name):
    d = os.path.join(tmp, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "card"))
    return d


def find_papercut():
    """paper-cut's test saves folder (a sibling of this checkout or of the main checkout of a worktree)"""
    d = REPO
    for _ in range(6):
        c = os.path.join(os.path.dirname(d), "paper-cut", "tests", "port", "upgrade")
        if os.path.isdir(c):
            return c
        d = os.path.dirname(d)
    return None


def native_valid(native, game):
    """The game's dataCheck/headerCheck on native (little-endian) bytes: per slot (main, backup) validity."""
    rel = RELEASES[game]
    meta = native[0:META_DATA]
    c1, c2 = u32(native, 0x1FF8, True), u32(native, 0x1FFC, True)
    header = c2 == (~c1 & 0xFFFFFFFF) and c1 == (sum(meta) & 0xFFFFFFFF)
    slots = []
    for s in range(4):
        ok = []
        for base in (0x2000, 0x12000):
            o = base + s * SLOT_SIZE
            sl = native[o:o + SLOT_SIZE]
            c1, c2 = u32(sl, 0x3FF8, True), u32(sl, 0x3FFC, True)
            ok.append(u32(sl, 0x3FF4, True) == rel["size"] and sl[0x3FF0:0x3FF4] == rel["version"] and
                      c2 == (~c1 & 0xFFFFFFFF) and c1 == (sum(sl[0:rel["size"]]) & 0xFFFFFFFF))
        slots.append(tuple(ok))
    return header, slots


def selftest_release(tmp, macro, game, res):
    rel = RELEASES[game]
    print("release %s (%s), card.c and save_swap.c built with %s" % (rel["name"], game.decode(),
                                                                     macro or "no macro (JP default)"))
    host = Host(tmp, macro, game)
    print("  swap tables: " + host.tables)
    other = b"G8MJ" if game != b"G8MJ" else b"G8ME"
    gamename = game.decode()
    native = make_save_data(game, le=True)
    be = make_save_data(game, le=False)
    res.check(native != be and be == swap_save(native, game) and swap_save(be, game) == native,
              "the swap is its own inverse and changes the data")
    inp = os.path.join(tmp, "in.bin")
    open(inp, "wb").write(native)
    out_bin = os.path.join(tmp, "out.bin")
    out = lambda: open(out_bin, "rb").read()

    # 1. The port writes: create + write + set status, as cardmgr.c's create does.
    d = fresh(tmp, "w_" + gamename)
    rc, log = host.run(d, "create", SAVE_NAME, str(len(native)), inp)
    path = os.path.join(d, "card", "01-%s-%s.gci" % (gamename, SAVE_NAME))
    res.check(rc == 0 and "setstatus 0" in log and os.path.exists(path),
              "the port creates %s" % os.path.basename(path))
    disk = open(path, "rb").read()
    g = parse(disk)
    check_entry(g)
    check_save(g)
    res.check(not g.errors, "the port's file is a valid GCI (%s)" % ("; ".join(g.errors) or "entry fields ok"))
    res.check(g.game == game and g.maker == b"01" and g.filename == SAVE_NAME.encode() and g.blocks == SAVE_BLOCKS
              and g.pad6 == 0xFF and g.pad3a == b"\xff\xff", "game code, maker, file name, block count, padding")
    res.check(g.icon_addr == 0x40 and g.banner_fmt == 2 and g.icon_fmt == 1 and g.icon_speed == 2 and
              g.comment_addr == 0 and g.perm == 4, "banner, icon and comment fields as the game set them")
    res.check(g.time == 1000000000, "modification time is on the 2000 epoch")
    res.check(g.endian == "BE" and g.header_ok and all(a and b for a, b, _ in g.slots[:1]),
              "the data on disk is big-endian and passes the game's checksums as the GameCube runs them")
    res.check(disk[HDR:] == be, "the file's data is exactly the GameCube byte order of the native save")
    rc, log = host.run(d, "read", SAVE_NAME, out_bin)
    res.check(rc == 0 and out() == native, "the port reads back the native data it wrote")
    h, sl = native_valid(out(), game)
    res.check(h and sl[0] == (True, True), "the data the game sees passes headerCheck and dataCheck")

    # 2. A GCI as Dolphin makes it: other file name, time, copy counter, permissions, comment offset, BE data.
    d = fresh(tmp, "dolphin_" + gamename)
    ent = entry(game=game, t=0x2A0D1C40, copies=2, perm=0x1C, first=0x2F, comment=0x20)
    open(os.path.join(d, "card", "Paper Mario %s.GCI" % gamename), "wb").write(ent + be)
    rc, log = host.run(d, "list")
    res.check(('"%s"' % SAVE_NAME in log) and ("time %d" % 0x2A0D1C40) in log and "comment 00000020" in log and
              "length %d" % (SAVE_BLOCKS * SECTOR) in log,
              "the port lists a Dolphin GCI under another file name (lookup is by the entry, not the file name)")
    res.check("copy count 2" in log and ("%d blocks" % SAVE_BLOCKS) in log, "the log shows blocks and copy counter")
    rc, log = host.run(d, "read", SAVE_NAME, out_bin)
    res.check(rc == 0 and out() == native and "old little-endian" not in log,
              "the port reads the Dolphin GCI's big-endian data as the native save")
    # a write to another file leaves the first file's entry (copy counter, permissions, time) alone
    rc, log = host.run(d, "create", "other_file", "8192", inp)
    after = parse(open(os.path.join(d, "card", "Paper Mario %s.GCI" % gamename), "rb").read())
    res.check(after.copies == 2 and after.perm == 0x1C and after.time == 0x2A0D1C40,
              "a write to another file leaves the Dolphin file's entry untouched")

    # 3. A save from an earlier port build (little-endian): read as it is, migrated by the next write
    d = fresh(tmp, "old_" + gamename)
    open(os.path.join(d, "card", "01-%s-%s.gci" % (gamename, SAVE_NAME)), "wb").write(entry(game=game) + native)
    rc, log = host.run(d, "read", SAVE_NAME, out_bin)
    res.check(rc == 0 and out() == native and "card: converted a save from the old little-endian layout" in log,
              "an old little-endian save reads as the same native data, and the log says it was converted")
    # a mixed file (one slot already rewritten big-endian, the rest old) reads whole
    mixed = swap_save(native, game, only={1, 5})
    open(os.path.join(d, "card", "01-%s-%s.gci" % (gamename, SAVE_NAME)), "wb").write(entry(game=game) + mixed)
    rc, log = host.run(d, "read", SAVE_NAME, out_bin)
    res.check(rc == 0 and out() == native, "a file with some regions old and some big-endian reads whole")
    # the next write stores it big-endian: write the native data back through the port
    os.remove(os.path.join(d, "card", "01-%s-%s.gci" % (gamename, SAVE_NAME)))
    open(os.path.join(d, "card", "01-%s-%s.gci" % (gamename, SAVE_NAME)), "wb").write(entry(game=game) + native)
    rc, log = host.run(d, "rewrite", SAVE_NAME, out_bin)
    disk = open(os.path.join(d, "card", "01-%s-%s.gci" % (gamename, SAVE_NAME)), "rb").read()
    res.check(disk[HDR:] == be, "writing the migrated save stores it big-endian")

    # 4. paper-cut's test saves: genuine big-endian game saves
    pc = find_papercut()
    if game == b"G8ME" and pc:
        for ver in sorted(os.listdir(pc)):
            f = os.path.join(pc, ver, "card1.gci")
            if not os.path.exists(f):
                continue
            raw = open(f, "rb").read()
            d = fresh(tmp, "pc_" + ver)
            open(os.path.join(d, "card", "card1.gci"), "wb").write(raw)
            rc, log = host.run(d, "read", SAVE_NAME, out_bin)
            want = swap_save(raw[HDR:], game)
            h, sl = native_valid(out(), game)
            res.check(rc == 0 and out() == want and h and sl[0] == (True, True),
                      "paper-cut %s card1.gci: reads to the native save and slot 1 passes the game's check" % ver)

    # 5. Another region's save, a truncated file, another game's file
    d = fresh(tmp, "foreign_" + gamename)
    open(os.path.join(d, "card", "01-%s-%s.gci" % (other.decode(), SAVE_NAME)), "wb").write(
        entry(game=other) + make_save_data(other, le=False))
    open(os.path.join(d, "card", "trunc.gci"), "wb").write(entry(game=game, name="trunc")[:0x40] + b"\0" * 5000)
    open(os.path.join(d, "card", "01-GALE-other.gci"), "wb").write(entry(game=b"GALE", name="other") + b"\0" * 0x2000)
    open(os.path.join(d, "card", "empty.gci"), "wb").write(b"\x01\x02")
    open(os.path.join(d, "card", "notes.txt"), "wb").write(b"not a gci")
    rc, log = host.run(d, "list")
    region = RELEASES[other]["name"]
    res.check(rc == 0 and not any(l.startswith("file ") for l in log.splitlines()),
              "none of them is offered to the game, and no crash (exit %d)" % rc)
    res.check("a %s save (%s)" % (region, other.decode()) in log and "skipped" in log,
              "the other region's save is logged as '%s save', not silently dropped" % region)
    res.check("trunc.gci skipped" in log and "bytes, but its header says %d blocks" % SAVE_BLOCKS in log,
              "the truncated file is logged with its size and block count")
    res.check("another game's file (GALE/01)" in log and "shorter than a GCI header" in log,
              "another game's file and a 2-byte file are logged")
    rc, log = host.run(d, "create", SAVE_NAME, str(len(native)), inp)
    res.check("create 0" in log and "write 0" in log, "creating a save next to foreign files works")

    # 6. Two files with one save inside: the newer is used
    d = fresh(tmp, "dup_" + gamename)
    new = make_save_data(game, le=True, seed=9)
    open(os.path.join(d, "card", "a_old.gci"), "wb").write(entry(game=game, t=1000) +
                                                          make_save_data(game, le=False, seed=3))
    open(os.path.join(d, "card", "b_new.gci"), "wb").write(entry(game=game, t=2000) + swap_save(new, game))
    rc, log = host.run(d, "read", SAVE_NAME, out_bin)
    res.check(rc == 0 and out() == new and "hold the same save file" in log,
              "two copies of one save under two names: the newer is used, and the log says so")


def selftest():
    if shutil.which("gcc") is None:
        print("gcc not found: skipping the self test of card.c")
        return None
    res = Results()
    tmp = tempfile.mkdtemp(prefix="test_gci_")
    try:
        selftest_release(tmp, "VERSION_G8ME01", b"G8ME", res)
        selftest_release(tmp, None, b"G8MJ", res)
        selftest_release(tmp, "VERSION_G8MP01", b"G8MP", res)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("self test: %d passed, %d failed" % (res.passed, res.failed))
    return res.failed == 0


def main(argv):
    if argv and argv[0] == "check":
        ok = True
        for p in argv[1:]:
            g = check_file(p)
            ok = ok and not g.errors
        return 0 if ok else 1
    if argv and argv[0] == "selftest":
        return 0 if selftest() is not False else 1
    ok = selftest()
    prof = os.path.join(REPO, "build", "azahar-profile", "data", "azahar-emu", "sdmc", "3ds", "ttyd", "card")
    if os.path.isdir(prof):
        print("saves in %s:" % prof)
        for n in sorted(os.listdir(prof)):
            if n.lower().endswith(".gci"):
                g = check_file(os.path.join(prof, n))
                ok = ok and not g.errors
    return 0 if ok is not False else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
