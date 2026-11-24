/* qtmbench: CPU tests (bandwidth, latency, random access, cache
 * maintenance, two cores at once) and the worker threads.
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

bool gx_copy(void* dst, const void* src, u32 n); /* gpu.c */

Thread spawn_on_core(int core, void (*fn)(void*), void* arg, int prio) {
    return threadCreate(fn, arg, 32 * 1024, prio, core, false);
}

bool run_on_core(int core, void (*fn)(void*), void* arg) {
    Thread t = spawn_on_core(core, fn, arg, PRIO_WORK);
    if (t == NULL) {
        return false;
    }
    threadJoin(t, U64_MAX);
    threadFree(t);
    return true;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* Sizes: 4 KiB .. 2 MiB, then 4 and 8 MiB for FCRAM. */
static int size_list(MemId m, u32* out) {
    static const u32 full[] = {4u << 10, 8u << 10, 16u << 10, 32u << 10, 64u << 10, 128u << 10, 256u << 10,
                               512u << 10, 1u << 20, 2u << 20, 4u << 20, 8u << 20};
    static const u32 quick[] = {4u << 10, 32u << 10, 256u << 10, 2u << 20, 8u << 20};
    const u32* src = g_opt.quick ? quick : full;
    int n = g_opt.quick ? 5 : 12, k = 0;
    for (int i = 0; i < n; i++) {
        if (src[i] <= g_mem[m].size) {
            out[k++] = src[i];
        }
    }
    return k;
}

static int core_list(int* out) {
    int n = 0;
    out[n++] = 0;
    if (g_opt.core1) {
        out[n++] = 1;
    }
    if (g_opt.core2) {
        out[n++] = 2;
    }
    return n;
}

/* ---- bandwidth ---- */

typedef enum { BW_READ, BW_WRITE, BW_COPY, BW_MEMCPY, BW_MEMSET, BW_COUNT } BwKind;
static const char* const k_bw_name[BW_COUNT] = {"read_ldm", "write_stm", "copy_ldmstm", "memcpy", "memset"};

static void bw_run(BwKind k, u8* a, u8* b, u32 size) {
    switch (k) {
    case BW_READ: k_read_ldm(a, size); break;
    case BW_WRITE: k_write_stm(a, size, 0x5A5A5A5Au); break;
    case BW_COPY: k_copy_ldmstm(b, a, size); break;
    case BW_MEMCPY: memcpy(b, a, size); break;
    case BW_MEMSET: memset(a, 0x33, size); break;
    default: break;
    }
}

/* Best and median MB/s of `trials` timed runs of `reps` passes. */
static void bw_measure(BwKind k, u8* a, u8* b, u32 size, double* best, double* med) {
    u32 reps = g_opt.budget / size;
    if (reps == 0) {
        reps = 1;
    }
    double v[16];
    int t = g_opt.trials > 16 ? 16 : g_opt.trials;
    bw_run(k, a, b, size); /* warm */
    for (int i = 0; i < t; i++) {
        u64 t0 = tick();
        for (u32 r = 0; r < reps; r++) {
            bw_run(k, a, b, size);
        }
        v[i] = mbps((u64)size * reps, tick() - t0);
    }
    qsort(v, t, sizeof(double), cmp_d);
    *best = v[t - 1];
    *med = v[t / 2];
}

typedef struct {
    MemId m;
    int core;
} JobArg;

static void bw_job(void* p) {
    JobArg* j = p;
    Region* r = &g_mem[j->m];
    u32 sizes[16];
    int ns = size_list(j->m, sizes);
    char note[32];
    snprintf(note, sizeof(note), "ran on core %d", (int)svcGetProcessorID());
    for (int k = 0; k < BW_COUNT; k++) {
        bool writes = k != BW_READ;
        bool two = k == BW_COPY || k == BW_MEMCPY;
        if (writes && !r->cpu_write) {
            continue;
        }
        for (int i = 0; i < ns; i++) {
            if (two && sizes[i] * 2 > r->size) {
                continue;
            }
            double best, med;
            bw_measure(k, r->base, r->base + sizes[i], sizes[i], &best, &med);
            rec("cpu_bw", k_bw_name[k], r->name, j->core, sizes[i], best, "MB/s", med, note);
        }
    }
    if (r->cpu_write) {
        cache_flush(r->base, r->size);
    }
}

/* ---- latency and random access ---- */

static const void* s_chain_start;

typedef struct {
    MemId m;
    int core;
    u32 size;
    const void* start;
    double ns;
} LatArg;

static void lat_run(void* p) {
    LatArg* a = p;
    u32 nodes = a->size / 32;
    u32 warm = nodes < g_opt.chase_loads ? nodes : g_opt.chase_loads;
    warm = (warm + 7) & ~7u;
    const void* q = k_chase(a->start, warm);
    u32 loads = g_opt.chase_loads;
    u64 t0 = tick();
    q = k_chase(q, loads);
    u64 dt = tick() - t0;
    s_chain_start = q; /* keeps the chase live */
    a->ns = t2us(dt) * 1000.0 / loads;
}

static bool chain_place(MemId m, u32 size, const void** start) {
    Region* r = &g_mem[m];
    if (r->cpu_write) {
        *start = chase_build((u32*)r->base, size, (u32)r->base, 0x1234567u + size);
        cache_flush(r->base, size);
        return *start != NULL;
    }
    if (m == MEM_VRAM && size <= 4 * QB_MIB) {
        /* Built in linear memory, copied in by the GPU. */
        *start = chase_build((u32*)g_lin_aux, size, (u32)r->base, 0x1234567u + size);
        cache_flush(g_lin_aux, size);
        bool ok = gx_copy(r->base, g_lin_aux, size);
        cache_inval(r->base, size);
        return ok && *start != NULL;
    }
    return false;
}

typedef struct {
    MemId m;
    int core;
    u32 size;
    bool write;
    double ns;
} RandArg;

static void rand_run(void* p) {
    RandArg* a = p;
    u32 mask = (a->size / 4 - 1) & ~7u;
    u32 n = g_opt.quick ? (1u << 16) : (1u << 20);
    u32* base = (u32*)g_mem[a->m].base;
    if (a->write) {
        k_rand_write(base, mask, n / 8, 99); /* warm */
        u64 t0 = tick();
        k_rand_write(base, mask, n, 7);
        a->ns = t2us(tick() - t0) * 1000.0 / n;
    } else {
        volatile u32 s = k_rand_read(base, mask, n / 8, 99);
        u64 t0 = tick();
        s += k_rand_read(base, mask, n, 7);
        a->ns = t2us(tick() - t0) * 1000.0 / n;
        (void)s;
    }
}

static void cpu_cross_copy(void) {
    static const struct {
        MemId a, b;
    } pairs[] = {
        {MEM_HEAP, MEM_QTM}, {MEM_QTM, MEM_HEAP},   {MEM_LINEAR, MEM_QTM}, {MEM_QTM, MEM_LINEAR},
        {MEM_HEAP, MEM_LINEAR}, {MEM_LINEAR, MEM_HEAP}, {MEM_VRAM, MEM_HEAP}, {MEM_VRAM, MEM_QTM},
    };
    const u32 size = QB_MIB;
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        Region* s = &g_mem[pairs[i].a];
        Region* d = &g_mem[pairs[i].b];
        if (!s->present || !d->present || !d->cpu_write) {
            continue;
        }
        /* Second MiB of the destination, so heap<->heap style overlaps cannot happen. */
        u8* dst = d->base + (d->size >= 2 * size ? size : 0);
        char name[20];
        snprintf(name, sizeof(name), "%s>%s", s->name, d->name);
        double best, med;
        bw_measure(BW_MEMCPY, s->base, dst, size, &best, &med);
        rec("cpu_xcopy", "memcpy", name, 0, size, best, "MB/s", med, NULL);
        bw_measure(BW_COPY, s->base, dst, size, &best, &med);
        rec("cpu_xcopy", "copy_ldmstm", name, 0, size, best, "MB/s", med, NULL);
        cache_flush(d->base, d->size);
    }
}

