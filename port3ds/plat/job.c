/* Fork-join jobs on spare cores (see include/plat/job.h).
 *
 * Two kinds of work:
 *
 * The ring (plat_job_submit / join): RING slots served by worker 0 (core 2). A job gets a sequence number;
 * its slot is seq % RING, and the slot is free again once the job is joined. Each slot moves FREE ->
 * QUEUED (submit) -> RUNNING (whoever claims it, by compare-and-swap: the worker or a joiner that steals
 * it) -> DONE -> FREE (join). The worker walks the sequence numbers in order and claims what is still
 * queued; a joiner claims its own job if the worker has not. Long background jobs (texasync) live here.
 *
 * Frame batches (plat_job_pfor_dyn): one batch at a time, split into at most 64 chunks of a power-of-two
 * size. Every participant (the caller, worker 0 between ring jobs, worker 1 on core 1 with jobw1=1) takes
 * the next chunk with one atomic add. The caller spins briefly for the last chunks, then waits on an event,
 * then takes over a chunk a preempted worker still holds: it computes it again into the caller's second
 * buffer (alt), and a compare-and-swap on the chunk's state picks the winner. The returned mask names the
 * chunks taken over. A worker that lost a chunk may still be writing it: the next batch waits for it.
 *
 * Release stores publish a job or a batch before its state; acquire loads read them. Each worker runs
 * with the submitter's FPSCR (rounding, flush-to-zero), so its float results equal the caller's.
 *
 * Idle workers spin for a short while on the atomics before they sleep on their own event; a submitter
 * signals only a worker that sleeps, so a busy frame costs no system calls in the fast path. */
#include <string.h>

#include "plat/job.h"
#include "plat/thread.h"
#include "plat_internal.h"

#define RING 64u
#define PFOR_CHUNKS 4u
#define DONE_HANDLE 0xFFFFFFFFu
#define DEFAULT_PRIORITY 0x30  /* below the render thread (0x2E) and the translator (0x2F) on core 2 */
#define DEFAULT_PRIORITY1 0x2F /* core 1: below render 0x2E, vblank, arq, IO and card; above log 0x3F and sdio 0x3E */
#define WORKER_STACK (32 * 1024)
#define NWORKERS 2
#define MAX_CHUNKS 64u

enum { S_FREE = 0, S_QUEUED, S_RUNNING, S_DONE };
/* batch chunk states */
enum { C_FREE = 0, C_WORKER, C_CALLER, C_DONE, C_TAKEN };

typedef struct {
    uint32_t state;
    uint32_t seq;
    uint32_t fpscr;
    PlatJobFn fn;
    void* arg;
} Slot;

typedef struct {
    PlatThread* thread;
    LightEvent wake;
    uint32_t sleeping;  /* 1 while it waits (or is about to wait) on wake */
    uint32_t in_batch;  /* 1 while it may touch the batch */
    uint32_t probe_req; /* jobprobe: 1 asks for an acknowledgement */
    uint64_t probe_ack; /* tick of the acknowledgement */
    int core, prio;
} Worker;

typedef struct {
    PlatJobDynFn fn;
    void* arg;
    uint32_t count, shift, nchunks;
    uint32_t next;     /* next chunk index to take (atomic add) */
    uint32_t done;     /* chunks finished */
    uint32_t waiting;  /* the caller sleeps on s_batch_ev */
    uint32_t fpscr;
    uint32_t state[MAX_CHUNKS];
} Batch;

static Slot s_ring[RING];
static uint32_t s_next = 1; /* next sequence number (under s_submit_lock) */
static uint32_t s_head = 1; /* sequence numbers before this are published */
static LightLock s_submit_lock;
static LightEvent s_done; /* worker 0 finished a ring job */
static Worker s_w[NWORKERS];
static int s_nworkers;
static uint32_t s_init_state; /* 0 not started, 1 starting, 2 ready */
static int s_enabled;
static int s_stats_every;
static uint32_t s_stats_frame;
static PlatJobStats s_stats;
static PlatJobStats s_logged;

static Batch s_batch;
static uint32_t s_batch_live;  /* 1 while a batch is published */
static uint32_t s_batch_owner; /* 1 while a caller runs plat_job_pfor_dyn (one batch at a time) */
static LightEvent s_batch_ev;
static uint64_t s_spin_ticks;     /* caller and worker spin before sleeping */
static uint64_t s_takeover_ticks; /* caller waits this long, then takes over held chunks */
static int s_check;               /* jobcheck: FPSCR asserted in the workers */
static uint32_t s_fpscr_bad;

