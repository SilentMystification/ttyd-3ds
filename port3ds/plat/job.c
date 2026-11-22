/* Fork-join jobs on a spare core (see include/plat/job.h).
 *
 * A ring of RING slots. A job gets a sequence number; its slot is
 * seq % RING, and the slot is free again once the job is joined. Each slot
 * moves FREE -> QUEUED (submit) -> RUNNING (whoever claims it, by
 * compare-and-swap: the worker or a joiner that steals it) -> DONE ->
 * FREE (join). The worker walks the sequence numbers in order and claims
 * what is still queued; a joiner claims its own job if the worker has not.
 * Only the claim is contended, so there is no lock around running jobs;
 * submit takes a lock only to hand out the next sequence number.
 *
 * Release stores publish a job (fn, arg) before its QUEUED state and its
 * results before its DONE state; acquire loads read them. */
#include <string.h>

#include "plat/job.h"
#include "plat/thread.h"
#include "plat_internal.h"

#define RING 64u
#define PFOR_CHUNKS 4u
#define DONE_HANDLE 0xFFFFFFFFu
#define DEFAULT_PRIORITY 0x30 /* below the render thread (0x2E) */
#define WORKER_STACK (32 * 1024)

enum { S_FREE = 0, S_QUEUED, S_RUNNING, S_DONE };

typedef struct {
    uint32_t state;
    uint32_t seq;
    PlatJobFn fn;
    void* arg;
} Slot;

static Slot s_ring[RING];
static uint32_t s_next = 1; /* next sequence number (under s_submit_lock) */
static uint32_t s_head = 1; /* sequence numbers before this are published */
static LightLock s_submit_lock;
static LightEvent s_work; /* work was queued */
static LightEvent s_done; /* the worker finished a job */
static PlatThread* s_thread;
static uint32_t s_init_state; /* 0 not started, 1 starting, 2 ready */
static int s_enabled;
static int s_stats_every;
static uint32_t s_stats_frame;
static PlatJobStats s_stats;
static PlatJobStats s_logged;

