/* Dolphin CARD on the SD card: memory card slot A as a folder of GCI files.
 *
 * sdmc:/3ds/ttyd/card/ holds one file per memory card file in the GCI
 * format (a 64-byte big-endian directory entry, then the data), named as
 * Dolphin's GCI folders name them ("01-G8ME-mariost_save_file.gci"), so
 * saves move between the port and Dolphin. The card reports 2043 free
 * blocks of 8 KiB (a Memory Card 251) in slot A and nothing in slot B.
 *
 * Asynchronous calls run on the card thread (core 1), which writes and
 * fsyncs the file, sets the channel's result and then calls the game's
 * callback as an interrupt, as the GameCube's EXI interrupt does.
 * cardmgr.c polls CARDGetResultCode between frames, as on the GameCube.
 */
#include <dirent.h>
#include <dolphin/card.h>
#include <dolphin/os.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plat/plat.h"
#include "plat/thread.h"
#include "port3ds/os_internal.h"
#include "version.h"

#define CARD_DIR "card"
#define SECTOR 8192
#define TOTAL_BLOCKS 2043
#define MAX_FILES 127

typedef struct {
    int used;
    char path[96];
    CARDDir dir;  /* native-endian copy of the entry */
} Slot;

static Slot s_files[MAX_FILES];
static int s_mounted;
static volatile s32 s_result[2] = {CARD_RESULT_READY, CARD_RESULT_NOCARD};

/* ---- GCI directory entries (big-endian on disk) ---- */

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static void put32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static void put16(u8* p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void dir_from_disk(const u8* d, CARDDir* e) {
    memcpy(e->gameName, d, 4);
    memcpy(e->company, d + 4, 2);
    e->_padding0 = d[6];
    e->bannerFormat = d[7];
    memcpy(e->fileName, d + 8, CARD_FILENAME_MAX);
    e->time = be32(d + 0x28);
    e->iconAddr = be32(d + 0x2C);
    e->iconFormat = be16(d + 0x30);
    e->iconSpeed = be16(d + 0x32);
    e->permission = d[0x34];
    e->copyTimes = d[0x35];
    e->startBlock = be16(d + 0x36);
    e->length = be16(d + 0x38);
    memcpy(e->_padding1, d + 0x3A, 2);
    e->commentAddr = be32(d + 0x3C);
}

static void dir_to_disk(const CARDDir* e, u8* d) {
    memcpy(d, e->gameName, 4);
    memcpy(d + 4, e->company, 2);
    d[6] = 0xFF;
    d[7] = e->bannerFormat;
    memcpy(d + 8, e->fileName, CARD_FILENAME_MAX);
    put32(d + 0x28, e->time);
    put32(d + 0x2C, e->iconAddr);
    put16(d + 0x30, e->iconFormat);
    put16(d + 0x32, e->iconSpeed);
    d[0x34] = e->permission;
    d[0x35] = e->copyTimes;
    put16(d + 0x36, e->startBlock);
    put16(d + 0x38, e->length);
    d[0x3A] = d[0x3B] = 0xFF;
    put32(d + 0x3C, e->commentAddr);
}

static int write_dir(const Slot* s) {
    FILE* f = fopen(s->path, "r+b");
    if (f == NULL) {
        return CARD_RESULT_IOERROR;
    }
    u8 d[64];
    dir_to_disk(&s->dir, d);
    int ok = fwrite(d, 1, 64, f) == 64;
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ok ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
}

/* The disc's game code and maker code (the file's owner). */
static void own_ids(u8 game[4], u8 company[2]) {
#if VERSION == VERSION_EU
    memcpy(game, "G8MP", 4);
#elif VERSION == VERSION_JP
    memcpy(game, "G8MJ", 4);
#else
    memcpy(game, "G8ME", 4);
#endif
    memcpy(company, "01", 2);
}

static void scan(void) {
    memset(s_files, 0, sizeof(s_files));
    mkdir(CARD_DIR, 0777);
    DIR* d = opendir(CARD_DIR);
    if (d == NULL) {
        return;
    }
    struct dirent* de;
    int n = 0;
    while ((de = readdir(d)) != NULL && n < MAX_FILES) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcasecmp(de->d_name + len - 4, ".gci") != 0) {
            continue;
        }
        Slot* s = &s_files[n];
        snprintf(s->path, sizeof(s->path), CARD_DIR "/%s", de->d_name);
        FILE* f = fopen(s->path, "rb");
        if (f == NULL) {
            continue;
        }
        u8 hdr[64];
        if (fread(hdr, 1, 64, f) == 64) {
            dir_from_disk(hdr, &s->dir);
            s->used = 1;
            n++;
        }
        fclose(f);
    }
    closedir(d);
}

