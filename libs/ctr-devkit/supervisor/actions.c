/* ctr-devkit supervisor: what it does to the game and the system (terminate, GPU reset, launch,
 * reboot, NDM). Every call here is bounded: PM, NS and AM return within their own timeouts, the
 * one call that can wait for another process (GSP's AcquireRight) runs on its own thread with a
 * 2 s limit, and nothing waits for the game.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "ctrdk/supervisor.h"
#include "policy.h"
#include "sv_internal.h"

#define MS_NS 1000000LL

/* ---- The watched title ---- */

/* svcGetProcessInfo type 0x10001 (the title ID) is a Luma3DS kernel extension; on anything else
 * every call fails and the state is "unknown". */
int sv_game_state(uint64_t tid, uint32_t* pid) {
    static u32 pids[96];
    s32 n = 0;
    bool known = false;
    if (pid != NULL) {
        *pid = 0;
    }
    if (R_FAILED(svcGetProcessList(&n, pids, (s32)(sizeof(pids) / sizeof(pids[0]))))) {
        return SV_GAME_UNKNOWN;
    }
    for (s32 i = 0; i < n; i++) {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) {
            continue;
        }
        s64 value = 0;
        Result r = svcGetProcessInfo(&value, h, 0x10001);
        svcCloseHandle(h);
        if (R_FAILED(r)) {
            continue;
        }
        known = true;
        if (tid != 0 && (uint64_t)value == tid) {
            if (pid != NULL) {
                *pid = pids[i];
            }
            return SV_GAME_RUNNING;
        }
    }
    return known ? SV_GAME_STOPPED : SV_GAME_UNKNOWN;
}

static const char* state_word(int s) { return s == SV_GAME_RUNNING ? "running" : s == SV_GAME_STOPPED ? "stopped" : "unknown"; }

static bool wait_gone(uint64_t tid, int ms) {
    for (int t = 0; t < ms; t += 100) {
        if (sv_game_state(tid, NULL) != SV_GAME_RUNNING) {
            return true;
        }
        svcSleepThread(100 * MS_NS);
    }
    return sv_game_state(tid, NULL) != SV_GAME_RUNNING;
}

/* ---- Terminate ---- */

int32_t sv_terminate(const SvConfig* cfg, uint64_t tid, char* report, size_t n) {
    uint32_t pid = 0;
    int before = sv_game_state(tid, &pid);
    if (before == SV_GAME_STOPPED) {
        snprintf(report, n, "terminate state=stopped (nothing to end)");
        return 0;
    }
    u64 t0 = svcGetSystemTick();
    Result r = pmAppInit();
    if (R_SUCCEEDED(r)) {
        r = PMAPP_TerminateTitle(tid, (s64)cfg->terminate_ms * MS_NS); /* 0x100, then by force after the timeout (F8) */
        pmAppExit();
    }
    bool gone = before == SV_GAME_UNKNOWN ? false : wait_gone(tid, 1000);
    Result kill = 0;
    bool forced = false;
    if (!gone && pid != 0) {
        /* PM did not end it (its call failed, or the process ignores even the forced end): the
         * kernel ends it. PM sees the process handle signal as for any exit. */
        Handle h;
        kill = svcOpenProcess(&h, pid);
        if (R_SUCCEEDED(kill)) {
            kill = svcTerminateProcess(h);
            svcCloseHandle(h);
        }
        forced = true;
        gone = wait_gone(tid, 1000);
    }
    unsigned ms = (unsigned)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000u));
    int after = sv_game_state(tid, NULL);
    snprintf(report, n, "terminate pm=%08lx pid=%lu ms=%u forced=%d kill=%08lx state=%s", (unsigned long)r,
             (unsigned long)pid, ms, forced ? 1 : 0, (unsigned long)kill, state_word(after));
    sv_log("supervisor: %s", report);
    return r;
}

/* ---- GPU reset (F13, test item H4) ---- */

static volatile int s_gpu_busy, s_gpu_level;
static volatile Result s_g_open, s_g_plain, s_g_acquire, s_g_reset, s_g_release;
static volatile bool s_g_done;

static Result gsp_call(Handle g, const u32* words, int count) {
    u32* c = getThreadCommandBuffer();
    memcpy(c, words, (size_t)count * 4u);
    Result r = svcSendSyncRequest(g);
    return R_FAILED(r) ? r : (Result)c[1];
}

static void gpu_thread(void* arg) {
    (void)arg;
    sv_guard_thread(3);
    Handle g = 0;
    s_g_open = srvGetServiceHandle(&g, "gsp::Gpu");
    if (R_SUCCEEDED(s_g_open)) {
        static const u32 reset[] = {0x001B0000};                 /* ResetGpuCore */
        static const u32 acquire[] = {0x00160042, 0, 0, 0xFFFF8001}; /* AcquireRight(0), this process */
        static const u32 release[] = {0x00170000};               /* ReleaseRight */
        s_g_plain = gsp_call(g, reset, 1);
        if (s_gpu_level >= 2) {
            s_g_acquire = gsp_call(g, acquire, 4); /* may wait while another process holds the right */
            if (R_SUCCEEDED(s_g_acquire)) {
                s_g_reset = gsp_call(g, reset, 1);
                s_g_release = gsp_call(g, release, 1);
            }
        }
        svcCloseHandle(g);
    }
    s_g_done = true;
    s_gpu_busy = 0;
}