static void xcopy_job(void* p) {
    (void)p;
    cpu_cross_copy();
}

void cpu_suite(void) {
    int cores[3];
    int nc = core_list(cores);
    if (suite_on("cpu_bw")) {
        for (int m = 0; m < MEM_COUNT; m++) {
            if (!g_mem[m].present) {
                continue;
            }
            for (int c = 0; c < nc; c++) {
                status("cpu_bw %s core %d", g_mem[m].name, cores[c]);
                JobArg j = {(MemId)m, cores[c]};
                if (!run_on_core(cores[c], bw_job, &j)) {
                    rec("cpu_bw", "thread", g_mem[m].name, cores[c], 0, 0, "ok", NAN, "threadCreate refused");
                }
                ui_poll();
            }
        }
        status("cpu_xcopy");
        run_on_core(0, xcopy_job, NULL);
    }
    if (suite_on("cpu_lat")) {
        for (int m = 0; m < MEM_COUNT; m++) {
            if (!g_mem[m].present) {
                continue;
            }
            u32 sizes[16];
            int ns = size_list((MemId)m, sizes);
            status("cpu_lat %s", g_mem[m].name);
            for (int i = 0; i < ns; i++) {
                LatArg a = {(MemId)m, 0, sizes[i], NULL, 0};
                if (!chain_place((MemId)m, sizes[i], &a.start)) {
                    rec("cpu_lat", "chase", g_mem[m].name, -1, sizes[i], NAN, "ns", NAN, "chain not placed");
                    continue;
                }
                for (int c = 0; c < nc; c++) {
                    a.core = cores[c];
                    if (run_on_core(cores[c], lat_run, &a)) {
                        rec("cpu_lat", "chase", g_mem[m].name, cores[c], sizes[i], a.ns, "ns", NAN, NULL);
                    }
                }
            }
            if (g_mem[m].cpu_write) {
                memset(g_mem[m].base, 0, g_mem[m].size);
                cache_flush(g_mem[m].base, g_mem[m].size);
            }
            ui_poll();
        }
    }
    if (suite_on("cpu_rand")) {
        static const u32 rsz[] = {16u << 10, 256u << 10, 2u << 20, 8u << 20};
        for (int m = 0; m < MEM_COUNT; m++) {
            if (!g_mem[m].present) {
                continue;
            }
            status("cpu_rand %s", g_mem[m].name);
            for (unsigned i = 0; i < sizeof(rsz) / sizeof(rsz[0]); i++) {
                if (rsz[i] > g_mem[m].size) {
                    continue;
                }
                for (int c = 0; c < nc; c++) {
                    for (int w = 0; w < 2; w++) {
                        if (w && !g_mem[m].cpu_write) {
                            continue;
                        }
                        RandArg a = {(MemId)m, cores[c], rsz[i], w != 0, 0};
                        if (run_on_core(cores[c], rand_run, &a)) {
                            rec("cpu_rand", w ? "rand_write" : "rand_read", g_mem[m].name, cores[c], rsz[i], a.ns,
                                "ns", 1000.0 / a.ns, "aux = M accesses/s");
                        }
                    }
                }
            }
            if (g_mem[m].cpu_write) {
                cache_flush(g_mem[m].base, g_mem[m].size);
            }
            ui_poll();
        }
    }
}

