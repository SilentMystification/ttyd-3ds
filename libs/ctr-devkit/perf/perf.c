/* ctr-devkit perf: ARM11 PMU and SCU counters (see include/ctrdk/perf.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "ctrdk/perf.h"

static CtrdkPerfConfig s_cfg;
static CtrdkPerfBackend s_backend = CTRDK_PERF_NONE;
static int32_t s_err;
static volatile u32 s_gen = 1;     /* event generation */
static volatile u32 s_core_gen[4]; /* generation programmed on each core */

/* ---- backdoor: kernel-mode CP15 access on the calling core ----
 * PMNC (c15,c12,0): bit 0 enable, 1 reset PMN0/PMN1, 2 reset CCNT, 3 CCNT/64,
 * 8-10 overflow flags (write 1 to clear), 12-19 EvtCount1, 20-27 EvtCount0.
 * CCNT c15,c12,1; PMN0 c15,c12,2; PMN1 c15,c12,3. */

static volatile u32 s_bd_pmnc;
static volatile u32 s_bd_val[4][4]; /* per core: pmnc, ccnt, pmn0, pmn1 */

__attribute__((target("arm"), noinline)) static u32 bd_core(void) {
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v));
    return v & 3;
}

__attribute__((target("arm"), noinline)) static s32 bd_program(void) {
    u32 v = s_bd_pmnc;
    __asm__ volatile("mcr p15, 0, %0, c15, c12, 0" ::"r"(v));
    return 0;
}

__attribute__((target("arm"), noinline)) static s32 bd_read(void) {
    u32 c = bd_core(), pmnc, ccnt, p0, p1;
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 0" : "=r"(pmnc));
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 1" : "=r"(ccnt));
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 2" : "=r"(p0));
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 3" : "=r"(p1));
    s_bd_val[c][0] = pmnc;
    s_bd_val[c][1] = ccnt;
    s_bd_val[c][2] = p0;
    s_bd_val[c][3] = p1;
    return 0;
}

static void bd_setup_core(void) {
    s_bd_pmnc = 1u | 2u | 4u | (7u << 8) | ((u32)(s_cfg.pmn1_event & 0xFF) << 12) |
                ((u32)(s_cfg.pmn0_event & 0xFF) << 20);
    svcBackdoor(bd_program);
}

/* ---- the kernel's SVC ---- */

static Result perf_op(u64* out, PerfCounterOperation op, u32 p1, u64 p2) {
    u64 dummy = 0;
    Result r = svcControlPerformanceCounter(out != NULL ? out : &dummy, op, p1, p2);
    if (R_FAILED(r) && s_err == 0) {
        s_err = r;
    }
    return r;
}

static void svc_setup_core(void) {
    perf_op(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, (u64)s_cfg.pmn0_event);
    perf_op(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, (u64)s_cfg.pmn1_event);
}

static bool svc_try(void) {
    s_err = 0;
    if (R_FAILED(perf_op(NULL, PERFCOUNTEROP_ENABLE, 0, 0))) {
        return false;
    }
    u64 v = 0;
    svcControlPerformanceCounter(&v, PERFCOUNTEROP_SET_VIRTUAL_COUNTER_ENABLED, 1, 0);
    svc_setup_core();
    for (int i = 0; i < s_cfg.nscu; i++) {
        perf_op(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_SCU_0 + i, (u64)s_cfg.scu_event[i]);
    }
    perf_op(NULL, PERFCOUNTEROP_RESET, 0xFFFFFFFFu, 0xFFFFFFFFull);
    u64 a = 0, b = 0;
    if (R_FAILED(perf_op(&a, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_CYCLE_COUNTER, 0))) {
        return false;
    }
    for (volatile int i = 0; i < 10000; i++) {
    }
    perf_op(&b, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_CYCLE_COUNTER, 0);
    if (b == a) {
        svcControlPerformanceCounter(&v, PERFCOUNTEROP_DISABLE, 0, 0);
        s_err = -1; /* the counter does not move */
        return false;
    }
    return true;
}

