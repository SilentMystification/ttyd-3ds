/* ctr-devkit supervisor: the system module (development only).
 *
 * Started by PM (the app calls ctrdk_supervisor_ensure(), which uses PMAPP_LaunchTitle), it runs
 * until the console reboots, outside the app's process: the app hanging, crashing, being ended
 * or not running at all does not touch it. It must never take the console down with it:
 *   - fixed, small heap (libctru's default would take all free memory of the region);
 *   - no APT, no GSP, no HID: it never competes with the app or the HOME Menu for them;
 *   - every fault ends this process quietly: an exception handler on each thread and a wrapped
 *     svcBreak call svcExitProcess, so Luma3DS's exception screen (which needs a reboot) is
 *     never reached from here;
 *   - it ends on PM's termination notification (0x100); PM ends it by force after its timeout
 *     anyway (Luma3DS pm, termination.c), so it cannot hold up a shutdown or reboot.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <malloc.h>
#include <string.h>

#include <3ds.h>

#include "build_stamp.h"
#include "ctrdk/supervisor.h"
#include "sv_internal.h"

#define HEAP_SIZE 0x30000      /* malloc: thread stacks, buffers */
#define SOC_SIZE 0x20000       /* soc:U context, from the heap below (memalign) */
#define SERVER_STACK 0x4000
#define SERVER_PRIO 0x30       /* below the system services; on the system core it never takes the app's time */

/* ---- Start-up overrides (libctru's are for applications) ---- */

extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;

void __system_allocateHeaps(void) {
    u32 addr = 0;
    __ctru_heap_size = HEAP_SIZE + SOC_SIZE + 0x1000;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    if (R_FAILED(svcControlMemory(&addr, __ctru_heap, 0, __ctru_heap_size, MEMOP_ALLOC,
                                  (MemPerm)(MEMPERM_READ | MEMPERM_WRITE)))) {
        svcExitProcess();
    }
    __ctru_linear_heap = 0;
    __ctru_linear_heap_size = 0; /* nothing here needs linear (GPU/DSP) memory */
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
}

void __appInit(void) {
    /* srv may not be up yet at boot; 0xD88007FA is "port full / not ready", retried like Luma's own modules. */
    Result r;
    for (int i = 0; i < 200; i++) {
        r = srvInit();
        if (r != (Result)0xD88007FA) {
            break;
        }
        svcSleepThread(50000000LL);
    }
    if (R_FAILED(r)) {
        svcExitProcess();
    }
    if (R_FAILED(fsInit())) {
        svcExitProcess();
    }
    archiveMountSdmc();
}

void __appExit(void) {
    socExit();
    archiveUnmountAll();
    fsExit();
    srvExit();
}

/* libctru and newlib call svcBreak on a fatal error: end quietly instead (linked with --wrap=svcBreak). */
void __wrap_svcBreak(UserBreakType reason) {
    (void)reason;
    svcExitProcess();
}

/* ---- Faults ---- */

static u8 s_exc_stack[2][0x1000] __attribute__((aligned(8)));

static void on_exception(ERRF_ExceptionInfo* info, CpuRegisters* regs) {
    (void)info;
    (void)regs;
    svcExitProcess(); /* a fault here must never become an exception screen */
}

static void guard_thread(int i) { threadOnException(on_exception, s_exc_stack[i] + sizeof(s_exc_stack[i]), WRITE_DATA_TO_HANDLER_STACK); }

/* ---- Server thread ---- */

static volatile bool s_quit;
static SvConfig s_cfg;

static void server_main(void* arg) {
    (void)arg;
    guard_thread(1);
    void* soc_buf = memalign(0x1000, SOC_SIZE);
    /* soc:U is registered by the socket module; at boot it may come late. */
    for (;;) {
        if (s_quit) {
            return;
        }
        bool reg = false;
        if (soc_buf != NULL && R_SUCCEEDED(srvIsServiceRegistered(&reg, "soc:U")) && reg &&
            R_SUCCEEDED(socInit(soc_buf, SOC_SIZE))) {
            break;
        }
        svcSleepThread(1000000000LL);
    }
    sv_log("supervisor %s up", s_cfg.version);
    sv_serve(&s_cfg, &s_quit);
}

int main(void) {
    guard_thread(0);
    s_cfg.port = CTRDK_SV_PORT;
    s_cfg.kind = "sysmodule";
    s_cfg.version = CTRDK_BUILD_STAMP;
    sv_config_load(&s_cfg);

    Handle notif = 0;
    if (R_FAILED(srvEnableNotification(&notif))) {
        return 0;
    }
    Thread t = threadCreate(server_main, NULL, SERVER_STACK, SERVER_PRIO, -2, false);
    if (t == NULL) {
        return 0;
    }
    for (;;) {
        if (R_FAILED(svcWaitSynchronization(notif, -1LL))) {
            break;
        }
        u32 id = 0;
        if (R_FAILED(srvReceiveNotification(&id))) {
            continue;
        }
        if (id == 0x100) { /* PM: terminate (shutdown, reboot, or PMAPP_TerminateTitle) */
            break;
        }
    }
    s_quit = true;
    threadJoin(t, 2000000000LL); /* the server checks s_quit every 0.5 s */
    svcCloseHandle(notif);
    return 0;
}
