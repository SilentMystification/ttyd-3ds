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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plat/plat.h"
#include "plat/thread.h"
#include "test_mode.h"
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

/* The game's save data is native in memory and big-endian in the file, as on the GameCube, so a GCI moves
 * between the port and Dolphin. The game-specific converter is port_card_convert (save_swap.c): it converts
 * bytes [offset, offset + length) of the file's data in place, to disk order (to_disk 1) or back. */
void port_card_convert(u32 offset, void* buf, u32 length, int to_disk);

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

/* Why a GCI in the card folder is not offered to the game, in words for the log. */
static const char* foreign_region(const u8 game[4], const u8 company[2]) {
    if (memcmp(company, "01", 2) != 0) {
        return NULL;
    }
    if (memcmp(game, "G8ME", 4) == 0) {
        return "US";
    }
    if (memcmp(game, "G8MJ", 4) == 0) {
        return "JP";
    }
    if (memcmp(game, "G8MP", 4) == 0) {
        return "PAL";
    }
    return NULL;
}

/* Reads the folder. The game's lookup key is the entry inside the file (game code, maker code, file name), not the
 * host file name, so a GCI copied in from Dolphin works under any name. A file is skipped with a log line when it is
 * not a usable GCI: too short for its block count, or made by another game or another region's release. */
static void scan(void) {
    memset(s_files, 0, sizeof(s_files));
    mkdir(CARD_DIR, 0777);
    DIR* d = opendir(CARD_DIR);
    if (d == NULL) {
        return;
    }
    u8 game[4], company[2];
    own_ids(game, company);
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
        long size = -1;
        int have = fread(hdr, 1, 64, f) == 64;
        if (have && fseek(f, 0, SEEK_END) == 0) {
            size = ftell(f);
        }
        fclose(f);
        if (!have) {
            PLOG_W("card: %s skipped: shorter than a GCI header", de->d_name);
            continue;
        }
        CARDDir e;
        dir_from_disk(hdr, &e);
        if (memcmp(e.gameName, game, 4) != 0 || memcmp(e.company, company, 2) != 0) {
            const char* region = foreign_region(e.gameName, e.company);
            if (region != NULL) {
                PLOG_W("card: %s skipped: a %s save (%.4s), this build reads %.4s saves", de->d_name, region,
                       (const char*)e.gameName, (const char*)game);
            } else {
                PLOG_I("card: %s skipped: another game's file (%.4s/%.2s)", de->d_name, (const char*)e.gameName,
                       (const char*)e.company);
            }
            continue;
        }
        if (e.length == 0 || size < 64 + (long)e.length * SECTOR) {
            PLOG_W("card: %s skipped: %ld bytes, but its header says %u blocks (%ld bytes)", de->d_name, size,
                   (unsigned)e.length, 64 + (long)e.length * SECTOR);
            continue;
        }
        /* Two files with one name inside (a copy kept under another name): the newer one is the save. */
        int dup = -1;
        for (int i = 0; i < n; i++) {
            if (s_files[i].used && strncmp((const char*)s_files[i].dir.fileName, (const char*)e.fileName,
                                           CARD_FILENAME_MAX) == 0) {
                dup = i;
                break;
            }
        }
        if (dup >= 0) {
            Slot* o = &s_files[dup];
            int newer = e.time > o->dir.time;
            PLOG_W("card: %s and %s hold the same save file; using the %s one", o->path + sizeof(CARD_DIR),
                   de->d_name, newer ? "second (newer)" : "first");
            if (newer) {
                strcpy(o->path, s->path);
                o->dir = e;
            }
            continue;
        }
        s->dir = e;
        s->used = 1;
        PLOG_I("card: %s: \"%.32s\", %u blocks, copy count %u", de->d_name, (const char*)e.fileName,
               (unsigned)e.length, (unsigned)e.copyTimes);
        n++;
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

typedef enum { JOB_MOUNT, JOB_CHECK, JOB_FORMAT, JOB_CREATE, JOB_READ, JOB_WRITE, JOB_SETSTATUS, JOB_REMOVE } JobKind;

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
    char path[96]; /* JOB_REMOVE: the host file to delete */
    u32 post_frame, due; /* determ: the game frame of the post, and the frame the modelled completion lands in */
} Job;

