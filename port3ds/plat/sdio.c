/* SD card service: one worker thread on core 1 (see include/plat/sdio.h). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plat/sdio.h"
#include "plat_internal.h"

#define QUEUE 128
#define PATH_MAX_LEN 128
#define MAX_QUEUED_BYTES (16u << 20) /* owned buffers waiting: a poster waits above this */

enum { K_WRITE, K_APPEND, K_WRITEAT, K_REMOVE, K_RENAME, K_MKDIR, K_READ, K_DISC, K_FENCE, K_N };
static const char* const kKindName[K_N] = {"write", "append", "writeat", "remove", "rename", "mkdir", "read", "disc", "fence"};

typedef struct {
    uint8_t kind;
    uint8_t core; /* the requester's core */
    uint8_t own;  /* data is a malloc'd block to free */
    uint32_t flags;
    char path[PATH_MAX_LEN];
    char path2[PATH_MAX_LEN];
    void* data;
    size_t n;
    uint64_t off;
    void** out_data; /* K_READ */
    size_t* out_n;
    LightEvent* done; /* sync requests */
} Req;

static Req* s_queue; /* heap: the executable image must stay under 16 MB (boot.c), and had 14 KB to spare */
static uint32_t s_head, s_tail;
static uint32_t s_qbytes, s_qmax;
static LightLock s_lock;
static LightEvent s_event;
static Thread s_thread;
static int s_started, s_boot_done, s_crashed;
static int s_stats_every;

static uint32_t s_ops[K_N];
static uint32_t s_by_core[4];
static uint64_t s_bytes_w, s_bytes_r;
static uint32_t s_direct[4];       /* SD calls made outside the service, by core (worker's own excluded) */
static uint32_t s_boot_direct[4];  /* the same before the game loop began */
static uint32_t s_viol[4];         /* after boot, on a core other than 1 */
static uint32_t s_viol_total;
static const char* s_viol_what;
static void* s_viol_pc;
static int s_viol_core = -1;

/* ---- Which core a thread runs on ----
 * svcGetProcessorID is right on hardware but returns garbage in Azahar, so the core each thread was
 * created for is recorded (linker --wrap of threadCreate) and preferred; threads made elsewhere (the
 * main thread, libctru's own) fall back to the syscall. */

typedef struct {
    Thread t;
    int core;
} ThreadCore;
#define THREAD_TAB 64
static ThreadCore s_tab[THREAD_TAB];
static uint32_t s_tab_n;

typedef struct {
    ThreadFunc entry;
    void* arg;
    int core;
} Tramp;

Thread __real_threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int affinity, bool detached);

static void tramp_entry(void* p) {
    Tramp t = *(Tramp*)p;
    free(p);
    uint32_t i = __atomic_fetch_add(&s_tab_n, 1, __ATOMIC_RELAXED) % THREAD_TAB;
    s_tab[i].core = t.core;
    __atomic_store_n(&s_tab[i].t, threadGetCurrent(), __ATOMIC_RELEASE);
    t.entry(t.arg);
}

Thread __wrap_threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int affinity, bool detached) {
    Tramp* t = affinity >= 0 && affinity <= 3 ? malloc(sizeof(*t)) : NULL;
    if (t == NULL) {
        return __real_threadCreate(entrypoint, arg, stack_size, prio, affinity, detached);
    }
    t->entry = entrypoint;
    t->arg = arg;
    t->core = affinity;
    Thread th = __real_threadCreate(tramp_entry, t, stack_size, prio, affinity, detached);
    if (th == NULL) {
        free(t);
    }
    return th;
}

int plat_sd_core(void) {
    Thread me = threadGetCurrent();
    if (me != NULL) {
        uint32_t n = __atomic_load_n(&s_tab_n, __ATOMIC_ACQUIRE);
        uint32_t cnt = n < THREAD_TAB ? n : THREAD_TAB;
        for (uint32_t k = 0; k < cnt; k++) { /* newest first: an address can be reused */
            uint32_t i = (n - 1 - k) % THREAD_TAB;
            if (__atomic_load_n(&s_tab[i].t, __ATOMIC_ACQUIRE) == me) {
                return s_tab[i].core;
            }
        }
    }
    return (int)svcGetProcessorID() & 3;
}

