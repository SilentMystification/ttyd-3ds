/* Disc image reads (see include/plat/disc.h). */
#include <string.h>

#include "plat/disc.h"
#include "plat_internal.h"

#define QUEUE 64

typedef struct {
    uint64_t offset;
    void* buf;
    uint32_t size;
    PlatDiscDone done;
    void* ctx;
    int core; /* the requester's core, for sdstats */
} Request;

static Handle s_file;
static int s_open;
static LightLock s_read_lock; /* one FSFILE_Read at a time on the handle */
static Request s_queue[QUEUE];
static uint32_t s_head, s_tail;
static LightLock s_queue_lock;
static LightEvent s_queue_event;
static Thread s_thread;
static uint32_t s_reads;
static uint64_t s_bytes;

static int32_t read_now(uint64_t offset, void* buf, uint32_t size) {
    if (!s_open) {
        return -1;
    }
    uint32_t got = 0;
    LightLock_Lock(&s_read_lock);
    Result r = FSFILE_Read(s_file, &got, offset, buf, size);
    s_reads++;
    s_bytes += got;
    LightLock_Unlock(&s_read_lock);
    if (R_FAILED(r)) {
        PLOG_E("disc: read of %lu bytes at %llx failed %08lx", (unsigned long)size, (unsigned long long)offset,
               (unsigned long)r);
        return -2;
    }
    return (int32_t)got;
}

/* Cores 0 and 2 never touch the card: their reads wait for the io thread on core 1 (this blocks
 * the caller, which is the game's own synchronous read by design). Boot-time reads before the
 * thread exists, and reads from core 1, run in place. */
static int on_io_core(void) { return s_thread == NULL || plat_sd_core() == 1; }

typedef struct {
    LightEvent ev;
    int32_t result;
} Wait;

static void wait_done(void* ctx, int32_t result) {
    Wait* w = ctx;
    w->result = result;
    LightEvent_Signal(&w->ev);
}

int32_t plat_disc_read(uint64_t offset, void* buf, uint32_t size) {
    if (!s_open) {
        return -1;
    }
    if (on_io_core()) {
        plat_sd_note_disc(plat_sd_core(), size);
        return read_now(offset, buf, size);
    }
    Wait w;
    LightEvent_Init(&w.ev, RESET_ONESHOT);
    while (plat_disc_read_async(offset, buf, size, wait_done, &w) != 0) {
        svcSleepThread(1000000); /* queue full */
    }
    LightEvent_Wait(&w.ev);
    return w.result;
}

static void io_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("dvd", 16 * 1024);
    for (;;) {
        LightLock_Lock(&s_queue_lock);
        while (s_head == s_tail) {
            LightLock_Unlock(&s_queue_lock);
            LightEvent_Wait(&s_queue_event);
            LightLock_Lock(&s_queue_lock);
        }
        Request req = s_queue[s_tail % QUEUE];
        LightLock_Unlock(&s_queue_lock);
        plat_sd_note_disc(req.core, req.size);
        int32_t result = read_now(req.offset, req.buf, req.size);
        LightLock_Lock(&s_queue_lock);
        s_tail++;
        LightLock_Unlock(&s_queue_lock);
        if (req.done != NULL) {
            req.done(req.ctx, result);
        }
    }
}

int plat_disc_read_async(uint64_t offset, void* buf, uint32_t size, PlatDiscDone done, void* ctx) {
    LightLock_Lock(&s_queue_lock);
    if (s_head - s_tail >= QUEUE) {
        LightLock_Unlock(&s_queue_lock);
        return -1;
    }
    s_queue[s_head % QUEUE] = (Request){offset, buf, size, done, ctx, plat_sd_core()};
    s_head++;
    LightLock_Unlock(&s_queue_lock);
    LightEvent_Signal(&s_queue_event);
    return 0;
}

int plat_disc_open(uint64_t* size_out) {
    if (s_open) {
        if (size_out != NULL) {
            FSFILE_GetSize(s_file, size_out);
        }
        return 0;
    }
    LightLock_Init(&s_read_lock);
    LightLock_Init(&s_queue_lock);
    LightEvent_Init(&s_queue_event, RESET_ONESHOT);
    Result r = FSUSER_OpenFileDirectly(&s_file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                       fsMakePath(PATH_ASCII, PLAT_DISC_PATH), FS_OPEN_READ, 0);
    if (R_FAILED(r)) {
        PLOG_E("disc: cannot open sdmc:%s (%08lx)", PLAT_DISC_PATH, (unsigned long)r);
        return -1;
    }
    uint64_t size = 0;
    FSFILE_GetSize(s_file, &size);
    s_open = 1;
    s_thread = threadCreate(io_thread, NULL, 16 * 1024, 0x2C, 1, false);
    if (s_thread == NULL) {
        plat_fatal("disc: no file thread");
    }
    PLOG_I("disc: sdmc:%s, %llu bytes", PLAT_DISC_PATH, (unsigned long long)size);
    if (size_out != NULL) {
        *size_out = size;
    }
    return 0;
}

void plat_disc_stats(uint32_t* reads, uint64_t* bytes) {
    *reads = s_reads;
    *bytes = s_bytes;
}
