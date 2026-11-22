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
 *
 * One report path (crash_report) serves exceptions, plat_fatal, the
 * watchdog and svcBreak (the linker wraps it: libctru and citro3d panics
 * call svcBreak). It writes the [CRASH] lines, then shows a crash screen on
 * both screens (what happened on top, the call chain on the bottom) until
 * START or 5 minutes, then releases GSP and the DSP and ends the process.
 * The watchdog thread ends the process if a report takes over 5 s before the
 * screen is up (or the exit takes over 4.5 s). A second fault in the report
 * ends the process at once; another thread's fault while a report runs just
 * parks that thread.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plat_internal.h"

#define HANDLER_STACK_BYTES 0x4000
#define STACK_PATTERN 0xA5A5A5A5u
#define MAX_THREADS 24
#define CHAIN_MAX 64

static u8 s_handler_stack[HANDLER_STACK_BYTES] __attribute__((aligned(8)));
static volatile const char* s_phase = "boot";
static char s_map[16] = "-";
static volatile u32 s_crash_ms; /* uptime in ms when a handler started, 0 if none */
static volatile u32 s_wd_hold;
static int s_wd_limit_s = 10;
static Thread s_wd_thread;
static volatile int s_wd_quit; /* set by plat_wd_quiesce: the monitor returns */
volatile uint32_t plat_wd_counter;

/* Every port thread registers itself at its entry: name, a duplicate of its
 * handle (for svcGetThreadContext from the watchdog) and its stack bounds. */
typedef struct {
    u32 lo, hi;
    Handle handle;
    char name[12];
} StackRec;
static StackRec s_stacks[MAX_THREADS];
static volatile int s_nstacks;
static volatile int s_frame_thread = -1; /* the thread that calls plat_wd_progress */
static void (*s_audio_abort)(void);
void plat_set_audio_abort(void (*fn)(void)) { s_audio_abort = fn; }

void plat_set_phase(const char* phase) { s_phase = phase; }

void plat_set_map_name(const char* map) {
    if (map == NULL) {
        map = "-";
    }
    strncpy(s_map, map, sizeof(s_map) - 1);
    s_map[sizeof(s_map) - 1] = '\0';
}

static void crashtest_frame(void);

