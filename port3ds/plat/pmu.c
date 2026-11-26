/* ARM11 performance counters per frame (include/plat/pmu.h), over
 * libs/ctr-devkit/perf (svcControlPerformanceCounter, or svcBackdoor under
 * Luma). Each slot is written by its own thread; the game thread reads the
 * totals through a sequence count and logs the change every N frames. */
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "ctrdk/perf.h"
#include "plat/plat.h"
#include "plat/pmu.h"

#ifndef TTYD_RETAIL /* the TTYD_RETAIL build has no PMU hooks (include/plat/pmu.h) */

typedef struct {
    volatile u32 seq; /* odd while the owner updates total */
    CtrdkPerfSample total;
    u32 spans;
    CtrdkPerfSample open;
} Slot;

static Slot s_slot[PLAT_PMU_SLOTS];
static CtrdkPerfSample s_last[PLAT_PMU_SLOTS];
static u32 s_last_spans[PLAT_PMU_SLOTS];
static volatile int s_on;
static int s_state; /* 0 not started, 1 on, -1 off */
static int s_every;
static u32 s_frames, s_frame_no;
static int s_rotate, s_pair;
static CtrdkPerfSample s_game_prev;

static const char* const k_slot_name[PLAT_PMU_SLOTS] = {"game", "translator", "present", "render"};

static const int k_pairs[][2] = {
    {CTRDK_EV_DCACHE_READ_MISS, CTRDK_EV_ICACHE_MISS},   {CTRDK_EV_STALL_DATA_DEP, CTRDK_EV_STALL_IBUF},
    {CTRDK_EV_BRANCH_MISPREDICT, CTRDK_EV_EXTERNAL_REQUEST}, {CTRDK_EV_STALL_LSU_FULL, CTRDK_EV_DCACHE_WRITE_MISS},
    {CTRDK_EV_MAIN_TLB_MISS, CTRDK_EV_DCACHE_EVICTION},  {CTRDK_EV_INST, CTRDK_EV_BRANCH},
};
#define NPAIRS ((int)(sizeof(k_pairs) / sizeof(k_pairs[0])))

int plat_pmu_on(void) { return __atomic_load_n(&s_on, __ATOMIC_ACQUIRE); }

static void slot_add(Slot* sl, const CtrdkPerfSample* d) {
    __atomic_add_fetch(&sl->seq, 1, __ATOMIC_ACQ_REL);
    ctrdk_perf_add(&sl->total, d);
    sl->spans++;
    __atomic_add_fetch(&sl->seq, 1, __ATOMIC_ACQ_REL);
}

static void slot_snapshot(Slot* sl, CtrdkPerfSample* out, u32* spans) {
    for (int tries = 0; tries < 100; tries++) {
        u32 s0 = __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u) {
            continue;
        }
        *out = sl->total;
        *spans = sl->spans;
        if (__atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE) == s0) {
            return;
        }
    }
    memset(out, 0, sizeof(*out));
    *spans = 0;
}

void plat_pmu_begin(int slot) {
    if (!plat_pmu_on() || slot <= PLAT_PMU_GAME || slot >= PLAT_PMU_SLOTS) {
        return;
    }
    ctrdk_perf_read_core(&s_slot[slot].open);
}

void plat_pmu_end(int slot) {
    if (!plat_pmu_on() || slot <= PLAT_PMU_GAME || slot >= PLAT_PMU_SLOTS) {
        return;
    }
    Slot* sl = &s_slot[slot];
    CtrdkPerfSample b, d;
    ctrdk_perf_read_core(&b);
    ctrdk_perf_diff(&sl->open, &b, &d);
    if (d.valid & 1u) {
        slot_add(sl, &d);
    }
}

static void set_pair(int i) {
    s_pair = i % NPAIRS;
    ctrdk_perf_set_events(k_pairs[s_pair][0], k_pairs[s_pair][1]);
}

