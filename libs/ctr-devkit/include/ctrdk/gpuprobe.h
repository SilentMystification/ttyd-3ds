/* ctr-devkit gpuprobe: finds which PICA200 command patterns hang, fault or draw wrong.
 *
 * Each case is a minimal citro3d (or raw GPUCMD) setup and one draw into a 64x64 render
 * target, with a known-good control next to the risky variants. Per case the probe submits
 * the list, waits with a timeout, reads the target back and classifies the case:
 *   OK     the expected pixels, nothing written outside the target
 *   WRONG  finished, but other pixels, or memory beside the target was written
 *   HANG   the GPU never signalled completion (the probe then resets the GPU and goes on)
 *   FAULT  the process died during the case (found on the next run, from the journal)
 *   ERR    GSP refused the command list (no GPU work happened)
 *   SKIP   not run (the GPU did not come back after a reset)
 * Results go to <dir>/gpuprobe.txt (table), <dir>/gpuprobe.journal (resume file) and
 * <dir>/gpuprobe/<case>.bin (the PICA command list of each case, for gpu_cmd_diff.py).
 *
 * Needs gfxInitDefault() done by the caller. libctru and citro3d only.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_GPUPROBE_H
#define CTRDK_GPUPROBE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* dir;        /* output folder, default "sdmc:/3ds/ctrdk" */
    int timeout_ms;         /* wait per case; 0: 200 on hardware, 5000 on an emulator */
    const char* only;       /* comma list of case ids, or group names or id prefixes ending in '*'; NULL: all */
    bool fresh;             /* ignore and delete the journal: run every case again */
    bool selftest;          /* add the case that never submits (tests timeout and reset) */
    bool emulator;          /* running on Azahar/Citra: longer timeout */
    bool no_dangerous;      /* skip the cases of danger level 3 (memory faults possible) */
    void (*log)(const char* line, void* user); /* NULL: printf */
    void* user;
} GpuProbeConfig;

typedef struct {
    int total, ok, wrong, hang, fault, err, skip, cached;
} GpuProbeSummary;

/* Runs the probe. Returns the number of cases that did not give OK. */
int gpuprobe_run(const GpuProbeConfig* cfg, GpuProbeSummary* out);

#ifdef __cplusplus
}
#endif

#endif
