/* Dolphin OS pieces with no deeper port machinery: alarms, cache
 * maintenance, the SI probe and the DEMO statistics hooks. */
#include <dolphin/os.h>
#include <dolphin/si.h>
#include <string.h>

#include "plat/memstable.h"
#include "plat/plat.h"
#include "plat/thread.h"
#include "port3ds/os_internal.h"

/* ---- Alarms ----
 * A timer host thread sleeps until the earliest alarm and runs its handler
 * as an interrupt (holding the interrupt lock), as the GameCube's decrementer
 * exception does. TTYD uses one alarm (seq_e3.c, the kiosk idle timer). */
static OSAlarm* s_alarms;
static PlatEvent* s_alarm_wake;
static PlatThread* s_alarm_thread;

static void alarm_unlink(OSAlarm* a) {
    OSAlarm** p = &s_alarms;
    while (*p != NULL && *p != a) {
        p = &(*p)->next;
    }
    if (*p == a) {
        *p = a->next;
    }
    a->next = a->prev = NULL;
}

static void alarm_thread(void* arg) {
    (void)arg;
    for (;;) {
        port_irq_enter();
        OSTime now = OSGetTime();
        OSAlarm* due = NULL;
        OSTime next = 0;
        for (OSAlarm* a = s_alarms; a != NULL; a = a->next) {
            if (a->fire <= now) {
                due = a;
                break;
            }
            if (next == 0 || a->fire < next) {
                next = a->fire;
            }
        }
        if (due != NULL) {
            OSAlarmHandler handler = due->handler;
            alarm_unlink(due);
            if (due->period > 0) {
                due->fire += due->period;
                due->next = s_alarms;
                s_alarms = due;
            } else {
                due->handler = NULL;
            }
            if (handler != NULL) {
                handler(due, NULL);
            }
            port_irq_leave();
            port_thread_dispatch_if_idle();
            continue;
        }
        port_irq_leave();
        if (next == 0) {
            plat_event_wait(s_alarm_wake);
        } else {
            s64 ns = (s64)OSTicksToMicroseconds(next - now) * 1000;
            plat_event_wait_timeout(s_alarm_wake, ns > 0 ? ns : 0);
        }
    }
}

void OSCreateAlarm(OSAlarm* alarm) {
    memset(alarm, 0, sizeof(*alarm));
}

static void alarm_insert(OSAlarm* alarm, OSTime fire, OSTime period, OSAlarmHandler handler) {
    BOOL level = OSDisableInterrupts();
    if (s_alarm_thread == NULL) {
        s_alarm_wake = plat_event_create();
        s_alarm_thread = plat_thread_create(alarm_thread, NULL, 16 * 1024, 0x2F, 1, "alarm");
        if (s_alarm_thread == NULL) {
            plat_fatal("OSSetAlarm: no timer thread");
        }
    }
    alarm_unlink(alarm);
    alarm->handler = handler;
    alarm->fire = fire;
    alarm->period = period;
    alarm->next = s_alarms;
    s_alarms = alarm;
    OSRestoreInterrupts(level);
    plat_event_signal(s_alarm_wake);
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    alarm_insert(alarm, OSGetTime() + tick, 0, handler);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) { alarm_insert(alarm, time, 0, handler); }

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    alarm_insert(alarm, start + period, period, handler);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL level = OSDisableInterrupts();
    alarm_unlink(alarm);
    alarm->handler = NULL;
    OSRestoreInterrupts(level);
}

/* ---- Cache maintenance ----
 * On the GameCube these keep the CPU caches coherent with the GPU, DSP and
 * DVD DMA. On the port no game-visible memory is read or written by another
 * bus master behind the CPU's back: the GX layer converts every texture,
 * vertex array and display list on the CPU and does its own flushes of the
 * buffers it hands to the PICA200, the audio layer flushes the buffers it
 * gives the DSP, and file reads are CPU copies. The calls therefore have
 * nothing to do; they are kept so the game's ordering stays as written.
 * A flush or store still says "the CPU wrote here", which the GameCube
 * requires before the GPU reads CPU-written memory: it goes to the memory
 * registry (include/plat/memstable.h), which caches of that memory rely on. */
void DCFlushRange(void* addr, u32 nBytes) { plat_memstable_write(addr, nBytes); }
void DCStoreRange(void* addr, u32 nBytes) { plat_memstable_write(addr, nBytes); }
void DCInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCFlushRangeNoSync(void* addr, u32 nBytes) { plat_memstable_write(addr, nBytes); }
void DCStoreRangeNoSync(void* addr, u32 nBytes) { plat_memstable_write(addr, nBytes); }
void ICInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }

/* ---- SI ----
 * The action command in ac_repeatedly.c reads SIProbe(0) for the controller
 * type: the port always has a standard GameCube controller on port 1. */
u32 SIProbe(s32 chan) { return chan == 0 ? SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE; }

/* ---- DEMO statistics (DEMOStats.c) ----
 * The game shows them when DemoStatEnable is set (never, in the retail
 * game); the port reports the frame's GX counters to the log instead. */
GXBool DemoStatEnable = GX_FALSE;

void DEMOUpdateStats(GXBool inc) { (void)inc; }

void DEMOPrintStats(void) {
    PLOG_D("DEMO stats: scheduler switches %u", port_thread_switch_count());
}