static bool emulator(void) {
    s64 v = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v == 1;
}

static bool bd_try(void) {
    if (emulator()) {
        return false;
    }
    bd_setup_core();
    u32 c = (u32)svcGetProcessorID() & 3;
    s_bd_val[c][0] = s_bd_val[c][1] = 0;
    Result r = svcBackdoor(bd_read);
    u32 a = s_bd_val[c][1];
    for (volatile int i = 0; i < 10000; i++) {
    }
    svcBackdoor(bd_read);
    if (R_FAILED(r)) {
        s_err = r;
        return false;
    }
    return (s_bd_val[c][0] & 1u) && s_bd_val[c][1] != a;
}

/* ---- public ---- */

void ctrdk_perf_defaults(CtrdkPerfConfig* cfg) {
    static const int scu[CTRDK_PERF_SCU_MAX] = {
        PERFCOUNTEREVT_SCU_EXTERNAL_READ,            PERFCOUNTEREVT_SCU_EXTERNAL_WRITE,
        PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE0, PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE1,
        PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE2, PERFCOUNTEREVT_SCU_READ_BUSY_PORT0,
        PERFCOUNTEREVT_SCU_READ_BUSY_PORT1,          PERFCOUNTEREVT_SCU_WRITE_BUSY_PORT0,
    };
    memset(cfg, 0, sizeof(*cfg));
    cfg->backend = CTRDK_PERF_AUTO;
    cfg->pmn0_event = CTRDK_EV_DCACHE_READ_MISS;
    cfg->pmn1_event = CTRDK_EV_ICACHE_MISS;
    cfg->nscu = -1;
    memcpy(cfg->scu_event, scu, sizeof(scu));
}

int32_t ctrdk_perf_init(const CtrdkPerfConfig* cfg) {
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        ctrdk_perf_defaults(&s_cfg);
    }
    if (s_cfg.nscu < 0 || s_cfg.nscu > CTRDK_PERF_SCU_MAX) {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        s_cfg.nscu = n3ds ? 8 : 4;
    }
    s_backend = CTRDK_PERF_NONE;
    s_gen++;
    if (s_cfg.backend == CTRDK_PERF_AUTO || s_cfg.backend == CTRDK_PERF_SVC) {
        if (svc_try()) {
            s_backend = CTRDK_PERF_SVC;
        }
    }
    if (s_backend == CTRDK_PERF_NONE && (s_cfg.backend == CTRDK_PERF_AUTO || s_cfg.backend == CTRDK_PERF_BACKDOOR)) {
        int32_t svc_err = s_err;
        if (bd_try()) {
            s_backend = CTRDK_PERF_BACKDOOR;
            s_err = 0;
        } else if (s_err == 0) {
            s_err = svc_err != 0 ? svc_err : -2;
        }
    }
    if (s_backend == CTRDK_PERF_NONE) {
        return s_err != 0 ? s_err : -2;
    }
    s_core_gen[svcGetProcessorID() & 3] = s_gen;
    return 0;
}

void ctrdk_perf_exit(void) {
    if (s_backend == CTRDK_PERF_SVC) {
        u64 v;
        svcControlPerformanceCounter(&v, PERFCOUNTEROP_DISABLE, 0, 0);
    }
    s_backend = CTRDK_PERF_NONE;
}

bool ctrdk_perf_available(void) { return s_backend == CTRDK_PERF_SVC || s_backend == CTRDK_PERF_BACKDOOR; }
CtrdkPerfBackend ctrdk_perf_backend(void) { return s_backend; }
const char* ctrdk_perf_backend_name(void) {
    switch (s_backend) {
    case CTRDK_PERF_SVC: return "svcControlPerformanceCounter";
    case CTRDK_PERF_BACKDOOR: return "svcBackdoor";
    default: return "none";
    }
}
int32_t ctrdk_perf_last_error(void) { return s_err; }
const CtrdkPerfConfig* ctrdk_perf_config(void) { return &s_cfg; }

