/* Fork-join jobs on a spare core (plat/job.c).
 *
 * A worker thread (core 2 by default) runs self-contained work the caller
 * hands it; the caller joins before it uses the result. Nothing here knows
 * the game: callers pass a function and an argument.
 *
 * Work stealing: plat_job_join runs a job the worker has not started on the
 * calling thread, so a busy worker never makes the caller wait for work that
 * has not begun. The caller only waits for a job the worker is running.
 *
 * Rules for job functions:
 *   - pure computation on memory the caller owns until the join: no GPU or
 *     system service calls, no game state another thread may touch, no
 *     waiting on other jobs;
 *   - they may run on the worker or on the joining thread, in any order, so
 *     two jobs of one batch must not write the same bytes (or must use
 *     atomic operations where their outputs share a word).
 * A job that fails a check may call plat_fatal (the crash path works from
 * any thread).
 *
 * The queue is a fixed ring: no allocation per job. Every handle must be
 * joined once. When the ring is full, or with the switch jobs=0, a job runs
 * at once inside plat_job_submit and its handle is already done.
 *
 * Suspend and sleep: the worker only runs jobs, which call no services, so
 * an APT suspend just stops it between instructions like any thread; the
 * job finishes when the process runs again. Nothing to hook.
 *
 * Frame batches (plat_job_pfor_dyn) are a second, separate path: short
 * parallel loops the caller needs back within the frame. Worker 0 takes part
 * between its ring jobs; worker 1 (switch jobw1=1, core 1) serves batches
 * only, so a long ring job never holds a frame batch.
 *
 * Workers run every job and batch with the submitter's FPSCR (rounding mode,
 * flush-to-zero), so float results match the submitting thread's.
 *
 * Switches: jobs=0 (everything inline, the A/B baseline), jobstats=N (log
 * the counters every N frames while jobs run), jobcore=C, jobprio=P (worker
 * 0, default core 2, 0x30), jobw1=1 with jobcore1=C, jobprio1=P (worker 1,
 * default core 1, 0x2F), jobspin=US (idle spin of workers and joiners before
 * they sleep, default 30), jobtakeover=US (a batch caller waits this long
 * for a held chunk before it computes it itself, default 300), jobcheck=1
 * (FPSCR asserted in the workers; split conversion runs compared). */
#ifndef PLAT_JOB_H
#define PLAT_JOB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PlatJobFn)(void* arg);
/* One chunk of a parallel loop: items [begin, end). */
typedef void (*PlatJobRangeFn)(void* arg, uint32_t begin, uint32_t end);

typedef uint32_t PlatJob; /* 0 is never a valid handle */

/* Starts the worker. core: 0..2 (or -1 for the default, 2 on a New 3DS);
 * priority: 0x18..0x3F (or -1 for the default 0x30, below the render
 * thread's 0x2E so rendering always wins). The switches jobcore and jobprio
 * override both. Calling it again does nothing. plat_job_submit calls it
 * with the defaults if nobody did. Returns 1 when a worker runs. */
int plat_job_init(int core, int priority);

/* 1 when jobs run on the worker (not jobs=0, and the worker started). */
int plat_job_enabled(void);

PlatJob plat_job_submit(PlatJobFn fn, void* arg);
void plat_job_join(PlatJob job);
/* Joins n jobs: first runs every one the worker has not started, then waits
 * for the rest. Better than joining one by one. */
void plat_job_join_all(const PlatJob* jobs, uint32_t n);

/* Runs fn over [0, count) in chunks of at least min_chunk items, on the
 * worker and the calling thread, and returns when every chunk is done.
 * Chunk edges are multiples of `align` items (1 for any). Small loops
 * (count < 2 * min_chunk) and jobs=0 run as one call on this thread. */
void plat_job_parallel_for(PlatJobRangeFn fn, void* arg, uint32_t count, uint32_t min_chunk, uint32_t align);

/* Counters since boot (all jobs, including inline ones). Times in ticks. */
typedef struct {
    uint32_t queued;    /* handed to the ring */
    uint32_t inline_;   /* ran inside plat_job_submit (jobs=0, ring full) */
    uint32_t worker;    /* ran on the worker */
    uint32_t stolen;    /* ran on the joining thread because the worker had not started them */
    uint32_t waits;     /* joins that had to wait for the worker */
    uint64_t wait_ticks;   /* time joiners spent waiting */
    uint64_t worker_ticks; /* time the workers spent running jobs and batch chunks */
    uint32_t batches;        /* plat_job_pfor_dyn calls split across threads */
    uint32_t batches_inline; /* ... run as one call (jobs=0, one chunk, a batch already running) */
    uint32_t chunks_caller, chunks_w0, chunks_w1; /* batch chunks by who ran them */
    uint32_t takeovers;      /* chunks a worker held that the caller computed again (alt) */
    uint32_t stale_waits;    /* batches that waited for a worker still inside the previous one */
    uint64_t spin_ticks;     /* batch callers spinning for the last chunks */
    uint64_t sleep_ticks;    /* ... then sleeping on the batch event */
} PlatJobStats;
void plat_job_stats(PlatJobStats* out);

/* Workers that run (0 with jobs=0; 2 with jobw1=1). */
int plat_job_workers(void);

/* One chunk of a dynamic parallel loop: items [begin, end). alt is 0 for the normal output and 1 when the
 * caller computes a chunk a worker still holds: then write the results to the second (alternate) buffer;
 * the worker may still be writing the normal one. */
typedef void (*PlatJobDynFn)(void* arg, uint32_t begin, uint32_t end, int alt);

/* Dynamic parallel-for over [0, count): chunks of 2^k items (k: the smallest with 2^k >= grain and at most
 * 64 chunks; *shift_out = k). The caller and the idle workers each take the next chunk with an atomic add;
 * the caller spins up to jobspin us for the last ones, sleeps until jobtakeover us, then computes any chunk
 * a worker still holds itself with alt = 1. Returns the mask of those chunks (bit i: items
 * [i << k, (i + 1) << k) are in the alternate buffer). One batch runs at a time; a second caller, jobs=0 or a
 * one-chunk loop runs fn(arg, 0, count, 0) on the calling thread and returns 0.
 * arg, and all memory fn reads or writes, must stay valid and the caller's until the next plat_job_pfor_dyn
 * or plat_job_pfor_quiesce: a worker that lost a chunk may still run it until then (so arg must not live on
 * the caller's stack; quiesce before refilling a static arg). fn follows the job rules above (no services, no GPU, no
 * heap, no locks). */
uint64_t plat_job_pfor_dyn(PlatJobDynFn fn, void* arg, uint32_t count, uint32_t grain, uint32_t* shift_out);
/* Waits until no worker is inside an earlier batch (before the caller frees or reuses its buffers). */
void plat_job_pfor_quiesce(void);

/* Wake-up round trip of worker w: from the request until the worker acknowledged it (*ticks). Returns 1,
 * 0 when it did not answer within 5 ms, -1 when there is no such worker. For the jobprobe switch. */
int plat_job_probe(int worker, uint64_t* ticks);

#ifdef __cplusplus
}
#endif

#endif
