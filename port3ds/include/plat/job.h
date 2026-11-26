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
 * Switches: jobs=0 (everything inline, the A/B baseline), jobstats=N (log
 * the counters every N frames while jobs run), jobcore=C, jobprio=P. */
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
    uint64_t worker_ticks; /* time the worker spent running jobs */
} PlatJobStats;
void plat_job_stats(PlatJobStats* out);

/* Round trip of an empty job to worker w (0: the ring worker), from submit until the worker has run it,
 * without stealing: *ticks. Returns 1, 0 when the worker did not run it within 5 ms (the caller then ran
 * it), -1 when there is no such worker. For the jobprobe switch. */
int plat_job_probe(int worker, uint64_t* ticks);

#ifdef __cplusplus
}
#endif

#endif
