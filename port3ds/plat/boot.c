/* Process entry for the TTYD 3DS port.
 *
 * Order matters:
 *   1. New 3DS check (the port is N3DS only), clock and L2 for a 3dsx (a CIA
 *      gets them from its ExHeader).
 *   2. Working directory sdmc:/3ds/ttyd, debug switches, log (direct writes
 *      until the writer thread runs), exception handler.
 *   3. Core 1 time limit and the APT hook, memory placement (QTM), log
 *      writer and watchdog on core 1.
 *   4. The game: ttyd_main() is the decomp's main(), which never returns.
 *      The frame loop calls plat_frame_poll(); when the system asks the app
 *      to close, plat_shutdown() ends the process cleanly.
 */
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <3ds/3dslink.h>

#include "plat_internal.h"

extern u32 __ctru_heap_size, __ctru_linear_heap_size;
#include "port3ds_build.h"

#define DATA_DIR "sdmc:/3ds/ttyd"

/* The decomp's main(), renamed for the port (port3ds/CMakeLists.txt). */
extern int ttyd_main(void);

/* Main thread stack for a 3dsx (a CIA takes StackSize from the ExHeader). */
u32 __stacksize__ = 0x40000;

static int s_new3ds;
static int s_netload;
static u64 s_boot_tick;
static u32 s_cpu_limit = 80;
static u32 s_cpu_limit_orig = 30; /* the limit before this process set its own */
static int s_cpu_restored;
static volatile int s_cpu_home_early; /* HOME press already lowered the limit (set before the suspend hook) */
static aptHookCookie s_apt_cookie;

void plat_cpu_limit_restore(void); /* below */

uint64_t plat_ticks(void) { return svcGetSystemTick(); }
uint64_t plat_boot_tick(void) { return s_boot_tick; }
uint32_t plat_uptime_ms(void) { return (uint32_t)((svcGetSystemTick() - s_boot_tick) / (PLAT_TICKS_PER_SEC / 1000u)); }
int plat_is_new3ds(void) { return s_new3ds; }

