/* ctr-devkit example: the crash handler.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * Buttons start a self test: A segv, B fatal, X svcBreak, Y hang (the
 * watchdog fires after 3 s), L "self" (the handler with made-up registers,
 * for emulators that never raise the guest's handler), R undefined
 * instruction. START exits.
 *
 * The report is appended to sdmc:/3ds/ctrdk-crash/crash.txt and written to
 * log.txt in the same folder. For unattended runs the test kind can come
 * from the first 3dslink argument or from sdmc:/3ds/ctrdk-crash/test.txt
 * ("break", "fatal", "hang", "self", ...); it runs at frame 30.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "ctrdk/crash.h"

#define DIR "sdmc:/3ds/ctrdk-crash"

extern u32 __stacksize__; /* libctru: the main thread's stack size (default 32 KiB) */

static FILE* s_log;

static void line_sink(const char* line, int len, void* user) {
    (void)user;
    if (s_log != NULL) {
        fwrite(line, 1, (size_t)len, s_log);
        fflush(s_log);
    }
}

static void log_line(int level, const char* text, void* user) {
    (void)user;
    if (s_log != NULL) {
        fprintf(s_log, "[%c] %s\n", "EWI"[level < 0 ? 0 : level > 2 ? 2 : level], text);
        fflush(s_log);
    }
}

static void app_info(CrashAppInfo* out, void* user) {
    (void)user;
    out->build = "example";
    out->phase = "main loop";
    out->lines[0] = "this report comes from the ctr-devkit crash example";
}

static void read_test(int argc, char** argv, char* out, size_t n) {
    out[0] = '\0';
    if (argc > 1 && argv[1] != NULL) {
        snprintf(out, n, "%s", argv[1]);
        return;
    }
    FILE* f = fopen(DIR "/test.txt", "r");
    if (f != NULL) {
        if (fgets(out, (int)n, f) == NULL) {
            out[0] = '\0';
        }
        fclose(f);
        out[strcspn(out, " \r\n")] = '\0';
    }
}

int main(int argc, char** argv) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(DIR, 0777);
    s_log = fopen(DIR "/log.txt", "w");

    static char test[16];
    read_test(argc, argv, test, sizeof(test));

    CrashConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.app_name = "ctr-devkit crash example";
    cfg.report_path = DIR "/crash.txt";
    cfg.line_sink = line_sink;
    cfg.app_info = app_info;
    cfg.log = log_line;
    cfg.watchdog_seconds = 3;
    cfg.screen_wait_seconds = test[0] ? 3 : 0; /* unattended: leave the screen after 3 s */
    cfg.wrap_svcbreak = true;
    cfg.test = test[0] ? test : NULL;
    cfg.test_frame = 30;
    cfg.test_addr = 4;
    crash_init(&cfg);
    crash_thread_register("main", __stacksize__); /* libctru allocates the main stack: __stacksize__ bytes */

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("ctr-devkit crash example\n\n");
    printf("A segv    B fatal   X svcBreak\nY hang    L self    R undef\nSTART exit\n\n");
    if (test[0]) {
        printf("test \"%s\" at frame 30\n", test);
    }
    log_line(2, "crash example running", NULL);

    while (aptMainLoop()) {
        crash_wd_progress();
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        const char* kind = (down & KEY_A) ? "segv" : (down & KEY_B) ? "fatal" : (down & KEY_X) ? "break"
                         : (down & KEY_Y) ? "hang" : (down & KEY_L) ? "self" : (down & KEY_R) ? "undef" : NULL;
        if (kind != NULL) {
            printf("running %s\n", kind);
            gfxFlushBuffers();
            gfxSwapBuffers();
            crash_test(kind);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    crash_wd_quiesce();
    gfxExit();
    if (s_log != NULL) {
        fclose(s_log);
    }
    return 0;
}