static inline uint32_t load_acq(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_rel(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint32_t load_sc(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void store_sc(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline int cas(uint32_t* p, uint32_t expect, uint32_t to) {
    return __atomic_compare_exchange_n(p, &expect, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline int claim(Slot* sl) { return cas(&sl->state, S_QUEUED, S_RUNNING); }
static inline void count32(uint32_t* p) { __atomic_fetch_add(p, 1u, __ATOMIC_RELAXED); }
static inline void count64(uint64_t* p, uint64_t v) { __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }

static inline uint32_t fpscr_get(void) {
    uint32_t v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    return v;
}
static inline void fpscr_set(uint32_t v) { __asm__ volatile("vmsr fpscr, %0" : : "r"(v) : "memory"); }

/* Runs fn with the submitter's FPSCR; returns to the worker's own afterwards. */
static inline uint32_t fpscr_enter(uint32_t want) {
    uint32_t own = fpscr_get();
    if (own != want) {
        fpscr_set(want);
    }
    if (s_check && fpscr_get() != want) {
        count32(&s_fpscr_bad);
    }
    return own;
}
static inline void fpscr_leave(uint32_t own, uint32_t want) {
    if (own != want) {
        fpscr_set(own);
    }
}

static inline uint32_t seq_after(uint32_t seq) {
    seq++;
    return seq == 0 || seq == DONE_HANDLE ? 1u : seq;
}

static void run_claimed(Slot* sl, int on_worker) {
    PlatJobFn fn = sl->fn;
    void* arg = sl->arg;
    if (on_worker) {
        uint32_t want = sl->fpscr;
        uint32_t own = fpscr_enter(want);
        fn(arg);
        fpscr_leave(own, want);
    } else {
        fn(arg);
    }
    store_rel(&sl->state, S_DONE);
}

/* ---- batches ---- */

static inline void chunk_range(const Batch* b, uint32_t i, uint32_t* begin, uint32_t* end) {
    *begin = i << b->shift;
    uint32_t e = (i + 1) << b->shift;
    *end = e < b->count ? e : b->count;
}

static void chunk_finished(void) {
    uint32_t d = __atomic_add_fetch(&s_batch.done, 1u, __ATOMIC_SEQ_CST);
    if (d == s_batch.nchunks && load_sc(&s_batch.waiting)) {
        LightEvent_Signal(&s_batch_ev);
    }
}

/* A worker's part of the live batch: chunks until none are left. */
static void worker_batch(Worker* w, int wi) {
    store_sc(&w->in_batch, 1);
    if (load_sc(&s_batch_live) == 0) { /* ended before we came in */
        store_sc(&w->in_batch, 0);
        return;
    }
    Batch* b = &s_batch;
    uint32_t want = b->fpscr;
    uint32_t own = fpscr_enter(want);
    uint64_t t0 = svcGetSystemTick();
    uint32_t got = 0;
    for (;;) {
        uint32_t i = __atomic_fetch_add(&b->next, 1u, __ATOMIC_ACQ_REL);
        if (i >= b->nchunks) {
            break;
        }
        if (!cas(&b->state[i], C_FREE, C_WORKER)) {
            continue; /* the caller took it over already */
        }
        uint32_t lo, hi;
        chunk_range(b, i, &lo, &hi);
        b->fn(b->arg, lo, hi, 0);
        if (cas(&b->state[i], C_WORKER, C_DONE)) {
            got++;
            chunk_finished();
        } /* else taken over: the caller's copy counts */
    }
    fpscr_leave(own, want);
    if (got != 0) {
        count64(&s_stats.worker_ticks, svcGetSystemTick() - t0);
        __atomic_fetch_add(wi == 0 ? &s_stats.chunks_w0 : &s_stats.chunks_w1, got, __ATOMIC_RELAXED);
    }
    store_sc(&w->in_batch, 0);
}

/* ---- workers ---- */

static int ring_pending(uint32_t seq) { return seq != load_acq(&s_head); }

static void worker_entry(void* p) {
    int wi = (int)(intptr_t)p;
    Worker* w = &s_w[wi];
    uint32_t seq = 1; /* ring walk (worker 0) */
    for (;;) {
        if (load_acq(&w->probe_req)) {
            __atomic_store_n(&w->probe_ack, svcGetSystemTick(), __ATOMIC_RELEASE);
            store_rel(&w->probe_req, 0);
        }
        if (load_sc(&s_batch_live)) {
            worker_batch(w, wi);
            if (load_acq(&s_batch.next) < s_batch.nchunks) {
                continue;
            }
        }
        if (wi == 0 && ring_pending(seq)) {
            Slot* sl = &s_ring[seq % RING];
            seq = seq_after(seq);
            /* A job already stolen (or a slot already reused by a later job, which the walk reaches
             * anyway) fails the claim. */
            if (claim(sl)) {
                uint64_t t0 = svcGetSystemTick();
                run_claimed(sl, 1);
                count64(&s_stats.worker_ticks, svcGetSystemTick() - t0);
                count32(&s_stats.worker);
                LightEvent_Signal(&s_done);
            }
            continue;
        }
        /* Nothing to do: spin a little on the atomics, then sleep. */
        uint64_t t0 = svcGetSystemTick();
        int found = 0;
        while (svcGetSystemTick() - t0 < s_spin_ticks) {
            if ((load_sc(&s_batch_live) && load_acq(&s_batch.next) < s_batch.nchunks) || (wi == 0 && ring_pending(seq)) ||
                load_acq(&w->probe_req)) {
                found = 1;
                break;
            }
        }
        if (found) {
            continue;
        }
        store_sc(&w->sleeping, 1);
        if ((load_sc(&s_batch_live) && load_acq(&s_batch.next) < s_batch.nchunks) || (wi == 0 && ring_pending(seq)) ||
            load_acq(&w->probe_req)) {
            store_sc(&w->sleeping, 0);
            continue;
        }
        LightEvent_Wait(&w->wake);
        store_sc(&w->sleeping, 0);
    }
}

static void wake(Worker* w) {
    if (w->thread != NULL && load_sc(&w->sleeping)) {
        LightEvent_Signal(&w->wake);
    }
}

static int start_worker(int wi, int core, int priority) {
    Worker* w = &s_w[wi];
    LightEvent_Init(&w->wake, RESET_ONESHOT);
    w->core = core;
    w->prio = priority;
    w->thread = plat_thread_create(worker_entry, (void*)(intptr_t)wi, WORKER_STACK, priority, core, wi == 0 ? "job" : "job1");
    if (w->thread == NULL) {
        PLOG_W("jobs: no worker %d on core %d", wi, core);
        return 0;
    }
    PLOG_I("jobs: worker %d on core %d, priority %#x%s", wi, core, priority, wi == 0 ? " (ring and batches)" : " (batches)");
    return 1;
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
    LightEvent_Init(&s_done, RESET_ONESHOT);
    LightEvent_Init(&s_batch_ev, RESET_ONESHOT);
    s_stats_every = plat_switch_int("jobstats", 0);
    s_check = plat_switch_int("jobcheck", 0) != 0;
    s_spin_ticks = (uint64_t)plat_switch_int("jobspin", 30) * (PLAT_TICKS_PER_SEC / 1000000u);
    s_takeover_ticks = (uint64_t)plat_switch_int("jobtakeover", 300) * (PLAT_TICKS_PER_SEC / 1000000u);
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
    } else if (start_worker(0, core, priority)) {
        s_enabled = 1;
        s_nworkers = 1;
        PLOG_I("jobs: %u ring slots, batch spin %d us, takeover after %d us", RING, plat_switch_int("jobspin", 30),
               plat_switch_int("jobtakeover", 300));
        if (plat_switch_int("jobw1", 0) != 0) {
            int c1 = plat_switch_int("jobcore1", 1);
            int p1 = plat_switch_int("jobprio1", DEFAULT_PRIORITY1);
            if (start_worker(1, c1, p1)) {
                s_nworkers = 2;
            }
        }
    } else {
        PLOG_W("jobs: every job runs inline");
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

int plat_job_workers(void) {
    ensure_init();
    return s_nworkers;
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
    if (now.batches != s_logged.batches) {
        PLOG_I("[JOBS]   batches +%lu (inline +%lu): chunks caller +%lu worker0 +%lu worker1 +%lu, taken over +%lu, "
               "caller spun +%llu us slept +%llu us, stale waits +%lu, fpscr mismatches %lu",
               (unsigned long)(now.batches - s_logged.batches), (unsigned long)(now.batches_inline - s_logged.batches_inline),
               (unsigned long)(now.chunks_caller - s_logged.chunks_caller), (unsigned long)(now.chunks_w0 - s_logged.chunks_w0),
               (unsigned long)(now.chunks_w1 - s_logged.chunks_w1), (unsigned long)(now.takeovers - s_logged.takeovers),
               (unsigned long long)((now.spin_ticks - s_logged.spin_ticks) / tpu),
               (unsigned long long)((now.sleep_ticks - s_logged.sleep_ticks) / tpu),
               (unsigned long)(now.stale_waits - s_logged.stale_waits), (unsigned long)load_acq(&s_fpscr_bad));
    }
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
            sl->fpscr = fpscr_get();
            store_rel(&sl->state, S_QUEUED);
            s_next = seq_after(seq);
            store_sc(&s_head, s_next);
            LightLock_Unlock(&s_submit_lock);
            count32(&s_stats.queued);
            wake(&s_w[0]);
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

/* The job is claimed by the worker or done: spin briefly, then wait for DONE, then free it. */
static void finish(Slot* sl) {
    if (load_acq(&sl->state) != S_DONE) {
        uint64_t t0 = svcGetSystemTick();
        while (load_acq(&sl->state) != S_DONE && svcGetSystemTick() - t0 < s_spin_ticks) {
        }
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
        run_claimed(sl, 0);
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

/* A worker that lost a chunk of the previous batch may still be inside it: wait until none is. */
static void quiesce(void) {
    for (int wi = 0; wi < s_nworkers; wi++) {
        if (load_sc(&s_w[wi].in_batch)) {
            /* usually a worker that is just leaving (it saw the batch end): spin; a worker preempted inside a
             * chunk it lost takes longer: sleep */
            uint64_t t0 = svcGetSystemTick();
            while (load_sc(&s_w[wi].in_batch) && svcGetSystemTick() - t0 < s_spin_ticks) {
            }
            if (load_sc(&s_w[wi].in_batch)) {
                count32(&s_stats.stale_waits);
                while (load_sc(&s_w[wi].in_batch)) {
                    svcSleepThread(20000);
                }
            }
        }
    }
}

void plat_job_pfor_quiesce(void) {
    if (s_enabled) {
        quiesce();
    }
}

uint64_t plat_job_pfor_dyn(PlatJobDynFn fn, void* arg, uint32_t count, uint32_t grain, uint32_t* shift_out) {
    uint32_t shift = 0;
    while ((1u << shift) < grain && shift < 31) {
        shift++;
    }
    while (shift < 31 && ((count + (1u << shift) - 1) >> shift) > MAX_CHUNKS) {
        shift++;
    }
    if (shift_out != NULL) {
        *shift_out = shift;
    }
    if (count == 0) {
        return 0;
    }
    uint32_t nchunks = (count + (1u << shift) - 1) >> shift;
    ensure_init();
    maybe_log();
    if (!s_enabled || nchunks < 2 || !cas(&s_batch_owner, 0, 1)) {
        count32(&s_stats.batches_inline);
        fn(arg, 0, count, 0);
        return 0;
    }
    count32(&s_stats.batches);
    quiesce();
    Batch* b = &s_batch;
    b->fn = fn;
    b->arg = arg;
    b->count = count;
    b->shift = shift;
    b->nchunks = nchunks;
    b->done = 0;
    b->waiting = 0;
    b->fpscr = fpscr_get();
    memset(b->state, 0, nchunks * sizeof(b->state[0]));
    store_sc(&b->next, 0);
    store_sc(&s_batch_live, 1);
    for (int wi = 0; wi < s_nworkers; wi++) {
        wake(&s_w[wi]);
    }
    /* the caller's part */
    uint32_t mine = 0;
    for (;;) {
        uint32_t i = __atomic_fetch_add(&b->next, 1u, __ATOMIC_ACQ_REL);
        if (i >= nchunks) {
            break;
        }
        if (!cas(&b->state[i], C_FREE, C_CALLER)) {
            continue;
        }
        uint32_t lo, hi;
        chunk_range(b, i, &lo, &hi);
        fn(arg, lo, hi, 0);
        store_rel(&b->state[i], C_DONE);
        mine++;
        chunk_finished();
    }
    __atomic_fetch_add(&s_stats.chunks_caller, mine, __ATOMIC_RELAXED);
    uint64_t mask = 0;
    if (load_sc(&b->done) != nchunks) {
        uint64_t t0 = svcGetSystemTick();
        while (load_sc(&b->done) != nchunks && svcGetSystemTick() - t0 < s_spin_ticks) {
        }
        uint64_t t1 = svcGetSystemTick();
        count64(&s_stats.spin_ticks, t1 - t0);
        if (load_sc(&b->done) != nchunks) {
            store_sc(&b->waiting, 1);
            while (load_sc(&b->done) != nchunks) {
                uint64_t now = svcGetSystemTick();
                if (now - t1 >= s_takeover_ticks) {
                    break;
                }
                LightEvent_WaitTimeout(&s_batch_ev, (int64_t)((s_takeover_ticks - (now - t1)) * 1000000000ull / PLAT_TICKS_PER_SEC));
            }
            store_sc(&b->waiting, 0);
            count64(&s_stats.sleep_ticks, svcGetSystemTick() - t1);
        }
        if (load_sc(&b->done) != nchunks) {
            /* Take over what a worker holds (claimed, or about to be). Afterwards every chunk is C_DONE (its
             * results released by the worker's CAS, read here with acquire), ours (C_CALLER) or C_TAKEN and
             * computed here: the batch is complete whatever the done count says (a worker preempted between
             * its CAS and its count adds it later; the next batch waits for it in quiesce). */
            for (uint32_t i = 0; i < nchunks; i++) {
                uint32_t st = load_acq(&b->state[i]);
                while ((st == C_FREE || st == C_WORKER) && !cas(&b->state[i], st, C_TAKEN)) {
                    st = load_acq(&b->state[i]); /* the worker moved it on (claimed, or done) */
                }
                if (st == C_FREE || st == C_WORKER) {
                    uint32_t lo, hi;
                    chunk_range(b, i, &lo, &hi);
                    fn(arg, lo, hi, 1);
                    mask |= 1ull << i;
                    count32(&s_stats.takeovers);
                }
            }
        }
    }
    store_sc(&s_batch_live, 0);
    store_rel(&s_batch_owner, 0);
    return mask;
}

int plat_job_probe(int worker, uint64_t* ticks) {
    ensure_init();
    if (worker < 0 || worker >= s_nworkers || !s_enabled) {
        return -1;
    }
    Worker* w = &s_w[worker];
    uint64_t t0 = svcGetSystemTick();
    __atomic_store_n(&w->probe_ack, 0, __ATOMIC_RELAXED);
    store_sc(&w->probe_req, 1);
    wake(w);
    const uint64_t limit = PLAT_TICKS_PER_SEC / 200u; /* 5 ms */
    while (load_acq(&w->probe_req) != 0 && svcGetSystemTick() - t0 < limit) {
    }
    if (load_acq(&w->probe_req) != 0) {
        store_sc(&w->probe_req, 0);
        return 0;
    }
    *ticks = __atomic_load_n(&w->probe_ack, __ATOMIC_ACQUIRE) - t0;
    return 1;
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
    out->batches = __atomic_load_n(&s_stats.batches, __ATOMIC_RELAXED);
    out->batches_inline = __atomic_load_n(&s_stats.batches_inline, __ATOMIC_RELAXED);
    out->chunks_caller = __atomic_load_n(&s_stats.chunks_caller, __ATOMIC_RELAXED);
    out->chunks_w0 = __atomic_load_n(&s_stats.chunks_w0, __ATOMIC_RELAXED);
    out->chunks_w1 = __atomic_load_n(&s_stats.chunks_w1, __ATOMIC_RELAXED);
    out->takeovers = __atomic_load_n(&s_stats.takeovers, __ATOMIC_RELAXED);
    out->stale_waits = __atomic_load_n(&s_stats.stale_waits, __ATOMIC_RELAXED);
    out->spin_ticks = __atomic_load_n(&s_stats.spin_ticks, __ATOMIC_RELAXED);
    out->sleep_ticks = __atomic_load_n(&s_stats.sleep_ticks, __ATOMIC_RELAXED);
}
