/* Non-blocking log for the TTYD 3DS port.
 *
 * Ported from the Paper Mario 3DS port (3ds/src/pm_log.c), keeping what the
 * hardware runs there taught:
 *   - Producers (game, render, audio threads) only format a line and copy it
 *     into a slot of a lock-free queue. They make no SD, socket or console
 *     call. A full queue drops the new line and counts it.
 *   - One writer thread at the lowest priority on core 1 writes log.txt,
 *     the replay ring, the live socket and the bottom console, and fsyncs
 *     about once a second. Before it runs, lines are written directly.
 *   - The console is skipped for a batch when more than 128 lines wait (it
 *     is the slowest output and log.txt has every line).
 *   - Crash path: the queue is written in the crashing thread, the drain lock
 *     is kept so the writer never runs again, later lines go direct.
 * Every line starts with #<sequence>; a gap means lost lines.
 */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "plat_internal.h"

#define LIVE_PORT 17492
#define RING_BYTES 32768
#define SLOTS 1024
#define SLOT_TEXT 256
#define SOC_BUFFER_BYTES 0x100000
#define LIVE_STALL_TICKS (3ull * PLAT_TICKS_PER_SEC)

typedef struct {
    u32 state; /* 0 free, 1 committed */
    u16 len;
    u16 pad;
    char text[SLOT_TEXT];
} Slot;

static Slot* s_slots; /* placed with PLAT_PLACE_LOG once memplace is up */
static u32 s_head, s_tail, s_seq;
static u32 s_dropped, s_drop_total;
static int s_level = PLAT_LOG_INFO;
static int s_log_fd = -1;
static int s_writer_on, s_writer_stop, s_crashed, s_dirty;
static Thread s_writer;
static LightLock s_drain_lock, s_console_lock;
static LightEvent s_wake, s_flushed;

static char s_ring[RING_BYTES];
static unsigned s_ring_pos, s_ring_len;
static int s_listen_fd = -1, s_client = -1, s_soc_up;
static u32* s_soc_buffer;
static u64 s_live_ok_tick;

static char s_console_pending[4096];
static unsigned s_console_pending_n;
static int s_console_up;

void plat_console_lock(void) { LightLock_Lock(&s_console_lock); }
int plat_console_trylock(void) { return LightLock_TryLock(&s_console_lock) == 0; }
void plat_console_unlock(void) { LightLock_Unlock(&s_console_lock); }
int plat_log_level(void) { return s_level; }
void plat_log_set_level(int level) { s_level = level < 0 ? 0 : level > 3 ? 3 : level; }
unsigned plat_log_dropped(void) { return s_drop_total + s_dropped; }

static void ring_add(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        s_ring[s_ring_pos++ % RING_BYTES] = s[i];
    }
    s_ring_len = s_ring_len + (unsigned)n < RING_BYTES ? s_ring_len + (unsigned)n : RING_BYTES;
}

static void ring_replay(int fd) {
    unsigned start = s_ring_len < RING_BYTES ? 0 : s_ring_pos % RING_BYTES;
    for (unsigned off = 0; off < s_ring_len;) {
        unsigned idx = (start + off) % RING_BYTES;
        unsigned chunk = RING_BYTES - idx;
        if (chunk > s_ring_len - off) {
            chunk = s_ring_len - off;
        }
        if (send(fd, s_ring + idx, chunk, 0) < 0) {
            return;
        }
        off += chunk;
    }
}

