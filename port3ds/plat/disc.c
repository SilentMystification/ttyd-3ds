/* Disc image reads (see include/plat/disc.h). */
#include <stdlib.h>
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
static uint64_t s_sd_ticks;
static uint64_t s_size;

/* Switch discra=KB: a read-ahead window on the io thread. A small read that begins where the previous read
 * ended (a sequential run of small reads) reads KB kilobytes from there in one call and answers the next
 * reads from memory. Reads over RA_MAX_REQ and non-sequential reads go straight to the card. The image is
 * read-only, so the window never needs invalidating. */
#define RA_MAX_REQ (64u * 1024u)
static uint8_t* s_ra_buf;
static uint32_t s_ra_cap; /* bytes; 0 off */
static uint64_t s_ra_off; /* disc offset of s_ra_buf[0] */
static uint32_t s_ra_len;
static uint64_t s_last_end = ~0ull;
static uint32_t s_ra_hits;

static int32_t read_now(uint64_t offset, void* buf, uint32_t size) {
    if (!s_open) {
        return -1;
    }
    uint32_t got = 0;
    LightLock_Lock(&s_read_lock);
    if (s_ra_cap != 0 && size <= RA_MAX_REQ) {
        if (offset >= s_ra_off && offset + size <= s_ra_off + s_ra_len) {
            memcpy(buf, s_ra_buf + (offset - s_ra_off), size);
            s_ra_hits++;
            s_last_end = offset + size;
            LightLock_Unlock(&s_read_lock);
            return (int32_t)size;
        }
        if (offset == s_last_end || (offset > s_last_end && offset - s_last_end <= 4096)) {
            uint32_t want = s_ra_cap;
            if (offset + want > s_size) {
                want = s_size > offset ? (uint32_t)(s_size - offset) : 0;
            }
            if (want >= size) {
                uint32_t g = 0;
                uint64_t t1 = svcGetSystemTick();
                Result rr = FSFILE_Read(s_file, &g, offset, s_ra_buf, want);
                s_sd_ticks += svcGetSystemTick() - t1;
                s_reads++;
                s_bytes += g;
                if (R_SUCCEEDED(rr) && g >= size) {
                    s_ra_off = offset;
                    s_ra_len = g;
                    memcpy(buf, s_ra_buf, size);
                    s_last_end = offset + size;
                    LightLock_Unlock(&s_read_lock);
                    return (int32_t)size;
                }
                s_ra_len = 0;
            }
        }
    }
    s_last_end = offset + size;
    uint64_t t0 = svcGetSystemTick();
    Result r = FSFILE_Read(s_file, &got, offset, buf, size);
    s_sd_ticks += svcGetSystemTick() - t0;
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

/* Switch discbatch=1: requests queued behind the one being read that continue it on the disc and in memory
 * (offset and buffer both follow on) are read in the same FSFILE_Read, straight into the first buffer. Each
 * request still completes in order with its own result. */
#define BATCH_MAX_BYTES (4u * 1024u * 1024u)
static int s_batch = -1;
static uint32_t s_merged; /* requests that rode along in another's read */

static void io_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("dvd", 16 * 1024);
    if (s_batch < 0) {
        s_batch = plat_switch_has("discbatch");
    }
    for (;;) {
        LightLock_Lock(&s_queue_lock);
        while (s_head == s_tail) {
            LightLock_Unlock(&s_queue_lock);
            LightEvent_Wait(&s_queue_event);
            LightLock_Lock(&s_queue_lock);
        }
        Request req = s_queue[s_tail % QUEUE];
        uint32_t n = 1;
        uint32_t total = req.size;
        if (s_batch) {
            while (s_tail + n != s_head) {
                const Request* nx = &s_queue[(s_tail + n) % QUEUE];
                if (nx->offset != req.offset + total || nx->buf != (uint8_t*)req.buf + total || total + nx->size > BATCH_MAX_BYTES) {
                    break;
                }
                total += nx->size;
                n++;
            }
        }
        LightLock_Unlock(&s_queue_lock);
        plat_sd_note_disc(req.core, req.size);
        if (n == 1) {
            int32_t result = read_now(req.offset, req.buf, req.size);
            LightLock_Lock(&s_queue_lock);
            s_tail++;
            LightLock_Unlock(&s_queue_lock);
            if (req.done != NULL) {
                req.done(req.ctx, result);
            }
            continue;
        }
        int32_t got = read_now(req.offset, req.buf, total);
        s_merged += n - 1;
        uint32_t pos = 0;
        for (uint32_t i = 0; i < n; i++) {
            LightLock_Lock(&s_queue_lock);
            Request r = s_queue[s_tail % QUEUE];
            s_tail++;
            LightLock_Unlock(&s_queue_lock);
            int32_t result = got < 0 ? got : (got > (int32_t)pos ? (got - (int32_t)pos < (int32_t)r.size ? got - (int32_t)pos : (int32_t)r.size) : 0);
            pos += r.size;
            if (r.done != NULL) {
                r.done(r.ctx, result);
            }
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
    s_size = size;
    int ra_kb = plat_switch_int("discra", 0);
    if (ra_kb > 0) {
        s_ra_cap = (uint32_t)ra_kb * 1024u;
        s_ra_buf = malloc(s_ra_cap);
        if (s_ra_buf == NULL) {
            s_ra_cap = 0;
        }
        PLOG_I("disc: read-ahead %u KB %s", (unsigned)ra_kb, s_ra_buf != NULL ? "on" : "no memory");
    }
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

uint64_t plat_disc_sd_ticks(void) { return s_sd_ticks; }
uint32_t plat_disc_cache_hits(void) { return s_ra_hits; }
uint32_t plat_disc_merged(void) { return s_merged; }

void plat_disc_stats(uint32_t* reads, uint64_t* bytes) {
    *reads = s_reads;
    *bytes = s_bytes;
}