void plat_wd_progress(void) {
    plat_wd_counter++;
    if (s_frame_thread < 0) {
        u32 here = (u32)&here;
        for (int i = 0; i < s_nstacks && i < MAX_THREADS; i++) {
            if (s_stacks[i].hi && here >= s_stacks[i].lo && here <= s_stacks[i].hi) {
                s_frame_thread = i;
            }
        }
    }
    crashtest_frame();
}
void plat_wd_hold(int on) {
    if (on) {
        __atomic_fetch_add(&s_wd_hold, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_sub(&s_wd_hold, 1, __ATOMIC_RELAXED);
    }
}

/* A thread records its stack bounds at its entry and fills the unused part
 * with a pattern for the high-water mark. Returns nothing; a full table just
 * leaves the thread out of the dumps. */
void plat_thread_register(const char* name, unsigned size) {
    int slot = __atomic_fetch_add(&s_nstacks, 1, __ATOMIC_RELAXED);
    if (slot >= MAX_THREADS) {
        return;
    }
    volatile u32 here;
    u32 top = (u32)&here;
    StackRec* r = &s_stacks[slot];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->handle = 0;
    svcDuplicateHandle(&r->handle, CUR_THREAD_HANDLE);
    r->lo = (top + 512 - size) & ~3u;
    for (volatile u32* p = (volatile u32*)r->lo; (u32)p < top - 256; p++) {
        *p = STACK_PATTERN;
    }
    r->hi = top + 512; /* the real top is a little above this frame; set last: it marks the slot valid */
}

void plat_stack_register(int slot, const char* name, unsigned size) {
    (void)slot;
    plat_thread_register(name, size);
}

static const StackRec* stack_of(u32 sp) {
    const StackRec* st = NULL;
    for (int i = 0; i < s_nstacks && i < MAX_THREADS; i++) {
        if (s_stacks[i].hi && sp >= s_stacks[i].lo && sp <= s_stacks[i].hi) {
            st = &s_stacks[i];
        }
    }
    return st;
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
void plat_cpu_limit_restore(void); /* boot.c */

void plat_exit_process(void) {
    for (int i = 0; i < 100 && s_wd_hold; i++) {
        svcSleepThread(10000000LL); /* a running save gets up to 1 s */
    }
    plat_cpu_limit_restore();
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

/* ---- The one report path ---- */

typedef struct {
    const char* kind;   /* "data abort", "fatal", "svcBreak", "watchdog", ... */
    char detail[200];   /* the fatal message, the break reason, the watchdog text */
    const char* thread; /* name of the thread that failed, or "other" */
    u32 pc, lr, sp, far, fsr, cpsr;
    int is_exception;
    const u32* regs;    /* r0-r12 for an exception, else NULL */
} CrashInfo;

static volatile int s_in_crash;
static volatile s32 s_crash_tid = -1; /* the thread running the report */
static volatile u32 s_deadline_ms;    /* uptime at which the watchdog ends the process, 0 = none */
static u32 s_chain[CHAIN_MAX];
static int s_nchain;

static int mem_readable(u32 addr) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) {
        return 0;
    }
    return (mi.perm & MEMPERM_READ) != 0 && mi.state != MEMSTATE_FREE;
}

/* The code-like words on a stack from sp up to its recorded top: candidate
 * return addresses, resolved offline (tools/port3ds/crash_report.py). Prints
 * them and keeps them for the crash screen. */
static void stack_scan(const StackRec* st, u32 sp) {
    s_nchain = 0;
    if (st == NULL || sp < st->lo || sp >= st->hi) {
        crash_printf("[CRASH] sp %08lx is outside the known stacks, no stack scan", (unsigned long)sp);
        return;
    }
    crash_printf("[CRASH] stack %s: %lu bytes in use", st->name, (unsigned long)(st->hi - sp));
    const u32* p = (const u32*)(sp & ~3u);
    const u32* end = (const u32*)st->hi;
    char line[200];
    int n = snprintf(line, sizeof(line), "[CRASH] stack");
    int found = 0;
    for (int i = 0; p + i < end && i < 4096 && found < 48; i++) {
        if (((u32)(p + i) & 0xFFF) == 0 && !mem_readable((u32)(p + i))) {
            break;
        }
        u32 v = p[i];
        if (v < 0x00100000u || v >= 0x02000000u) {
            continue;
        }
        if (s_nchain < CHAIN_MAX) {
            s_chain[s_nchain++] = v;
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08lx", (unsigned long)v);
        if (++found % 12 == 0) {
            crash_printf("%s", line);
            n = snprintf(line, sizeof(line), "[CRASH] stack");
        }
    }
    if (found % 12 != 0) {
        crash_printf("%s", line);
    }
}

/* SVC 0x3B, not declared by libctru: (ThreadContext* out, Handle thread).
 * The context is r0-r12, sp, lr, pc, cpsr, then the VFP registers. */
static Result get_thread_context(u32* ctx, Handle thread) {
    register u32 r0 __asm__("r0") = (u32)ctx;
    register u32 r1 __asm__("r1") = thread;
    register u32 r2 __asm__("r2") = 0;
    __asm__ volatile("svc 0x3B" : "+r"(r0), "+r"(r1), "+r"(r2) : : "r3", "r12", "memory", "cc");
    return (Result)r0;
}

/* Registers and the top of the stack of every other port thread, for a hang.
 * Result of SVC 0x3B is logged; Azahar may refuse it. The stuck thread's
 * context is returned in ctx_out (when got). */
static int dump_threads(int stuck, u32* ctx_out) {
    u32 me = (u32)&me;
    int got_stuck = 0;
    for (int i = 0; i < s_nstacks && i < MAX_THREADS; i++) {
        const StackRec* r = &s_stacks[i];
        if (r->hi == 0 || r->handle == 0 || (me >= r->lo && me <= r->hi)) {
            continue; /* not valid yet, or the thread running this dump */
        }
        u32 ctx[64];
        memset(ctx, 0, sizeof(ctx));
        Result res = get_thread_context(ctx, r->handle);
        if (R_FAILED(res)) {
            crash_printf("[CRASH] thread %s%s: svcGetThreadContext failed (%08lx)", r->name, i == stuck ? " (stuck)" : "",
                         (unsigned long)res);
            continue;
        }
        if (ctx[15] == 0 && ctx[13] == 0) {
            crash_printf("[CRASH] thread %s%s: svcGetThreadContext returned success but an empty context (not "
                         "implemented here?)", r->name, i == stuck ? " (stuck)" : "");
            continue;
        }
        crash_printf("[CRASH] thread %s%s: pc=%08lx lr=%08lx sp=%08lx cpsr=%08lx", r->name, i == stuck ? " (stuck)" : "",
                     (unsigned long)ctx[15], (unsigned long)ctx[14], (unsigned long)ctx[13], (unsigned long)ctx[16]);
        u32 sp = ctx[13] & ~3u;
        for (int row = 0; row < 4; row++) {
            u32 w[4];
            int ok = 1;
            for (int k = 0; k < 4; k++) {
                u32 a = sp + (u32)(row * 16 + k * 4);
                if (!mem_readable(a)) {
                    ok = 0;
                    break;
                }
                w[k] = *(const u32*)a;
            }
            if (!ok) {
                crash_printf("[CRASH]   %08lx: unreadable", (unsigned long)(sp + (u32)row * 16));
                break;
            }
            crash_printf("[CRASH]   %08lx: %08lx %08lx %08lx %08lx", (unsigned long)(sp + (u32)row * 16),
                         (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3]);
        }
        if (i == stuck) {
            memcpy(ctx_out, ctx, 17 * sizeof(u32));
            got_stuck = 1;
        }
    }
    return got_stuck;
}

void plat_set_audio_abort(void (*fn)(void));
void plat_crash_gpu_down(void); /* boot.c: citro3d and gfx down without waiting on other threads */
int plat_crash_file_ok(void);   /* log.c */

static PrintConsole s_top_con, s_bot_con;

static void crash_screen(const CrashInfo* ci) {
    plat_console_trylock(); /* taken for good: the log must not draw on the old console */
    plat_crash_gpu_down();
    gfxInitDefault();
    gfxSet3D(false);
    consoleInit(GFX_TOP, &s_top_con);
    consoleInit(GFX_BOTTOM, &s_bot_con);
    s_deadline_ms = 0; /* the screen is up: the watchdog stops timing the report */

    consoleSelect(&s_top_con);
    printf("TTYD 3DS crashed\nbuild %s\n\n", plat_build_string());
    printf("%s%s%s\n", ci->kind, ci->detail[0] ? ": " : "", ci->detail);
    printf("thread %s\n", ci->thread);
    printf("pc %08lx  lr %08lx\n", (unsigned long)ci->pc, (unsigned long)ci->lr);
    if (ci->is_exception) {
        printf("far %08lx  fsr %08lx\n", (unsigned long)ci->far, (unsigned long)ci->fsr);
    }
    printf("map %s  frame %lu\n", s_map, (unsigned long)plat_wd_counter);
    printf("crash.txt written: %s\n", plat_crash_file_ok() ? "yes" : "no");
    printf("\nSTART: exit\n");

    consoleSelect(&s_bot_con);
    printf("call chain (stack scan)\n");
    int rows = 0;
    for (int i = 0; i < s_nchain && rows < 27; i += 2, rows++) {
        if (i + 1 < s_nchain) {
            printf("%08lx %08lx\n", (unsigned long)s_chain[i], (unsigned long)s_chain[i + 1]);
        } else {
            printf("%08lx\n", (unsigned long)s_chain[i]);
        }
    }
    if (s_nchain == 0) {
        printf("(none)\n");
    }

    u64 start = svcGetSystemTick();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        if (svcGetSystemTick() - start > (u64)plat_switch_int("crashwait", 300) * PLAT_TICKS_PER_SEC) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    s_deadline_ms = (plat_uptime_ms() + 4500u) | 1u; /* the release below must not hang the console */
    gfxExit();
}

static void crash_report(CrashInfo* ci, int thread_dump_stuck, const u32* sp_for_scan) {
    s32 tid = -1;
    svcGetThreadId((u32*)&tid, CUR_THREAD_HANDLE);
    if (__atomic_exchange_n(&s_in_crash, 1, __ATOMIC_ACQ_REL)) {
        if (tid == s_crash_tid) {
            plat_exit_process(); /* the report itself faulted */
        }
        for (;;) {
            svcSleepThread(1000000000LL); /* another thread's problem now: leave the screen to the first */
        }
    }
    s_crash_tid = tid;
    s_crash_ms = plat_uptime_ms() | 1u;
    s_deadline_ms = (s_crash_ms + 5000u) | 1u;
    if (ci->is_exception) {
        crash_printf("\n[CRASH] %s thread=%s pc=%08lx lr=%08lx sp=%08lx far=%08lx fsr=%08lx cpsr=%08lx", ci->kind,
                     ci->thread, (unsigned long)ci->pc, (unsigned long)ci->lr, (unsigned long)ci->sp,
                     (unsigned long)ci->far, (unsigned long)ci->fsr, (unsigned long)ci->cpsr);
        for (int i = 0; i < 13; i += 4) {
            char line[160];
            int n = snprintf(line, sizeof(line), "[CRASH]");
            for (int j = i; j < i + 4 && j < 13; j++) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " r%d=%08lx", j, (unsigned long)ci->regs[j]);
            }
            crash_printf("%s", line);
        }
    } else {
        crash_printf("\n[CRASH] %s: %s (thread=%s pc=%08lx lr=%08lx sp=%08lx)", ci->kind, ci->detail, ci->thread,
                     (unsigned long)ci->pc, (unsigned long)ci->lr, (unsigned long)ci->sp);
    }
    report_common();
    (void)sp_for_scan;
    if (ci->sp != 0) { /* the watchdog has no registers of its own to scan */
        stack_scan(stack_of(ci->sp), ci->sp);
    }
    if (thread_dump_stuck) {
        u32 ctx[17];
        int stuck = s_frame_thread;
        if (stuck >= 0 && stuck < MAX_THREADS) {
            crash_printf("[CRASH] the watchdog thinks %s is stuck (it makes the frame progress calls)",
                         s_stacks[stuck].name);
        } else {
            crash_printf("[CRASH] the watchdog does not know which thread is stuck");
        }
        if (dump_threads(stuck, ctx)) {
            ci->pc = ctx[15];
            ci->lr = ctx[14];
            ci->sp = ctx[13];
            ci->thread = s_stacks[stuck].name;
            stack_scan(stack_of(ci->sp), ci->sp);
        }
    }
    crash_printf("[CRASH] showing the crash screen");
    crash_screen(ci);
    crash_printf("[CRASH] exiting the process");
    if (s_audio_abort != NULL) {
        s_audio_abort();
    }
    plat_exit_process();
}