static void accept_client(void) {
    if (s_listen_fd < 0) {
        return;
    }
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(s_listen_fd, (struct sockaddr*)&addr, &len);
    if (fd < 0) {
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    if (s_client >= 0) {
        close(s_client);
    }
    s_client = fd;
    s_live_ok_tick = 0;
    ring_replay(fd);
}

void plat_log_poll(void) {
    if (!__atomic_load_n(&s_writer_on, __ATOMIC_ACQUIRE)) {
        accept_client();
    }
}

static void console_write(const char* body, size_t n) {
    if (!s_console_up) {
        if (s_console_pending_n + n < sizeof(s_console_pending)) {
            memcpy(s_console_pending + s_console_pending_n, body, n);
            s_console_pending_n += (unsigned)n;
        }
        return;
    }
    fwrite(body, 1, n, stdout);
    fflush(stdout);
}

void plat_log_console_ready(void) {
    plat_console_lock();
    s_console_up = 1;
    if (s_console_pending_n) {
        fwrite(s_console_pending, 1, s_console_pending_n, stdout);
        fflush(stdout);
        s_console_pending_n = 0;
    }
    plat_console_unlock();
}

void plat_log_init(int live) {
    LightLock_Init(&s_console_lock);
    LightLock_Init(&s_drain_lock);
    LightEvent_Init(&s_wake, RESET_ONESHOT);
    LightEvent_Init(&s_flushed, RESET_ONESHOT);

    /* Keep the previous run: a crash trace must survive the next boot. */
    unlink("log.old");
    rename("log.txt", "log.old");
    s_log_fd = open("log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);

    char lv[16];
    if (plat_switch_value("loglevel", lv, sizeof(lv))) {
        s_level = lv[0] >= '0' && lv[0] <= '3' ? lv[0] - '0'
                : lv[0] == 'e' ? PLAT_LOG_ERROR
                : lv[0] == 'w' ? PLAT_LOG_WARN
                : lv[0] == 'd' ? PLAT_LOG_DEBUG
                : PLAT_LOG_INFO;
    }

    if (!live) {
        return;
    }
    /* socInit needs a page-aligned heap buffer (not linear memory). */
    s_soc_buffer = memalign(0x1000, SOC_BUFFER_BYTES);
    Result rc = s_soc_buffer ? socInit(s_soc_buffer, SOC_BUFFER_BYTES) : -1;
    if (R_FAILED(rc)) {
        PLOG_W("live log: off (socInit %08lx)", (unsigned long)rc);
        free(s_soc_buffer);
        s_soc_buffer = NULL;
        return;
    }
    s_soc_up = 1;
    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        PLOG_E("live log: socket failed errno %d", errno);
        return;
    }
    int yes = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LIVE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(s_listen_fd, 1) < 0) {
        PLOG_E("live log: bind/listen on %d failed errno %d", LIVE_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return;
    }
    fcntl(s_listen_fd, F_SETFL, O_NONBLOCK);
    PLOG_W("live log: port %d", LIVE_PORT);
}

/* Before the writer: write the line now (boot lines, and every line after a
 * crash). */
static void direct_write(const char* line, int n, const char* body, size_t bl) {
    if (s_log_fd >= 0 && write(s_log_fd, line, (size_t)n) > 0) {
        fsync(s_log_fd);
    }
    ring_add(line, n);
    if (s_client >= 0 && send(s_client, line, (size_t)n, 0) < 0) {
        close(s_client);
        s_client = -1;
    }
    if (plat_console_trylock()) {
        console_write(body, bl);
        plat_console_unlock();
    }
}

void plat_vlog(int level, const char* fmt, va_list ap) {
    if (level > s_level) {
        return;
    }
    int on = __atomic_load_n(&s_writer_on, __ATOMIC_ACQUIRE);
    if (on && __atomic_load_n(&s_head, __ATOMIC_RELAXED) - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) >= SLOTS) {
        __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    char body[SLOT_TEXT];
    vsnprintf(body, sizeof(body), fmt, ap);
    size_t bl = strlen(body);
    if ((bl == 0 || body[bl - 1] != '\n') && bl + 1 < sizeof(body)) {
        body[bl++] = '\n';
        body[bl] = '\0';
    }
    if (!on) {
        char line[SLOT_TEXT + 16];
        int n = snprintf(line, sizeof(line), "#%lu %s", (unsigned long)__atomic_fetch_add(&s_seq, 1, __ATOMIC_RELAXED), body);
        if (n > 0) {
            direct_write(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1, body, bl);
        }
        return;
    }
    u32 h = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    for (;;) {
        if (h - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) >= SLOTS) {
            __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
            return;
        }
        if (__atomic_compare_exchange_n(&s_head, &h, h + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }
    Slot* sl = &s_slots[h % SLOTS];
    int n = snprintf(sl->text, SLOT_TEXT, "#%lu %s", (unsigned long)__atomic_fetch_add(&s_seq, 1, __ATOMIC_RELAXED), body);
    if (n < 0) {
        n = 0;
    }
    if (n >= SLOT_TEXT) {
        n = SLOT_TEXT - 1;
        sl->text[n - 1] = '\n';
    }
    sl->len = (u16)n;
    __atomic_store_n(&sl->state, 1, __ATOMIC_RELEASE);
    u32 t = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
    if (h == t || h - t == SLOTS / 8) {
        LightEvent_Signal(&s_wake);
    }
}

void plat_log(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    plat_vlog(level, fmt, ap);
    va_end(ap);
}

static void out_batch(const char* lines, int n, const char* bodies, int bn) {
    if (n <= 0) {
        return;
    }
    if (s_log_fd >= 0 && write(s_log_fd, lines, (size_t)n) > 0) {
        s_dirty = 1;
    }
    ring_add(lines, n);
    if (s_client >= 0) {
        int r = send(s_client, lines, (size_t)n, 0);
        if (r >= 0) {
            s_live_ok_tick = 0;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(s_client);
            s_client = -1;
        } else {
            /* A reader that stopped: close after 3 s of only EAGAIN; the
             * PC script reconnects and the ring replay fills the gap. */
            u64 now = svcGetSystemTick();
            if (!s_live_ok_tick) {
                s_live_ok_tick = now;
            } else if (now - s_live_ok_tick > LIVE_STALL_TICKS) {
                close(s_client);
                s_client = -1;
                s_live_ok_tick = 0;
            }
        }
    }
    if (__atomic_load_n(&s_head, __ATOMIC_RELAXED) - __atomic_load_n(&s_tail, __ATOMIC_RELAXED) > 128) {
        return;
    }
    /* After a crash the console lock may belong to the dead thread. */
    if (__atomic_load_n(&s_crashed, __ATOMIC_ACQUIRE) ? plat_console_trylock() : (plat_console_lock(), 1)) {
        console_write(bodies, (size_t)bn);
        plat_console_unlock();
    }
}

static void report_drops(void) {
    u32 d = __atomic_exchange_n(&s_dropped, 0, __ATOMIC_RELAXED);
    if (!d) {
        return;
    }
    s_drop_total += d;
    char m[80];
    int n = snprintf(m, sizeof(m), "#%lu log: %lu lines dropped (queue full)\n",
                     (unsigned long)__atomic_fetch_add(&s_seq, 1, __ATOMIC_RELAXED), (unsigned long)d);
    out_batch(m, n, m, n);
}

/* The caller holds s_drain_lock. */
static void drain_locked(void) {
    static char lb[8192], bb[8192];
    int ln = 0, bn = 0;
    u32 t = s_tail;
    for (;;) {
        Slot* sl = &s_slots[t % SLOTS];
        if (__atomic_load_n(&sl->state, __ATOMIC_ACQUIRE) != 1) {
            break;
        }
        int n = sl->len;
        const char* sp = memchr(sl->text, ' ', (size_t)n);
        int skip = sp ? (int)(sp - sl->text) + 1 : 0;
        if (ln + n > (int)sizeof(lb) || bn + n - skip > (int)sizeof(bb)) {
            out_batch(lb, ln, bb, bn);
            ln = bn = 0;
            report_drops();
        }
        memcpy(lb + ln, sl->text, (size_t)n);
        ln += n;
        memcpy(bb + bn, sl->text + skip, (size_t)(n - skip));
        bn += n - skip;
        __atomic_store_n(&sl->state, 0, __ATOMIC_RELEASE);
        t++;
        __atomic_store_n(&s_tail, t, __ATOMIC_RELEASE);
    }
    out_batch(lb, ln, bb, bn);
    report_drops();
}

static void writer_main(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    u64 last_sync = svcGetSystemTick();
    for (;;) {
        LightEvent_WaitTimeout(&s_wake, 250000000LL);
        accept_client();
        LightLock_Lock(&s_drain_lock);
        if (!__atomic_load_n(&s_crashed, __ATOMIC_ACQUIRE)) {
            drain_locked();
        }
        LightLock_Unlock(&s_drain_lock);
        if (__atomic_load_n(&s_crashed, __ATOMIC_ACQUIRE)) {
            LightEvent_Signal(&s_flushed);
        }
        u64 now = svcGetSystemTick();
        int stop = __atomic_load_n(&s_writer_stop, __ATOMIC_ACQUIRE);
        if (s_dirty && s_log_fd >= 0 && (stop || now - last_sync >= PLAT_TICKS_PER_SEC)) {
            fsync(s_log_fd);
            s_dirty = 0;
            last_sync = now;
        }
        if (stop) {
            return;
        }
    }
}

void plat_log_start_writer(void) {
    if (s_writer != NULL) {
        return;
    }
    s_slots = plat_place_alloc(PLAT_PLACE_LOG, sizeof(Slot) * SLOTS, 64);
    if (s_slots == NULL) {
        plat_fatal("log: no memory for the log queue");
    }
    memset(s_slots, 0, sizeof(Slot) * SLOTS);
    /* Core 1 first (needs APT_SetAppCpuTimeLimit), then core 0. */
    s_writer = threadCreate(writer_main, NULL, 32 * 1024, 0x3F, 1, false);
    int core = 1;
    if (s_writer == NULL) {
        s_writer = threadCreate(writer_main, NULL, 32 * 1024, 0x3F, 0, false);
        core = 0;
    }
    if (s_writer == NULL) {
        PLOG_E("log: writer thread not created, the log stays direct");
        return;
    }
    __atomic_store_n(&s_writer_on, 1, __ATOMIC_RELEASE);
    PLOG_I("log: writer thread on core %d", core);
}

void plat_log_stop(void) {
    if (s_writer == NULL) {
        return;
    }
    __atomic_store_n(&s_writer_stop, 1, __ATOMIC_RELEASE);
    LightEvent_Signal(&s_wake);
    threadJoin(s_writer, 2000000000ULL);
    __atomic_store_n(&s_writer_on, 0, __ATOMIC_RELEASE);
    LightLock_Lock(&s_drain_lock);
    drain_locked();
    LightLock_Unlock(&s_drain_lock);
    if (s_log_fd >= 0) {
        fsync(s_log_fd);
    }
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    if (s_soc_up) {
        socExit();
        s_soc_up = 0;
        free(s_soc_buffer);
        s_soc_buffer = NULL;
    }
}

void plat_log_crash_flush(void) {
    if (!__atomic_load_n(&s_writer_on, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&s_crashed, 1, __ATOMIC_RELEASE);
    int got = LightLock_TryLock(&s_drain_lock) == 0;
    if (!got) {
        LightEvent_WaitTimeout(&s_flushed, 500000000LL);
        got = LightLock_TryLock(&s_drain_lock) == 0;
    }
    if (got) {
        drain_locked(); /* the lock stays taken: the writer must not run again */
        if (s_log_fd >= 0) {
            fsync(s_log_fd);
        }
    }
    __atomic_store_n(&s_writer_on, 0, __ATOMIC_RELEASE);
}

void plat_log_crash_close(void) {
    if (s_log_fd >= 0) {
        fsync(s_log_fd);
        close(s_log_fd);
        s_log_fd = -1;
    }
}

void plat_crash_line(const char* s, int n) {
    static int crash_fd = -1;
    static int flushed;
    if (!flushed) {
        flushed = 1;
        plat_log_crash_flush();
    }
    svcOutputDebugString(s, n);
    if (s_log_fd >= 0) {
        write(s_log_fd, s, (size_t)n);
        fsync(s_log_fd);
    }
    /* crash.txt keeps every crash (append), across boots. */
    if (crash_fd < 0) {
        crash_fd = open("crash.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (crash_fd >= 0) {
        write(crash_fd, s, (size_t)n);
        fsync(crash_fd);
    }
    if (s_client >= 0) {
        send(s_client, s, (size_t)n, MSG_DONTWAIT);
    }
    if (plat_console_trylock()) {
        console_write(s, (size_t)n);
        plat_console_unlock();
    }
}
