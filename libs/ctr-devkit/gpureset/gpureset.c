/* ctr-devkit gpureset: see include/ctrdk/gpureset.h.
 *
 * Why the GX queue needs a push: libctru's gxCmdQueueWait (what citro3d's C3D_FrameBegin and C3D_Fini wait
 * in) loops until the global isRunning flag drops. That flag drops in gxCmdQueueInterrupt, which the GSP event
 * thread calls for each PSC0, PPF, P3D and DMA interrupt, once per queue entry. A command list the PICA never
 * finishes never gives its P3D interrupt, so the flag stays up for good. Resetting the core does not give the
 * missing interrupt either (not known on a console). So gpureset_core sets the queue's stop flag and then
 * calls the interrupt function itself once: with the stop flag set it ends the queue (curQueue NULL,
 * isRunning 0) instead of sending the next entry. Whatever entries were left in the queue are dropped
 * by the owner of the queue (citro3d clears it in C3D_Fini).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include "ctrdk/gpureset.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* libctru defines it (weak, gxqueue.o) and the GSP event thread calls it; gx.h does not declare it. */
void gxCmdQueueInterrupt(GSPGPU_Event id);

Result gpureset_core(void) {
    gxCmdQueueStop(NULL); /* uses only the global state: the argument is not read */
    Result rc = GSPGPU_ResetGpuCore();
    gxCmdQueueInterrupt(GSPGPU_EVENT_P3D);
    return rc;
}

static LightEvent s_ev;
static void on_p3d(void* user) { LightEvent_Signal((LightEvent*)user); }

int gpureset_probe(int timeout_ms, uint32_t* took_us) {
    static u32* buf;
    if (buf == NULL) {
        buf = linearMemAlign(256, 16);
        if (buf == NULL) {
            return -1;
        }
    }
    GPUCMD_SetBuffer(buf, 64, 0);
    GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, 0);
    u32* list;
    u32 words;
    GPUCMD_Split(&list, &words); /* appends the FINALIZE write: the P3D interrupt */
    GSPGPU_FlushDataCache(list, words * 4);
    LightEvent_Init(&s_ev, RESET_STICKY);
    gspSetEventCallback(GSPGPU_EVENT_P3D, on_p3d, &s_ev, false);
    u64 t0 = svcGetSystemTick();
    Result rc = GX_ProcessCommandList(list, words * 4, 0);
    int ok = -1;
    if (R_SUCCEEDED(rc)) {
        ok = LightEvent_WaitTimeout(&s_ev, (s64)timeout_ms * 1000000LL) == 0 ? 1 : 0;
    }
    gspSetEventCallback(GSPGPU_EVENT_P3D, NULL, NULL, false);
    if (took_us != NULL) {
        *took_us = (u32)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000u));
    }
    return ok;
}

static void say(GpuResetLog log, void* user, int level, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void say(GpuResetLog log, void* user, int level, const char* fmt, ...) {
    if (log == NULL) {
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log(level, line, user);
}

int gpureset_startup(int timeout_ms, GpuResetLog log, void* user) {
    u32 us = 0;
    int r = gpureset_probe(timeout_ms, &us);
    if (r == 1) {
        say(log, user, 2, "gpu: start-up check: the GPU answered in %lu us", (unsigned long)us);
        return 0;
    }
    if (r < 0) {
        say(log, user, 1, "gpu: start-up check: could not send the test command list");
        return 0;
    }
    say(log, user, 1, "gpu: start-up check: no answer in %d ms, the GPU was left hung by an earlier run: resetting it",
        timeout_ms);
    Result rc = gpureset_core();
    r = gpureset_probe(timeout_ms * 2, &us);
    if (r == 1) {
        say(log, user, 1, "gpu: start-up reset (rc %08lX) worked, the GPU answered in %lu us", (unsigned long)rc,
            (unsigned long)us);
        return 1;
    }
    say(log, user, 0, "gpu: start-up reset (rc %08lX) did not bring the GPU back", (unsigned long)rc);
    return -1;
}

bool gpureset_allow(GpuResetLimiter* lim, int max, int window_ms) {
    if (max > 8) {
        max = 8;
    }
    u64 now = svcGetSystemTick();
    u64 window = (u64)window_ms * (SYSCLOCK_ARM11 / 1000u);
    int keep = 0;
    for (int i = 0; i < lim->n; i++) {
        if (now - lim->stamp[i] < window) {
            lim->stamp[keep++] = lim->stamp[i];
        }
    }
    lim->n = keep;
    if (lim->n >= max) {
        return false;
    }
    lim->stamp[lim->n++] = now;
    return true;
}