static void crash_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    static const char* const names[] = {"prefetch abort", "data abort", "undefined instruction", "VFP"};
    const StackRec* st = stack_of(regs->sp);
    CrashInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.kind = excep->type < 4 ? names[excep->type] : "exception";
    ci.thread = st ? st->name : "other";
    ci.pc = regs->pc;
    ci.lr = regs->lr;
    ci.sp = regs->sp;
    ci.far = excep->far;
    ci.fsr = excep->fsr;
    ci.cpsr = regs->cpsr;
    ci.is_exception = 1;
    ci.regs = regs->r;
    crash_report(&ci, 0, NULL);
}

void plat_crash_install_thread(void) {
    threadOnException(crash_handler, s_handler_stack + HANDLER_STACK_BYTES, WRITE_DATA_TO_HANDLER_STACK);
}

static const char* thread_name_of(u32 sp) {
    const StackRec* st = stack_of(sp);
    return st ? st->name : "other";
}

void plat_fatal(const char* fmt, ...) {
    CrashInfo ci;
    memset(&ci, 0, sizeof(ci));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ci.detail, sizeof(ci.detail), fmt, ap);
    va_end(ap);
    ci.kind = "fatal";
    ci.sp = (u32)__builtin_frame_address(0);
    ci.thread = thread_name_of(ci.sp);
    ci.pc = (u32)__builtin_return_address(0);
    crash_report(&ci, 0, NULL);
    for (;;) {
    }
}