void ctrdk_perf_set_events(int pmn0_event, int pmn1_event) {
    s_cfg.pmn0_event = pmn0_event;
    s_cfg.pmn1_event = pmn1_event;
    __atomic_add_fetch(&s_gen, 1, __ATOMIC_RELEASE);
}

static void read_impl(CtrdkPerfSample* out, bool scu) {
    memset(out, 0, sizeof(*out));
    int core = svcGetProcessorID();
    out->core = core;
    if (!ctrdk_perf_available() || core < 0 || core > 3) {
        out->tick = svcGetSystemTick();
        return;
    }
    u32 gen = __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE);
    if (s_core_gen[core] != gen) {
        if (s_backend == CTRDK_PERF_BACKDOOR) {
            bd_setup_core();
        } else {
            svc_setup_core();
        }
        s_core_gen[core] = gen;
    }
    out->gen = gen;
    if (s_backend == CTRDK_PERF_BACKDOOR) {
        svcBackdoor(bd_read);
        out->tick = svcGetSystemTick();
        u32 c = (u32)core;
        out->cycles = s_bd_val[c][1];
        out->pmn0 = s_bd_val[c][2];
        out->pmn1 = s_bd_val[c][3];
        out->valid = (s_bd_val[c][0] & 1u) ? 1u : 0u;
        return;
    }
    u64 v;
    bool core_ok = true, scu_ok = true;
    core_ok &= R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_CYCLE_COUNTER, 0));
    out->cycles = v;
    core_ok &= R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_COUNT_REG_0, 0));
    out->pmn0 = v;
    core_ok &= R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_COUNT_REG_1, 0));
    out->pmn1 = v;
    out->nscu = scu ? s_cfg.nscu : 0;
    for (int i = 0; i < out->nscu; i++) {
        scu_ok &= R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_SCU_0 + i, 0));
        out->scu[i] = v;
    }
    out->tick = svcGetSystemTick();
    out->valid = (core_ok ? 1u : 0u) | (scu_ok && out->nscu > 0 ? 2u : 0u);
}

void ctrdk_perf_read(CtrdkPerfSample* out) { read_impl(out, true); }
void ctrdk_perf_read_core(CtrdkPerfSample* out) { read_impl(out, false); }

void ctrdk_perf_diff(const CtrdkPerfSample* a, const CtrdkPerfSample* b, CtrdkPerfSample* out) {
    CtrdkPerfSample d;
    memset(&d, 0, sizeof(d));
    d.core = b->core;
    d.nscu = b->nscu;
    d.gen = b->gen;
    d.valid = a->valid & b->valid;
    if (a->core != b->core || a->gen != b->gen) {
        d.valid &= ~1u; /* another core's counters, or other events */
    }
    d.tick = b->tick - a->tick;
    d.cycles = b->cycles - a->cycles;
    d.pmn0 = b->pmn0 - a->pmn0;
    d.pmn1 = b->pmn1 - a->pmn1;
    if (s_backend == CTRDK_PERF_BACKDOOR) { /* 32-bit counters */
        d.cycles &= 0xFFFFFFFFull;
        d.pmn0 &= 0xFFFFFFFFull;
        d.pmn1 &= 0xFFFFFFFFull;
    }
    for (int i = 0; i < CTRDK_PERF_SCU_MAX; i++) {
        d.scu[i] = b->scu[i] - a->scu[i];
    }
    *out = d;
}

void ctrdk_perf_add(CtrdkPerfSample* acc, const CtrdkPerfSample* d) {
    if (!(d->valid & 1u)) {
        return;
    }
    if (acc->valid == 0 || acc->gen != d->gen) {
        memset(acc, 0, sizeof(*acc));
        acc->gen = d->gen;
        acc->core = d->core;
        acc->nscu = d->nscu;
        acc->valid = d->valid;
    }
    acc->valid &= d->valid;
    acc->tick += d->tick;
    acc->cycles += d->cycles;
    acc->pmn0 += d->pmn0;
    acc->pmn1 += d->pmn1;
    for (int i = 0; i < CTRDK_PERF_SCU_MAX; i++) {
        acc->scu[i] += d->scu[i];
    }
}

