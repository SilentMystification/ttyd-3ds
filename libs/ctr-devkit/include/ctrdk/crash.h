/* ctr-devkit crash: crash and hang handler for libctru homebrew.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * One report path serves CPU exceptions, crash_fatal(), the hang watchdog
 * and svcBreak (link with -Wl,--wrap=svcBreak to catch libctru and citro3d
 * panics). A report is a run of lines starting with "[CRASH]", the first
 * one tagged with the format version ("[CRASH] v1 data abort ..."). They go
 * to the app's line sink, the debugger (svcOutputDebugString) and an
 * optional report file (appended, kept across boots). Then a crash screen
 * shows what happened on the top screen and the call chain on the bottom
 * one until START or a timeout, and the process ends with svcExitProcess.
 *
 * Every thread calls crash_thread_register() at its entry: that installs
 * the handler for the thread (one shared handler stack) and records the
 * thread's name, a handle and its stack bounds, for the stack scan and the
 * watchdog's thread dump.
 *
 * The watchdog thread (core 1, lowest priority) reports a hang when the app
 * makes no crash_wd_progress() call for watchdog_seconds while it is in the
 * foreground, and ends the process when a report itself takes too long.
 *
 * Usage:
 *     CrashConfig cfg = {0};
 *     cfg.app_name = "My App";
 *     cfg.report_path = "sdmc:/3ds/myapp/crash.txt";
 *     cfg.watchdog_seconds = 10;
 *     crash_init(&cfg);                     // in main, first thing
 *     extern u32 __stacksize__;            // libctru's main thread stack size
 *     crash_thread_register("main", __stacksize__);
 *     for (;;) { crash_wd_progress(); ... } // once per frame
 */
#ifndef CTRDK_CRASH_H
#define CTRDK_CRASH_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRASH_REPORT_VERSION 1

/* Filled by the app's app_info callback during a report. Every field may be
 * left NULL. The strings must stay valid until the callback's caller is
 * done with them (static storage is the safe choice). */
typedef struct {
    const char* build;       /* "#171 2681d5eb": names the ELF for the report resolver */
    const char* phase;       /* what the app is doing ("game frame") */
    const char* scene_label; /* what a scene is called ("map"); NULL = "scene" */
    const char* scene;       /* the current scene */
    const char* memory_note; /* appended to the memory line ("QTM on") */
    const char* lines[4];    /* extra report lines, written as "[CRASH] <line>" */
} CrashAppInfo;

/* When a shutdown hook runs. Hooks of a stage run in ascending order. */
typedef enum {
    CRASH_STAGE_SCREEN = 0, /* before the crash screen: take the GPU down (must not wait for other threads) */
    CRASH_STAGE_REPORT_END, /* after the screen, at the end of a complete report: release audio */
    CRASH_STAGE_EXIT,       /* every exit the library makes: restore system settings, close files */
    CRASH_STAGE_COUNT
} CrashStage;