static int find(const char* name) {
    u8 game[4], company[2];
    own_ids(game, company);
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i].used && memcmp(s_files[i].dir.gameName, game, 4) == 0 &&
            memcmp(s_files[i].dir.company, company, 2) == 0 &&
            strncmp((const char*)s_files[i].dir.fileName, name, CARD_FILENAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

static int used_blocks(void) {
    int n = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i].used) {
            n += s_files[i].dir.length;
        }
    }
    return n;
}

/* ---- The card thread ---- */

typedef enum { JOB_MOUNT, JOB_CHECK, JOB_FORMAT, JOB_CREATE, JOB_READ, JOB_WRITE, JOB_SETSTATUS } JobKind;

typedef struct {
    JobKind kind;
    s32 chan;
    CARDCallback cb;
    CARDFileInfo* info;
    void* buf;
    s32 length, offset;
    char name[CARD_FILENAME_MAX + 1];
    u32 size;
    s32 file_no;
    CARDStat stat;
} Job;

#define JOBS 16
static Job s_jobs[JOBS];
static volatile u32 s_job_head, s_job_tail;
static PlatMutex* s_lock;
static PlatEvent* s_wake;
static PlatThread* s_thread;
static volatile int s_cancel;

static s32 do_create(Job* j) {
    if (strlen(j->name) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    if (find(j->name) >= 0) {
        return CARD_RESULT_EXIST;
    }
    u32 blocks = (j->size + SECTOR - 1) / SECTOR;
    if (used_blocks() + (int)blocks > TOTAL_BLOCKS) {
        return CARD_RESULT_INSSPACE;
    }
    int n = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!s_files[i].used) {
            n = i;
            break;
        }
    }
    if (n < 0) {
        return CARD_RESULT_NOENT;
    }
    Slot* s = &s_files[n];
    memset(s, 0, sizeof(*s));
    own_ids(s->dir.gameName, s->dir.company);
    strncpy((char*)s->dir.fileName, j->name, CARD_FILENAME_MAX);
    s->dir.time = (u32)OSTicksToSeconds(OSGetTime());
    s->dir.iconAddr = 0xFFFFFFFF;
    s->dir.commentAddr = 0xFFFFFFFF;
    s->dir.permission = CARD_ATTR_PUBLIC;
    s->dir.startBlock = (u16)(5 + used_blocks());
    s->dir.length = (u16)blocks;
    snprintf(s->path, sizeof(s->path), CARD_DIR "/%.2s-%.4s-%s.gci", (const char*)s->dir.company,
             (const char*)s->dir.gameName, j->name);
    FILE* f = fopen(s->path, "wb");
    if (f == NULL) {
        return CARD_RESULT_IOERROR;
    }
    u8 d[64];
    dir_to_disk(&s->dir, d);
    static u8 zero[SECTOR];
    int ok = fwrite(d, 1, 64, f) == 64;
    for (u32 b = 0; b < blocks && ok; b++) {
        ok = fwrite(zero, 1, SECTOR, f) == SECTOR;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (!ok) {
        remove(s->path);
        return CARD_RESULT_IOERROR;
    }
    s->used = 1;
    if (j->info != NULL) {
        j->info->chan = j->chan;
        j->info->fileNo = n;
        j->info->offset = 0;
        j->info->length = (s32)(blocks * SECTOR);
        j->info->iBlock = s->dir.startBlock;
    }
    return CARD_RESULT_READY;
}

static s32 do_rw(Job* j, int write) {
    CARDFileInfo* fi = j->info;
    if (fi == NULL || fi->fileNo < 0 || fi->fileNo >= MAX_FILES || !s_files[fi->fileNo].used) {
        return CARD_RESULT_NOFILE;
    }
    Slot* s = &s_files[fi->fileNo];
    u32 size = (u32)s->dir.length * SECTOR;
    if (j->offset < 0 || j->length < 0 || (u32)(j->offset + j->length) > size) {
        return CARD_RESULT_LIMIT;
    }
    FILE* f = fopen(s->path, write ? "r+b" : "rb");
    if (f == NULL) {
        return CARD_RESULT_IOERROR;
    }
    int ok = fseek(f, 64 + j->offset, SEEK_SET) == 0;
    if (ok) {
        ok = write ? fwrite(j->buf, 1, (size_t)j->length, f) == (size_t)j->length
                   : fread(j->buf, 1, (size_t)j->length, f) == (size_t)j->length;
    }
    if (write) {
        fflush(f);
        fsync(fileno(f));
    }
    fclose(f);
    return ok ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
}