#define JOBS 16
static Job s_jobs[JOBS];
static volatile u32 s_job_head, s_job_tail;
static PlatMutex* s_lock;
static PlatEvent* s_wake;
static PlatThread* s_thread;
static volatile int s_cancel;
static volatile u32 s_exec; /* determ: the next job the card thread runs (completions are delivered in the frame hook) */
static volatile int s_fin[JOBS];
static s32 s_res[JOBS];

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
    if (ok && write) {
        /* convert a copy: the caller's buffer stays native for the game */
        void* disk = malloc(j->length > 0 ? (size_t)j->length : 1);
        ok = disk != NULL;
        if (ok) {
            memcpy(disk, j->buf, (size_t)j->length);
            port_card_convert((u32)j->offset, disk, (u32)j->length, 1);
            ok = fwrite(disk, 1, (size_t)j->length, f) == (size_t)j->length;
            free(disk);
        }
    } else if (ok) {
        ok = fread(j->buf, 1, (size_t)j->length, f) == (size_t)j->length;
        if (ok) {
            port_card_convert((u32)j->offset, j->buf, (u32)j->length, 0);
        }
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
    case JOB_REMOVE:
        remove(j->path);
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

/* ---- switch cardstats: per job, its wall time, the longest game frame while it ran (against the average frame), and the disc reads in between */
#include "plat/disc.h"

static int s_stats = -1;
static uint64_t s_st_t0, s_st_last, s_st_worst, s_st_idle_avg, s_st_dvd_bytes0;
static uint32_t s_st_dvd_reads0, s_st_frames;
static int s_st_busy;

static int stats_on(void) {
    if (s_stats < 0) {
        s_stats = plat_switch_has("cardstats") ? 1 : 0;
    }
    return s_stats;
}

static void stat_post(const Job* j) {
    (void)j;
    if (stats_on() && !s_st_busy) {
        s_st_busy = 1;
        s_st_t0 = plat_ticks();
        s_st_worst = 0;
        s_st_frames = 0;
        plat_disc_stats(&s_st_dvd_reads0, &s_st_dvd_bytes0);
    }
}

static void stat_done(const Job* j, s32 r) {
    if (!stats_on() || !s_st_busy) {
        return;
    }
    uint32_t reads;
    uint64_t bytes;
    plat_disc_stats(&reads, &bytes);
    PLOG_I("[CARDSTAT] job %d result %d wall %u ms, %u frames, worst frame %u us (average frame without a job: %u us), disc reads %u (%u KB)",
           (int)j->kind, (int)r, (unsigned)((plat_ticks() - s_st_t0) * 1000 / PLAT_TICKS_PER_SEC), (unsigned)s_st_frames,
           (unsigned)(s_st_worst * 1000000 / PLAT_TICKS_PER_SEC), (unsigned)(s_st_idle_avg * 1000000 / PLAT_TICKS_PER_SEC),
           (unsigned)(reads - s_st_dvd_reads0), (unsigned)((bytes - s_st_dvd_bytes0) / 1024));
    s_st_busy = 0;
}

/* each frame hook: the time since the last one, kept as the worst of the frames with a job running and of the rest */
static void stat_frame(void) {
    uint64_t now = plat_ticks();
    if (s_st_last != 0) {
        uint64_t dt = now - s_st_last;
        if (s_st_busy) {
            s_st_frames++;
            if (dt > s_st_worst) {
                s_st_worst = dt;
            }
        } else {
            s_st_idle_avg = s_st_idle_avg == 0 ? dt : s_st_idle_avg + (int64_t)(dt - s_st_idle_avg) / 64;
        }
    }
    s_st_last = now;
}

/* ---- determ: modelled completion time ----
 * Under determ a job finishes at a time a GameCube card would take, not when the card thread is done, and the
 * completion callback runs at the start of the game frame it falls in, so card timing is the same every run
 * and matches the GameCube debug DOL under Dolphin. The durations D are in 40.5 MHz ticks, fitted to that DOL
 * (determ, padfile pad3, an empty card: mount, check, 18 reads, create, two writes, set-status):
 *   mount 3204170, check 15406, read 39790 per 512-byte sector, create 401173 per 8 KB block of the file
 *   (17 blocks: 6.82M), set-status 3407969,
 *   write 3.26M per 8 KB block plus 0.51M per 8 KB block of the start offset (two points only: 73728 bytes at
 *   offset 0 took 29.34M, 65536 at 73728 took 30.66M).
 * Format is a guess (as create). The game posts a job near the end of the frame's wait, about 4575 ticks before
 * the frame counter moves on; a job of at least 50000 ticks therefore completes in the frame
 *     post + 1 + (D - 4575) / retrace,
 * a shorter one (check) in the frame of its post. */
#define MODEL_POST_EARLY_TICKS 4575
#define MODEL_SECTOR_TICKS 39790
#define MODEL_MOUNT_TICKS 3204170
#define MODEL_SMALL_TICKS 15406
#define MODEL_CREATE_BLOCK_TICKS 401173
#define MODEL_SETSTATUS_TICKS 3407969
#define MODEL_WRITE_BLOCK_TICKS 3260000
#define MODEL_WRITE_OFFSET_TICKS 509000
#define MODEL_BLOCK 8192

static int determ_on(void) {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = tst_determ() ? 1 : 0;
    }
    return s_on;
}

