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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plat_internal.h"
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
static aptHookCookie s_apt_cookie;

uint64_t plat_ticks(void) { return svcGetSystemTick(); }
uint32_t plat_uptime_ms(void) { return (uint32_t)((svcGetSystemTick() - s_boot_tick) / (PLAT_TICKS_PER_SEC / 1000u)); }
int plat_is_new3ds(void) { return s_new3ds; }
uint64_t plat_rtc_ms_since_1900(void) { return osGetTime(); }
int plat_started_by_netload(void) { return s_netload; }
const char* plat_build_string(void) { return PORT3DS_BUILD_STRING; }

static void cpu_limit_set(u32 value, const char* why) {
    Result r = APT_SetAppCpuTimeLimit(value);
    PLOG_I("cpu limit: %lu%% (%s) result %08lx", (unsigned long)value, why, (unsigned long)r);
}

/* Runs in the thread that calls aptMainLoop (the game thread). The HOME Menu
 * and sleep get core 1 back; closing them restores the game's value. */
static void apt_hook(APT_HookType type, void* param) {
    (void)param;
    switch (type) {
    case APTHOOK_ONSUSPEND: cpu_limit_set(30, "HOME menu opens"); break;
    case APTHOOK_ONSLEEP: cpu_limit_set(30, "sleep"); break;
    case APTHOOK_ONRESTORE: cpu_limit_set(s_cpu_limit, "HOME menu closed"); break;
    case APTHOOK_ONWAKEUP: cpu_limit_set(s_cpu_limit, "wake"); break;
    case APTHOOK_ONEXIT: cpu_limit_set(30, "exit"); break;
    default: break;
    }
}

void plat_cpu_limit_init(void) {
    s_cpu_limit = (u32)plat_switch_int("cpulimit", 80);
    if (s_cpu_limit < 5 || s_cpu_limit > 80) {
        s_cpu_limit = 80;
    }
    if (R_FAILED(APT_SetAppCpuTimeLimit(s_cpu_limit)) && s_cpu_limit == 80) {
        s_cpu_limit = 79;
        APT_SetAppCpuTimeLimit(s_cpu_limit);
    }
    PLOG_I("cpu limit: core 1 at %lu%%", (unsigned long)s_cpu_limit);
    aptHook(&s_apt_cookie, apt_hook, NULL);
}

int plat_frame_poll(void) {
    plat_wd_progress();
    return aptMainLoop() ? 1 : 0;
}

void plat_shutdown(void) {
    PLOG_I("shutdown: the system asked the app to close");
    plat_log_stop();
    plat_log_crash_close();
    exit(0);
}

/* A message the user must read: shown on the top screen until START (or
 * the HOME Menu). Works before and after the game set up the GPU. */
static void (*s_gpu_shutdown)(void);
void plat_set_gpu_shutdown(void (*fn)(void)) { s_gpu_shutdown = fn; }

static void message_screen(const char* text) {
    if (s_gpu_shutdown != NULL) {
        s_gpu_shutdown(); /* citro3d and gfx down, so the console can take the screen */
        s_gpu_shutdown = NULL;
    }
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\n  Paper Mario: The Thousand-Year Door\n  (3DS port, build %s)\n\n", PORT3DS_BUILD_STRING);
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

int main(int argc, char** argv) {
    s_boot_tick = svcGetSystemTick();

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
    s_netload = argc > 0 && argv[0] != NULL && strncmp(argv[0], "3dslink:", 8) == 0;
    plat_switches_load(argc, argv);
    plat_log_init(s_netload || plat_switch_has("livelog"));
    plat_crash_install_thread();
    plat_stack_register(0, "game", __stacksize__);
    PLOG_I("TTYD 3DS build %s, switches \"%s\"", PORT3DS_BUILD_STRING, plat_switches_text());

    plat_cpu_limit_init();
    plat_memplace_init();
    plat_log_start_writer();
    plat_crash_init();
    plat_memplace_report();

    plat_set_phase("game init");
    ttyd_main();
    plat_fatal("ttyd_main returned");
}
