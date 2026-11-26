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
 *   SKIP   not run (the GPU did not come back after a reset, or a needed resource is absent)
 * Command-processor cases (cb_jump*, cb_call*, cb_vram, cb_qtm, cb_irq) build the main list and the chunks by
 * hand; cp_thru measures command-processor time; dt_tile_* compares DisplayTransfer tiling with a CPU tiler.
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
    const char* dir;        /* output folder, default "sdmc:/3ds/qtmbench" */
    int timeout_ms;         /* wait per case; 0: 200 on hardware, 5000 on an emulator */
    const char* only;       /* comma list of case ids, or group names or id prefixes ending in '*'; NULL: all */
    bool fresh;             /* ignore and delete the journal: run every case again */
    bool selftest;          /* add the case that never submits (tests timeout and reset) */
    bool emulator;          /* running on Azahar/Citra: longer timeout */
    bool no_dangerous;      /* skip the cases of danger level 3 (memory faults possible) */
    void (*log)(const char* line, void* user); /* NULL: printf */
    void* user;
    /* Free New 3DS extra RAM ("QTM") the probe may use for the cb_qtm/vb_qtm/ib_qtm cases: CPU address, size and
     * physical address (0x1F000000 + offset). NULL: those cases are SKIP. */
    void* qtm;
    unsigned qtm_bytes;
    unsigned qtm_phys;
    /* dt_tile reference: the app's CPU tiler. Puts RGBA8 rows (row 0 first, bytes R,G,B,A) of w x h into the tiled
     * bytes of DisplayTransfer format fmt (GX_TRANSFER_FMT_*), as the app uploads a texture. Returns 0 when it has
     * no tiler for fmt; the probe's own Morton tiler (row 0 to the last tile row) is used then. NULL: always the
     * probe's own. */
    int (*tile_ref)(int fmt, const unsigned char* rgba, unsigned w, unsigned h, unsigned char* out, void* user);
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