void sv_gpu_reset(int level, char* report, size_t n) {
    if (level <= 0) {
        snprintf(report, n, "gpureset off");
        return;
    }
    if (s_gpu_busy) {
        snprintf(report, n, "gpureset skipped (the last attempt still waits in GSP)");
        sv_log("supervisor: %s", report);
        return;
    }
    s_gpu_busy = 1;
    s_gpu_level = level;
    s_g_done = false;
    s_g_open = s_g_plain = s_g_acquire = s_g_reset = s_g_release = -1;
    if (threadCreate(gpu_thread, NULL, 0x1000, 0x30, -2, true) == NULL) {
        s_gpu_busy = 0;
        snprintf(report, n, "gpureset failed (no thread)");
        return;
    }
    for (int t = 0; t < 2000 && !s_g_done; t += 20) {
        svcSleepThread(20 * MS_NS);
    }
    snprintf(report, n, "gpureset level=%d open=%08lx plain=%08lx acquire=%08lx reset=%08lx release=%08lx%s", level,
             (unsigned long)s_g_open, (unsigned long)s_g_plain, (unsigned long)s_g_acquire, (unsigned long)s_g_reset,
             (unsigned long)s_g_release, s_g_done ? "" : " (timed out after 2 s: GSP still holds the call)");
    sv_log("supervisor: %s", report);
}

/* ---- Launch ---- */

int32_t sv_launch(const SvConfig* cfg, uint64_t tid, const char* switches, char* report, size_t n) {
    static char text[0x300];
    if (sv_launch_args_text(text, sizeof(text), switches) < 0) {
        snprintf(report, n, "launch refused: switches too long");
        return -1;
    }
    mkdir("sdmc:/3ds", 0777);
    FILE* f = fopen(SV_LAUNCH_ARGS_FILE, "w");
    bool wrote = false;
    if (f != NULL) {
        wrote = fwrite(text, 1, strlen(text), f) == strlen(text);
        fclose(f);
    }
    Result r;
    u32 pid = 0;
    if (!cfg->launch_pm) {
        r = nsInit();
        if (R_SUCCEEDED(r)) {
            r = NS_LaunchTitle(tid, cfg->launch_flags, &pid);
            nsExit();
        }
    } else {
        r = pmAppInit();
        if (R_SUCCEEDED(r)) {
            FS_ProgramInfo p = {tid, ((tid >> 32) & 0x10) ? MEDIATYPE_NAND : MEDIATYPE_SD, {0}};
            r = PMAPP_LaunchTitle(&p, cfg->launch_flags);
            pmAppExit();
        }
    }
    if (R_FAILED(r)) {
        remove(SV_LAUNCH_ARGS_FILE); /* a later manual start must not pick up these switches */
    }
    snprintf(report, n, "launch %016llx via=%s flags=%lx rc=%08lx pid=%lu args_file=%s", (unsigned long long)tid,
             cfg->launch_pm ? "pm" : "ns", (unsigned long)cfg->launch_flags, (unsigned long)r, (unsigned long)pid,
             wrote ? "written" : "failed");
    sv_log("supervisor: %s", report);
    return r;
}

/* ---- Reboot ---- */

int32_t sv_reboot(uint64_t tid, bool hard, char* report, size_t n) {
    Result r = -1;
    if (!hard && tid != 0) {
        r = nsInit();
        if (R_SUCCEEDED(r)) {
            r = NS_RebootToTitle(MEDIATYPE_SD, tid, 0); /* comes back in the game, whose ensure() starts us (F11) */
            nsExit();
        }
        sv_log("supervisor: reboot to %016llx: %08lx", (unsigned long long)tid, (unsigned long)r);
        if (R_SUCCEEDED(r)) {
            svcSleepThread(15000 * MS_NS);
            sv_log("supervisor: no reboot 15 s after NS_RebootToTitle: hardware reset");
        }
    }
    Result h = APT_HardwareResetAsync(); /* no aptInit needed (as Rosalina) */
    sv_log("supervisor: APT_HardwareResetAsync: %08lx", (unsigned long)h);
    if (R_SUCCEEDED(h)) {
        svcSleepThread(15000 * MS_NS);
    }
    snprintf(report, n, "reboot failed: ns=%08lx apt=%08lx", (unsigned long)r, (unsigned long)h);
    return R_FAILED(h) ? h : -1;
}

/* ---- NDM (Q2, owner choice, off by default) ---- */

int32_t sv_ndm_hold(bool on) {
    static bool held;
    Result r = 0;
    if (on && !held) {
        r = ndmuInit();
        if (R_SUCCEEDED(r)) {
            r = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
            if (R_SUCCEEDED(r)) {
                r = NDMU_LockState();
                held = true;
            } else {
                ndmuExit();
            }
        }
    } else if (!on && held) {
        NDMU_UnlockState();
        r = NDMU_LeaveExclusiveState();
        ndmuExit();
        held = false;
    }
    return r;
}
