/* qtmbench: New 3DS memory benchmark. QTM (the New 3DS extra RAM at
 * 0x1E800000, first 2 MiB) against FCRAM (heap and linear) and VRAM, from
 * the CPU (each core) and the GPU, with cache-coherency checks and a
 * contention matrix.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * Runs every test at launch, writes sdmc:/3ds/qtmbench/results.json and
 * results.txt (and log.txt as it goes), then idles with the devupdate server
 * up, so the PC can pull the results ("devupdate.py files get
 * @/results.json") and push or restart it with other switches.
 *
 * Switches (APT deliver arguments from devupdate, 3dslink arguments, or
 * sdmc:/3ds/qtmbench/switches.txt):
 *   quick         short run (emulators): fewer sizes, small budgets
 *   only=a,b      suites: cpu_bw,cpu_lat,cpu_rand,cache,conc,gpu,coherency,contend,perf
 *   budget=KiB    bytes per bandwidth trial (default 2048)
 *   trials=N      trials per bandwidth point (default 3)
 *   window=ms     concurrency and contention window (default 250)
 *   chase=N       loads per latency point (default 262144)
 *   gpucore=N     core of the GPU feeder threads in contend (default 1)
 *   devupdateport=N  the devupdate server port (default 5050)
 *   nocore1 nocore2 nopte norun autoexit
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bench.h"
#include "build_stamp.h"
#include "ctrdk/crash.h"
#include "ctrdk/devupdate.h"

extern u32 __stacksize__;

Opts g_opt;
static char s_sw[1024];
static PrintConsole s_top, s_bottom;

static bool sw_has(const char* word) {
    size_t n = strlen(word);
    for (const char* p = s_sw; (p = strstr(p, word)) != NULL; p += n) {
        if ((p == s_sw || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ' || p[n] == '=')) {
            return true;
        }
    }
    return false;
}

static bool sw_value(const char* key, char* out, size_t n) {
    size_t kl = strlen(key);
    for (const char* p = s_sw; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == s_sw || p[-1] == ' ') && p[kl] == '=') {
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

static long sw_int(const char* key, long def) {
    char v[24];
    return sw_value(key, v, sizeof(v)) ? strtol(v, NULL, 0) : def;
}

static void read_switches(int argc, char** argv) {
    snprintf(s_sw, sizeof(s_sw), "%s", devupdate_launch_args());
    for (int i = 1; i < argc; i++) {
        size_t l = strlen(s_sw);
        snprintf(s_sw + l, sizeof(s_sw) - l, "%s%s", l ? " " : "", argv[i]);
    }
    FILE* f = fopen(QB_DIR "/switches.txt", "r");
    if (f != NULL) {
        size_t l = strlen(s_sw);
        if (l > 0 && l + 2 < sizeof(s_sw)) {
            s_sw[l++] = ' ';
        }
        size_t got = fread(s_sw + l, 1, sizeof(s_sw) - l - 1, f);
        s_sw[l + got] = '\0';
        fclose(f);
        for (char* c = s_sw; *c; c++) {
            if (*c == '\r' || *c == '\n') {
                *c = ' ';
            }
        }
    }
}

/* ---- crash and devupdate glue ---- */

static void on_recovery_exit(bool relaunch, void* user) { crash_recovery_exit(relaunch); }
static bool on_recovery(const char* report, void* user) { return devupdate_enter_recovery(report); }
static void crash_sink(const char* line, int len, void* user) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s", len, line);
    buf[strcspn(buf, "\n")] = '\0';
    logf_("%s", buf);
}
static void crash_info(CrashAppInfo* out, void* user) {
    out->build = CTRDK_BUILD_STAMP;
    out->phase = "benchmark";
}
static void du_log(int level, const char* text, void* user) { logf_("devupdate: %s", text); }

static void relaunch_now(void) {
    out_write();
    logf_("relaunching for devupdate");
    out_close();
    crash_wd_quiesce();
    devupdate_relaunch();
}

void ui_poll(void) {
    if (devupdate_relaunch_requested()) {
        relaunch_now();
    }
}

/* ---- system facts ---- */

static void core_probe(void* arg) { *(volatile int*)arg = 1; }

/* The kernel accepted a thread for the core and it ran (svcGetProcessorID is
 * not checked: Azahar does not report it). */
static bool core_usable(int core) {
    volatile int ran = 0;
    Thread t = threadCreate(core_probe, (void*)&ran, 4096, PRIO_WORK, core, false);
    if (t == NULL) {
        return false;
    }
    threadJoin(t, U64_MAX);
    threadFree(t);
    return ran == 1;
}