/* ---- cache maintenance ---- */

static double time_call(int op, u8* p, u32 n, Result* rc) {
    u64 t0 = tick();
    switch (op) {
    case 0: *rc = GSPGPU_FlushDataCache(p, n); break;
    case 1: *rc = GSPGPU_InvalidateDataCache(p, n); break;
    case 2: *rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, n); break;
    case 3: *rc = svcStoreProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, n); break;
    case 4: *rc = svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, n); break;
    default: *rc = -1; break;
    }
    return t2us(tick() - t0);
}

static void cache_job(void* p) {
    (void)p;
    static const u32 sz[] = {4u << 10, 64u << 10, 256u << 10, 1u << 20, 2u << 20};
    static const char* const opname[5] = {"gsp_flush", "gsp_inval", "svc_flush", "svc_store", "svc_inval"};
    for (int m = 0; m < MEM_COUNT; m++) {
        Region* r = &g_mem[m];
        if (!r->present) {
            continue;
        }
        for (unsigned i = 0; i < sizeof(sz) / sizeof(sz[0]); i++) {
            u32 n = sz[i];
            for (int op = 0; op < 5; op++) {
                bool inval = op == 1 || op == 4;
                /* dirty: the range was just written (write-back has work to do) */
                for (int dirty = 0; dirty < 2; dirty++) {
                    if (dirty && (inval || !r->cpu_write)) {
                        continue;
                    }
                    double best = 1e30;
                    Result rc = 0;
                    for (int t = 0; t < 3; t++) {
                        if (dirty) {
                            memset(r->base, t, n);
                        } else {
                            k_read_ldm(r->base, n); /* clean lines present in the cache */
                        }
                        double us = time_call(op, r->base, n, &rc);
                        if (us < best) {
                            best = us;
                        }
                    }
                    char test[24], note[40];
                    snprintf(test, sizeof(test), "%s_%s", opname[op], dirty ? "dirty" : "clean");
                    snprintf(note, sizeof(note), "rc=%08lX aux=us per MiB", (unsigned long)rc);
                    rec("cache", test, r->name, 0, n, R_SUCCEEDED(rc) ? best : NAN, "us",
                        R_SUCCEEDED(rc) ? best * QB_MIB / n : NAN, note);
                }
            }
        }
        if (r->cpu_write) {
            cache_flush(r->base, r->size);
        }
    }
}

