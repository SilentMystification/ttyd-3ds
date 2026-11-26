/* ctr-devkit supervisor: an optional, development-only system module that stays reachable on the
 * network while the app is hung, crashed, killed, boot-looping or not running, and the in-app
 * client API that installs, starts, checks and removes it.
 *
 * Development builds only. Never ship the supervisor or call these functions in a release build.
 * Design and the facts behind it: docs/CTR_DEVKIT_PLAN.md, "Supervisor".
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_SUPERVISOR_H
#define CTRDK_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The supervisor's title: a system module (high word 00040130) whose low byte is 02. PM panics
 * when NATIVE_FIRM launches a 00040130 title whose low byte is not 02 (Luma3DS pm, launch.c). */
#define CTRDK_SV_TITLE_ID 0x0004013000FD8A02ULL
#define CTRDK_SV_PORT 5051
/* Configuration, read at the supervisor's start and again before each request (port= only at the
 * start). Lines "key=value":
 *   port=N            TCP port (default 5051)
 *   game=<16 hex>     the watched title (written by ensure() and install())
 *   dir=<path>        the app's own folder, served read-only by `files` (written by ensure(): the
 *                     app's working directory)
 *   token=<text>      shared secret, checked as devupdate checks its token
 *   watch=N           watchdog armed at start with N s (default 0: off until the PC arms it)
 *   max_relaunches=N  automatic relaunches per push (default 2)
 *   terminate_ms=N    PM's grace before it ends the game by force (default 3000)
 *   gpureset=0|1|2    between terminate and launch: 0 off, 1 ResetGpuCore only, 2 also with the
 *                     GPU right (default 2)
 *   launch=ns|pm      launch the game through NS (default) or PM
 *   launch_flags=N    PM launch flags for that call (default 1, PMLAUNCHFLAG_NORMAL_APPLICATION)
 *   ndm_hold=0|1      hold NDM's infrastructure state as Rosalina does (default 0)
 *   log=0|1           append to CTRDK_SV_LOG (default 1) */
#define CTRDK_SV_CONFIG "sdmc:/3ds/ctrdk/supervisor.cfg"
#define CTRDK_SV_LOG "sdmc:/3ds/ctrdk/supervisor.log"
/* The heartbeat service (one IPC port; the supervisor registers it). */
#define CTRDK_SV_SERVICE "ctrdk:sv"

/* Commands. 1 (ping), 2 (push) and 6 (files) have devupdate's numbers and wire format; the
 * supervisor's own commands are 0x100 and up, so they never collide with devupdate's. The header is
 * devupdate's 64-byte CDU1 header. */
enum {
    CTRDK_SV_CMD_PING = 1,
    CTRDK_SV_CMD_PUSH = 2,
    CTRDK_SV_CMD_FILES = 6,
    CTRDK_SV_CMD_STATUS = 0x101,
    CTRDK_SV_CMD_TERMINATE = 0x102,
    CTRDK_SV_CMD_LAUNCH = 0x103,
    CTRDK_SV_CMD_WATCH = 0x104,
    CTRDK_SV_CMD_REBOOT = 0x105,
    CTRDK_SV_CMD_EXIT = 0x106,
};

/* Heartbeat flags. */
#define CTRDK_SV_HB_PAUSED 1u   /* a menu or an update holds the game loop (never "hung") */
#define CTRDK_SV_HB_LOADING 2u  /* a load (the watchdog limit is 4 times longer) */
#define CTRDK_SV_HB_RECOVERY 4u /* the crash screen waits for the PC (never "hung") */

typedef struct {
    bool installed;          /* AM lists the title on NAND */
    bool running;            /* a process with the title ID runs */
    bool running_known;      /* false: the process list could not be read (not Luma3DS, or an emulator) */
    uint16_t version;        /* installed title version (0 if not installed) */
} CtrdkSvStatus;

typedef struct {
    int32_t result;          /* 0, or the failing call's Result */
    const char* stage;       /* "open", "am-start", "am-write", "am-finish", "launch", "terminate", "delete" */
    bool started;            /* the supervisor runs now */
    bool reboot_needed;      /* true only when it is installed but could not be started */
} CtrdkSvResult;

/* Where it stands. Never blocks for long (AM and the process list only). */
int32_t ctrdk_supervisor_status(CtrdkSvStatus* out);

/* Installs the supervisor CIA at cia_path (sdmc:/... or romfs:/...) to NAND through AM, writes
 * CTRDK_SV_CONFIG with game_title_id and port (0 = CTRDK_SV_PORT), and starts it through PM. An
 * older running supervisor is ended first (PM, 3 s), so this is also the update path. The CIA
 * must carry CTRDK_SV_TITLE_ID; anything else is refused. Needs am:net and pm:app. */
int32_t ctrdk_supervisor_install(const char* cia_path, uint64_t game_title_id, uint16_t port, CtrdkSvResult* out);

/* Ends the supervisor (PM, 3 s) and deletes its title from NAND. Removes CTRDK_SV_CONFIG. */
int32_t ctrdk_supervisor_uninstall(CtrdkSvResult* out);

/* Starts the supervisor when it is installed and not running; does nothing otherwise. For the
 * app's start-up, before anything that can hang (GPU, audio, disc). Cheap when it already runs
 * (PM returns at once for a running title). When it is installed, it also writes this app's title
 * ID (game=) and working directory (dir=) to CTRDK_SV_CONFIG if they changed, so the supervisor
 * watches the app that started it. Returns 0 or the PM result. */
int32_t ctrdk_supervisor_ensure(void);

/* Once per frame (or less often) from the app's frame loop. It only stores frame and flags
 * (CTRDK_SV_HB_*) and never blocks: a low-priority client thread, made on the first call, sends the
 * latest values to the supervisor's service once per second. Without the supervisor it does
 * nothing but look for the service once every 10 s, and calls ensure() then. When devupdate is
 * linked, the thread adds CTRDK_SV_HB_PAUSED while an update holds the app and CTRDK_SV_HB_RECOVERY
 * in recovery mode. */
void ctrdk_supervisor_heartbeat(uint32_t frame, uint32_t flags);

/* The client thread's view: 1 a session to the service is open, 0 not; sent counts the heartbeats
 * the service took. For a status line. */
int ctrdk_supervisor_heartbeat_state(uint32_t* sent);

#ifdef __cplusplus
}
#endif

#endif
