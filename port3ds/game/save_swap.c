/* The save file's byte order at the CARD boundary (card.c calls port_card_convert).
 *
 * The GameCube's memory card file is big-endian, and so is a Dolphin GCI, so the port writes the save big-endian
 * and reads it back into the 3DS's native (little-endian) structs. The game's own code (cardmgr.c) always sees
 * native structs and computes its checksums over them.
 *
 * Which bytes to swap comes from the compiler, not from a list here: gen_swap.py's "save" format makes a table of
 * the 2/4/8-byte units of CardMetadata (the 0x2000-byte header block) and CardSaveSlot (one 0x4000-byte slot, its
 * GlobalWork, pouch, version, size and checksum words included). The byte sum the game uses as its checksum does
 * not change when bytes move inside a unit, so a checksum the GameCube made over big-endian bytes is the same
 * number over the swapped bytes, and the stored checksum words only need their own swap.
 *
 * The file's layout (cardmgr.h): the header block at 0, then eight slots of 0x4000 bytes from 0x2000 (the four
 * slots, then their four backup copies).
 *
 * Older port builds wrote the save little-endian. On a read each region (the header block, each slot) is
 * recognised by its checksum words: read as big-endian they must match the byte sum of the data and each other;
 * if they do not but match as little-endian, the region is an old one, which is already native, so it is left as
 * it is. The next write of the region stores it big-endian. A region that matches neither way (blank, damaged)
 * is taken as big-endian, and the game's own check then reports it. */
#include <stdlib.h>
#include <string.h>

#include "plat/plat.h"
#include "port_swap_gen.h"

#ifndef PORT_SAVE_HOST_TEST
#include "manager/cardmgr.h"
#define META_DATA_SIZE ((u32)sizeof(((CardMetadata*)0)->data))
#define SLOT_DATA_SIZE ((u32)sizeof(((CardSaveSlot*)0)->data))
#endif

#define META_SIZE 0x2000u
#define SLOT_SIZE 0x4000u
#define SLOT_BASE 0x2000u
#define SLOT_COUNT 8u
#define SUM_OFFSET_META 0x1FF8u /* checksum1; checksum2 follows */
#define SUM_OFFSET_SLOT 0x3FF8u

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 le32(const u8* p) { return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; }

static void swap_units(u8* p, u32 width, u32 count) {
    for (u32 i = 0; i < count; i++, p += width) {
        for (u32 a = 0, b = width - 1; a < b; a++, b--) {
            u8 t = p[a];
            p[a] = p[b];
            p[b] = t;
        }
    }
}

static void swap_struct(u8* base, const PortSwapType* t) {
    for (u32 i = 0; i < t->nruns; i++) {
        const PortSwapRun* r = &t->runs[i];
        if (r->width != 0) {
            swap_units(base + r->offset, r->width, r->count);
        }
    }
}

/* 1: the region's checksum words are valid read big-endian, or valid in neither order (taken as big-endian);
 * 0: valid little-endian only (an old port save). */
static int region_is_big_endian(const u8* r, u32 data_size, u32 sum_offset) {
    u32 sum = 0;
    for (u32 i = 0; i < data_size; i++) {
        sum += r[i];
    }
    u32 b1 = be32(r + sum_offset), b2 = be32(r + sum_offset + 4);
    if (b1 == sum && b2 == ~b1) {
        return 1;
    }
    u32 l1 = le32(r + sum_offset), l2 = le32(r + sum_offset + 4);
    return !(l1 == sum && l2 == ~l1);
}

static void convert_region(u8* p, int to_disk, int meta) {
    const PortSwapType* t = meta ? &port_swap_type_CardMetadata : &port_swap_type_CardSaveSlot;
    if (!to_disk) {
        u32 data = meta ? META_DATA_SIZE : SLOT_DATA_SIZE;
        if (!region_is_big_endian(p, data, meta ? SUM_OFFSET_META : SUM_OFFSET_SLOT)) {
            static int logged;
            if (!logged) {
                logged = 1;
                PLOG_I("card: converted a save from the old little-endian layout");
            }
            return;
        }
    }
    swap_struct(p, t);
}

/* Converts the card file bytes [offset, offset + length) of buf between disk (big-endian) and native order, in
 * place. Offsets are into the file's data, after the 0x40-byte GCI entry. to_disk 1 before a write, 0 after a
 * read. Only whole regions are converted: the game reads and writes the header block and the slots whole. */
void port_card_convert(u32 offset, void* buf, u32 length, int to_disk) {
    u8* b = buf;
    u32 end = offset + length;
    for (u32 r = 0; r <= SLOT_COUNT; r++) {
        u32 start = r == 0 ? 0 : SLOT_BASE + (r - 1) * SLOT_SIZE;
        u32 size = r == 0 ? META_SIZE : SLOT_SIZE;
        if (start >= end || start + size <= offset) {
            continue;
        }
        if (start < offset || start + size > end) {
            PLOG_W("card: access to 0x%lx..0x%lx cuts through the save region at 0x%lx; not converted",
                   (unsigned long)offset, (unsigned long)end, (unsigned long)start);
            continue;
        }
        convert_region(b + (start - offset), to_disk, r == 0);
    }
}
