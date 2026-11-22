/* ctr-devkit devupdate: over-the-air CIA install and relaunch for libctru
 * homebrew, and a recovery mode after a crash. DEVELOPMENT USE ONLY: anyone
 * on the network can install a title while it runs (an optional token
 * limits that to whoever knows it).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * A server thread listens on a TCP port (default 5050). The PC tool
 * (devupdate/devupdate.py) sends a 64-byte header and a payload:
 *   ping     title ID, version, free space, whether it runs as a CIA or 3dsx
 *   push     a CIA, streamed into AM (no copy in RAM), CRC32-checked before
 *            the install is committed; on any error the install is cancelled
 *   restart  relaunch this title with the given switches
 *   report   the crash report and the app's log tail (recovery mode)
 *   launch   jump into an installed title (the bootstrap)
 * The device answers with text lines and ends each request with "done" or
 * "error <code> <text>".
 *
 * Switches for the next launch travel as APT deliver arguments
 * (aptSetChainloaderArgs); devupdate_launch_args() reads them at boot. They
 * belong to that one launch: a cold start from the HOME Menu gets none.
 */
#ifndef CTRDK_DEVUPDATE_H
#define CTRDK_DEVUPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVUPDATE_DEFAULT_PORT 5050
#define DEVUPDATE_MAX_ARGS 0x2F0 /* switch text per launch (APT allows 0x300 with the prefix) */

typedef struct {
    uint16_t port;           /* 0 = 5050 */
    const char* token;       /* shared secret, up to 31 characters; NULL = none (LAN dev use only) */
    int core;                /* server and recovery threads; 0 = core 1 (falls back to core 0) */
    const char* version;     /* the app's version string, for ping */
    /* A 3dsx under the Homebrew Launcher refuses CIA installs unless this is
     * set (the generic bootstrap sets it). */
    bool allow_3dsx_install;
    const char* log_path;    /* the app's log, whose tail "report" sends; NULL = none */
    int recovery_net_seconds;    /* recovery ends the process if the network is not up in time; 0 = 30 */
    uint32_t soc_buffer_size;    /* 0 = 0x80000; reserved at start if the app has not set up sockets */

    /* A push or restart begins: pause the app at a safe point, quiet audio. */
    void (*on_begin)(void* user);
    void (*on_progress)(uint32_t done, uint32_t total, void* user);
    /* Installed (or restart): let running saves finish, close files. true =
     * relaunch now; false = the app calls devupdate_relaunch() later. */
    bool (*on_ready)(void* user);
    /* The request ended without a relaunch (it failed, or a push asked for
     * none) after on_begin: resume the app. */
    void (*on_resume)(void* user);
    /* Relaunch now: the chainloader and the arguments are set; the app ends
     * through its own exit path (exit() runs aptExit, which starts the next
     * title). NULL: the library only sets devupdate_relaunch_requested(),
     * which the app's main loop checks. */
    void (*on_relaunch)(void* user);
    /* Recovery mode ends the process: relaunch = the chainloader is set.
     * NULL: aptExit() when relaunching, then svcExitProcess(). */
    void (*on_recovery_exit)(bool relaunch, void* user);
    /* Informational messages: level 0 error, 1 warning, 2 info. */
    void (*log)(int level, const char* text, void* user);
    void* user;
} DevUpdateConfig;

/* Starts the server thread and prepares recovery mode (its thread, the
 * socket buffer, the AM session, the title ID). Returns 0 or an error. */
int32_t devupdate_start(const DevUpdateConfig* cfg);
void devupdate_stop(void);
bool devupdate_running(void);

/* An update is installed and waits for the relaunch. */
bool devupdate_pending(void);
/* A relaunch was requested and on_relaunch is NULL: call devupdate_relaunch(). */
bool devupdate_relaunch_requested(void);
/* From the app's main thread: sets the chainloader (this title, or the
 * title a "launch" or bootstrap push named) with the requested arguments
 * and exits through exit(0). Under the Homebrew Launcher it jumps with
 * APT_PrepareToDoApplicationJump/APT_DoApplicationJump and returns; the
 * app's aptMainLoop then ends. */
void devupdate_relaunch(void);
/* Sets the chainloader for the pending relaunch without exiting, for apps
 * with their own exit path. */
void devupdate_prepare_relaunch(void);

/* The switch text this launch received as APT deliver arguments; "" if
 * none. Read once (the first call), any time, also before devupdate_start. */
const char* devupdate_launch_args(void);

/* From the app's crash handler, after its report and crash screen: wakes
 * the recovery thread prepared by devupdate_start and returns true (false
 * if devupdate is not running). The caller keeps its screen up and waits;
 * recovery ends the process through on_recovery_exit. */
bool devupdate_enter_recovery(const char* report);
bool devupdate_in_recovery(void);

#ifdef __cplusplus
}
#endif

#endif