/* ---- Direct-call watch (linker --wrap) ---- */

static void note_direct(const char* what, void* pc) {
    if (s_thread != NULL && threadGetCurrent() == s_thread) {
        return;
    }
    int core = plat_sd_core();
    if (!__atomic_load_n(&s_boot_done, __ATOMIC_ACQUIRE)) {
        if (core != 1) {
            __atomic_fetch_add(&s_boot_direct[core], 1, __ATOMIC_RELAXED);
        }
        return;
    }
    __atomic_fetch_add(&s_direct[core], 1, __ATOMIC_RELAXED);
    if (core != 1 && !__atomic_load_n(&s_crashed, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&s_viol[core], 1, __ATOMIC_RELAXED);
        if (__atomic_fetch_add(&s_viol_total, 1, __ATOMIC_RELAXED) == 0) {
            s_viol_what = what;
            s_viol_pc = pc;
            s_viol_core = core;
        }
    }
}

int __real_open(const char* path, int flags, ...);
FILE* __real_fopen(const char* path, const char* mode);
DIR* __real_opendir(const char* path);
int __real_remove(const char* path);
int __real_rename(const char* a, const char* b);
int __real_mkdir(const char* path, mode_t mode);
int __real_unlink(const char* path);
int __real_fsync(int fd);
int __real_stat(const char* path, struct stat* st);

int __wrap_open(const char* path, int flags, ...) {
    note_direct("open", __builtin_return_address(0));
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return __real_open(path, flags, mode);
}
FILE* __wrap_fopen(const char* path, const char* mode) {
    note_direct("fopen", __builtin_return_address(0));
    return __real_fopen(path, mode);
}
DIR* __wrap_opendir(const char* path) {
    note_direct("opendir", __builtin_return_address(0));
    return __real_opendir(path);
}
int __wrap_remove(const char* path) {
    note_direct("remove", __builtin_return_address(0));
    return __real_remove(path);
}
int __wrap_rename(const char* a, const char* b) {
    note_direct("rename", __builtin_return_address(0));
    return __real_rename(a, b);
}
int __wrap_mkdir(const char* path, mode_t mode) {
    note_direct("mkdir", __builtin_return_address(0));
    return __real_mkdir(path, mode);
}
int __wrap_unlink(const char* path) {
    note_direct("unlink", __builtin_return_address(0));
    return __real_unlink(path);
}
int __wrap_fsync(int fd) {
    note_direct("fsync", __builtin_return_address(0));
    return __real_fsync(fd);
}
int __wrap_stat(const char* path, struct stat* st) {
    note_direct("stat", __builtin_return_address(0));
    return __real_stat(path, st);
}

void plat_sd_boot_done(void) { __atomic_store_n(&s_boot_done, 1, __ATOMIC_RELEASE); }
void plat_sd_crashed(void) { __atomic_store_n(&s_crashed, 1, __ATOMIC_RELEASE); }

void plat_sd_note_disc(int req_core, uint32_t bytes) {
    (void)bytes;
    __atomic_fetch_add(&s_ops[K_DISC], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_by_core[req_core & 3], 1, __ATOMIC_RELAXED);
}

/* ---- Execution (on the worker; on the caller before it starts) ---- */

static int write_all(int fd, const void* p, size_t n) {
    const uint8_t* b = p;
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) {
            return -1;
        }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int write_path(const char* path, int oflags, uint64_t off, int seek, const void* data, size_t n, uint32_t flags) {
    int fd = open(path, O_WRONLY | oflags, 0666);
    if (fd < 0) {
        return -1;
    }
    int rc = 0;
    if (seek && lseek(fd, (off_t)off, SEEK_SET) < 0) {
        rc = -1;
    }
    if (rc == 0 && write_all(fd, data, n) != 0) {
        rc = -1;
    }
    if (rc == 0 && (flags & PLAT_SD_FSYNC)) {
        fsync(fd);
    }
    if (close(fd) != 0) {
        rc = -1;
    }
    return rc;
}

