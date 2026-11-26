/* ctr-devkit supervisor: the heartbeat service "ctrdk:sv" (one IPC port).
 *
 * Command 1 (IPC header 0x00010080): word 1 the app's frame value, word 2 its flags
 * (CTRDK_SV_HB_*). Reply 0x00010080: word 1 result 0, word 2 the watchdog's limit in seconds (0 = off).
 * Anything else: 0x00000040 with 0xD900182F (invalid command).
 *
 * The thread does nothing but copy two words and reply, so a client never waits on it for more
 * than a context switch. It never calls into the app.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <string.h>

#include <3ds.h>

#include "ctrdk/supervisor.h"
#include "sv_internal.h"

#define MAX_SESSIONS 3
#define HB_STACK 0x1000
#define ERR_SESSION_CLOSED ((Result)0xC920181A)
#define ERR_INVALID_COMMAND ((Result)0xD900182F)

static Handle s_port, s_quit_ev;
static Thread s_thread;
static Result s_reg_rc = -1;
static LightLock s_lock;
static SvBeat s_beat;
static volatile uint32_t s_watch_s;
static volatile uint32_t s_sessions;

static void handle_command(void) {
    u32* c = getThreadCommandBuffer();
    if (c[0] == IPC_MakeHeader(1, 2, 0)) {
        LightLock_Lock(&s_lock);
        s_beat.frame = c[1];
        s_beat.flags = c[2];
        s_beat.count++;
        s_beat.tick = svcGetSystemTick();
        LightLock_Unlock(&s_lock);
        c[0] = IPC_MakeHeader(1, 2, 0);
        c[1] = 0;
        c[2] = s_watch_s;
        return;
    }
    c[0] = IPC_MakeHeader(0, 1, 0);
    c[1] = (u32)ERR_INVALID_COMMAND;
}

static void hb_main(void* arg) {
    (void)arg;
    sv_guard_thread(2);
    Handle h[2 + MAX_SESSIONS];
    s32 n = 2;
    h[0] = s_quit_ev;
    h[1] = s_port;
    Handle reply = 0;
    int errors = 0;
    for (;;) {
        s32 idx = -1;
        if (reply == 0) {
            getThreadCommandBuffer()[0] = 0xFFFF0000; /* nothing to reply to */
        }
        Result r = svcReplyAndReceive(&idx, h, n, reply);
        Handle replied = reply;
        reply = 0;
        if (r == ERR_SESSION_CLOSED) {
            /* idx is the closed session, or -1 when it was the one being replied to */
            s32 dead = idx;
            if (dead < 0) {
                for (s32 i = 2; i < n; i++) {
                    if (h[i] == replied) {
                        dead = i;
                    }
                }
            }
            if (dead >= 2 && dead < n) {
                svcCloseHandle(h[dead]);
                h[dead] = h[--n];
                s_sessions = (uint32_t)(n - 2);
            }
            continue;
        }
        if (R_FAILED(r)) {
            if (++errors > 50) {
                sv_log("supervisor: heartbeat service stops after errors (last %08lx)", (unsigned long)r);
                break;
            }
            svcSleepThread(100000000LL);
            continue;
        }
        errors = 0;
        if (idx == 0) {
            break; /* quit */
        }
        if (idx == 1) {
            Handle s = 0;
            if (R_SUCCEEDED(svcAcceptSession(&s, s_port))) {
                if (n < (s32)(sizeof(h) / sizeof(h[0]))) {
                    h[n++] = s;
                    s_sessions = (uint32_t)(n - 2);
                } else {
                    svcCloseHandle(s);
                }
            }
            continue;
        }
        handle_command();
        reply = h[idx];
    }
    for (s32 i = 2; i < n; i++) {
        svcCloseHandle(h[i]);
    }
    s_sessions = 0;
}

bool sv_hb_start(void) {
    LightLock_Init(&s_lock);
    memset(&s_beat, 0, sizeof(s_beat));
    s_reg_rc = srvRegisterService(&s_port, CTRDK_SV_SERVICE, MAX_SESSIONS);
    if (R_FAILED(s_reg_rc)) {
        sv_log("supervisor: register %s failed: %08lx", CTRDK_SV_SERVICE, (unsigned long)s_reg_rc);
        return false;
    }
    if (R_FAILED(svcCreateEvent(&s_quit_ev, RESET_ONESHOT))) {
        srvUnregisterService(CTRDK_SV_SERVICE);
        svcCloseHandle(s_port);
        s_reg_rc = -2;
        return false;
    }
    /* Above the server thread: a heartbeat is answered at once, also during a push. */
    s_thread = threadCreate(hb_main, NULL, HB_STACK, 0x2F, -2, false);
    if (s_thread == NULL) {
        srvUnregisterService(CTRDK_SV_SERVICE);
        svcCloseHandle(s_port);
        svcCloseHandle(s_quit_ev);
        s_reg_rc = -3;
        return false;
    }
    return true;
}

void sv_hb_stop(void) {
    if (s_thread == NULL) {
        return;
    }
    svcSignalEvent(s_quit_ev);
    threadJoin(s_thread, 1000000000LL);
    srvUnregisterService(CTRDK_SV_SERVICE);
    svcCloseHandle(s_port);
    svcCloseHandle(s_quit_ev);
    s_thread = NULL;
}

void sv_hb_get(SvBeat* out) {
    if (s_reg_rc != 0) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LightLock_Lock(&s_lock);
    *out = s_beat;
    LightLock_Unlock(&s_lock);
}

int32_t sv_hb_result(void) { return s_reg_rc; }
uint32_t sv_hb_sessions(void) { return s_sessions; }
void sv_hb_set_watch(uint32_t secs) { s_watch_s = secs; }