static u32 model_due(const Job* j) {
    s64 d;
    if (j->kind == JOB_REMOVE) {
        return j->post_frame;
    }
    switch (j->kind) {
    case JOB_MOUNT: d = MODEL_MOUNT_TICKS; break;
    case JOB_FORMAT: d = (s64)17 * MODEL_CREATE_BLOCK_TICKS; break;
    case JOB_READ: d = (s64)((j->length + 511) / 512) * MODEL_SECTOR_TICKS; break;
    case JOB_WRITE:
        d = (s64)((j->length + MODEL_BLOCK - 1) / MODEL_BLOCK) * MODEL_WRITE_BLOCK_TICKS +
            (s64)(j->offset / MODEL_BLOCK) * MODEL_WRITE_OFFSET_TICKS;
        break;
    case JOB_CREATE: d = (s64)((j->size + MODEL_BLOCK - 1) / MODEL_BLOCK) * MODEL_CREATE_BLOCK_TICKS; break;
    case JOB_SETSTATUS: d = MODEL_SETSTATUS_TICKS; break;
    default: d = MODEL_SMALL_TICKS; break;
    }
    if (d < 50000) {
        return j->post_frame;
    }
    return j->post_frame + 1 + (u32)((d - MODEL_POST_EARLY_TICKS) / (s64)tst_retrace_ticks());
}

static void card_complete(const Job* j, s32 r, u32 frame_label) {
    if (j->kind == JOB_REMOVE) {
        return;
    }
    stat_done(j, r);
    if (tst_rngtrace_wanted()) {
        tst_log("[CARD] job %d done result %d at game frame %u len=%d off=%d", (int)j->kind, (int)r,
                (unsigned)frame_label, (int)(j->kind == JOB_CREATE ? (s32)j->size : j->length), (int)j->offset);
    }
    port_irq_enter();
    s_result[j->chan & 1] = r;
    if (j->cb != NULL) {
        j->cb(j->chan, r);
    }
    port_irq_leave();
    port_thread_dispatch_if_idle();
}

/* Called from the frame hook (game thread, after the frame counter moved to `frame`): delivers the completions
 * of the jobs whose modelled time has come, waiting for the card thread if it is not done with one yet. A
 * completion in the frame of its post is seen by the next frame's logic, so it is delivered then. */
