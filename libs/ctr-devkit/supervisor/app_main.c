/* ctr-devkit supervisor, app build: the same server as the system module, run as an ordinary
 * application. For emulators (Azahar cannot start a custom system module) and for checking the
 * protocol from the PC. It is not a supervisor: it ends with the app.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <malloc.h>
#include <stdio.h>
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

int main(void) {
    LightLock_Init(&s_lock);
    sv_log_hook = log_hook;
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("ctr-devkit supervisor (app build)\n%s\n\nSTART: exit\n\n", CTRDK_BUILD_STAMP);
    APT_SetAppCpuTimeLimit(30);

    s_cfg.port = CTRDK_SV_PORT;
    s_cfg.kind = "app";
    s_cfg.version = CTRDK_BUILD_STAMP;
    sv_config_load(&s_cfg);

    void* soc_buf = memalign(0x1000, SOC_SIZE);
    Thread t = NULL;
    if (soc_buf != NULL && R_SUCCEEDED(socInit(soc_buf, SOC_SIZE))) {
        t = threadCreate(server_main, NULL, 0x4000, 0x30, 1, false);
    }
    if (t == NULL) {
        printf("socInit or thread failed\n");
    }
    sv_log("supervisor (app) port %u", (unsigned)s_cfg.port);

    while (aptMainLoop()) {
        LightLock_Lock(&s_lock);
        for (; s_shown < s_n; s_shown++) {
            printf("%.50s\n", s_lines[s_shown % 8]);
        }
        LightLock_Unlock(&s_lock);
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
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
    socExit();
    gfxExit();
    return 0;
}