static inline uint32_t load_acq(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_rel(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline int claim(Slot* sl) {
    uint32_t expect = S_QUEUED;
    return __atomic_compare_exchange_n(&sl->state, &expect, S_RUNNING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline void count32(uint32_t* p) { __atomic_fetch_add(p, 1u, __ATOMIC_RELAXED); }
static inline void count64(uint64_t* p, uint64_t v) { __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }

static inline uint32_t seq_after(uint32_t seq) {
    seq++;
    return seq == 0 || seq == DONE_HANDLE ? 1u : seq;
}

static void run_claimed(Slot* sl) {
    PlatJobFn fn = sl->fn;
    void* arg = sl->arg;
    fn(arg);
    store_rel(&sl->state, S_DONE);
}

static void worker_entry(void* unused) {
    (void)unused;
    uint32_t seq = 1;
    for (;;) {
        if (seq == load_acq(&s_head)) {
            LightEvent_Wait(&s_work);
            continue;
        }
        Slot* sl = &s_ring[seq % RING];
        seq = seq_after(seq);
        /* A job already stolen (or a slot already reused by a later job,
         * which the walk reaches anyway) fails the claim. */
        if (claim(sl)) {
            uint64_t t0 = svcGetSystemTick();
            run_claimed(sl);
            count64(&s_stats.worker_ticks, svcGetSystemTick() - t0);
            count32(&s_stats.worker);
            LightEvent_Signal(&s_done);
        }
    }
}

int plat_job_init(int core, int priority) {
    uint32_t expect = 0;
    if (!__atomic_compare_exchange_n(&s_init_state, &expect, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (load_acq(&s_init_state) != 2) {
            svcSleepThread(100000);
        }
        return s_enabled;
    }
    LightLock_Init(&s_submit_lock);
    LightEvent_Init(&s_work, RESET_ONESHOT);
    LightEvent_Init(&s_done, RESET_ONESHOT);
    s_stats_every = plat_switch_int("jobstats", 0);
    if (core < 0) {
        core = plat_is_new3ds() ? 2 : 1;
    }
    if (priority < 0) {
        priority = DEFAULT_PRIORITY;
    }
    core = plat_switch_int("jobcore", core);
    priority = plat_switch_int("jobprio", priority);
    if (plat_switch_int("jobs", 1) == 0) {
        PLOG_I("jobs: off (jobs=0), every job runs inline");
    } else {
        s_thread = plat_thread_create(worker_entry, NULL, WORKER_STACK, priority, core, "job");
        if (s_thread == NULL) {
            PLOG_W("jobs: no worker on core %d, every job runs inline", core);
        } else {
            s_enabled = 1;
            PLOG_I("jobs: worker on core %d, priority %#x, %u slots", core, priority, RING);
        }
    }
    store_rel(&s_init_state, 2);
    return s_enabled;
}

static void ensure_init(void) {
    if (load_acq(&s_init_state) != 2) {
        plat_job_init(-1, -1);
    }
}

int plat_job_enabled(void) {
    ensure_init();
    return s_enabled;
}

static void maybe_log(void) {
    if (s_stats_every <= 0) {
        return;
    }
    uint32_t frame = plat_vblank_count();
    if (frame - s_stats_frame < (uint32_t)s_stats_every) {
        return;
    }
    s_stats_frame = frame;
    PlatJobStats now;
    plat_job_stats(&now);
    if (memcmp(&now, &s_logged, sizeof(now)) == 0) {
        return;
    }
    const uint64_t tpu = PLAT_TICKS_PER_SEC / 1000000u;
    PLOG_I("[JOBS] frame %lu: queued %lu worker %lu stolen %lu inline %lu, %lu waits %llu us, worker busy %llu us "
           "(since last: +%lu worker +%lu stolen, wait +%llu us)",
           (unsigned long)frame, (unsigned long)now.queued, (unsigned long)now.worker, (unsigned long)now.stolen,
           (unsigned long)now.inline_, (unsigned long)now.waits, (unsigned long long)(now.wait_ticks / tpu),
           (unsigned long long)(now.worker_ticks / tpu), (unsigned long)(now.worker - s_logged.worker),
           (unsigned long)(now.stolen - s_logged.stolen),
           (unsigned long long)((now.wait_ticks - s_logged.wait_ticks) / tpu));
    s_logged = now;
}

PlatJob plat_job_submit(PlatJobFn fn, void* arg) {
    ensure_init();
    maybe_log();
    if (s_enabled) {
        LightLock_Lock(&s_submit_lock);
        uint32_t seq = s_next;
        Slot* sl = &s_ring[seq % RING];
        if (load_acq(&sl->state) == S_FREE) {
            sl->seq = seq;
            sl->fn = fn;
            sl->arg = arg;
            store_rel(&sl->state, S_QUEUED);
            s_next = seq_after(seq);
            store_rel(&s_head, s_next);
            LightLock_Unlock(&s_submit_lock);
            count32(&s_stats.queued);
            LightEvent_Signal(&s_work);
            return seq;
        }
        LightLock_Unlock(&s_submit_lock); /* that slot's job is not joined yet: the ring is full */
    }
    count32(&s_stats.inline_);
    fn(arg);
    return DONE_HANDLE;
}

static Slot* slot_of(PlatJob job) {
    Slot* sl = &s_ring[job % RING];
    if (sl->seq != job || load_acq(&sl->state) == S_FREE) {
        plat_fatal("jobs: join of job %lu, which is not pending (joined twice?)", (unsigned long)job);
    }
    return sl;
}

/* The job is claimed by the worker or done: wait for DONE, then free it. */
static void finish(Slot* sl) {
    if (load_acq(&sl->state) != S_DONE) {
        uint64_t t0 = svcGetSystemTick();
        while (load_acq(&sl->state) != S_DONE) {
            LightEvent_WaitTimeout(&s_done, 1000000); /* 1 ms: a signal for another joiner's job may be taken */
        }
        count64(&s_stats.wait_ticks, svcGetSystemTick() - t0);
        count32(&s_stats.waits);
    }
    store_rel(&sl->state, S_FREE);
}

static void steal(Slot* sl) {
    if (claim(sl)) {
        run_claimed(sl);
        count32(&s_stats.stolen);
    }
}

void plat_job_join(PlatJob job) {
    if (job == DONE_HANDLE || job == 0) {
        return;
    }
    Slot* sl = slot_of(job);
    steal(sl);
    finish(sl);
}

void plat_job_join_all(const PlatJob* jobs, uint32_t n) {
    /* From the back: the worker takes the front first. */
    for (uint32_t i = n; i-- > 0;) {
        if (jobs[i] != DONE_HANDLE && jobs[i] != 0) {
            steal(slot_of(jobs[i]));
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (jobs[i] != DONE_HANDLE && jobs[i] != 0) {
            finish(slot_of(jobs[i]));
        }
    }
}

typedef struct {
    PlatJobRangeFn fn;
    void* arg;
    uint32_t begin, end;
} Chunk;

static void chunk_entry(void* p) {
    Chunk* c = p;
    c->fn(c->arg, c->begin, c->end);
}

void plat_job_parallel_for(PlatJobRangeFn fn, void* arg, uint32_t count, uint32_t min_chunk, uint32_t align) {
    if (count == 0) {
        return;
    }
    if (min_chunk == 0) {
        min_chunk = 1;
    }
    if (align == 0) {
        align = 1;
    }
    if (!plat_job_enabled() || count / 2 < min_chunk) {
        fn(arg, 0, count);
        return;
    }
    uint32_t n = count / min_chunk;
    if (n > PFOR_CHUNKS) {
        n = PFOR_CHUNKS;
    }
    uint32_t per = (count + n - 1) / n;
    per = (per + align - 1) / align * align;
    Chunk chunks[PFOR_CHUNKS];
    PlatJob jobs[PFOR_CHUNKS];
    uint32_t used = 0;
    for (uint32_t b = 0; b < count && used < PFOR_CHUNKS; b += per) {
        chunks[used] = (Chunk){fn, arg, b, count - b > per ? b + per : count};
        used++;
    }
    for (uint32_t i = 1; i < used; i++) {
        jobs[i] = plat_job_submit(chunk_entry, &chunks[i]);
    }
    chunk_entry(&chunks[0]);
    plat_job_join_all(jobs + 1, used - 1);
}

void plat_job_stats(PlatJobStats* out) {
    memset(out, 0, sizeof(*out)); /* the padding too: maybe_log compares with memcmp */
    out->queued = __atomic_load_n(&s_stats.queued, __ATOMIC_RELAXED);
    out->inline_ = __atomic_load_n(&s_stats.inline_, __ATOMIC_RELAXED);
    out->worker = __atomic_load_n(&s_stats.worker, __ATOMIC_RELAXED);
    out->stolen = __atomic_load_n(&s_stats.stolen, __ATOMIC_RELAXED);
    out->waits = __atomic_load_n(&s_stats.waits, __ATOMIC_RELAXED);
    out->wait_ticks = __atomic_load_n(&s_stats.wait_ticks, __ATOMIC_RELAXED);
    out->worker_ticks = __atomic_load_n(&s_stats.worker_ticks, __ATOMIC_RELAXED);
}