static const struct {
    int ev;
    const char* name;
} k_names[] = {
    {CTRDK_EV_ICACHE_MISS, "imiss"},
    {CTRDK_EV_STALL_IBUF, "stall_ibuf"},
    {CTRDK_EV_STALL_DATA_DEP, "stall_dep"},
    {CTRDK_EV_IMICRO_TLB_MISS, "iutlb"},
    {CTRDK_EV_DMICRO_TLB_MISS, "dutlb"},
    {CTRDK_EV_BRANCH, "branch"},
    {CTRDK_EV_BRANCH_NOT_PREDICTED, "brnopred"},
    {CTRDK_EV_BRANCH_MISPREDICT, "brmiss"},
    {CTRDK_EV_INST, "inst"},
    {CTRDK_EV_FOLDED_INST, "folded"},
    {CTRDK_EV_DCACHE_READ, "drd"},
    {CTRDK_EV_DCACHE_READ_MISS, "drmiss"},
    {CTRDK_EV_DCACHE_WRITE, "dwr"},
    {CTRDK_EV_DCACHE_WRITE_MISS, "dwmiss"},
    {CTRDK_EV_DCACHE_EVICTION, "devict"},
    {CTRDK_EV_PC_CHANGE, "pcchg"},
    {CTRDK_EV_MAIN_TLB_MISS, "tlbmiss"},
    {CTRDK_EV_EXTERNAL_REQUEST, "ext"},
    {CTRDK_EV_STALL_LSU_FULL, "stall_lsu"},
    {CTRDK_EV_STORE_BUFFER_DRAIN, "sbdrain"},
    {CTRDK_EV_STORE_BUFFER_MERGE, "sbmerge"},
    {CTRDK_EV_CYCLES, "cyc"},
    {PERFCOUNTEREVT_SCU_EXTERNAL_READ, "scu_rd"},
    {PERFCOUNTEREVT_SCU_EXTERNAL_WRITE, "scu_wr"},
    {PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE0, "scu_miss0"},
    {PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE1, "scu_miss1"},
    {PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE2, "scu_miss2"},
    {PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE3, "scu_miss3"},
    {PERFCOUNTEREVT_SCU_READ_BUSY_PORT0, "scu_rbusy0"},
    {PERFCOUNTEREVT_SCU_READ_BUSY_PORT1, "scu_rbusy1"},
    {PERFCOUNTEREVT_SCU_WRITE_BUSY_PORT0, "scu_wbusy0"},
    {PERFCOUNTEREVT_SCU_WRITE_BUSY_PORT1, "scu_wbusy1"},
};

const char* ctrdk_perf_event_name(int e) {
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (k_names[i].ev == e) {
            return k_names[i].name;
        }
    }
    return "evt";
}

int ctrdk_perf_event_from_name(const char* name) {
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (strcmp(k_names[i].name, name) == 0) {
            return k_names[i].ev;
        }
    }
    return -1;
}

int ctrdk_perf_format(const CtrdkPerfSample* d, char* buf, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (d->valid == 0) {
        return snprintf(buf, n, "core%d perf n/a", d->core);
    }
    int w;
    if (d->valid & 1u) {
        w = snprintf(buf, n, "core%d cyc=%llu %s=%llu %s=%llu", d->core, (unsigned long long)d->cycles,
                     ctrdk_perf_event_name(s_cfg.pmn0_event), (unsigned long long)d->pmn0,
                     ctrdk_perf_event_name(s_cfg.pmn1_event), (unsigned long long)d->pmn1);
    } else {
        w = snprintf(buf, n, "core%d", d->core);
    }
    if (d->valid & 2u) {
        for (int i = 0; i < d->nscu && w > 0 && (size_t)w < n; i++) {
            w += snprintf(buf + w, n - (size_t)w, " %s=%llu", ctrdk_perf_event_name(s_cfg.scu_event[i]),
                          (unsigned long long)d->scu[i]);
        }
    }
    return w < 0 ? 0 : ((size_t)w >= n ? (int)n - 1 : w);
}
