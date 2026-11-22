/* Dolphin DVD for the 3DS port: files from the user's disc image.
 *
 * DVDInit opens sdmc:/3ds/ttyd/game.iso, checks the game ID against the
 * version this build is for, and loads the FST. Paths resolve as on the
 * GameCube (relative to the root, "." and ".." allowed, names compared
 * without case). A read runs on the platform file thread while the calling
 * game thread sleeps, so other game threads run meanwhile as they do while
 * the GameCube's drive works: the DVD manager thread's background loads
 * overlap the main loop.
 *
 * rel/<area>.rel is answered with a module header naming the static REL
 * (os_link.c); the disc's REL code is never used.
 */
#include <ctype.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <malloc.h>
#include <string.h>

#include "plat/disc.h"
#include "plat/plat.h"
#include "port3ds/fst.h"
#include "port3ds/os_internal.h"
#include "port3ds/rel_static.h"
#include "version.h"

void port_disc_data_fill(void);

#if VERSION == VERSION_US
#define EXPECTED_ID "G8ME01"
#elif VERSION == VERSION_JP
#define EXPECTED_ID "G8MJ01"
#elif VERSION == VERSION_EU
#define EXPECTED_ID "G8MP01"
#else
#error "the 3DS port supports the G8MJ01, G8ME01 and G8MP01 releases"
#endif

/* Marks a DVDFileInfo that reads a static REL's module header. */
#define REL_FILE_MARK 0x52454C00u /* 'REL\0' */

typedef PortFstEntry FstEntry;

static PortFst s_tab; /* the parsed FST (port3ds/fst.h) */
static FstEntry* s_fst;
static const char* s_names;
static u32 s_count;
static u32 s_dol_offset;
static int s_ready;

static inline u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

u32 port_dvd_dol_offset(void) { return s_dol_offset; }

/* Disc offset and size of a file, bypassing the static-REL redirection
 * (for reading data out of the disc's own RELs). Returns 0 if absent. */
u32 port_dvd_raw_file(const char* path, u32* size) {
    s32 e = DVDConvertPathToEntrynum(path);
    if (e <= 0 || s_fst[e].is_dir) {
        return 0;
    }
    *size = s_fst[e].size;
    return s_fst[e].offset;
}

/* Plain synchronous read for boot-time use (before threads matter). */
s32 port_dvd_read_raw(u32 offset, void* buf, u32 size) { return plat_disc_read(offset, buf, size); }

void DVDInit(void) {
    if (s_ready) {
        return;
    }
    u64 size = 0;
    if (plat_disc_open(&size) != 0) {
        plat_user_error("  No disc image found.\n\n  Copy your Paper Mario: The Thousand-Year\n  Door disc image (" EXPECTED_ID ") to\n\n  sdmc:/3ds/ttyd/game.iso");
    }
    u8 header[0x440];
    if (size < sizeof(header) || plat_disc_read(0, header, sizeof(header)) != (s32)sizeof(header)) {
        plat_user_error("  sdmc:/3ds/ttyd/game.iso cannot be read.");
    }
    if (memcmp(header, EXPECTED_ID, 6) != 0 || be32(header + 0x1C) != 0xC2339F3Du) {
        char id[7];
        memcpy(id, header, 6);
        for (int i = 0; i < 6; i++) {
            id[i] = isprint((unsigned char)id[i]) ? id[i] : '?';
        }
        id[6] = '\0';
        plat_user_error("  sdmc:/3ds/ttyd/game.iso is not the right\n  game: found %s, this build needs\n  " EXPECTED_ID ".", id);
    }
    if (header[7] != 0) {
        plat_user_error("  This build supports revision 0 of " EXPECTED_ID ";\n  the image is revision %d.", header[7]);
    }
    s_dol_offset = be32(header + 0x420);
    u32 fst_offset = be32(header + 0x424);
    u32 fst_size = be32(header + 0x428);
    u8* raw = memalign(32, fst_size);
    if (raw == NULL || plat_disc_read(fst_offset, raw, fst_size) != (s32)fst_size) {
        plat_user_error("  The disc image's file table cannot be read.");
    }
    if (port_fst_load(&s_tab, raw, fst_size) != 0) {
        plat_user_error("  The disc image's file table is damaged.");
    }
    s_fst = s_tab.entries;
    s_names = s_tab.names; /* the string table stays in the FST buffer */
    s_count = s_tab.count;
    s_ready = 1;
    PLOG_I("DVD: " EXPECTED_ID " image, %lu FST entries, main.dol at %08lx", (unsigned long)s_count,
           (unsigned long)s_dol_offset);
    port_disc_data_fill();
}

