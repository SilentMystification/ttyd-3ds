/* ctr-devkit supervisor: the parts shared by the system module and its app build (for emulators).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_SV_INTERNAL_H
#define CTRDK_SV_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t port;
    uint64_t game_title_id;   /* the title it watches (0 = none) */
    char token[32];
    char dir[96];             /* the app's folder, read-only through `files` ("" = none) */
    uint32_t watch_s;         /* watchdog armed at start (0 = off until the PC arms it) */
    uint32_t max_relaunches;  /* automatic relaunches per push */
    uint32_t terminate_ms;    /* PM's grace before it ends the game by force */
    int gpureset;             /* 0 off, 1 ResetGpuCore only, 2 also with the GPU right */
    bool launch_pm;           /* launch through PM instead of NS */
    uint32_t launch_flags;    /* PM launch flags */
    bool ndm_hold;            /* hold NDM's infrastructure state */
    bool log_file;            /* append to CTRDK_SV_LOG */
    const char* kind;         /* "sysmodule" or "app" */
    const char* version;      /* build stamp */
} SvConfig;

/* The defaults (port, watch off, 2 relaunches, 3 s, gpureset 2, NS, flags 1, no NDM hold, log on). */
void sv_config_defaults(SvConfig* cfg);
/* Reads CTRDK_SV_CONFIG over the values already in cfg (port= only when with_port). */
void sv_config_load(SvConfig* cfg, bool with_port);

/* The server loop: accepts one client at a time and answers it, and runs the watchdog between
 * requests. Returns when *quit is set (checked at least every 0.5 s) or after the exit command
 * (then sv_exit_requested is true). socInit must have succeeded. */
void sv_serve(SvConfig* cfg, volatile bool* quit);
extern volatile bool sv_exit_requested;
/* Uptime starts here (call first thing in main). */
void sv_server_init_clock(void);

/* Line to the debug output (svcOutputDebugString), to CTRDK_SV_LOG when enabled and, when set, to
 * the app build's screen. Any thread. */
extern void (*sv_log_hook)(const char* line);
void sv_log_enable_file(bool on);
void sv_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* The fault guard of thread slot i (0 main, 1 server, 2 heartbeat, 3 GPU reset): an exception
 * handler that ends the process. A no-op in the app build. */
void sv_guard_thread(int i);

/* ---- hb.c: the heartbeat service ---- */
typedef struct {
    uint32_t frame, flags, count; /* count: heartbeats since the start */
    uint64_t tick;                /* svcGetSystemTick of the last one */
} SvBeat;
bool sv_hb_start(void);
void sv_hb_stop(void);
void sv_hb_get(SvBeat* out);
int32_t sv_hb_result(void); /* the registration's result */
uint32_t sv_hb_sessions(void);
void sv_hb_set_watch(uint32_t secs);

/* ---- actions.c: what the supervisor does to the game and the system ---- */
int sv_game_state(uint64_t tid, uint32_t* pid); /* SV_GAME_* (policy.h) */
/* PM ends the game (terminate_ms, then by force); if it still runs after that, svcTerminateProcess.
 * Fills a one-line report. Returns PM's result. */
int32_t sv_terminate(const SvConfig* cfg, uint64_t tid, char* report, size_t n);
/* The GPU reset attempt (level as cfg->gpureset): never waits more than 2 s; one-line report. */
void sv_gpu_reset(int level, char* report, size_t n);
/* Writes the launch arguments file, then launches the game. Returns the launch result. */
int32_t sv_launch(const SvConfig* cfg, uint64_t tid, const char* switches, char* report, size_t n);
/* Reboot: NS_RebootToTitle(SD, tid) unless hard; when that fails (or 15 s pass with no reboot),
 * APT_HardwareResetAsync. Returns only on failure of both. */
int32_t sv_reboot(uint64_t tid, bool hard, char* report, size_t n);
/* NDM's infrastructure hold (on) or its release (off). */
int32_t sv_ndm_hold(bool on);

#define SV_LAUNCH_ARGS_FILE "sdmc:/3ds/ctrdk-launch-args.txt"

#endif
