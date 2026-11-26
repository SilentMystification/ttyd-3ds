/* ARM11 performance counters per frame (plat/pmu.c over ctr-devkit perf).
 *
 * Switch pmu=N logs every N frames, per measured thread (and so per core):
 * cycles and two event counters per frame, plus the SCU's external reads and
 * writes for the game thread. pmuev=<a>,<b> picks the two events (names:
 * drmiss imiss stall_dep stall_ibuf brmiss ext stall_lsu dwmiss tlbmiss
 * devict inst ...); the default pmuev=rotate takes the next pair each log
 * interval. In Azahar (no PMU) the log says "pmu: not available" once and
 * every call is a no-op. */
#ifndef PLAT_PMU_H
#define PLAT_PMU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PLAT_PMU_GAME = 0,    /* the game thread, frame to frame (plat_pmu_frame) */
    PLAT_PMU_TRANSLATOR,  /* the GX translator's busy spans (gxthread=2) */
    PLAT_PMU_PRESENT,     /* translating GXCopyDisp, the frame's submit */
    PLAT_PMU_RENDER,      /* the render thread replaying a record */
    PLAT_PMU_SLOTS
};

#ifdef TTYD_RETAIL
/* The TTYD_RETAIL build: no counters, every hook compiles out (plat/pmu.c is empty). */
static inline int plat_pmu_on(void) { return 0; }
static inline void plat_pmu_begin(int slot) { (void)slot; }
static inline void plat_pmu_end(int slot) { (void)slot; }
static inline void plat_pmu_frame(void) {}
#else
int plat_pmu_on(void);
/* Brackets a span on one slot; every slot belongs to one thread. */
void plat_pmu_begin(int slot);
void plat_pmu_end(int slot);
/* The game thread, once per frame: closes the GAME slot's frame and logs
 * every N frames. */
void plat_pmu_frame(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