static void exec_req(Req* r) {
    int rc = 0;
    switch (r->kind) {
    case K_WRITE:
        if (r->flags & PLAT_SD_ATOMIC) {
            char tmp[PATH_MAX_LEN + 8];
            snprintf(tmp, sizeof(tmp), "%s.tmp", r->path);
            rc = write_path(tmp, O_CREAT | O_TRUNC, 0, 0, r->data, r->n, r->flags);
            if (rc == 0) {
                remove(r->path);
                rc = rename(tmp, r->path);
            } else {
                remove(tmp);
            }
        } else {
            rc = write_path(r->path, O_CREAT | O_TRUNC, 0, 0, r->data, r->n, r->flags);
        }
        break;
    case K_APPEND:
        rc = write_path(r->path, O_CREAT | O_APPEND, 0, 0, r->data, r->n, r->flags);
        break;
    case K_WRITEAT:
        rc = write_path(r->path, O_CREAT, r->off, 1, r->data, r->n, r->flags);
        break;
    case K_REMOVE:
        rc = remove(r->path);
        break;
    case K_RENAME:
        rc = rename(r->path, r->path2);
        break;
    case K_MKDIR:
        rc = mkdir(r->path, 0777);
        if (rc != 0 && access(r->path, F_OK) == 0) {
            rc = 0; /* already there */
        }
        break;
    case K_READ: {
        *r->out_data = NULL;
        *r->out_n = 0;
        int fd = open(r->path, O_RDONLY);
        if (fd < 0) {
            rc = -1;
            break;
        }
        off_t size = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        uint8_t* buf = size >= 0 ? malloc((size_t)size + 1) : NULL;
        size_t got = 0;
        if (buf != NULL) {
            while (got < (size_t)size) {
                ssize_t k = read(fd, buf + got, (size_t)size - got);
                if (k <= 0) {
                    break;
                }
                got += (size_t)k;
            }
            buf[got] = '\0';
            *r->out_data = buf;
            *r->out_n = got;
            __atomic_fetch_add(&s_bytes_r, got, __ATOMIC_RELAXED);
        } else {
            rc = -1;
        }
        close(fd);
        break;
    }
    default:
        break;
    }
    if (r->kind <= K_WRITEAT && rc == 0) {
        __atomic_fetch_add(&s_bytes_w, r->n, __ATOMIC_RELAXED);
    }
    if (rc != 0 && r->kind != K_READ && r->kind != K_FENCE && r->kind != K_MKDIR) {
        PLOG_W("sd: %s %s failed (errno %d)", kKindName[r->kind], r->path, errno);
    }
    __atomic_fetch_add(&s_ops[r->kind], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_by_core[r->core & 3], 1, __ATOMIC_RELAXED);
    if (r->own) {
        free(r->data);
    }
}

/* ---- Stats ---- */

