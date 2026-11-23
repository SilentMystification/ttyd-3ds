/* ctr-devkit example: devupdate, and the generic bootstrap.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * Shows its build stamp and the arguments this launch received, and runs
 * the devupdate server: from the PC, devupdate.py ping / push / restart /
 * report. A push of this example's own CIA relaunches it, so the build
 * stamp visibly changes.
 *
 * Built as a 3dsx (devupdate-boot.3dsx) it is the generic bootstrap: sent
 * with 3dslink, it receives any CIA (devupdate.py bootstrap), installs it
 * through AM and jumps into it with the switches as deliver arguments.
 *
 * It links the crash module too: X runs crash_test("fatal"), the launch
 * argument crashtest=<kind> runs a test at frame 120, and for emulators a
 * file DIR/crashnow (holding the kind) runs one at once. A crash hands over
 * to devupdate's recovery mode (report, push, restart from the PC).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>

#include "build_stamp.h"
#include "ctrdk/crash.h"
#include "ctrdk/devupdate.h"

#define DIR "sdmc:/3ds/ctrdk-devupdate"
#define LOG_PATH DIR "/latest.log"

extern u32 __stacksize__; /* libctru: the main thread's stack size (default 32 KiB) */

static FILE* s_log;
static LightLock s_lock;
static char s_msgs[8][96];
static int s_nmsg, s_shown;

static void add_msg(int level, const char* text) {
    LightLock_Lock(&s_lock);
    if (s_log != NULL) {
        fprintf(s_log, "[%c] %s\n", "EWI"[level < 0 ? 0 : level > 2 ? 2 : level], text);
        fflush(s_log);
    }
    snprintf(s_msgs[s_nmsg % 8], sizeof(s_msgs[0]), "%s", text);
    s_nmsg++;
    LightLock_Unlock(&s_lock);
}

static void msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void msg(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    add_msg(2, buf);
}

/* Printed by the main thread only (the console is not thread-safe). */
static void show_msgs(PrintConsole* con) {
    LightLock_Lock(&s_lock);
    consoleSelect(con);
    for (; s_shown < s_nmsg; s_shown++) {
        printf("%.40s\n", s_msgs[s_shown % 8]);
    }
    LightLock_Unlock(&s_lock);
}

static void log_cb(int level, const char* text, void* user) {
    (void)user;
    add_msg(level, text);
}

static void on_begin(void* user) {
    (void)user;
    msg("update: begin");
}
static void on_progress(uint32_t done, uint32_t total, void* user) {
    (void)user;
    if ((done & 0xFFFFF) < 0x10000 || done == total) {
        msg("update: %lu / %lu KiB", (unsigned long)(done >> 10), (unsigned long)(total >> 10));
    }
}
static bool on_ready(void* user) {
    (void)user;
    msg("update: ready, relaunching");
    if (s_log != NULL) {
        fflush(s_log);
    }
    return true;
}
static void on_resume(void* user) {
    (void)user;
    msg("update: resumed");
}
static void on_recovery_exit(bool relaunch, void* user) {
    (void)user;
    crash_recovery_exit(relaunch);
}

static bool on_recovery(const char* report, void* user) {
    (void)user;
    return devupdate_enter_recovery(report);
}
static void crash_sink(const char* line, int len, void* user) {
    (void)user;
    if (s_log != NULL) {
        fwrite(line, 1, (size_t)len, s_log);
        fflush(s_log);
    }
}
static void crash_info(CrashAppInfo* out, void* user) {
    (void)user;
    out->build = CTRDK_BUILD_STAMP;
    out->phase = "main loop";
}