static void start(void) {
    s_every = plat_switch_int("pmu", 0);
    if (s_every <= 0) {
        s_state = -1;
        return;
    }
    CtrdkPerfConfig cfg;
    ctrdk_perf_defaults(&cfg);
    char ev[48] = "rotate";
    plat_switch_value("pmuev", ev, sizeof(ev));
    s_rotate = strcmp(ev, "rotate") == 0;
    if (!s_rotate) {
        char* comma = strchr(ev, ',');
        if (comma != NULL) {
            *comma = '\0';
            int a = ctrdk_perf_event_from_name(ev), b = ctrdk_perf_event_from_name(comma + 1);
            if (a >= 0 && b >= 0 && a < 0x100 && b < 0x100) {
                cfg.pmn0_event = a;
                cfg.pmn1_event = b;
            } else {
                PLOG_W("pmu: unknown events in pmuev=%s,%s; using drmiss,imiss", ev, comma + 1);
            }
        }
    }
    cfg.nscu = 2; /* external reads and writes */
    int32_t rc = ctrdk_perf_init(&cfg);
    if (rc != 0 || !ctrdk_perf_available()) {
        s_state = -1;
        PLOG_I("pmu: not available (rc %08lX)", (unsigned long)rc);
        return;
    }
    s_state = 1;
    PLOG_I("pmu: on via %s, every %d frames, events %s", ctrdk_perf_backend_name(), s_every,
           s_rotate ? "rotate" : ev);
    if (s_rotate) {
        set_pair(0);
    }
    __atomic_store_n(&s_on, 1, __ATOMIC_RELEASE);
}

static int fmt_slot(char* buf, size_t n, int slot, const CtrdkPerfSample* d, u32 spans, u32 frames) {
    const CtrdkPerfConfig* c = ctrdk_perf_config();
    int w = snprintf(buf, n, " | %s c%d cyc %lluK %s %llu %s %llu", k_slot_name[slot], d->core,
                     (unsigned long long)(d->cycles / frames / 1000u), ctrdk_perf_event_name(c->pmn0_event),
                     (unsigned long long)(d->pmn0 / frames), ctrdk_perf_event_name(c->pmn1_event),
                     (unsigned long long)(d->pmn1 / frames));
    if (w > 0 && (size_t)w < n && slot == PLAT_PMU_GAME && (d->valid & 2u)) {
        w += snprintf(buf + w, n - (size_t)w, " scu_rd %llu scu_wr %llu", (unsigned long long)(d->scu[0] / frames),
                      (unsigned long long)(d->scu[1] / frames));
    }
    if (w > 0 && (size_t)w < n && slot != PLAT_PMU_GAME) {
        w += snprintf(buf + w, n - (size_t)w, " spans %lu", (unsigned long)(spans / frames));
    }
    return w < 0 ? 0 : ((size_t)w >= n ? (int)n - 1 : w);
}

void plat_pmu_frame(void) {
    if (s_state == 0) {
        start();
    }
    if (s_state != 1) {
        return;
    }
    s_frame_no++;
    CtrdkPerfSample now, d;
    ctrdk_perf_read(&now);
    if (s_game_prev.valid & 1u) {
        ctrdk_perf_diff(&s_game_prev, &now, &d);
        if (d.valid & 1u) {
            slot_add(&s_slot[PLAT_PMU_GAME], &d);
        }
    }
    s_game_prev = now;
    if (++s_frames < (u32)s_every) {
        return;
    }
    char line[512];
    const CtrdkPerfConfig* c = ctrdk_perf_config();
    int w = snprintf(line, sizeof(line), "pmu frame %lu [%s %s] per frame over %lu", (unsigned long)s_frame_no,
                     ctrdk_perf_event_name(c->pmn0_event), ctrdk_perf_event_name(c->pmn1_event),
                     (unsigned long)s_frames);
    for (int s = 0; s < PLAT_PMU_SLOTS && w > 0 && (size_t)w < sizeof(line); s++) {
        CtrdkPerfSample snap, delta;
        u32 spans;
        slot_snapshot(&s_slot[s], &snap, &spans);
        if (snap.valid == 0) {
            continue;
        }
        if (snap.gen == s_last[s].gen && (s_last[s].valid & 1u)) {
            delta = snap; /* totals of one slot (64-bit sums): plain subtraction */
            delta.cycles -= s_last[s].cycles;
            delta.pmn0 -= s_last[s].pmn0;
            delta.pmn1 -= s_last[s].pmn1;
            for (int i = 0; i < CTRDK_PERF_SCU_MAX; i++) {
                delta.scu[i] -= s_last[s].scu[i];
            }
        } else {
            delta = snap; /* counted since this interval's events were set */
        }
        u32 dsp = spans - s_last_spans[s];
        s_last[s] = snap;
        s_last_spans[s] = spans;
        if (delta.cycles == 0) {
            continue;
        }
        w += fmt_slot(line + w, sizeof(line) - (size_t)w, s, &delta, dsp, s_frames);
    }
    PLOG_I("%s", line);
    s_frames = 0;
    if (s_rotate) {
        set_pair(s_pair + 1);
    }
}

#endif /* TTYD_RETAIL */
