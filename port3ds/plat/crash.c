/* Global exception handler, fatal stops, hang watchdog and process exit.
 *
 * Based on the Paper Mario 3DS port (pm_main.c crash_handler, pm_wdog.c):
 *   - Every thread installs the handler (threadOnException) on one shared
 *     16 KiB handler stack.
 *   - The report goes through plat_crash_line: the log queue is written
 *     first, then each [CRASH] line goes to log.txt, crash.txt (kept across
 *     boots), the live log, the debugger channel and the bottom console.
 *   - The process then ends with svcExitProcess: svcBreak left the console
 *     frozen on hardware. The system returns to the HOME Menu or the
 *     Homebrew Launcher.
 *   - A monitor thread (core 1, lowest priority) ends the process when the
 *     game loop makes no progress for wdog=N seconds (default 10) while the
 *     app is in the foreground, and when a crash handler itself hangs.
 *   - Azahar never calls the guest handler on a real fault, so the switch
 *     crashtest=self runs the handler directly with made-up registers.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plat_internal.h"

#define HANDLER_STACK_BYTES 0x4000
#define STACK_PATTERN 0xA5A5A5A5u
#define STACK_SLOTS 6

static u8 s_handler_stack[HANDLER_STACK_BYTES] __attribute__((aligned(8)));
static volatile const char* s_phase = "boot";
static char s_map[16] = "-";
static volatile u32 s_crash_ms; /* uptime in ms when a handler started, 0 if none */
static volatile u32 s_wd_hold;
static int s_wd_limit_s = 10;
static Thread s_wd_thread;
static volatile int s_wd_quit; /* set by plat_wd_quiesce: the monitor returns */
volatile uint32_t plat_wd_counter;

typedef struct {
    u32 lo, hi;
    const char* name;
} StackRec;
static StackRec s_stacks[STACK_SLOTS];

void plat_set_phase(const char* phase) { s_phase = phase; }

void plat_set_map_name(const char* map) {
    if (map == NULL) {
        map = "-";
    }
    strncpy(s_map, map, sizeof(s_map) - 1);
    s_map[sizeof(s_map) - 1] = '\0';
}

void plat_wd_progress(void) { plat_wd_counter++; }
void plat_wd_hold(int on) {
    if (on) {
        __atomic_fetch_add(&s_wd_hold, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_sub(&s_wd_hold, 1, __ATOMIC_RELAXED);
    }
}

/* A thread records its stack bounds at its entry (slot 0: game thread) and
 * fills the unused part with a pattern for the high-water mark. */
void plat_stack_register(int slot, const char* name, unsigned size) {
    if (slot < 0 || slot >= STACK_SLOTS) {
        return;
    }
    volatile u32 here;
    u32 top = (u32)&here;
    StackRec* r = &s_stacks[slot];
    r->name = name;
    r->hi = top + 512; /* the real top is a little above this frame */
    r->lo = (top + 512 - size) & ~3u;
    for (volatile u32* p = (volatile u32*)r->lo; (u32)p < top - 256; p++) {
        *p = STACK_PATTERN;
    }
}

static void crash_printf(const char* fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    plat_crash_line(buf, n);
}

/* Crash and watchdog exits only. svcExitProcess skips exit(), so the atexit
 * handlers (audio, apt, gfx, soc) never run and the process dies holding GSP
 * and the DSP. Normal exits go through plat_quit (boot.c). */
void plat_exit_process(void) {
    for (int i = 0; i < 100 && s_wd_hold; i++) {
        svcSleepThread(10000000LL); /* a running save gets up to 1 s */
    }
    plat_log_crash_close();
    svcExitProcess();
    for (;;) {
    }
}

/* Before a normal exit: waits for a running save (the hold, as above), then
 * stops the monitor so it cannot end the process while exit() runs. */
void plat_wd_quiesce(void) {
    for (int i = 0; i < 100 && s_wd_hold; i++) {
        svcSleepThread(10000000LL);
    }
    s_wd_quit = 1;
}

static void report_common(void) {
    crash_printf("[CRASH] phase %s, map %s, frames %lu, uptime %lu ms, build %s", s_phase, s_map,
                 (unsigned long)plat_wd_counter, (unsigned long)plat_uptime_ms(), plat_build_string());
    s64 used = 0;
    svcGetSystemInfo(&used, 0, 1);
    crash_printf("[CRASH] memory: APPLICATION used %lld KiB, linear free %lu KiB, QTM %s", used / 1024,
                 (unsigned long)(linearSpaceFree() / 1024), plat_qtm_available() ? "on" : "off");
}

static void crash_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    static const char* const names[] = {"prefetch abort", "data abort", "undefined instruction", "VFP"};
    s_crash_ms = plat_uptime_ms() | 1u;
    const StackRec* st = NULL;
    for (int i = 0; i < STACK_SLOTS; i++) {
        if (s_stacks[i].hi && regs->sp >= s_stacks[i].lo && regs->sp <= s_stacks[i].hi) {
            st = &s_stacks[i];
        }
    }
    crash_printf("\n[CRASH] %s thread=%s pc=%08lx lr=%08lx sp=%08lx far=%08lx fsr=%08lx cpsr=%08lx",
                 excep->type < 4 ? names[excep->type] : "?", st ? st->name : "other", (unsigned long)regs->pc,
                 (unsigned long)regs->lr, (unsigned long)regs->sp, (unsigned long)excep->far, (unsigned long)excep->fsr,
                 (unsigned long)regs->cpsr);
    for (int i = 0; i < 13; i += 4) {
        char line[160];
        int n = snprintf(line, sizeof(line), "[CRASH]");
        for (int j = i; j < i + 4 && j < 13; j++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " r%d=%08lx", j, (unsigned long)regs->r[j]);
        }
        crash_printf("%s", line);
    }
    report_common();
    /* Stack scan inside the recorded bounds: code-like words are candidate
     * return addresses, resolved offline against the archived ELF. */
    if (st != NULL && regs->sp >= st->lo && regs->sp < st->hi) {
        crash_printf("[CRASH] stack %s: %lu bytes in use", st->name, (unsigned long)(st->hi - regs->sp));
        const u32* sp = (const u32*)(regs->sp & ~3u);
        const u32* end = (const u32*)st->hi;
        char line[200];
        int n = snprintf(line, sizeof(line), "[CRASH] stack");
        for (int i = 0, found = 0; sp + i < end && i < 4096 && found < 48; i++) {
            u32 v = sp[i];
            if (v < 0x00100000u || v >= 0x02000000u) {
                continue;
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %08lx", (unsigned long)v);
            if (++found % 12 == 0) {
                crash_printf("%s", line);
                n = snprintf(line, sizeof(line), "[CRASH] stack");
            }
        }
        crash_printf("%s", line);
    } else {
        crash_printf("[CRASH] sp %08lx is outside the known stacks, no stack scan", (unsigned long)regs->sp);
    }
    crash_printf("[CRASH] exiting the process");
    plat_exit_process();
}