typedef struct {
    const char* app_name;    /* crash screen title: "<app_name> crashed" */
    const char* report_path; /* report file, appended; NULL = none. Written with open/write, no heap */
    /* Every report line (with its '\n'), for the app's log. Called on the
     * crashing thread: it must not take locks another thread may hold. */
    void (*line_sink)(const char* line, int len, void* user);
    void (*app_info)(CrashAppInfo* out, void* user);
    /* Informational messages (not reports): level 0 error, 1 warning, 2 info. */
    void (*log)(int level, const char* text, void* user);
    int watchdog_seconds;    /* hang limit; 0 = no hang detection (report deadlines still apply) */
    int screen_wait_seconds; /* crash screen timeout; 0 = 300, negative = no wait */
    /* true: svcBreak is turned into a report (needs -Wl,--wrap=svcBreak).
     * false: a wrapped svcBreak goes to the real one. */
    bool wrap_svcbreak;
    /* After the report and the crash screen are up: hand the process to a
     * recovery service (devupdate_enter_recovery). Return true if it took
     * over; the crashing thread then shows "recovery: waiting for the PC"
     * and waits for START or crash_recovery_exit(). NULL or false: the
     * normal screen wait and exit. */
    bool (*on_recovery)(const char* report, void* user);
    void* user;
    /* Self tests (the app reads its own switches and fills these):
     * test = "self", "null", "undef", "boot-fatal" (run by crash_start), or
     * "segv", "fatal", "break", "hang" (run on the thread that calls
     * crash_wd_progress, at its test_frame-th call). */
    const char* test;
    int test_frame;          /* 0 = 60 */
    uintptr_t test_addr;     /* address the "null" test writes */
    uint64_t boot_tick;      /* svcGetSystemTick at process start, for "uptime"; 0 = at crash_setup */
    const char* screen_dump; /* test aid: path prefix; once the crash screen is drawn both screens are written as
                                <prefix>_top.ppm and <prefix>_bottom.ppm; NULL = off */
} CrashConfig;

/* crash_setup + crash_start. */
void crash_init(const CrashConfig* cfg);
/* Copies the config and installs the handler on the calling thread. No
 * threads are started. Safe to call before the app's log exists if the sink
 * copes with that. */
void crash_setup(const CrashConfig* cfg);
/* Starts the watchdog thread and runs a boot self test if one is set. */
void crash_start(void);

/* Call at each thread's entry (including main). stack_bytes is the thread's
 * stack size; the unused part is filled with a pattern. Up to 24 threads. */
void crash_thread_register(const char* name, unsigned stack_bytes);
/* Installs the handler on the calling thread only (no registry entry). */
void crash_install_handler(void);

/* Up to 8 hooks per stage. */
void crash_add_shutdown_hook(CrashStage stage, void (*fn)(void* user), void* user, int order);

/* Watchdog. progress: once per frame from the main loop. hold: a long
 * legitimate wait (saving), nestable. quiesce: before a normal exit, waits
 * up to 1 s for holds to end, then stops the watchdog. */
void crash_wd_progress(void);
void crash_wd_hold(bool hold);
void crash_wd_quiesce(void);
int crash_wd_held(void);
/* Call from a thread that keeps ticking while the game thread may not (the vblank thread). */
void crash_wd_alive(void);
uint32_t crash_frame_count(void);

/* A report of kind "fatal" with the message, then the exit. */
void crash_fatal(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
/* The same, with the caller's pc and sp given (for an app's own wrapper). */
void crash_vfatal_at(uint32_t pc, uint32_t sp, const char* fmt, va_list ap) __attribute__((noreturn));
/* Runs a self test now: "segv", "null", "undef", "fatal", "break", "hang", "self". */
void crash_test(const char* kind);

/* The crash exit: waits for holds (up to 1 s), runs the EXIT hooks and ends
 * the process with svcExitProcess. */
void crash_exit_process(void) __attribute__((noreturn));
/* From a recovery service: ends the crashed thread's wait. chainload: run
 * aptExit() before the exit, so a chainloader set with aptSetChainloader*
 * takes effect. */
void crash_recovery_exit(bool chainload);
/* The last-resort guard: a thread of its own that does no I/O and takes no lock. If a checkpoint is armed and
 * not disarmed in ms, it ends the process with svcExitProcess (so the HOME Menu or a supervisor can start
 * the app again) instead of leaving a console with a stuck process and nobody listening. The crash path
 * arms it itself (report, exit); an app arms it around its own waits that depend on a service or on
 * another thread (a relaunch, an update). what is only for the debugger's log line and must stay valid
 * (a literal). One checkpoint at a time: a later arm replaces the earlier one. */
void crash_guard_arm(const char* what, unsigned ms);
void crash_guard_disarm(void);
/* Whether the report file was opened and written (for the crash screen). */
bool crash_report_file_ok(void);

#ifdef __cplusplus
}
#endif

#endif
