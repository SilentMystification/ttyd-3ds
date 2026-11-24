/* qtmbench: shared declarations.
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef QTMBENCH_BENCH_H
#define QTMBENCH_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include <3ds.h>

#include "ctrdk/perf.h"

#define QB_DIR "sdmc:/3ds/qtmbench"
#define QB_TICKS_PER_SEC 268111856.0 /* svcGetSystemTick (SYSCLOCK_ARM11) */
#define QB_MIB (1024u * 1024u)
#define QTM_VADDR 0x1E800000u
#define QTM_APP_SIZE 0x00200000u /* the application part of the mapping */

/* Worker threads run below the GSP event thread (0x1A) so GPU completion
 * events are delivered while a core is hammered; the controlling thread
 * runs above the workers so it can stop them. */
#define PRIO_CTRL 0x20
#define PRIO_WORK 0x28
#define PRIO_MAIN 0x30

typedef enum { MEM_HEAP, MEM_LINEAR, MEM_QTM, MEM_VRAM, MEM_COUNT } MemId;

typedef struct {
    const char* name;
    u8* base;
    u32 size;
    bool present;
    bool cpu_write; /* the mapping is writable by the CPU */
    bool gpu;       /* has a physical address the GPU can use */
} Region;

extern Region g_mem[MEM_COUNT];
extern u8* g_aggr[2]; /* two 4 MiB heap buffers for CPU aggressors */
extern u8* g_lin_aux; /* 4 MiB linear: GPU staging, display-transfer load, NDSP */

typedef struct {
    bool quick;          /* short run (emulators): fewer sizes, smaller budgets */
    bool emulator;       /* Azahar/Citra (svcGetSystemInfo 0x20000) */
    u32 budget;          /* bytes moved per bandwidth trial */
    int trials;          /* trials per point; best and median reported */
    u32 chase_loads;     /* loads per latency point */
    u32 window_ms;       /* concurrency / contention measuring window */
    bool core1, core2;   /* cores usable for tests */
    int gpu_core;        /* core of the GPU feeder threads in contend; -1 = core 1 */
    bool pte;            /* walk the page tables (svcBackdoor) at the end */
    char only[64];       /* comma list of suites, "" = all */
    bool autoexit;
} Opts;
extern Opts g_opt;

/* out.c ---------------------------------------------------------------- */
void out_open(void);
void out_close(void);
void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void status(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
/* One result. core -1 and size 0 mean "n/a". aux: a second number (median,
 * baseline, ...); NAN when unused. */
void rec(const char* suite, const char* test, const char* mem, int core, u32 size, double value, const char* unit,
         double aux, const char* note);
void info(const char* key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void out_write(void); /* results.json and results.txt (may be called again) */
bool suite_on(const char* name);
void ui_poll(void);   /* keeps the screen and devupdate alive between tests */

/* timing */
static inline u64 tick(void) { return svcGetSystemTick(); }
static inline double t2us(u64 t) { return (double)t * 1e6 / QB_TICKS_PER_SEC; }
static inline double mbps(u64 bytes, u64 ticks) { return ticks ? (double)bytes / t2us(ticks) : 0.0; }

/* Runs fn(arg) on a new thread on `core` at PRIO_WORK and waits for it.
 * false if the kernel refused the thread (core not available). */
bool run_on_core(int core, void (*fn)(void*), void* arg);
Thread spawn_on_core(int core, void (*fn)(void*), void* arg, int prio);

/* mem.c ---------------------------------------------------------------- */
bool mem_setup(void);
void mem_report(void);   /* svcQueryMemory, permission and alias tests */
void mem_pte_walk(void); /* page table attributes through svcBackdoor (console only) */
void cache_flush(const void* p, u32 n); /* GSPGPU_FlushDataCache */
void cache_inval(const void* p, u32 n); /* GSPGPU_InvalidateDataCache */
void fill_pattern(u32* p, u32 words, u32 seed);
/* Builds a random single-cycle pointer chain of `size` bytes at `base`,
 * one node per 32-byte line, with pointers valid at `addr_base` (the CPU
 * address the chain will be read through). Returns the start pointer. */
const void* chase_build(u32* base, u32 size, u32 addr_base, u32 seed);

/* kern.s --------------------------------------------------------------- */
void k_read_ldm(const void* p, u32 bytes);
void k_write_stm(void* p, u32 bytes, u32 value);
void k_copy_ldmstm(void* dst, const void* src, u32 bytes);
const void* k_chase(const void* p, u32 loads);
u32 k_rand_read(const u32* base, u32 mask, u32 loads, u32 seed);
void k_rand_write(u32* base, u32 mask, u32 stores, u32 seed);

/* Concurrent workers (cpu.c), started together and stopped by the controller. */
typedef enum { W_READ, W_WRITE, W_COPY, W_CHASE } WorkKind;
typedef struct {
    int core;
    WorkKind kind;
    u8* buf;
    u8* buf2;
    u32 size;
    const void* chase;
    volatile int* go;
    volatile int* stop;
    /* results */
    u64 units; /* bytes, or loads for W_CHASE */
    u64 ticks;
    CtrdkPerfSample p0, p1;
    bool ran;
    Thread th;
} Worker;
bool worker_start(Worker* w);
void worker_join(Worker* w);
double worker_rate(const Worker* w); /* MB/s, or ns per load for W_CHASE */

/* cpu.c ---------------------------------------------------------------- */
void cpu_suite(void);
void cache_suite(void);
void conc_suite(void);

/* gpu.c ---------------------------------------------------------------- */
bool gpu_init(void);
void gpu_exit(void);
void gpu_suite(void);
void coherency_suite(void);
/* GPU loads for the contention matrix: a prepared command list or transfer
 * that streams from one memory; gpu_load_once runs it once and returns the
 * microseconds it took (< 0 on a timeout). Bytes it reads per run. */
typedef enum { GLOAD_TEX, GLOAD_VTX, GLOAD_DT, GLOAD_COUNT } GLoad;
bool gpu_load_prepare(GLoad kind, MemId src);
double gpu_load_once(GLoad kind, MemId src);
u32 gpu_load_bytes(GLoad kind);
void gpu_clear_timeouts(void); /* the contention cells start fresh */

/* contend.c ------------------------------------------------------------ */
void contend_suite(void);
void perf_suite(void);

#endif
