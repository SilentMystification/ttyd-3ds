/* ctr-devkit supervisor: the parts shared by the system module and its app build (for emulators).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_SV_INTERNAL_H
#define CTRDK_SV_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t port;
    uint64_t game_title_id;   /* the title it watches (0 = none) */
    char token[32];
    const char* kind;         /* "sysmodule" or "app" */
    const char* version;      /* build stamp */
} SvConfig;

/* Reads CTRDK_SV_CONFIG over the defaults already in cfg. */
void sv_config_load(SvConfig* cfg);

/* The server loop: accepts one client at a time and answers it. Returns when *quit is set
 * (checked at least every 0.5 s). socInit must have succeeded. */
void sv_serve(const SvConfig* cfg, volatile bool* quit);

/* Line to the debug output (svcOutputDebugString) and, when set, to the app build's screen. */
extern void (*sv_log_hook)(const char* line);
void sv_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
