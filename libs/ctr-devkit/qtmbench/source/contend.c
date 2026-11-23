/* qtmbench: the contention matrix (victim x aggressor x memory) and the
 * performance-counter checks.
 *
 * A victim is one measurement (CPU bandwidth or latency on a core, or the
 * time of a GPU command list); an aggressor is traffic started first and kept
 * running while the victim is measured (GPU texture or vertex streaming from
 * a memory, a display transfer, NDSP playback, other cores reading a
 * memory). The matrix entry is the victim's slowdown against its run with no
 * aggressor: 1.00 unaffected, 2.00 twice as slow.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

/* ---- aggressors ---- */

typedef enum {
    A_NONE,
    A_GPU_TEX_LIN,
    A_GPU_TEX_QTM,
    A_GPU_TEX_VRAM,
    A_GPU_VTX_LIN,
    A_GPU_VTX_QTM,
    A_DT_LIN,
    A_NDSP,
    A_CPU2_HEAP,
    A_CPU2_QTM,
    A_CPU02_HEAP,
    A_CPU02_QTM,
    A_COUNT
} Aggr;

static const char* const k_aggr_name[A_COUNT] = {
    "none",     "gpu_tex:lin", "gpu_tex:qtm", "gpu_tex:vram", "gpu_vtx:lin", "gpu_vtx:qtm",
    "dt:lin>lin", "ndsp",      "c2:heap",     "c2:qtm",       "c0+2:heap",   "c0+2:qtm",
};

static bool aggr_is_gpu(Aggr a) { return a >= A_GPU_TEX_LIN && a <= A_DT_LIN; }

typedef struct {
    GLoad kind;
    MemId mem;
    volatile int* stop;
    u32 runs;
    double us; /* total GPU-op time */
    u64 ticks;
    bool failed;
} GpuLoop;

static void gpu_loop_main(void* p) {
    GpuLoop* g = p;
    u64 t0 = tick();
    while (!*g->stop) {
        double us = gpu_load_once(g->kind, g->mem);
        if (us < 0) {
            g->failed = true;
            break;
        }
        g->us += us;
        g->runs++;
    }
    g->ticks = tick() - t0;
}

static int s_gpu_core = -1; /* the core GPU feeder threads run on */

/* ---- NDSP ---- */

static ndspWaveBuf s_wave[8];
static bool s_ndsp_ok;