void cache_suite(void) {
    if (!suite_on("cache")) {
        return;
    }
    status("cache maintenance");
    run_on_core(0, cache_job, NULL);
}

/* ---- workers ---- */

static void worker_main(void* p) {
    Worker* w = p;
    while (!*w->go) {
    }
    ctrdk_perf_read(&w->p0);
    u64 t0 = tick();
    u64 units = 0;
    u32 chunk = w->size < (64u << 10) ? w->size : (64u << 10);
    u32 off = 0;
    const void* q = w->chase;
    while (!*w->stop) {
        switch (w->kind) {
        case W_READ: k_read_ldm(w->buf + off, chunk); break;
        case W_WRITE: k_write_stm(w->buf + off, chunk, (u32)units); break;
        case W_COPY: k_copy_ldmstm(w->buf2 + off, w->buf + off, chunk); break;
        case W_CHASE: q = k_chase(q, 4096); break;
        }
        if (w->kind == W_CHASE) {
            units += 4096;
        } else {
            units += chunk;
            off += chunk;
            if (off + chunk > w->size) {
                off = 0;
            }
        }
    }
    w->ticks = tick() - t0;
    ctrdk_perf_read(&w->p1);
    w->units = units;
    w->chase = q;
    w->ran = true;
}

bool worker_start(Worker* w) {
    w->ran = false;
    w->units = w->ticks = 0;
    w->th = spawn_on_core(w->core, worker_main, w, PRIO_WORK);
    return w->th != NULL;
}

void worker_join(Worker* w) {
    if (w->th != NULL) {
        threadJoin(w->th, U64_MAX);
        threadFree(w->th);
        w->th = NULL;
    }
}

double worker_rate(const Worker* w) {
    if (!w->ran || w->ticks == 0 || w->units == 0) {
        return NAN;
    }
    if (w->kind == W_CHASE) {
        return t2us(w->ticks) * 1000.0 / (double)w->units;
    }
    return mbps(w->units, w->ticks);
}

/* ---- two cores at once ---- */

typedef struct {
    const char* name;
    int core[2];
    u8* buf[2];
    u32 size[2];
    const char* mem[2];
} ConcCfg;