static void stats_tick(void) {
    static uint64_t next;
    static uint32_t prev_ops[K_N], prev_core[4], prev_direct1;
    static uint64_t prev_w, prev_r;
    if (s_stats_every <= 0) {
        return;
    }
    uint64_t now = svcGetSystemTick();
    if (next == 0) {
        next = now + (uint64_t)s_stats_every * PLAT_TICKS_PER_SEC;
        return;
    }
    if (now < next) {
        return;
    }
    next = now + (uint64_t)s_stats_every * PLAT_TICKS_PER_SEC;
    char ops[96];
    int o = 0;
    for (int k = 0; k < K_FENCE; k++) {
        uint32_t v = __atomic_load_n(&s_ops[k], __ATOMIC_RELAXED);
        o += snprintf(ops + o, sizeof(ops) - (size_t)o, "%s%s %lu", k ? " " : "", kKindName[k], (unsigned long)(v - prev_ops[k]));
        prev_ops[k] = v;
    }
    uint32_t c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = __atomic_load_n(&s_by_core[i], __ATOMIC_RELAXED);
    }
    LightLock_Lock(&s_lock);
    uint32_t depth = s_head - s_tail, qmax = s_qmax;
    s_qmax = depth;
    LightLock_Unlock(&s_lock);
    uint64_t w = s_bytes_w, r = s_bytes_r;
    uint32_t d1 = __atomic_load_n(&s_direct[1], __ATOMIC_RELAXED);
    PLOG_I("sdstats %ds: ops [%s] requested by core 0:%lu 1:%lu 2:%lu | queue %lu now, %lu max | bytes written %lu read %lu | "
           "direct SD calls: core 1 %lu, cores 0/2 %lu (total %lu, before the game loop %lu)",
           s_stats_every, ops, (unsigned long)(c[0] - prev_core[0]), (unsigned long)(c[1] - prev_core[1]),
           (unsigned long)(c[2] - prev_core[2]), (unsigned long)depth, (unsigned long)qmax, (unsigned long)(w - prev_w),
           (unsigned long)(r - prev_r), (unsigned long)(d1 - prev_direct1),
           (unsigned long)(__atomic_load_n(&s_viol[0], __ATOMIC_RELAXED) + __atomic_load_n(&s_viol[2], __ATOMIC_RELAXED)),
           (unsigned long)__atomic_load_n(&s_viol_total, __ATOMIC_RELAXED),
           (unsigned long)(s_boot_direct[0] + s_boot_direct[2]));
    if (__atomic_load_n(&s_viol_total, __ATOMIC_RELAXED) != 0) {
        PLOG_W("sdstats: first direct SD call from core %d: %s at pc %p", s_viol_core, s_viol_what, s_viol_pc);
    }
    for (int i = 0; i < 4; i++) {
        prev_core[i] = c[i];
    }
    prev_w = w;
    prev_r = r;
    prev_direct1 = d1;
}

/* ---- The worker and the queue ---- */

static void worker(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("sdio", 32 * 1024);
    for (;;) {
        LightLock_Lock(&s_lock);
        while (s_head == s_tail) {
            LightLock_Unlock(&s_lock);
            LightEvent_WaitTimeout(&s_event, 250000000LL);
            stats_tick();
            LightLock_Lock(&s_lock);
        }
        Req req = s_queue[s_tail % QUEUE];
        LightLock_Unlock(&s_lock);
        exec_req(&req);
        LightLock_Lock(&s_lock);
        s_tail++;
        if (req.kind <= K_WRITEAT) {
            s_qbytes -= (uint32_t)req.n;
        }
        LightLock_Unlock(&s_lock);
        if (req.done != NULL) {
            LightEvent_Signal(req.done);
        }
        stats_tick();
    }
}

int plat_sd_start(void) {
    if (s_started) {
        return 0;
    }
    LightLock_Init(&s_lock);
    LightEvent_Init(&s_event, RESET_ONESHOT);
    s_stats_every = plat_switch_int("sdstats", 0);
    s_queue = calloc(QUEUE, sizeof(Req));
    if (s_queue == NULL) {
        PLOG_E("sd: no memory for the queue; SD writes stay on the calling thread");
        return -1;
    }
    /* Core 1 only: the SD card's file service lives there, so a write on core 0 or 2 would
     * take time from the game or render thread. No fallback to another core. */
    /* Must run after plat_cpu_limit_init (boot.c): a thread on core 1 is refused until the process has its core-1 limit. */
    s_thread = threadCreate(worker, NULL, 32 * 1024, 0x3E, 1, false);
    if (s_thread == NULL) {
        Result pr = plat_thread_refusal(0x3E, 1);
        PLOG_E("sd: worker thread on core 1 failed: result %08lx; SD writes stay on the calling thread",
               (unsigned long)pr);
        return -1;
    }
    s_started = 1;
    atexit(plat_sd_flush); /* registered first, so it runs after every other atexit writer */
    PLOG_I("sd: worker thread on core 1%s", s_stats_every ? ", sdstats on" : "");
    return 0;
}