static void sys_info(void) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    info("build", "%s", CTRDK_BUILD_STAMP);
    info("switches", "%s", s_sw);
    info("model", "%s", n3ds ? "New 3DS" : "Old 3DS");
    /* Citra/Azahar answer 0x20000 (CITRA_INFORMATION) with 1, and the Luma
     * version query 0x10000 with 0; Luma itself gives its (non-zero) version. */
    s64 v = 0, lv = -1;
    bool citra = R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v == 1;
    bool lq = R_SUCCEEDED(svcGetSystemInfo(&lv, 0x10000, 0));
    g_opt.emulator = citra || (lq && lv == 0);
    info("emulator", "%s", g_opt.emulator ? "yes (Azahar/Citra: timings are not hardware timings)" : "no");
    if (lq && lv != 0) {
        info("luma", "%lld.%lld.%lld", (lv >> 24) & 0xFF, (lv >> 16) & 0xFF, (lv >> 8) & 0xFF);
    }
    u32 kv = osGetKernelVersion();
    info("kernel", "%lu.%lu-%lu", (unsigned long)GET_VERSION_MAJOR(kv), (unsigned long)GET_VERSION_MINOR(kv),
         (unsigned long)GET_VERSION_REVISION(kv));
    s64 appmem = 0;
    svcGetSystemInfo(&appmem, 0, 1);
    info("app_memory", "%lld KiB used of %lu KiB", appmem / 1024,
         (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024));
    info("linear_free", "%lu KiB", (unsigned long)(linearSpaceFree() / 1024));
    Result r = APT_SetAppCpuTimeLimit(80);
    u32 lim = 0;
    APT_GetAppCpuTimeLimit(&lim);
    info("core1_limit", "%lu%% (set 80: rc %08lX)", (unsigned long)lim, (unsigned long)r);
    if (n3ds) {
        osSetSpeedupEnable(true);
    }
    g_opt.core1 = !sw_has("nocore1") && core_usable(1);
    g_opt.core2 = !sw_has("nocore2") && core_usable(2);
    info("cores", "core0 yes, core1 %s, core2 %s", g_opt.core1 ? "yes" : "no", g_opt.core2 ? "yes" : "no");
    /* svcGetSystemTick cost, for reading the small timings */
    u64 t0 = tick();
    volatile u64 sink = 0;
    for (int i = 0; i < 10000; i++) {
        sink += svcGetSystemTick();
    }
    (void)sink;
    info("tick_cost", "%.3f us per svcGetSystemTick", t2us(tick() - t0) / 10000);
}

int main(int argc, char** argv) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(QB_DIR, 0777);
    mkdir(QB_DIR "/files", 0777);
    out_open();
    read_switches(argc, argv);

    g_opt.quick = sw_has("quick");
    g_opt.budget = (u32)sw_int("budget", g_opt.quick ? 256 : 2048) * 1024u;
    g_opt.trials = (int)sw_int("trials", g_opt.quick ? 1 : 3);
    g_opt.window_ms = (u32)sw_int("window", g_opt.quick ? 30 : 250);
    g_opt.chase_loads = ((u32)sw_int("chase", g_opt.quick ? 8192 : 262144) + 7) & ~7u;
    g_opt.pte = !sw_has("nopte");
    g_opt.gpu_core = (int)sw_int("gpucore", -1);
    g_opt.autoexit = sw_has("autoexit");
    sw_value("only", g_opt.only, sizeof(g_opt.only));
    if (g_opt.trials < 1) {
        g_opt.trials = 1;
    }

    CrashConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.app_name = "qtmbench";
    cc.report_path = QB_DIR "/crash.txt";
    cc.line_sink = crash_sink;
    cc.app_info = crash_info;
    cc.watchdog_seconds = 0; /* long tests make no frames */
    cc.wrap_svcbreak = true;
    cc.on_recovery = on_recovery;
    crash_init(&cc);
    crash_thread_register("main", __stacksize__);

    DevUpdateConfig dc;
    memset(&dc, 0, sizeof(dc));
    dc.version = CTRDK_BUILD_STAMP;
    dc.log_path = QB_DIR "/log.txt";
    dc.files_dir = QB_DIR "/files";
    dc.files_ro_dir = QB_DIR;
    dc.on_recovery_exit = on_recovery_exit;
    dc.log = du_log;
    dc.port = (uint16_t)sw_int("devupdateport", 0);
    int32_t drc = devupdate_start(&dc);

    gfxInitDefault();
    consoleInit(GFX_TOP, &s_top);
    consoleInit(GFX_BOTTOM, &s_bottom);
    consoleSelect(&s_bottom);
    printf("qtmbench %s\n\nResults: sdmc:/3ds/qtmbench/\nPC: devupdate.py files get\n  @/results.json\n\n"
           "START: exit\n",
           CTRDK_BUILD_STAMP);
    printf("devupdate: %s\n", drc == 0 ? "listening on 5050" : "not started");
    consoleSelect(&s_top);
    status("qtmbench %s", CTRDK_BUILD_STAMP);
    sys_info();
    if (drc != 0) {
        info("devupdate", "start failed %08lX", (unsigned long)drc);
    }

    bool run = !sw_has("norun");
    if (run && !mem_setup()) {
        status("allocation failed");
        run = false;
    }
    if (run) {
        u64 t0 = tick();
        info("options", "quick=%d budget=%luK trials=%d window=%lums chase=%lu only=%s", g_opt.quick,
             (unsigned long)(g_opt.budget / 1024), g_opt.trials, (unsigned long)g_opt.window_ms,
             (unsigned long)g_opt.chase_loads, g_opt.only[0] ? g_opt.only : "all");
        bool gpu = gpu_init();
        perf_suite();
        cpu_suite();
        cache_suite();
        conc_suite();
        if (gpu) {
            gpu_suite();
            coherency_suite();
        }
        contend_suite();
        out_write();
        status("mapping report");
        mem_report();
        out_write();
        if (g_opt.pte) {
            /* Last: a wrong guess about the kernel's mappings would stop the
             * console here, after everything else is saved. */
            status("page tables");
            mem_pte_walk();
        }
        info("duration", "%.1f s", t2us(tick() - t0) / 1e6);
        out_write();
        if (gpu) {
            gpu_exit();
        }
        status("QTMBENCH DONE");
    }

    if (!g_opt.autoexit) {
        printf("\nSTART: exit\n");
    }
    while (aptMainLoop() && !g_opt.autoexit) {
        ui_poll();
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    crash_wd_quiesce();
    devupdate_stop();
    out_close();
    gfxExit();
    return 0;
}
