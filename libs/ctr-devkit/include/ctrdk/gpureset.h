/* ctr-devkit gpureset: get out of a hung PICA200.
 *
 * A bad command list can leave the PICA busy for good. Its P3D interrupt never comes, libctru's GX queue stays
 * "running" and every wait on it (citro3d's C3D_FrameBegin, C3D_Fini) never ends. The hang also outlives the
 * process: the next process's first frame hangs the same way. This module does the parts that do not depend on
 * the app:
 *   - gpureset_core: unstick libctru's GX queue and reset the GPU core (GSPGPU_ResetGpuCore). The caller
 *     then rebuilds its citro3d state (C3D_Fini, C3D_Init, render targets: see port3ds/plat/gpu.c);
 *   - gpureset_startup: before C3D_Init, send a tiny command list and wait for its interrupt with a time
 *     limit; on a timeout reset the core and try again. Run it after gfxInitDefault in every start;
 *   - GpuResetLimiter: "N resets within W ms" so an app can give up into its crash path.
 *
 * The reset sequence was only proven in Azahar (libs/ctr-devkit/gpuprobe); the console may need more.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_GPURESET_H
#define CTRDK_GPURESET_H

#include <stdbool.h>
#include <stdint.h>

#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GpuResetLog)(int level, const char* text, void* user); /* level: 0 error, 1 warning, 2 info */

/* Unstick libctru's GX queue (stop it and complete the entry the dead GPU never finishes), then reset the
 * GPU core. Any thread. Returns the GSPGPU_ResetGpuCore result. After it, a thread blocked in a GX queue
 * wait wakes up at the next GSP event (a vblank). */
Result gpureset_core(void);

/* 1: the GPU answered a minimal command list within timeout_ms (took_us set when not NULL); 0: it did not;
 * -1: the list could not be sent. Needs gfx up and no citro3d context (it uses libctru's global command
 * buffer, as the app's own would). */
int gpureset_probe(int timeout_ms, uint32_t* took_us);

/* Start-up check, before C3D_Init: 0 the GPU was fine, 1 it was hung and the reset fixed it, -1 it is still
 * dead after the reset (the app goes on: the watchdog and the crash path take over). */
int gpureset_startup(int timeout_ms, GpuResetLog log, void* user);

/* "At most max resets within window_ms". gpureset_allow records one attempt now and says whether it is
 * still within the limit. */
typedef struct {
    uint64_t stamp[8];
    int n;
} GpuResetLimiter;
bool gpureset_allow(GpuResetLimiter* lim, int max, int window_ms);

#ifdef __cplusplus
}
#endif

#endif