static bool ndsp_start(void) {
    if (R_FAILED(ndspInit())) {
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    u8* buf = g_lin_aux + 3 * QB_MIB;
    memset(buf, 0, QB_MIB);
    cache_flush(buf, QB_MIB);
    for (int ch = 0; ch < 8; ch++) {
        float mix[12];
        memset(mix, 0, sizeof(mix));
        ndspChnReset(ch);
        ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
        ndspChnSetRate(ch, 48000.0f);
        ndspChnSetFormat(ch, NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetMix(ch, mix);
        memset(&s_wave[ch], 0, sizeof(s_wave[ch]));
        s_wave[ch].data_vaddr = buf + ch * (128u << 10);
        s_wave[ch].nsamples = (128u << 10) / 4;
        s_wave[ch].looping = true;
        ndspChnWaveBufAdd(ch, &s_wave[ch]);
    }
    return true;
}

static void ndsp_stop(void) {
    for (int ch = 0; ch < 8; ch++) {
        ndspChnWaveBufClear(ch);
    }
    ndspExit();
}

/* ---- running aggressors ---- */

typedef struct {
    Aggr a;
    volatile int go, stop;
    Worker w[2];
    int nw;
    GpuLoop g;
    Thread gt;
    bool on;
} AggrRun;

static bool aggr_start(AggrRun* r, Aggr a) {
    memset(r, 0, sizeof(*r));
    r->a = a;
    if (a == A_NONE) {
        return true;
    }
    if (aggr_is_gpu(a)) {
        static const GLoad kind[] = {GLOAD_TEX, GLOAD_TEX, GLOAD_TEX, GLOAD_VTX, GLOAD_VTX, GLOAD_DT};
        static const MemId mem[] = {MEM_LINEAR, MEM_QTM, MEM_VRAM, MEM_LINEAR, MEM_QTM, MEM_LINEAR};
        int i = a - A_GPU_TEX_LIN;
        if (s_gpu_core < 0) {
            return false;
        }
        r->g.kind = kind[i];
        r->g.mem = mem[i];
        r->g.stop = &r->stop;
        r->gt = spawn_on_core(s_gpu_core, gpu_loop_main, &r->g, PRIO_WORK);
        r->on = r->gt != NULL;
        return r->on;
    }
    if (a == A_NDSP) {
        r->on = s_ndsp_ok && ndsp_start();
        return r->on;
    }
    /* CPU readers */
    u8* qtm = g_mem[MEM_QTM].present ? g_mem[MEM_QTM].base : NULL;
    if ((a == A_CPU2_QTM || a == A_CPU02_QTM) && qtm == NULL) {
        return false;
    }
    int cores[2] = {2, 0};
    r->nw = (a == A_CPU02_HEAP || a == A_CPU02_QTM) ? 2 : 1;
    if (!g_opt.core2) {
        return false;
    }
    bool q = a == A_CPU2_QTM || a == A_CPU02_QTM;
    for (int i = 0; i < r->nw; i++) {
        Worker* w = &r->w[i];
        w->core = cores[i];
        w->kind = W_READ;
        w->buf = q ? qtm + (i ? QTM_APP_SIZE / 2 : 0) : g_aggr[i ? 0 : 1];
        w->size = q ? (r->nw == 2 ? QTM_APP_SIZE / 2 : QTM_APP_SIZE) : 4 * QB_MIB;
        w->go = &r->go;
        w->stop = &r->stop;
        if (!worker_start(w)) {
            r->stop = r->go = 1;
            for (int k = 0; k < i; k++) {
                worker_join(&r->w[k]);
            }
            return false;
        }
    }
    r->go = 1;
    r->on = true;
    return true;
}

/* Stops it; returns its achieved rate (MB/s), NAN if none. */
static double aggr_stop(AggrRun* r) {
    double rate = NAN;
    if (!r->on) {
        return rate;
    }
    r->stop = 1;
    if (aggr_is_gpu(r->a)) {
        threadJoin(r->gt, U64_MAX);
        threadFree(r->gt);
        if (r->g.runs > 0 && !r->g.failed) {
            rate = (double)gpu_load_bytes(r->g.kind) * r->g.runs / t2us(r->g.ticks);
        }
    } else if (r->a == A_NDSP) {
        ndsp_stop();
    } else {
        rate = 0;
        for (int i = 0; i < r->nw; i++) {
            worker_join(&r->w[i]);
            rate += worker_rate(&r->w[i]);
        }
    }
    r->on = false;
    return rate;
}

/* ---- victims ---- */

typedef enum {
    V_C0_BW_HEAP,
    V_C0_LAT_HEAP,
    V_C02_BW_HEAP,
    V_C0_BW_QTM,
    V_C0_LAT_QTM,
    V_GPU_TEX_LIN,
    V_GPU_VTX_LIN,
    V_GPU_TEX_QTM,
    V_COUNT
} Victim;

static const char* const k_vict_name[V_COUNT] = {
    "c0_read_heap4M", "c0_lat_heap4M", "c0+2_read_heap", "c0_read_qtm2M",
    "c0_lat_qtm1M",   "gpu_tex_lin",   "gpu_vtx_lin",    "gpu_tex_qtm",
};
static const char* const k_vict_unit[V_COUNT] = {"MB/s", "ns", "MB/s", "MB/s", "ns", "us", "us", "us"};
static bool vict_is_gpu(Victim v) { return v >= V_GPU_TEX_LIN; }

static const void* s_chain_heap;
static const void* s_chain_qtm;

static bool valid(Victim v, Aggr a) {
    if (a == A_NONE) {
        return true;
    }
    if (vict_is_gpu(v)) {
        return !aggr_is_gpu(a) && a != A_CPU2_HEAP && a != A_CPU2_QTM;
    }
    if (a == A_CPU02_HEAP || a == A_CPU02_QTM) {
        return false;
    }
    if (v == V_C02_BW_HEAP && (a == A_CPU2_HEAP || a == A_CPU2_QTM)) {
        return false;
    }
    return true;
}

typedef struct {
    double value;
    CtrdkPerfSample perf; /* core 0 victim's counters over the window */
} VResult;

static VResult measure(Victim v) {
    VResult res = {NAN, {0}};
    static volatile int go, stop;
    go = stop = 0;
    if (vict_is_gpu(v)) {
        GpuLoop g;
        memset(&g, 0, sizeof(g));
        g.kind = v == V_GPU_VTX_LIN ? GLOAD_VTX : GLOAD_TEX;
        g.mem = v == V_GPU_TEX_QTM ? MEM_QTM : MEM_LINEAR;
        g.stop = &stop;
        if (s_gpu_core < 0) {
            return res;
        }
        Thread t = spawn_on_core(s_gpu_core, gpu_loop_main, &g, PRIO_WORK);
        if (t == NULL) {
            return res;
        }
        svcSleepThread((s64)g_opt.window_ms * 1000000LL);
        stop = 1;
        threadJoin(t, U64_MAX);
        threadFree(t);
        if (g.runs > 0 && !g.failed) {
            res.value = g.us / g.runs;
        }
        return res;
    }
    Worker w[2];
    memset(w, 0, sizeof(w));
    int n = 1;
    u8* heap = g_mem[MEM_HEAP].base;
    w[0].core = 0;
    w[0].go = w[1].go = &go;
    w[0].stop = w[1].stop = &stop;
    switch (v) {
    case V_C0_BW_HEAP:
        w[0].kind = W_READ;
        w[0].buf = heap;
        w[0].size = 4 * QB_MIB;
        break;
    case V_C0_LAT_HEAP:
        w[0].kind = W_CHASE;
        w[0].chase = s_chain_heap;
        break;
    case V_C02_BW_HEAP:
        if (!g_opt.core2) {
            return res;
        }
        n = 2;
        w[0].kind = w[1].kind = W_READ;
        w[0].buf = heap;
        w[1].buf = heap + 4 * QB_MIB;
        w[0].size = w[1].size = 4 * QB_MIB;
        w[1].core = 2;
        break;
    case V_C0_BW_QTM:
        w[0].kind = W_READ;
        w[0].buf = g_mem[MEM_QTM].base;
        w[0].size = QTM_APP_SIZE;
        break;
    case V_C0_LAT_QTM:
        w[0].kind = W_CHASE;
        w[0].chase = s_chain_qtm;
        break;
    default: return res;
    }
    if (w[0].kind == W_CHASE && w[0].chase == NULL) {
        return res;
    }
    for (int i = 0; i < n; i++) {
        if (!worker_start(&w[i])) {
            stop = go = 1;
            for (int k = 0; k < i; k++) {
                worker_join(&w[k]);
            }
            return res;
        }
    }
    svcSleepThread(2 * 1000000LL);
    go = 1;
    svcSleepThread((s64)g_opt.window_ms * 1000000LL);
    stop = 1;
    for (int i = 0; i < n; i++) {
        worker_join(&w[i]);
    }
    res.value = worker_rate(&w[0]);
    if (n == 2) {
        res.value += worker_rate(&w[1]);
    }
    ctrdk_perf_diff(&w[0].p0, &w[0].p1, &res.perf);
    return res;
}

/* Bandwidth victims: higher is better; the rest: lower is better. */
static double slowdown(Victim v, double base, double x) {
    if (isnan(base) || isnan(x) || base <= 0 || x <= 0) {
        return NAN;
    }
    bool bw = v == V_C0_BW_HEAP || v == V_C02_BW_HEAP || v == V_C0_BW_QTM;
    return bw ? base / x : x / base;
}

void contend_suite(void) {
    if (!suite_on("contend")) {
        return;
    }
    status("contention matrix");
    s_gpu_core = g_opt.core1 ? 1 : (g_opt.core2 ? 2 : -1);
    info("contend_gpu_core", "%d (GPU feeder threads; -1 = none, GPU rows skipped)", s_gpu_core);
    /* the chains: heap 4..8 MiB, QTM 1..2 MiB (the GPU loads use QTM 0..1 MiB) */
    s_chain_heap = chase_build((u32*)(g_mem[MEM_HEAP].base + 4 * QB_MIB), 4 * QB_MIB,
                               (u32)(g_mem[MEM_HEAP].base + 4 * QB_MIB), 77);
    cache_flush(g_mem[MEM_HEAP].base + 4 * QB_MIB, 4 * QB_MIB);
    bool qtm = g_mem[MEM_QTM].present && g_mem[MEM_QTM].cpu_write;
    if (qtm) {
        s_chain_qtm = chase_build((u32*)(QTM_VADDR + QB_MIB), QB_MIB, QTM_VADDR + QB_MIB, 78);
        cache_flush((void*)(QTM_VADDR + QB_MIB), QB_MIB);
    }
    bool ok_tex[MEM_COUNT] = {0}, ok_vtx[MEM_COUNT] = {0};
    for (int m = MEM_LINEAR; m < MEM_COUNT; m++) {
        ok_tex[m] = gpu_load_prepare(GLOAD_TEX, (MemId)m);
        ok_vtx[m] = gpu_load_prepare(GLOAD_VTX, (MemId)m);
    }
    /* NDSP once, to know whether it starts at all (needs sdmc:/3ds/dspfirm.cdc) */
    s_ndsp_ok = ndsp_start();
    if (s_ndsp_ok) {
        ndsp_stop();
    }
    info("ndsp", "%s", s_ndsp_ok ? "ok" : "ndspInit failed (no sdmc:/3ds/dspfirm.cdc?)");

    svcSetThreadPriority(CUR_THREAD_HANDLE, PRIO_CTRL);
    for (int v = 0; v < V_COUNT; v++) {
        if ((v == V_C0_BW_QTM || v == V_C0_LAT_QTM || v == V_GPU_TEX_QTM) && !qtm) {
            continue;
        }
        if ((v == V_GPU_TEX_LIN && !ok_tex[MEM_LINEAR]) || (v == V_GPU_VTX_LIN && !ok_vtx[MEM_LINEAR]) ||
            (v == V_GPU_TEX_QTM && !ok_tex[MEM_QTM])) {
            continue;
        }
        double base = NAN;
        for (int a = 0; a < A_COUNT; a++) {
            if (!valid((Victim)v, (Aggr)a)) {
                continue;
            }
            if ((a == A_GPU_TEX_LIN && !ok_tex[MEM_LINEAR]) || (a == A_GPU_TEX_QTM && !ok_tex[MEM_QTM]) ||
                (a == A_GPU_TEX_VRAM && !ok_tex[MEM_VRAM]) || (a == A_GPU_VTX_LIN && !ok_vtx[MEM_LINEAR]) ||
                (a == A_GPU_VTX_QTM && !ok_vtx[MEM_QTM])) {
                continue;
            }
            AggrRun ar;
            if (!aggr_start(&ar, (Aggr)a)) {
                rec("contend_raw", k_vict_name[v], k_aggr_name[a], -1, 0, NAN, k_vict_unit[v], NAN,
                    "aggressor did not start");
                continue;
            }
            svcSleepThread(20 * 1000000LL);
            VResult r = measure((Victim)v);
            double arate = aggr_stop(&ar);
            if (a == A_NONE) {
                base = r.value;
            }
            char note[100] = "";
            if (r.perf.valid & 1u) {
                snprintf(note, sizeof(note), "aux=aggr MB/s; dmiss=%llu ext=%llu", (unsigned long long)r.perf.pmn0,
                         (unsigned long long)r.perf.pmn1);
            } else {
                snprintf(note, sizeof(note), "aux=aggressor MB/s");
            }
            rec("contend_raw", k_vict_name[v], k_aggr_name[a], -1, 0, r.value, k_vict_unit[v], arate, note);
            rec("contend", k_vict_name[v], k_aggr_name[a], -1, 0, slowdown((Victim)v, base, r.value), "x", r.value,
                NULL);
        }
        ui_poll();
    }
    svcSetThreadPriority(CUR_THREAD_HANDLE, PRIO_MAIN);
    memset(g_mem[MEM_HEAP].base, 0, g_mem[MEM_HEAP].size);
    cache_flush(g_mem[MEM_HEAP].base, g_mem[MEM_HEAP].size);
}

/* ---- performance counters ---- */

typedef struct {
    int core;
} PerfArg;

static void perf_job(void* p) {
    PerfArg* a = p;
    CtrdkPerfSample s0, s1, d;
    char line[200];
    /* cycles per system tick: 3.0 at 804 MHz, 1.0 at 268 MHz, if CCNT counts this core */
    ctrdk_perf_read(&s0);
    volatile u32 x = 1;
    for (u32 i = 0; i < 2000000u; i++) {
        x = x * 1664525u + 1013904223u;
    }
    ctrdk_perf_read(&s1);
    ctrdk_perf_diff(&s0, &s1, &d);
    ctrdk_perf_format(&d, line, sizeof(line));
    logf_("perf core %d busy loop: %s ticks=%llu", a->core, line, (unsigned long long)d.tick);
    char note[100];
    snprintf(note, sizeof(note), "read on core %d; valid=%lu", d.core, (unsigned long)d.valid);
    rec("perf", "cycles_per_tick", "-", a->core, 0, (d.valid & 1u) && d.tick ? (double)d.cycles / d.tick : NAN, "x",
        NAN, note);
    /* a 4 MiB heap read: misses and external requests per 32-byte line */
    u32 lines = 4 * QB_MIB / 32 * 4;
    ctrdk_perf_read(&s0);
    for (int i = 0; i < 4; i++) {
        k_read_ldm(g_aggr[0], 4 * QB_MIB);
    }
    ctrdk_perf_read(&s1);
    ctrdk_perf_diff(&s0, &s1, &d);
    ctrdk_perf_format(&d, line, sizeof(line));
    logf_("perf core %d heap 16 MiB read: %s", a->core, line);
    bool ok = d.valid & 1u;
    rec("perf", "pmn0_per_line_heap", "-", a->core, 4 * QB_MIB, ok ? (double)d.pmn0 / lines : NAN, "/line", NAN,
        ctrdk_perf_event_name(ctrdk_perf_config()->pmn0_event));
    rec("perf", "pmn1_per_line_heap", "-", a->core, 4 * QB_MIB, ok ? (double)d.pmn1 / lines : NAN, "/line", NAN,
        ctrdk_perf_event_name(ctrdk_perf_config()->pmn1_event));
    if (d.valid & 2u) {
        rec("perf", "scu0_per_line_heap", "-", a->core, 4 * QB_MIB, (double)d.scu[0] / lines, "/line", NAN,
            ctrdk_perf_event_name(ctrdk_perf_config()->scu_event[0]));
    }
    if (g_mem[MEM_QTM].present) {
        ctrdk_perf_read(&s0);
        for (int i = 0; i < 8; i++) {
            k_read_ldm(g_mem[MEM_QTM].base, QTM_APP_SIZE);
        }
        ctrdk_perf_read(&s1);
        ctrdk_perf_diff(&s0, &s1, &d);
        ctrdk_perf_format(&d, line, sizeof(line));
        logf_("perf core %d qtm 16 MiB read: %s", a->core, line);
        ok = d.valid & 1u;
        rec("perf", "pmn0_per_line_qtm", "-", a->core, QTM_APP_SIZE, ok ? (double)d.pmn0 / lines : NAN, "/line", NAN,
            ctrdk_perf_event_name(ctrdk_perf_config()->pmn0_event));
        rec("perf", "pmn1_per_line_qtm", "-", a->core, QTM_APP_SIZE, ok ? (double)d.pmn1 / lines : NAN, "/line", NAN,
            ctrdk_perf_event_name(ctrdk_perf_config()->pmn1_event));
        if (d.valid & 2u) {
            rec("perf", "scu0_per_line_qtm", "-", a->core, QTM_APP_SIZE, (double)d.scu[0] / lines, "/line", NAN,
                ctrdk_perf_event_name(ctrdk_perf_config()->scu_event[0]));
        }
    }
}

void perf_suite(void) {
    CtrdkPerfConfig pc;
    ctrdk_perf_defaults(&pc);
    pc.pmn0_event = CTRDK_EV_DCACHE_READ_MISS;
    pc.pmn1_event = CTRDK_EV_EXTERNAL_REQUEST;
    int32_t rc = ctrdk_perf_init(&pc);
    info("pmu", "%s via %s (rc %08lX); user-mode CP15 c15 is privileged-only on the ARM11 MPCore",
         ctrdk_perf_available() ? "available" : "unavailable", ctrdk_perf_backend_name(), (unsigned long)rc);
    rec("perf", "available", "-", -1, 0, ctrdk_perf_available() ? 1 : 0, "ok", NAN, NULL);
    if (!ctrdk_perf_available() || !suite_on("perf")) {
        return;
    }
    status("performance counters");
    int cores[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++) {
        if ((i == 1 && !g_opt.core1) || (i == 2 && !g_opt.core2)) {
            continue;
        }
        PerfArg a = {cores[i]};
        run_on_core(cores[i], perf_job, &a);
    }
}