/* Queues r (copied). A full queue (requests or bytes) makes the poster wait. */
static int post(const Req* r) {
    if (!s_started) {
        Req tmp = *r;
        tmp.done = NULL;
        exec_req(&tmp); /* boot: before the worker exists */
        if (r->done != NULL) {
            LightEvent_Signal(r->done);
        }
        return 0;
    }
    uint32_t add = r->kind <= K_WRITEAT ? (uint32_t)r->n : 0;
    for (;;) {
        LightLock_Lock(&s_lock);
        if (s_head - s_tail < QUEUE && (s_qbytes + add <= MAX_QUEUED_BYTES || s_head == s_tail)) {
            break;
        }
        LightLock_Unlock(&s_lock);
        svcSleepThread(1000000);
    }
    s_queue[s_head % QUEUE] = *r;
    s_queue[s_head % QUEUE].core = (uint8_t)plat_sd_core();
    s_head++;
    s_qbytes += add;
    uint32_t depth = s_head - s_tail;
    if (depth > s_qmax) {
        s_qmax = depth;
    }
    LightLock_Unlock(&s_lock);
    LightEvent_Signal(&s_event);
    return 0;
}

static int set_path(char* dst, const char* src) {
    size_t n = src != NULL ? strlen(src) : PATH_MAX_LEN;
    if (n == 0 || n >= PATH_MAX_LEN) {
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static int post_data(int kind, const char* path, void* data, size_t n, int own, uint64_t off, uint32_t flags) {
    Req r;
    memset(&r, 0, sizeof(r));
    r.kind = (uint8_t)kind;
    r.flags = flags;
    r.off = off;
    r.n = n;
    r.data = data;
    r.own = (uint8_t)own;
    if (set_path(r.path, path) != 0) {
        if (own) {
            free(data);
        }
        return -1;
    }
    return post(&r);
}

static int post_copy(int kind, const char* path, const void* data, size_t n, uint64_t off, uint32_t flags) {
    void* copy = malloc(n > 0 ? n : 1);
    if (copy == NULL) {
        PLOG_W("sd: no memory for %lu bytes to %s", (unsigned long)n, path);
        return -1;
    }
    memcpy(copy, data, n);
    return post_data(kind, path, copy, n, 1, off, flags);
}

int plat_sd_write_file(const char* path, const void* data, size_t n, uint32_t flags) {
    return post_copy(K_WRITE, path, data, n, 0, flags);
}
int plat_sd_write_file_own(const char* path, void* data, size_t n, uint32_t flags) {
    return post_data(K_WRITE, path, data, n, 1, 0, flags);
}
int plat_sd_append(const char* path, const void* data, size_t n) { return post_copy(K_APPEND, path, data, n, 0, 0); }
int plat_sd_write_at(const char* path, uint64_t offset, const void* data, size_t n) {
    return post_copy(K_WRITEAT, path, data, n, offset, 0);
}
int plat_sd_remove(const char* path) { return post_data(K_REMOVE, path, NULL, 0, 0, 0, 0); }
int plat_sd_mkdir(const char* path) { return post_data(K_MKDIR, path, NULL, 0, 0, 0, 0); }
int plat_sd_rename(const char* from, const char* to) {
    Req r;
    memset(&r, 0, sizeof(r));
    r.kind = K_RENAME;
    if (set_path(r.path, from) != 0 || set_path(r.path2, to) != 0) {
        return -1;
    }
    return post(&r);
}

void plat_sd_flush(void) {
    if (!s_started) {
        return;
    }
    LightEvent done;
    LightEvent_Init(&done, RESET_ONESHOT);
    Req r;
    memset(&r, 0, sizeof(r));
    r.kind = K_FENCE;
    r.done = &done;
    post(&r);
    LightEvent_Wait(&done);
}

void* plat_sd_read_file(const char* path, size_t* n) {
    void* data = NULL;
    size_t got = 0;
    LightEvent done;
    LightEvent_Init(&done, RESET_ONESHOT);
    Req r;
    memset(&r, 0, sizeof(r));
    r.kind = K_READ;
    r.out_data = &data;
    r.out_n = &got;
    r.done = &done;
    if (set_path(r.path, path) != 0) {
        return NULL;
    }
    post(&r);
    if (s_started) {
        LightEvent_Wait(&done);
    }
    if (n != NULL) {
        *n = got;
    }
    return data;
}
