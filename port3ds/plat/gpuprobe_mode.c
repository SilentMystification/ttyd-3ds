/* In-game development tool: the PICA rules probe (libs/ctr-devkit/gpuprobe, ctrdk/gpuprobe.h).
 *
 * Switch "gpuprobe" (development builds only; a release build ignores it) makes the process run the
 * probe INSTEAD of the game: boot.c calls plat_gpuprobe_mode_run() after the log, crash handler,
 * watchdog and devupdate server are up and before any game or GPU init. It never returns: after the
 * probe it keeps the devupdate server answering (restart, push, files get) until START or a relaunch.
 *
 *   gpuprobe              run the probe
 *   probefresh            ignore the journal, run every case again (without it a restart resumes after a FAULT)
 *   probeselftest         add the case that never submits (tests the timeout and the reset)
 *   probe=ids             only these case ids, groups, or prefix* (comma list)
 *   probesafe             skip the cases that may fault memory
 *   probetimeout=ms       wait per case
 *
 * Output: sdmc:/3ds/ttyd/gpuprobe.txt (table), gpuprobe.journal, gpuprobe/<case>.bin; from the PC:
 *   devupdate.py files get @/gpuprobe.txt
 * The log line "GPUPROBE DONE ..." gives the summary.
 */
#include <stdio.h>
#include <string.h>

#include "ctrdk/gpuprobe.h"
#include "plat_internal.h"

#define PROBE_DIR "sdmc:/3ds/ttyd" /* devupdate's read-only folder: files get @/gpuprobe.txt */

static int s_mode;

/* The switch is honoured in development builds only. */
int plat_gpuprobe_mode(void) { return s_mode; }
int plat_gpuprobe_requested(void) { return plat_is_dev_build() && plat_switch_has("gpuprobe"); }

static void probe_log(const char* line, void* user) {
    (void)user;
    PLOG_I("%s", line);
}

static bool on_emulator(void) {
    s64 v = 0, lv = -1;
    bool citra = R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v == 1;
    bool lq = R_SUCCEEDED(svcGetSystemInfo(&lv, 0x10000, 0));
    return citra || (lq && lv == 0);
}

static void show(const char* l1, const char* l2) {
    consoleInit(GFX_TOP, NULL);
    printf("\n  TTYD 3DS port: GPU probe mode\n  build %s\n\n  %s\n  %s\n\n  The game does not start in this mode.\n"
           "  devupdate.py restart \"<switches>\" to leave.\n  START: exit\n",
           plat_build_string(), l1, l2);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void plat_gpuprobe_mode_run(void) {
    s_mode = 1;
    PLOG_I("gpuprobe mode: the game does not start; switches \"%s\"", plat_switches_text());
    plat_set_phase("gpuprobe");
    gfxInitDefault();
    show("Running the probe...", "(the GPU hangs on purpose)");

    GpuProbeConfig pc;
    memset(&pc, 0, sizeof(pc));
    pc.dir = PROBE_DIR;
    pc.timeout_ms = plat_switch_int("probetimeout", 0);
    char only[128];
    pc.only = plat_switch_value("probe", only, sizeof(only)) ? only : NULL;
    pc.fresh = plat_switch_has("probefresh") != 0;
    pc.selftest = plat_switch_has("probeselftest") != 0;
    pc.no_dangerous = plat_switch_has("probesafe") != 0;
    pc.emulator = on_emulator();
    pc.log = probe_log;

    plat_wd_hold(1); /* a hung case makes no frames */
    GpuProbeSummary ps;
    memset(&ps, 0, sizeof(ps));
    int bad = gpuprobe_run(&pc, &ps);
    plat_wd_hold(0);
    PLOG_I("GPUPROBE DONE: %d OK, %d not OK (%d WRONG, %d HANG, %d FAULT, %d ERR, %d SKIP; %d of %d cases from the journal)",
           ps.ok, bad, ps.wrong, ps.hang, ps.fault, ps.err, ps.skip, ps.cached, ps.total);

    char l1[64], l2[96];
    snprintf(l1, sizeof l1, "GPUPROBE DONE: %d OK, %d not OK", ps.ok, bad);
    snprintf(l2, sizeof l2, "files get @/gpuprobe.txt");
    show(l1, l2);
    while (aptMainLoop()) {
        plat_wd_progress();
        plat_devupdate_frame(); /* pauses for an update and leaves for a relaunch */
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gspWaitForVBlank();
    }
    plat_quit(0);
}
