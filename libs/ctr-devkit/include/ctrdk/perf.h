/* ctr-devkit perf: the ARM11 MPCore performance monitor (per core: the cycle
 * counter CCNT and two event counters PMN0/PMN1) and, when the kernel
 * provides them, the SCU event counters (shared by all cores).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * User mode cannot touch CP15 c15 on the ARM11 MPCore (privileged only, and
 * the core has no user-enable bit), so there are two ways in:
 *   CTRDK_PERF_SVC       the kernel's svcControlPerformanceCounter (SVC 0x3E);
 *                        CP15 and SCU counters, 64-bit when the kernel keeps
 *                        virtual counters. The ExHeader must allow it:
 *                            SystemCallAccess:
 *                              ControlPerformanceCounter: 62
 *   CTRDK_PERF_BACKDOOR  svcBackdoor (SVC 0x7B; Luma implements it, the
 *                        ExHeader must allow "Backdoor: 123"): a few CP15
 *                        instructions in kernel mode on the calling core.
 *                        CP15 counters only, 32 bits (diffs wrap correctly
 *                        up to 2^32 cycles, 5.3 s at 804 MHz).
 * CTRDK_PERF_AUTO tries the SVC, then the backdoor (never in an emulator).
 *
 * The CP15 counters belong to the core the calling thread runs on: read them
 * on the thread you measure, and keep that thread on one core. Events are
 * programmed on each core lazily, at that core's next read after
 * ctrdk_perf_init or ctrdk_perf_set_events; a sample taken across a change
 * of events is marked invalid by ctrdk_perf_diff.
 *
 *     ctrdk_perf_init(NULL);              // defaults: D-cache read miss, I-cache miss
 *     CtrdkPerfSample a, b, d;
 *     ctrdk_perf_read(&a);  ... work ...  ctrdk_perf_read(&b);
 *     ctrdk_perf_diff(&a, &b, &d);
 *     char line[160];
 *     ctrdk_perf_format(&d, line, sizeof(line));  // "core0 cyc=.. drmiss=.. imiss=.."
 *
 * Everything is safe when no way in works (Azahar): ctrdk_perf_available()
 * is false and samples have valid == 0.
 */
#ifndef CTRDK_PERF_H
#define CTRDK_PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTRDK_PERF_SCU_MAX 8

typedef enum { CTRDK_PERF_AUTO = 0, CTRDK_PERF_SVC = 1, CTRDK_PERF_BACKDOOR = 2, CTRDK_PERF_NONE = 3 } CtrdkPerfBackend;

/* ARM11 MPCore event numbers (the same as libctru's PerfCounterEvent core events). */
enum {
    CTRDK_EV_ICACHE_MISS = 0x00,
    CTRDK_EV_STALL_IBUF = 0x01,     /* instruction buffer cannot deliver */
    CTRDK_EV_STALL_DATA_DEP = 0x02, /* stall on a data dependency */
    CTRDK_EV_IMICRO_TLB_MISS = 0x03,
    CTRDK_EV_DMICRO_TLB_MISS = 0x04,
    CTRDK_EV_BRANCH = 0x05,
    CTRDK_EV_BRANCH_NOT_PREDICTED = 0x06,
    CTRDK_EV_BRANCH_MISPREDICT = 0x07,
    CTRDK_EV_INST = 0x08,
    CTRDK_EV_FOLDED_INST = 0x09,
    CTRDK_EV_DCACHE_READ = 0x0A,
    CTRDK_EV_DCACHE_READ_MISS = 0x0B,
    CTRDK_EV_DCACHE_WRITE = 0x0C,
    CTRDK_EV_DCACHE_WRITE_MISS = 0x0D,
    CTRDK_EV_DCACHE_EVICTION = 0x0E,
    CTRDK_EV_PC_CHANGE = 0x0F,
    CTRDK_EV_MAIN_TLB_MISS = 0x10,
    CTRDK_EV_EXTERNAL_REQUEST = 0x11, /* explicit external data access */
    CTRDK_EV_STALL_LSU_FULL = 0x12,
    CTRDK_EV_STORE_BUFFER_DRAIN = 0x13,
    CTRDK_EV_STORE_BUFFER_MERGE = 0x14,
    CTRDK_EV_CYCLES = 0xFF,
};

typedef struct {
    int backend;    /* CtrdkPerfBackend; default AUTO */
    int pmn0_event; /* default CTRDK_EV_DCACHE_READ_MISS */
    int pmn1_event; /* default CTRDK_EV_ICACHE_MISS */
    int nscu;       /* SVC backend: SCU counters to program, 0..8; -1 = all present */
    int scu_event[CTRDK_PERF_SCU_MAX]; /* libctru PERFCOUNTEREVT_SCU_* values; default: external
                                          read, external write, line-fill miss from core 0/1/2,
                                          read busy port 0/1, write busy port 0 */
} CtrdkPerfConfig;

typedef struct {
    uint64_t cycles; /* CCNT of the core that read it (CPU cycles) */
    uint64_t pmn0, pmn1;
    uint64_t scu[CTRDK_PERF_SCU_MAX];
    uint64_t tick;   /* svcGetSystemTick at the read */
    int core;        /* the core the sample was read on */
    int nscu;
    uint32_t gen;    /* event generation the counters were counting */
    uint32_t valid;  /* bit 0 core counters, bit 1 SCU counters */
} CtrdkPerfSample;

void ctrdk_perf_defaults(CtrdkPerfConfig* cfg);
/* Picks the way in, programs the events on the calling core and resets
 * the counters. Returns 0, or the result of the last way tried. */
int32_t ctrdk_perf_init(const CtrdkPerfConfig* cfg);
void ctrdk_perf_exit(void);
bool ctrdk_perf_available(void);
CtrdkPerfBackend ctrdk_perf_backend(void);
const char* ctrdk_perf_backend_name(void);
int32_t ctrdk_perf_last_error(void);
/* New events for PMN0/PMN1 (any thread); each core switches at its next read. */
void ctrdk_perf_set_events(int pmn0_event, int pmn1_event);
void ctrdk_perf_read(CtrdkPerfSample* out);
/* The calling core's CCNT/PMN0/PMN1 only (3 SVCs with the SVC backend, 1 with
 * the backdoor; ctrdk_perf_read adds one per SCU counter). */
void ctrdk_perf_read_core(CtrdkPerfSample* out);
/* b - a; valid only where both are, on the same core and event generation. */
void ctrdk_perf_diff(const CtrdkPerfSample* a, const CtrdkPerfSample* b, CtrdkPerfSample* out);
/* Adds d into acc (same events), for per-frame sums. */
void ctrdk_perf_add(CtrdkPerfSample* acc, const CtrdkPerfSample* d);
/* One line, no newline; returns the length written. */
int ctrdk_perf_format(const CtrdkPerfSample* d, char* buf, size_t n);
/* Short event names ("drmiss", "imiss", "brmiss", "ext", "scu_rd", ...) and back. */
const char* ctrdk_perf_event_name(int event);
int ctrdk_perf_event_from_name(const char* name); /* -1 if unknown */
const CtrdkPerfConfig* ctrdk_perf_config(void);

#ifdef __cplusplus
}
#endif

#endif