/* Heap and linear memory in use, for the requirements checked at boot. */
void plat_memory_log(const char* when) {
    struct mallinfo mi = mallinfo();
    PLOG_I("memory %s: heap %lu KiB in use of %lu (arena %lu), linear %lu KiB free of %lu", when,
           (unsigned long)(mi.uordblks >> 10), (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(mi.arena >> 10),
           (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(__ctru_linear_heap_size >> 10));
}
uint64_t plat_rtc_ms_since_1900(void) { return osGetTime(); }
int plat_started_by_netload(void) { return s_netload; }
const char* plat_build_string(void) { return PORT3DS_BUILD_STRING; }

static void cpu_limit_set(u32 value, const char* why) {
    Result r = APT_SetAppCpuTimeLimit(value);
    PLOG_I("cpu limit: %lu%% (%s) result %08lx", (unsigned long)value, why, (unsigned long)r);
}

/* Called from the vblank thread (60 Hz, independent of the game's frame rate)
 * the first time aptShouldJumpToHome() is true, i.e. as soon as libctru's APT
 * event thread has seen the HOME signal. The suspend hook below only runs
 * inside aptMainLoop on the game thread, once per game frame, so this gets
 * core 1 back to the HOME Menu sooner. Idempotent; the restore hook clears it.
 * Makes an IPC call and takes no lock the game thread can hold. */
void plat_cpu_limit_home_pressed(void) {
    if (s_cpu_home_early) {
        return;
    }
    s_cpu_home_early = 1;
    char why[48];
    snprintf(why, sizeof why, "HOME pressed, early, vblank %lu", (unsigned long)plat_vblank_count());
    cpu_limit_set(30, why);
}

/* Runs in the thread that calls aptMainLoop (the game thread). The HOME Menu
 * and sleep get core 1 back; closing them restores the game's value. */
static void apt_hook(APT_HookType type, void* param) {
    (void)param;
    switch (type) {
    case APTHOOK_ONSUSPEND:
        if (s_cpu_home_early) {
            PLOG_I("cpu limit: HOME menu opens (30%% already set early)");
        } else {
            cpu_limit_set(30, "HOME menu opens");
        }
        break;
    case APTHOOK_ONSLEEP: cpu_limit_set(30, "sleep"); break;
    case APTHOOK_ONRESTORE:
        s_cpu_home_early = 0;
        cpu_limit_set(s_cpu_limit, "HOME menu closed");
        break;
    case APTHOOK_ONWAKEUP: cpu_limit_set(s_cpu_limit, "wake"); break;
    case APTHOOK_ONEXIT: plat_cpu_limit_restore(); break;
    default: break;
    }
}

/* Puts back the limit the process had before plat_cpu_limit_init. The
 * Homebrew Launcher runs in this process after a title takeover, so a limit
 * left at 80 would starve its netloader of core 1. */
void plat_cpu_limit_restore(void) {
    if (s_cpu_restored) {
        return;
    }
    s_cpu_restored = 1;
    if (s_cpu_limit_orig == 0) {
        /* Setting 0 is refused (e0e01bf4); the process started with it, so
         * there is nothing to put back. */
        PLOG_I("cpu limit: original 0%%, nothing to restore");
        return;
    }
    Result r = APT_SetAppCpuTimeLimit(s_cpu_limit_orig);
    PLOG_I("cpu limit: restored to %lu%% result %08lx", (unsigned long)s_cpu_limit_orig, (unsigned long)r);
}

void plat_cpu_limit_init(void) {
    u32 orig = 0;
    if (R_SUCCEEDED(APT_GetAppCpuTimeLimit(&orig))) {
        s_cpu_limit_orig = orig;
    } else {
        s_cpu_limit_orig = 30;
    }
    PLOG_I("cpu limit: before this process %lu%%", (unsigned long)s_cpu_limit_orig);
    /* The process maximum for core 1 comes from the ExHeader (MaxCpu); a
     * limit above it is refused and core 1 stays at 0. */
    s64 max = 0;
    Handle rl = 0;
    Result rr = svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rr)) {
        ResourceLimitType cat = RESLIMIT_CPUTIME;
        s64 values[1] = {0};
        rr = svcGetResourceLimitLimitValues(values, rl, &cat, 1);
        if (R_SUCCEEDED(rr)) {
            max = values[0];
        }
        svcCloseHandle(rl);
    }
    PLOG_I("cpu limit: process maximum %lld%% result %08lx", (long long)max, (unsigned long)rr);

    s_cpu_limit = (u32)plat_switch_int("cpulimit", 80);
    if (s_cpu_limit < 5 || s_cpu_limit > 80) {
        s_cpu_limit = 80;
    }
    if (max > 0 && (s64)s_cpu_limit > max) {
        s_cpu_limit = (u32)max;
    }
    Result sr = APT_SetAppCpuTimeLimit(s_cpu_limit);
    u32 now = 0;
    APT_GetAppCpuTimeLimit(&now);
    PLOG_I("cpu limit: core 1 set %lu%% result %08lx, now %lu%%", (unsigned long)s_cpu_limit, (unsigned long)sr,
           (unsigned long)now);
    aptHook(&s_apt_cookie, apt_hook, NULL);
}

/* devupdate (plat/devupdate.c): during a transfer the network gets more of core 1. */
void plat_cpu_limit_transfer(int on) {
    cpu_limit_set(on ? 30 : s_cpu_limit, on ? "devupdate transfer" : "devupdate transfer done");
}

int plat_frame_poll(void) {
    static int s_boot_marked;
    if (!s_boot_marked) {
        s_boot_marked = 1;
        plat_sd_boot_done(); /* the game loop has begun: SD calls on cores 0 and 2 count as regressions */
    }
    plat_wd_progress();
    plat_devupdate_frame();
    return aptMainLoop() ? 1 : 0;
}

/* A message the user must read: shown on the top screen until START (or
 * the HOME Menu). Works before and after the game set up the GPU. */
static void (*s_gpu_shutdown)(void);
void plat_set_gpu_shutdown(void (*fn)(void)) { s_gpu_shutdown = fn; }
static void (*s_gpu_abort)(void);
void plat_set_gpu_abort(void (*fn)(void)) { s_gpu_abort = fn; }

/* The crash screen's version of message_screen's first step: citro3d and gfx
 * down so the consoles can take both screens, without waiting for the render
 * thread (it may be the one that crashed). */
void plat_crash_gpu_down(void) {
    void (*fn)(void) = s_gpu_abort != NULL ? s_gpu_abort : s_gpu_shutdown;
    s_gpu_abort = NULL;
    s_gpu_shutdown = NULL;
    if (fn != NULL) {
        fn();
    }
}

/* Normal exit. exit() runs the atexit handlers (ndsp, apt, gfx, soc), which
 * the raw svcExitProcess in plat_exit_process would skip. relaunch: ask APT
 * to start this app again when the process ends (the Homebrew Launcher may
 * ignore it and just return). */
