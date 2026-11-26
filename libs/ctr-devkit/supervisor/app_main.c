/* ctr-devkit supervisor, app build: the same server as the system module, run as an ordinary
 * application. For emulators (Azahar cannot start a custom system module) and for checking the
 * protocol from the PC. It is not a supervisor: it ends with the app.
 *
 * It also sends itself heartbeats through the real client (ctrdk_supervisor_heartbeat) and the real
 * service, so the IPC path and the watchdog run here too. Configuration key read only by this build:
 *   app_hb_stop=N   stop the heartbeats N s after the start (0 = never): a stuck game for the watchdog
 *   app_hb_flags=N  the flags it sends (CTRDK_SV_HB_*)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "build_stamp.h"
#include "ctrdk/supervisor.h"
#include "sv_internal.h"

#define SOC_SIZE 0x20000

static volatile bool s_quit;
static SvConfig s_cfg;
static LightLock s_lock;
static char s_lines[8][64];
static int s_n, s_shown;

void sv_guard_thread(int i) { (void)i; }

static void log_hook(const char* line) {
    LightLock_Lock(&s_lock);
    snprintf(s_lines[s_n % 8], sizeof(s_lines[0]), "%s", line);
    s_n++;
    LightLock_Unlock(&s_lock);
}

static void server_main(void* arg) {
    (void)arg;
    sv_serve(&s_cfg, &s_quit);
}

static long cfg_int(const char* key, long def) {
    FILE* f = fopen(CTRDK_SV_CONFIG, "r");
    if (f == NULL) {
        return def;
    }
    char line[160];
    size_t k = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, k) == 0 && line[k] == '=') {
            def = strtol(line + k + 1, NULL, 0);
        }
    }
    fclose(f);
    return def;
}

int main(void) {
    sv_server_init_clock();
    LightLock_Init(&s_lock);
    sv_log_hook = log_hook;
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("ctr-devkit supervisor (app build)\n%s\n\nSTART: exit\n\n", CTRDK_BUILD_STAMP);
    APT_SetAppCpuTimeLimit(30);

    s_cfg.kind = "app";
    s_cfg.version = CTRDK_BUILD_STAMP;
    sv_config_defaults(&s_cfg);
    /* This process holds the GPU itself, and Azahar does not implement ResetGpuCore; its AcquireRight from
     * the right's holder fails an assertion and ends the emulator (gsp_gpu.cpp). Off unless set. */
    s_cfg.gpureset = 0;
    sv_config_load(&s_cfg, true);
    sv_log_enable_file(s_cfg.log_file);
    long hb_stop = cfg_int("app_hb_stop", 0);
    u32 hb_flags = (u32)cfg_int("app_hb_flags", 0);
    sv_log("supervisor (app) port %u game %016llx dir %s", (unsigned)s_cfg.port, (unsigned long long)s_cfg.game_title_id,
           s_cfg.dir[0] ? s_cfg.dir : "none");
    bool hb = sv_hb_start();
    sv_log("supervisor (app): service %s %s (rc %08lx)", CTRDK_SV_SERVICE, hb ? "registered" : "failed",
           (unsigned long)sv_hb_result());

    void* soc_buf = memalign(0x1000, SOC_SIZE);
    Thread t = NULL;
    if (soc_buf != NULL && R_SUCCEEDED(socInit(soc_buf, SOC_SIZE))) {
        t = threadCreate(server_main, NULL, 0x4000, 0x30, 1, false);
    }
    if (t == NULL) {
        printf("socInit or thread failed\n");
    }

    u64 start = svcGetSystemTick();
    u32 frame = 0;
    bool stopped = false;
    while (aptMainLoop() && !sv_exit_requested) {
        LightLock_Lock(&s_lock);
        for (; s_shown < s_n; s_shown++) {
            printf("%.50s\n", s_lines[s_shown % 8]);
        }
        LightLock_Unlock(&s_lock);
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        bool live = hb_stop <= 0 || (svcGetSystemTick() - start) / SYSCLOCK_ARM11 < (u64)hb_stop;
        if (live) {
            ctrdk_supervisor_heartbeat(++frame, hb_flags);
        } else if (!stopped) {
            stopped = true;
            sv_log("supervisor (app): heartbeat frame frozen at %lu (app_hb_stop)", (unsigned long)frame);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    s_quit = true;
    if (t != NULL) {
        threadJoin(t, 2000000000LL);
        threadFree(t);
    }
    sv_hb_stop();
    socExit();
    gfxExit();
    return 0;
}
