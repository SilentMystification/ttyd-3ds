/* TTYD's glue to the ctr-devkit crash handler (libs/ctr-devkit/crash/).
 *
 * The library does the work: the exception handler on every thread, the
 * one report path for exceptions, fatals, the watchdog and svcBreak (the
 * game links with --wrap=svcBreak), the crash screen and the crash exit.
 * See libs/ctr-devkit/README.md. This file connects it to the port:
 *   - report lines go to plat_crash_line (log.c): the log queue is written
 *     first, then each [CRASH] line goes to latest.log, the live log and the
 *     bottom console; the library itself appends them to crash.txt (kept
 *     across boots) and the debugger channel;
 *   - the app info is the build string, the phase and the map, and whether
 *     QTM is in use;
 *   - shutdown hooks: the console lock and the GPU before the crash screen,
 *     the DSP after it, the core 1 limit and latest.log before the exit;
 *   - switches: wdog=N (default 10), crashwait=N, crashtest=<kind>,
 *     crashframe=N, crashaddr=N;
 *   - with the devupdate switch, a crash hands over to the updater's
 *     recovery mode (plat/devupdate.c) instead of waiting for START.
 * The plat_* names stay as wrappers, so callers do not change.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ctrdk/crash.h"
#include "plat_internal.h"

static volatile const char* s_phase = "boot";
static char s_map[16] = "-";
static void (*s_audio_abort)(void);

void plat_set_audio_abort(void (*fn)(void)) { s_audio_abort = fn; }
void plat_set_phase(const char* phase) { s_phase = phase; }
const char* plat_get_phase(void) { return (const char*)s_phase; }
const char* plat_get_map_name(void) { return s_map; }

void plat_set_map_name(const char* map) {
    if (map == NULL) {
        map = "-";
    }
    strncpy(s_map, map, sizeof(s_map) - 1);
    s_map[sizeof(s_map) - 1] = '\0';
}

void plat_wd_progress(void) { crash_wd_progress(); }
void plat_wd_hold(int on) { crash_wd_hold(on != 0); }
void plat_wd_quiesce(void) { crash_wd_quiesce(); }
uint32_t plat_wd_frames(void) { return crash_frame_count(); }
void plat_thread_register(const char* name, unsigned size) { crash_thread_register(name, size); }
void plat_crash_install_thread(void) { crash_install_handler(); }
void plat_exit_process(void) { crash_exit_process(); }

void plat_stack_register(int slot, const char* name, unsigned size) {
    (void)slot;
    crash_thread_register(name, size);
}

void plat_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    crash_vfatal_at((u32)__builtin_return_address(0), (u32)__builtin_frame_address(0), fmt, ap);
}

/* ---- Library callbacks ---- */

static void line_sink(const char* line, int len, void* user) {
    (void)user;
    plat_crash_line(line, len);
}

static void app_info(CrashAppInfo* out, void* user) {
    (void)user;
    out->build = plat_build_string();
    out->phase = (const char*)s_phase;
    out->scene_label = "map";
    out->scene = s_map;
    out->memory_note = plat_qtm_available() ? "QTM on" : "QTM off";
}

static void log_line(int level, const char* text, void* user) {
    (void)user;
    plat_log(level, "%s", text);
}

void plat_cpu_limit_restore(void);  /* boot.c */
void plat_crash_gpu_down(void);     /* boot.c: citro3d and gfx down without waiting on other threads */

static void hook_console(void* u) {
    (void)u;
    plat_console_trylock(); /* taken for good: the log must not draw on the old console */
}
static void hook_gpu(void* u) {
    (void)u;
    plat_crash_gpu_down();
}
static void hook_audio(void* u) {
    (void)u;
    if (s_audio_abort != NULL) {
        s_audio_abort();
    }
}
static void hook_cpu(void* u) {
    (void)u;
    plat_cpu_limit_restore();
}
static void hook_log(void* u) {
    (void)u;
    plat_log_crash_close();
}

static char s_test[16];

/* Early, from main (before the log writer and the watchdog): the config and
 * the main thread's handler. */
void plat_crash_setup(void) {
    CrashConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.app_name = "TTYD 3DS";
    cfg.report_path = "crash.txt"; /* the working directory is sdmc:/3ds/ttyd */
    cfg.line_sink = line_sink;
    cfg.app_info = app_info;
    cfg.log = log_line;
    cfg.watchdog_seconds = plat_switch_int("wdog", 10);
    int wait = plat_switch_int("crashwait", 300);
    cfg.screen_wait_seconds = wait > 0 ? wait : -1;
    cfg.wrap_svcbreak = true;
    cfg.on_recovery = plat_devupdate_recovery_hook(); /* NULL without the devupdate switch */
    if (plat_switch_value("crashtest", s_test, sizeof(s_test))) {
        cfg.test = s_test;
    }
    cfg.test_frame = plat_switch_int("crashframe", 60);
    cfg.test_addr = (uintptr_t)plat_switch_int("crashaddr", 4);
    cfg.boot_tick = plat_boot_tick();
    crash_setup(&cfg);
    crash_add_shutdown_hook(CRASH_STAGE_SCREEN, hook_console, NULL, 0);
    crash_add_shutdown_hook(CRASH_STAGE_SCREEN, hook_gpu, NULL, 10);
    crash_add_shutdown_hook(CRASH_STAGE_REPORT_END, hook_audio, NULL, 0);
    crash_add_shutdown_hook(CRASH_STAGE_EXIT, hook_cpu, NULL, 0);
    crash_add_shutdown_hook(CRASH_STAGE_EXIT, hook_log, NULL, 10);
}

/* The watchdog thread on core 1, then a boot crash test if one is set. */
void plat_crash_init(void) { crash_start(); }