static void conc_one(const ConcCfg* c, int n) {
    static volatile int go, stop;
    Worker w[2];
    memset(w, 0, sizeof(w));
    go = stop = 0;
    for (int i = 0; i < n; i++) {
        w[i].core = c->core[i];
        w[i].kind = W_READ;
        w[i].buf = c->buf[i];
        w[i].size = c->size[i];
        w[i].go = &go;
        w[i].stop = &stop;
        if (!worker_start(&w[i])) {
            stop = go = 1;
            for (int k = 0; k < i; k++) {
                worker_join(&w[k]);
            }
            rec("conc", c->name, "-", c->core[i], 0, NAN, "MB/s", NAN, "threadCreate refused");
            return;
        }
    }
    svcSleepThread(5 * 1000000LL);
    go = 1;
    svcSleepThread((s64)g_opt.window_ms * 1000000LL);
    stop = 1;
    for (int i = 0; i < n; i++) {
        worker_join(&w[i]);
    }
    char mem[20];
    if (n == 1) {
        snprintf(mem, sizeof(mem), "c%d:%s", c->core[0], c->mem[0]);
        rec("conc", c->name, mem, c->core[0], c->size[0], worker_rate(&w[0]), "MB/s", NAN, NULL);
    } else {
        snprintf(mem, sizeof(mem), "c%d:%s+c%d:%s", c->core[0], c->mem[0], c->core[1], c->mem[1]);
        double a = worker_rate(&w[0]), b = worker_rate(&w[1]);
        char note[100];
        snprintf(note, sizeof(note), "aux = core %d MB/s; sum %.0f", c->core[1], a + b);
        rec("conc", c->name, mem, c->core[0], c->size[0], a, "MB/s", b, note);
    }
}

void conc_suite(void) {
    if (!suite_on("conc")) {
        return;
    }
    status("conc: two cores");
    svcSetThreadPriority(CUR_THREAD_HANDLE, PRIO_CTRL);
    u8* heapA = g_aggr[0];
    u8* heapB = g_aggr[1];
    u8* lin = g_mem[MEM_LINEAR].base;
    u8* qtm = g_mem[MEM_QTM].present ? g_mem[MEM_QTM].base : NULL;
    const u32 F = 4 * QB_MIB, Q = QTM_APP_SIZE;
    ConcCfg one[] = {
        {"single", {0, 0}, {heapA, NULL}, {F, 0}, {"heap", ""}},
        {"single", {0, 0}, {qtm, NULL}, {Q, 0}, {"qtm", ""}},
        {"single", {2, 0}, {heapA, NULL}, {F, 0}, {"heap", ""}},
    };
    ConcCfg two[] = {
        {"same_region", {0, 2}, {heapA, heapA}, {F, F}, {"heap", "heap"}},
        {"same_region", {0, 2}, {qtm, qtm}, {Q, Q}, {"qtm", "qtm"}},
        {"same_region", {0, 1}, {heapA, heapA}, {F, F}, {"heap", "heap"}},
        {"diff_buffers", {0, 2}, {heapA, heapB}, {F, F}, {"heap", "heap2"}},
        {"diff_buffers", {0, 2}, {heapA, lin}, {F, F}, {"heap", "linear"}},
        {"diff_region", {0, 2}, {qtm, heapB}, {Q, F}, {"qtm", "heap2"}},
        {"diff_region", {0, 2}, {heapA, qtm}, {F, Q}, {"heap", "qtm"}},
        {"diff_region", {0, 1}, {qtm, heapB}, {Q, F}, {"qtm", "heap2"}},
    };
    for (unsigned i = 0; i < sizeof(one) / sizeof(one[0]); i++) {
        if (one[i].buf[0] == NULL || (one[i].core[0] == 2 && !g_opt.core2)) {
            continue;
        }
        conc_one(&one[i], 1);
    }
    for (unsigned i = 0; i < sizeof(two) / sizeof(two[0]); i++) {
        ConcCfg* c = &two[i];
        if (c->buf[0] == NULL || c->buf[1] == NULL || (c->core[1] == 2 && !g_opt.core2) ||
            (c->core[1] == 1 && !g_opt.core1)) {
            continue;
        }
        conc_one(c, 2);
    }
    svcSetThreadPriority(CUR_THREAD_HANDLE, PRIO_MAIN);
}