void plat_quit(int relaunch) {
    plat_wd_quiesce();
    if (s_gpu_shutdown != NULL) {
        s_gpu_shutdown(); /* waits for the render thread, then C3D_Fini and gfxExit */
        s_gpu_shutdown = NULL;
    }
    plat_cpu_limit_restore();
    plat_devupdate_stop();
    plat_sd_flush();
    plat_log_stop();
    plat_log_crash_close();
    if (relaunch) {
        aptSetChainloaderToSelf();
    }
    exit(0);
}

void plat_shutdown(void) {
    PLOG_I("shutdown: the system asked the app to close");
    plat_quit(0);
}

static void message_screen(const char* text) {
    if (s_gpu_shutdown != NULL) {
        s_gpu_shutdown(); /* citro3d and gfx down, so the console can take the screen */
        s_gpu_shutdown = NULL;
    }
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\n  Paper Mario: The Thousand-Year Door\n  (3DS port v%s, build %s)\n\n", PORT3DS_VERSION, PORT3DS_BUILD_STRING);
    printf("%s\n\n  Press START to exit.\n", text);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
}

void plat_user_error(const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    PLOG_E("user error: %s", text);
    plat_log_stop();
    message_screen(text);
    plat_log_crash_close();
    exit(0);
}

extern char __end__[];
#define REQUIRED_HEAP (64u << 20) /* measured peak about 49 MB (field and battle) */
#define REQUIRED_LINEAR (24u << 20) /* measured peak about 15 MB */

int main(int argc, char** argv) {
    s_boot_tick = svcGetSystemTick();
    if ((u32)__end__ > 0x01000000u) {
        /* port_addr_impl.h: sound ids with flag bits (up to 0x07FFFFFF) must
         * not fall inside the executable image */
        svcBreak(USERBREAK_PANIC);
    }

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    s_new3ds = n3ds ? 1 : 0;
    if (!s_new3ds) {
        message_screen("  This port needs a New 3DS, New 3DS XL\n  or New 2DS XL.");
        return 0;
    }
    osSetSpeedupEnable(true);

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    if (chdir(DATA_DIR) != 0) {
        /* No SD card or a write-protected one: nothing can be logged. */
        message_screen("  The SD card cannot be used\n  (missing or write-protected).");
        return 0;
    }
    /* The Homebrew Launcher's netloader may not pass a "3dslink:" argv[0]; libctru's
     * __3dslink_host (the pushing host) is set whenever the launcher gave it. */
    s_netload = (argc > 0 && argv[0] != NULL && strncmp(argv[0], "3dslink:", 8) == 0) || __3dslink_host.s_addr != 0;
    plat_switches_load(argc, argv);
    plat_log_init(s_netload || plat_switch_has("livelog"));
    plat_crash_setup();
    plat_sd_start(); /* all run-time SD access goes through its core 1 worker (include/plat/sdio.h) */
    plat_stack_register(0, "main", __stacksize__);
    PLOG_I("TTYD 3DS v%s build %s, switches \"%s\"", PORT3DS_VERSION, PORT3DS_BUILD_STRING, plat_switches_text());
    PLOG_I("boot: argc %d argv[0] \"%s\" 3dslink_host 0x%08lx netload %d", argc,
           (argc > 0 && argv[0] != NULL) ? argv[0] : "(null)", (unsigned long)__3dslink_host.s_addr, s_netload);

    plat_cpu_limit_init();
    plat_memplace_init();
    plat_log_start_writer();
    plat_crash_init();
    plat_devupdate_init();
    plat_memplace_report();

    /* libctru gives the heap and the linear heap whatever the APPLICATION
     * region leaves (a 3dsx gets the region of the title it runs in), so
     * check them against what the game takes at most (measured: see
     * plat_memory_log) instead of failing an allocation later. Switch
     * lowmem: try anyway. */
    PLOG_I("memory: APPLICATION region %lu KiB, heap %lu KiB, linear heap %lu KiB",
           (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10), (unsigned long)(__ctru_heap_size >> 10),
           (unsigned long)(__ctru_linear_heap_size >> 10));
    if ((__ctru_heap_size < REQUIRED_HEAP || __ctru_linear_heap_size < REQUIRED_LINEAR) && !plat_switch_has("lowmem")) {
        plat_user_error("  Not enough memory: heap %lu MB (needs %lu),\n  linear %lu MB (needs %lu).\n\n"
                        "  Install the CIA and start it from the\n  HOME Menu (or use the lowmem switch).",
                        (unsigned long)(__ctru_heap_size >> 20), (unsigned long)(REQUIRED_HEAP >> 20),
                        (unsigned long)(__ctru_linear_heap_size >> 20), (unsigned long)(REQUIRED_LINEAR >> 20));
    }

    plat_set_phase("game init");
    ttyd_main();
    plat_fatal("ttyd_main returned");
}