/* "crashtest=fatal" in the switch text. */
static bool switch_value(const char* text, const char* key, char* out, size_t n) {
    size_t kl = strlen(key);
    for (const char* p = text; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == text || p[-1] == ' ') && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (p[i] != '\0' && p[i] != ' ' && i + 1 < n) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    LightLock_Init(&s_lock);
    mkdir("sdmc:/3ds", 0777);
    mkdir(DIR, 0777);
    unlink(DIR "/previous.log");
    rename(LOG_PATH, DIR "/previous.log");
    s_log = fopen(LOG_PATH, "w");
    APT_SetAppCpuTimeLimit(30); /* the server thread runs on core 1 */

    /* Switches: this launch's deliver arguments, then the 3dslink ones, then
     * DIR/switches.txt (for emulators, which cannot relaunch). */
    static char sw[1024];
    snprintf(sw, sizeof(sw), "%s", devupdate_launch_args());
    for (int i = 1; i < argc; i++) {
        size_t l = strlen(sw);
        snprintf(sw + l, sizeof(sw) - l, "%s%s", l ? " " : "", argv[i]);
    }
    FILE* sf = fopen(DIR "/switches.txt", "r");
    if (sf != NULL) {
        size_t l = strlen(sw);
        if (l + 2 < sizeof(sw) && l > 0) {
            sw[l++] = ' ';
        }
        size_t got = fread(sw + l, 1, sizeof(sw) - l - 1, sf);
        sw[l + got] = '\0';
        fclose(sf);
        sw[strcspn(sw, "\r\n")] = '\0';
    }
    static char test[16];
    bool have_test = switch_value(sw, "crashtest", test, sizeof(test));

    CrashConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.app_name = "ctr-devkit devupdate example";
    cc.report_path = DIR "/crash.txt";
    cc.line_sink = crash_sink;
    cc.app_info = crash_info;
    cc.log = log_cb;
    cc.watchdog_seconds = 5;
    cc.wrap_svcbreak = true;
    cc.on_recovery = on_recovery;
    cc.test = have_test ? test : NULL;
    cc.test_frame = 120;
    crash_init(&cc);
    crash_thread_register("main", __stacksize__); /* libctru allocates the main stack: __stacksize__ bytes */

    msg("build %s", CTRDK_BUILD_STAMP);
    msg("launch args \"%s\"", devupdate_launch_args());

    DevUpdateConfig dc;
    memset(&dc, 0, sizeof(dc));
    dc.version = CTRDK_BUILD_STAMP;
    dc.allow_3dsx_install = envIsHomebrew(); /* the 3dsx build is the bootstrap */
    dc.log_path = LOG_PATH;
    dc.on_begin = on_begin;
    dc.on_progress = on_progress;
    dc.on_ready = on_ready;
    dc.on_resume = on_resume;
    dc.on_recovery_exit = on_recovery_exit;
    dc.log = log_cb;
    char port[8];
    if (switch_value(sw, "devupdateport", port, sizeof(port))) {
        dc.port = (uint16_t)atoi(port);
    }
    int32_t rc = devupdate_start(&dc);
    if (rc != 0) {
        msg("devupdate_start failed: %08lx", (unsigned long)rc);
    }

    gfxInitDefault();
    PrintConsole top, bottom;
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    consoleSelect(&top);
    printf("ctr-devkit devupdate example\n\n");
    printf("build  %s\n", CTRDK_BUILD_STAMP);
    printf("kind   %s%s\n", envIsHomebrew() ? "3dsx" : "CIA", envIsHomebrew() ? " (bootstrap)" : "");
    printf("args   \"%.60s\"\n", sw);
    printf("\nX: crash (recovery)   START: exit\n");

    while (aptMainLoop()) {
        crash_wd_progress();
        show_msgs(&bottom);
        if (devupdate_relaunch_requested()) {
            show_msgs(&bottom);
            if (s_log != NULL) {
                fclose(s_log);
                s_log = NULL;
            }
            crash_wd_quiesce();
            devupdate_relaunch(); /* a CIA exits here; a 3dsx jump returns and aptMainLoop ends */
        }
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        if (down & KEY_X) {
            crash_test("fatal");
        }
        /* For emulator tests (no buttons): DIR/crashnow holding a test kind. */
        if (crash_frame_count() % 30 == 0) {
            FILE* cf = fopen(DIR "/crashnow", "r");
            if (cf != NULL) {
                char kind[16] = "fatal";
                if (fgets(kind, sizeof(kind), cf) == NULL || kind[0] < 'a') {
                    strcpy(kind, "fatal");
                }
                fclose(cf);
                unlink(DIR "/crashnow");
                kind[strcspn(kind, " \r\n")] = '\0';
                crash_test(kind);
            }
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    crash_wd_quiesce();
    devupdate_stop();
    gfxExit();
    if (s_log != NULL) {
        fclose(s_log);
    }
    return 0;
}