void port_card_frame(u32 frame) {
    if (stats_on()) {
        stat_frame();
    }
    if (!determ_on() || s_lock == NULL) { /* no lock before CARDInit */
        return;
    }
    for (;;) {
        plat_mutex_lock(s_lock);
        if (s_job_tail == s_job_head) {
            plat_mutex_unlock(s_lock);
            return;
        }
        u32 idx = s_job_tail % JOBS;
        Job j = s_jobs[idx];
        plat_mutex_unlock(s_lock);
        u32 deadline = j.due > j.post_frame ? j.due : j.post_frame + 1;
        if (frame < deadline) {
            return;
        }
        while (!s_fin[idx]) {
            plat_thread_yield();
        }
        plat_mutex_lock(s_lock);
        s_job_tail++;
        s_fin[idx] = 0;
        s32 r = s_res[idx];
        plat_mutex_unlock(s_lock);
        card_complete(&j, r, j.due);
    }
}

static void card_thread(void* arg) {
    (void)arg;
    for (;;) {
        plat_event_wait(s_wake);
        if (determ_on()) {
            for (;;) {
                plat_mutex_lock(s_lock);
                if (s_exec == s_job_head) {
                    plat_mutex_unlock(s_lock);
                    break;
                }
                u32 idx = s_exec % JOBS;
                Job j = s_jobs[idx];
                plat_mutex_unlock(s_lock);
                plat_wd_hold(1);
                s32 r = s_cancel && j.kind != JOB_REMOVE ? CARD_RESULT_CANCELED : run_job(&j);
                plat_wd_hold(0);
                plat_mutex_lock(s_lock);
                s_res[idx] = r;
                s_fin[idx] = 1;
                s_exec++;
                plat_mutex_unlock(s_lock);
            }
            continue;
        }
        for (;;) {
            plat_mutex_lock(s_lock);
            if (s_job_tail == s_job_head) {
                plat_mutex_unlock(s_lock);
                break;
            }
            Job j = s_jobs[s_job_tail % JOBS];
            plat_mutex_unlock(s_lock);
            plat_wd_hold(1);
            s32 r = s_cancel && j.kind != JOB_REMOVE ? CARD_RESULT_CANCELED : run_job(&j);
            plat_wd_hold(0);
            plat_mutex_lock(s_lock);
            s_job_tail++;
            plat_mutex_unlock(s_lock);
            if (j.kind == JOB_REMOVE) {
                continue;
            }
            stat_done(&j, r);
            if (tst_rngtrace_wanted()) {
                tst_log("[CARD] job %d done result %d at game frame %u len=%d off=%d", (int)j.kind, (int)r,
                        (unsigned)tst_cur_frame(), (int)(j.kind == JOB_CREATE ? (s32)j.size : j.length), (int)j.offset);
            }
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
    if (j->kind != JOB_REMOVE && tst_rngtrace_wanted()) {
        tst_log("[CARD] job %d posted at game frame %u len=%d off=%d", (int)j->kind, (unsigned)tst_cur_frame(),
                (int)(j->kind == JOB_CREATE ? (s32)j->size : j->length), (int)j->offset);
    }
    if (j->kind != JOB_REMOVE) {
        stat_post(j);
    }
    s_jobs[s_job_head % JOBS] = *j;
    s_jobs[s_job_head % JOBS].post_frame = tst_cur_frame();
    s_jobs[s_job_head % JOBS].due = model_due(&s_jobs[s_job_head % JOBS]);
    s_job_head++;
    if (j->kind != JOB_REMOVE) {
        s_result[0] = CARD_RESULT_BUSY;
        s_cancel = 0;
    }
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
    /* the bookkeeping is immediate (the SDK's CARDDelete is synchronous); the file goes on the card thread,
     * ordered before any job posted later */
    Job j = {.kind = JOB_REMOVE, .chan = chan};
    strncpy(j.path, s_files[n].path, sizeof(j.path) - 1);
    s_files[n].used = 0;
    if (post(&j) != CARD_RESULT_READY) {
        remove(j.path); /* queue full: rare, and a stall is better than a stale file */
    }
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