void plat_crash_install_thread(void) {
    threadOnException(crash_handler, s_handler_stack + HANDLER_STACK_BYTES, WRITE_DATA_TO_HANDLER_STACK);
}

void plat_fatal(const char* fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    s_crash_ms = plat_uptime_ms() | 1u;
    crash_printf("\n[CRASH] fatal: %s", msg);
    report_common();
    crash_printf("[CRASH] exiting the process");
    plat_exit_process();
}

static void watchdog_main(void* arg) {
    (void)arg;
    u32 last = plat_wd_counter;
    int stalled = 0;
    for (;;) {
        svcSleepThread(1000000000LL);
        if (s_wd_quit) {
            return;
        }
        u32 started = s_crash_ms;
        if (started) {
            if (plat_uptime_ms() - started >= 3000u) {
                static const char m[] = "[CRASH] watchdog: the crash handler did not finish in 3 s, ending the process\n";
                plat_crash_line(m, (int)sizeof(m) - 1);
                plat_exit_process();
            }
            continue;
        }
        u32 now = plat_wd_counter;
        /* Not armed before the first frame; a suspended app (HOME Menu,
         * sleep) and a hold (saving) are not hangs. */
        if (s_wd_limit_s <= 0 || now == 0 || now != last || s_wd_hold || !aptIsActive()) {
            last = now;
            stalled = 0;
            continue;
        }
        if (++stalled < s_wd_limit_s) {
            continue;
        }
        crash_printf("\n[CRASH] watchdog: no frame for %d s", stalled);
        report_common();
        crash_printf("[CRASH] exiting the process");
        plat_exit_process();
    }
}

static void selftest(void) {
    ERRF_ExceptionInfo e;
    CpuRegisters r;
    memset(&e, 0, sizeof(e));
    memset(&r, 0, sizeof(r));
    e.type = ERRF_EXCEPTION_DATA_ABORT;
    e.fsr = 0x805;
    e.far = 4;
    for (int i = 0; i < 13; i++) {
        r.r[i] = 0x1000u + (u32)i;
    }
    r.sp = (u32)(uintptr_t)&e;
    r.lr = (u32)(uintptr_t)__builtin_return_address(0);
    r.pc = (u32)(uintptr_t)&selftest;
    r.cpsr = 0x60000010u;
    crash_handler(&e, &r);
}

void plat_crash_init(void) {
    plat_crash_install_thread();
    s_wd_limit_s = plat_switch_int("wdog", 10);
    s_wd_thread = threadCreate(watchdog_main, NULL, 8 * 1024, 0x3F, 1, false);
    if (s_wd_thread == NULL) {
        s_wd_thread = threadCreate(watchdog_main, NULL, 8 * 1024, 0x3F, 0, false);
    }
    PLOG_I("watchdog: %d s, monitor thread %s", s_wd_limit_s, s_wd_thread ? "started" : "NOT STARTED");

    /* crashtest=self (handler called directly, works in Azahar),
     * null (null write), undef (undefined instruction), fatal, hang. */
    char test[16];
    if (plat_switch_value("crashtest", test, sizeof(test))) {
        PLOG_W("crashtest=%s", test);
        if (strcmp(test, "self") == 0) {
            selftest();
        } else if (strcmp(test, "null") == 0) {
            volatile u32* volatile target = (volatile u32*)(uintptr_t)plat_switch_int("crashaddr", 4);
            *target = 1;
        } else if (strcmp(test, "undef") == 0) {
            __builtin_trap();
        } else if (strcmp(test, "fatal") == 0) {
            plat_fatal("crashtest fatal");
        }
    }
}