static s32 run_job(Job* j) {
    switch (j->kind) {
    case JOB_MOUNT:
        scan();
        s_mounted = 1;
        return CARD_RESULT_READY;
    case JOB_CHECK: return CARD_RESULT_READY;
    case JOB_FORMAT:
        for (int i = 0; i < MAX_FILES; i++) {
            if (s_files[i].used) {
                remove(s_files[i].path);
            }
        }
        memset(s_files, 0, sizeof(s_files));
        return CARD_RESULT_READY;
    case JOB_CREATE: return do_create(j);
    case JOB_READ: return do_rw(j, 0);
    case JOB_WRITE: return do_rw(j, 1);
    case JOB_SETSTATUS: {
        if (j->file_no < 0 || j->file_no >= MAX_FILES || !s_files[j->file_no].used) {
            return CARD_RESULT_NOFILE;
        }
        CARDDir* e = &s_files[j->file_no].dir;
        e->bannerFormat = j->stat.bannerFormat;
        e->iconAddr = j->stat.iconAddr;
        e->iconFormat = j->stat.iconFormat;
        e->iconSpeed = j->stat.iconSpeed;
        e->commentAddr = j->stat.commentAddr;
        e->time = (u32)OSTicksToSeconds(OSGetTime());
        return write_dir(&s_files[j->file_no]);
    }
    }
    return CARD_RESULT_FATAL_ERROR;
}

static void card_thread(void* arg) {
    (void)arg;
    for (;;) {
        plat_event_wait(s_wake);
        for (;;) {
            plat_mutex_lock(s_lock);
            if (s_job_tail == s_job_head) {
                plat_mutex_unlock(s_lock);
                break;
            }
            Job j = s_jobs[s_job_tail % JOBS];
            plat_mutex_unlock(s_lock);
            plat_wd_hold(1);
            s32 r = s_cancel ? CARD_RESULT_CANCELED : run_job(&j);
            plat_wd_hold(0);
            plat_mutex_lock(s_lock);
            s_job_tail++;
            plat_mutex_unlock(s_lock);
            port_irq_enter();
            s_result[j.chan & 1] = r;
            if (j.cb != NULL) {
                j.cb(j.chan, r);
            }
            port_irq_leave();
            port_thread_dispatch_if_idle();
        }
    }
}

static s32 post(const Job* j) {
    if (j->chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    plat_mutex_lock(s_lock);
    if (s_job_head - s_job_tail >= JOBS) {
        plat_mutex_unlock(s_lock);
        return CARD_RESULT_BUSY;
    }
    s_jobs[s_job_head % JOBS] = *j;
    s_job_head++;
    s_result[0] = CARD_RESULT_BUSY;
    s_cancel = 0;
    plat_mutex_unlock(s_lock);
    plat_event_signal(s_wake);
    return CARD_RESULT_READY;
}

/* A card operation is queued or running (for a devupdate relaunch). */
static int card_busy(void) { return s_job_head != s_job_tail; }

/* ---- API ---- */

void CARDInit(void) {
    if (s_thread != NULL) {
        return;
    }
    plat_set_save_busy(card_busy);
    s_lock = plat_mutex_create();
    s_wake = plat_event_create();
    s_thread = plat_thread_create(card_thread, NULL, 32 * 1024, 0x2D, 1, "card");
    if (s_thread == NULL) {
        plat_fatal("CARDInit: no card thread");
    }
}

s32 CARDGetResultCode(s32 chan) { return (chan & ~1) ? CARD_RESULT_FATAL_ERROR : s_result[chan]; }

int CARDProbe(s32 chan) { return chan == 0; }

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    if (memSize) {
        *memSize = 16; /* Mbit: a Memory Card 251 */
    }
    if (sectorSize) {
        *sectorSize = SECTOR;
    }
    return CARD_RESULT_READY;
}

BOOL CARDSetFastMode(BOOL enable) { return enable; }

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback) {
    (void)workArea;
    (void)detachCallback;
    Job j = {.kind = JOB_MOUNT, .chan = chan, .cb = attachCallback};
    return post(&j);
}

