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
/* Optional configuration, read once at the supervisor's start: lines "port=N", "game=<16 hex>",
 * "token=<text>". Written by ctrdk_supervisor_install() and by the PC tool. */
#define CTRDK_SV_CONFIG "sdmc:/3ds/ctrdk/supervisor.cfg"

/* Commands. 1 (ping) is devupdate's; the supervisor's own commands are 0x100 and up, so they
 * never collide with devupdate's. The header is devupdate's 64-byte CDU1 header. */
enum {
    CTRDK_SV_CMD_PING = 1,
    CTRDK_SV_CMD_STATUS = 0x101,
};

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
 * (PM returns at once for a running title). Returns 0 or the PM result. */
int32_t ctrdk_supervisor_ensure(void);

#ifdef __cplusplus
}
#endif

#endif