/* Every svcBreak (the linker wraps it, -Wl,--wrap=svcBreak): libctru and
 * citro3d call it on a panic. The real one leaves the console stuck, so it is
 * never called. */
void __wrap_svcBreak(UserBreakType reason) __attribute__((noreturn));
void __wrap_svcBreak(UserBreakType reason) {
    static const char* const names[] = {"PANIC", "ASSERT", "USER", "LOADRO", "UNLOADRO"};
    CrashInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.kind = "svcBreak";
    snprintf(ci.detail, sizeof(ci.detail), "reason %d (%s)", (int)reason,
             (unsigned)reason < 5 ? names[reason] : "?");
    ci.sp = (u32)__builtin_frame_address(0);
    ci.thread = thread_name_of(ci.sp);
    ci.pc = (u32)__builtin_return_address(0);
    ci.lr = ci.pc;
    crash_report(&ci, 0, NULL);
    for (;;) {
    }
}

/* The watchdog thread runs the report in a thread of its own: the report may
 * be the thing that hangs, and the watchdog must still be able to end the
 * process. */
static int s_wd_stalled_s;
static void hang_report_main(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    CrashInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.kind = "watchdog";
    snprintf(ci.detail, sizeof(ci.detail), "no frame for %d s", s_wd_stalled_s);
    int ft = s_frame_thread;
    ci.thread = (ft >= 0 && ft < MAX_THREADS) ? s_stacks[ft].name : "?";
    crash_report(&ci, 1, NULL);
}