s32 CARDUnmount(s32 chan) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    s_mounted = 0;
    return CARD_RESULT_READY;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback) {
    Job j = {.kind = JOB_CHECK, .chan = chan, .cb = callback};
    return post(&j);
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback) {
    Job j = {.kind = JOB_FORMAT, .chan = chan, .cb = callback};
    return post(&j);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    int files = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        files += s_files[i].used;
    }
    if (byteNotUsed) {
        *byteNotUsed = (TOTAL_BLOCKS - used_blocks()) * SECTOR;
    }
    if (filesNotUsed) {
        *filesNotUsed = MAX_FILES - files;
    }
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback) {
    Job j = {.kind = JOB_CREATE, .chan = chan, .cb = callback, .info = fileInfo, .size = size};
    strncpy(j.name, fileName, CARD_FILENAME_MAX);
    return post(&j);
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    int n = find(fileName);
    if (n < 0) {
        return CARD_RESULT_NOFILE;
    }
    fileInfo->chan = chan;
    fileInfo->fileNo = n;
    fileInfo->offset = 0;
    fileInfo->length = (s32)s_files[n].dir.length * SECTOR;
    fileInfo->iBlock = s_files[n].dir.startBlock;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo) {
    fileInfo->chan = -1;
    return CARD_RESULT_READY;
}

s32 CARDDelete(s32 chan, const char* fileName) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    int n = find(fileName);
    if (n < 0) {
        return CARD_RESULT_NOFILE;
    }
    remove(s_files[n].path);
    s_files[n].used = 0;
    return CARD_RESULT_READY;
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback) {
    Job j = {.kind = JOB_READ, .chan = fileInfo->chan, .cb = callback, .info = fileInfo, .buf = buf,
             .length = length, .offset = offset};
    return post(&j);
}

s32 CARDWriteAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback) {
    Job j = {.kind = JOB_WRITE, .chan = fileInfo->chan, .cb = callback, .info = fileInfo, .buf = buf,
             .length = length, .offset = offset};
    return post(&j);
}

s32 CARDCancel(CARDFileInfo* fileInfo) {
    (void)fileInfo;
    s_cancel = 1;
    return CARD_RESULT_READY;
}

/* The SDK's offsets of the banner, icons and data from the entry's formats
 * (CARDStat.c UpdateIconOffsets). */
static void update_icon_offsets(const CARDDir* ent, CARDStat* stat) {
    u32 offset = ent->iconAddr;
    BOOL iconTlut = FALSE;
    if (offset == 0xffffffff) {
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }
    switch (CARDGetBannerFormat(ent)) {
    case CARD_STAT_BANNER_C8:
        stat->offsetBanner = offset;
        offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case CARD_STAT_BANNER_RGB5A3:
        stat->offsetBanner = offset;
        offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
        stat->offsetBannerTlut = 0xffffffff;
        break;
    default:
        stat->offsetBanner = 0xffffffff;
        stat->offsetBannerTlut = 0xffffffff;
        break;
    }
    for (int i = 0; i < CARD_ICON_MAX; ++i) {
        switch (CARDGetIconFormat(ent, i)) {
        case CARD_STAT_ICON_C8:
            stat->offsetIcon[i] = offset;
            offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
            iconTlut = TRUE;
            break;
        case CARD_STAT_ICON_RGB5A3:
            stat->offsetIcon[i] = offset;
            offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
            break;
        default:
            stat->offsetIcon[i] = 0xffffffff;
            break;
        }
    }
    if (iconTlut) {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        stat->offsetIconTlut = 0xffffffff;
    }
    stat->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= MAX_FILES || !s_files[fileNo].used) {
        return CARD_RESULT_NOFILE;
    }
    const CARDDir* ent = &s_files[fileNo].dir;
    memcpy(stat->gameName, ent->gameName, sizeof(stat->gameName));
    memcpy(stat->company, ent->company, sizeof(stat->company));
    stat->length = (u32)ent->length * SECTOR;
    memcpy(stat->fileName, ent->fileName, CARD_FILENAME_MAX);
    stat->time = ent->time;
    stat->bannerFormat = ent->bannerFormat;
    stat->iconAddr = ent->iconAddr;
    stat->iconFormat = ent->iconFormat;
    stat->iconSpeed = ent->iconSpeed;
    stat->commentAddr = ent->commentAddr;
    update_icon_offsets(ent, stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback) {
    Job j = {.kind = JOB_SETSTATUS, .chan = chan, .cb = callback, .file_no = fileNo, .stat = *stat};
    return post(&j);
}

/* A fixed serial: the game keeps it with the save to notice a card swap. */
s32 CARDGetSerialNo(s32 chan, u64* serialNo) {
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    *serialNo = 0x5454594433445331ull; /* "TTYD3DS1" */
    return CARD_RESULT_READY;
}