s32 DVDConvertPathToEntrynum(const char* path) {
    if (!s_ready || path == NULL) {
        return -1;
    }
    return port_fst_find(&s_tab, path);
}

/* "rel/<area>.rel" (with any root prefix) -> static REL index, else -1. */
static int rel_index_of(s32 entry) {
    if (entry <= 0 || s_fst[entry].is_dir) {
        return -1;
    }
    const char* name = s_names + s_fst[entry].name;
    size_t n = strlen(name);
    if (n < 5 || n > 12 || strcmp(name + n - 4, ".rel") != 0) {
        return -1;
    }
    u32 parent = 0;
    for (u32 i = (u32)entry; i-- > 0;) {
        if (s_fst[i].is_dir && s_fst[i].size > (u32)entry) {
            parent = i;
            break;
        }
    }
    if (parent == 0 || strcasecmp(s_names + s_fst[parent].name, "rel") != 0) {
        return -1;
    }
    char area[9];
    memcpy(area, name, n - 4);
    area[n - 4] = '\0';
    return port_rel_index(area);
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    if (entrynum < 0 || (u32)entrynum >= s_count || s_fst[entrynum].is_dir) {
        return FALSE;
    }
    memset(fileInfo, 0, sizeof(*fileInfo));
    int rel = rel_index_of(entrynum);
    if (rel >= 0) {
        fileInfo->startAddr = REL_FILE_MARK | (u32)rel;
        fileInfo->length = port_rel_header_size();
    } else {
        fileInfo->startAddr = s_fst[entrynum].offset;
        fileInfo->length = s_fst[entrynum].size;
    }
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    s32 entry = DVDConvertPathToEntrynum(fileName);
    if (entry < 0) {
        PLOG_W("DVDOpen: %s not on the disc", fileName);
        return FALSE;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

/* ---- Reads ---- */

typedef struct {
    OSThreadQueue queue;
    volatile s32 result;
    volatile int done;
} SyncRead;

static void sync_done(void* ctx, int32_t result) {
    SyncRead* r = ctx;
    port_irq_enter();
    r->result = result;
    r->done = 1;
    OSWakeupThread(&r->queue);
    port_irq_leave();
}

static s32 read_file(DVDFileInfo* fi, void* addr, s32 length, s32 offset) {
    if (offset < 0 || length < 0 || (u32)offset > fi->length) {
        return DVD_RESULT_FATAL_ERROR;
    }
    if ((u32)length > fi->length - (u32)offset) {
        length = (s32)(fi->length - (u32)offset);
    }
    if ((fi->startAddr & 0xFFFFFF00u) == REL_FILE_MARK) {
        u8 header[0x100];
        port_rel_make_header((int)(fi->startAddr & 0xFF), header);
        memcpy(addr, header + offset, (size_t)length);
        return length;
    }
    if (length == 0) {
        return 0;
    }
    if (OSGetCurrentThread() == NULL || port_irq_depth() > 0) {
        /* Not from a schedulable game thread: read in place. */
        return plat_disc_read((u64)fi->startAddr + (u32)offset, addr, (u32)length);
    }
    SyncRead r;
    OSInitThreadQueue(&r.queue);
    r.done = 0;
    r.result = 0;
    BOOL level = OSDisableInterrupts();
    if (plat_disc_read_async((u64)fi->startAddr + (u32)offset, addr, (u32)length, sync_done, &r) != 0) {
        OSRestoreInterrupts(level);
        return plat_disc_read((u64)fi->startAddr + (u32)offset, addr, (u32)length);
    }
    while (!r.done) {
        OSSleepThread(&r.queue);
    }
    OSRestoreInterrupts(level);
    return r.result;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    (void)prio;
    fileInfo->cb.state = DVD_STATE_BUSY;
    s32 result = read_file(fileInfo, addr, length, offset);
    fileInfo->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    if (result < 0) {
        plat_fatal("DVD: read of %ld bytes at +%ld from %08lx failed (%ld)", (long)length, (long)offset,
                   (unsigned long)fileInfo->startAddr, (long)result);
    }
    return result;
}

/* The drive never reports a problem: the image is on the SD card. */
s32 DVDGetDriveStatus(void) { return DVD_STATE_END; }

/* Disc audio streaming (DTK). TTYD never prepares a stream (sound.c only
 * stops one), so there is never one to cancel: the command completes at
 * once, as the drive's does when nothing streams. */
s32 DVDCancelStream(DVDCommandBlock* block) {
    if (block != NULL) {
        block->state = DVD_STATE_END;
    }
    return 0;
}
