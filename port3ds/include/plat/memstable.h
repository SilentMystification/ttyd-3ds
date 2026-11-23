/* Content-stable memory registry.
 *
 * A cache that keeps something derived from game memory (decoded display
 * lists, converted textures) must know when that memory changes. Hashing it
 * on every use is always correct but costs about as much as the work it
 * saves. The registry is the cheaper proof for memory whose every change goes
 * through a known path:
 *
 *  - Memory filled from a stable source (a disc read, an ARAM transfer into
 *    main memory, a module's data restored at link) is registered with
 *    plat_memstable_fill: it is "covered".
 *  - Every later path that can change covered memory reports it:
 *      plat_memstable_write   the CPU changed bytes there (a data cache flush
 *                             or store, which the GameCube requires before
 *                             the GPU reads CPU-written memory; a relink);
 *      plat_memstable_release the block is freed or handed to a new owner
 *                             (an allocator, a display list being recorded
 *                             into it): it is no longer covered;
 *      plat_memstable_fill    filled again.
 *  - A consumer that has checked its inputs in full (by hashing them) asks
 *    plat_memstable_trust whether they all lie in covered memory that did not
 *    change since the check began. If so it gets the current trust
 *    generation, and may skip the check for as long as plat_memstable_gen()
 *    returns that value. Any report that touches memory a consumer relies on
 *    bumps the generation, and every consumer checks in full again once.
 *
 * The contract: covered memory changes only through the reported paths.
 * A CPU store into covered memory with no flush or store after it (which the
 * GameCube's GPU would not see either, unless the line left the cache by
 * chance) is not seen. Consumers keep a verify mode that checks in full
 * anyway and counts what trust would have missed.
 *
 * Thread-safe: reports may come from any thread (the disc and ARAM threads).
 * Game-agnostic: it knows addresses, nothing about what is stored there. */
#ifndef PLAT_MEMSTABLE_H
#define PLAT_MEMSTABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void plat_memstable_fill(const void* p, uint32_t n);
void plat_memstable_write(const void* p, uint32_t n);
void plat_memstable_release(const void* p, uint32_t n);

/* Changes to covered memory so far: read before a full check, pass to plat_memstable_trust. */
uint32_t plat_memstable_epoch(void);
/* The trust generation (never 0). */
uint32_t plat_memstable_gen(void);

typedef struct {
    const void* p;
    uint32_t n;
} PlatMemSpan;

/* After a full check that began at `epoch`: when every span lies in covered
 * memory and nothing covered changed since `epoch`, marks the spans as relied
 * on and returns the trust generation; otherwise 0. */
uint32_t plat_memstable_trust(const PlatMemSpan* spans, uint32_t count, uint32_t epoch);

typedef struct {
    uint32_t ranges, covered_kb;
    uint32_t fills, writes, releases; /* reports since the last call */
    uint32_t revokes;                 /* trust generation bumps since the last call */
    uint32_t dropped;                 /* fills not registered (table full), in total */
} PlatMemStableStats;
/* Counts since the previous call (ranges and covered_kb: now). */
void plat_memstable_stats(PlatMemStableStats* out);

/* An observer of every report, called on the reporting thread before the
 * report is applied and without the registry's lock held, so it may block
 * (a consumer that reads the memory on another thread can wait there until
 * it is done with it). One hook; NULL removes it. Covered or not, every
 * report reaches it (n may be 0 for an empty report, which is skipped). */
enum { PLAT_MEMSTABLE_FILL = 1, PLAT_MEMSTABLE_WRITE, PLAT_MEMSTABLE_RELEASE };
typedef void (*PlatMemStableHook)(int kind, const void* p, uint32_t n);
void plat_memstable_set_hook(PlatMemStableHook fn);

#ifdef __cplusplus
}
#endif

#endif