static void watchdog_main(void* arg) {
    (void)arg;
    u32 last = plat_wd_counter;
    int stalled = 0;
    for (;;) {
        svcSleepThread(s_crash_ms ? 100000000LL : 1000000000LL);
        if (s_wd_quit) {
            return;
        }
        u32 started = s_crash_ms;
        if (started) {
            u32 dl = s_deadline_ms;
            if (dl && (s32)(plat_uptime_ms() - dl) >= 0) {
                static const char m[] = "[CRASH] watchdog: the crash handler did not finish in time, ending the process\n";
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
        s_wd_stalled_s = stalled;
        s_crash_ms = plat_uptime_ms() | 1u; /* the report below is timed from here */
        s_deadline_ms = (s_crash_ms + 5000u) | 1u;
        Thread t = threadCreate(hang_report_main, NULL, 16 * 1024, 0x3F, 1, false);
        if (t == NULL) {
            t = threadCreate(hang_report_main, NULL, 16 * 1024, 0x3F, 0, false);
        }
        if (t == NULL) {
            plat_exit_process();
        }
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

/* Switch crashtest=<kind>: self (the handler called directly: Azahar never
 * calls the guest handler on a real fault), null/undef/fatal at boot, and at
 * frame crashframe (default 60) on the game thread: segv (write to address
 * 0), fatal, break (svcBreak(USERBREAK_PANIC)), hang (endless loop). */
static char s_test[16];

__attribute__((noinline)) static void crashtest_segv(void) {
    volatile u32* volatile target = (volatile u32*)0;
    *target = 1;
}
__attribute__((noinline)) static void crashtest_fatal(void) { plat_fatal("crashtest fatal"); }
__attribute__((noinline)) static void crashtest_break(void) {
    svcBreak(USERBREAK_PANIC);
    __asm__ volatile("" ::: "memory"); /* no tail call: the return address stays in this function */
}
__attribute__((noinline)) static void crashtest_hang(void) {
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}

static void crashtest_frame(void) {
    if (s_test[0] == '\0' || (int)plat_wd_counter != plat_switch_int("crashframe", 60)) {
        return;
    }
    PLOG_W("crashtest=%s at frame %lu", s_test, (unsigned long)plat_wd_counter);
    if (strcmp(s_test, "segv") == 0) {
        crashtest_segv();
    } else if (strcmp(s_test, "fatal") == 0) {
        crashtest_fatal();
    } else if (strcmp(s_test, "break") == 0) {
        crashtest_break();
    } else if (strcmp(s_test, "hang") == 0) {
        crashtest_hang();
    }
}

void plat_crash_init(void) {
    plat_crash_install_thread();
    s_wd_limit_s = plat_switch_int("wdog", 10);
    s_wd_thread = threadCreate(watchdog_main, NULL, 8 * 1024, 0x3F, 1, false);
    if (s_wd_thread == NULL) {
        s_wd_thread = threadCreate(watchdog_main, NULL, 8 * 1024, 0x3F, 0, false);
    }
    PLOG_I("watchdog: %d s, monitor thread %s", s_wd_limit_s, s_wd_thread ? "started" : "NOT STARTED");

    if (plat_switch_value("crashtest", s_test, sizeof(s_test))) {
        PLOG_W("crashtest=%s", s_test);
        if (strcmp(s_test, "self") == 0) {
            selftest();
        } else if (strcmp(s_test, "null") == 0) {
            volatile u32* volatile target = (volatile u32*)(uintptr_t)plat_switch_int("crashaddr", 4);
            *target = 1;
        } else if (strcmp(s_test, "undef") == 0) {
            __builtin_trap();
        } else if (strcmp(s_test, "boot-fatal") == 0) {
            plat_fatal("crashtest fatal");
        }
    }
}
