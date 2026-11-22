/* TTYD's glue to the ctr-devkit updater (libs/ctr-devkit/devupdate/).
 * Development use only: off unless the switch devupdate is given.
 *
 *   devupdate             start the server at boot (TCP 5050)
 *   devupdateport=N       another port
 *   devupdatetoken=TEXT   a shared secret the PC tool must send
 *
 * From the PC: libs/ctr-devkit/devupdate/devupdate.py push|restart|ping|report.
 *   - on_begin (a push or restart starts) pauses the game thread at the next
 *     frame boundary (plat_frame_poll), with the GPU idle, mutes the DSP,
 *     lowers the core 1 limit to 30% (the network has more of core 1) and
 *     holds the watchdog;
 *   - on_ready waits for a save write in progress (see below);
 *   - on_relaunch wakes the game thread, which leaves through plat_quit(1):
 *     the normal exit, whose aptExit starts the new build with the switches
 *     as APT deliver arguments (plat_switches_load reads them);
 *   - on_resume undoes on_begin when there is no relaunch;
 *   - a crash hands over to the updater's recovery mode (plat/crash.c).
 */
#include <string.h>

#include "ctrdk/crash.h"
#include "ctrdk/devupdate.h"
#include "plat/audio.h"
#include "plat/gpu.h"
#include "plat_internal.h"

static volatile int s_pause_req, s_paused, s_relaunch;
static LightEvent s_resume;
static int (*s_save_busy)(void);
static char s_token[32];

void plat_set_save_busy(int (*fn)(void)) { s_save_busy = fn; }
/* The switch, or this launch is a devupdate relaunch (which keeps the server
 * on even when the restart gave no switches). */
int plat_devupdate_enabled(void) { return plat_switch_has("devupdate") || devupdate_launched(); }

/* Game thread, once per frame (plat_frame_poll): the pause point. */
void plat_devupdate_frame(void) {
    if (!s_pause_req) {
        return;
    }
    plat_gpu_sync(); /* the render thread has run everything this frame recorded */
    s_paused = 1;
    PLOG_I("devupdate: game thread paused at frame %lu", (unsigned long)plat_wd_frames());
    LightEvent_Wait(&s_resume);
    s_paused = 0;
    if (s_relaunch) {
        PLOG_I("devupdate: relaunching");
        plat_quit(1);
    }
    PLOG_I("devupdate: game thread resumed");
}

static void on_begin(void* user) {
    (void)user;
    crash_wd_hold(true); /* no frames while paused: not a hang */
    s_relaunch = 0;
    LightEvent_Clear(&s_resume);
    s_pause_req = 1;
    for (int i = 0; i < 300 && !s_paused; i++) {
        svcSleepThread(10000000LL); /* up to 3 s for the game to reach a frame boundary */
    }
    if (!s_paused) {
        PLOG_W("devupdate: the game thread did not reach a frame boundary in 3 s; going on");
    }
    if (plat_audio_has_output()) {
        ndspSetMasterVol(0.0f);
    }
    plat_cpu_limit_transfer(1);
}

static void on_resume(void* user) {
    (void)user;
    plat_cpu_limit_transfer(0);
    if (plat_audio_has_output()) {
        ndspSetMasterVol(1.0f);
    }
    s_pause_req = 0;
    LightEvent_Signal(&s_resume);
    crash_wd_hold(false);
}

/* Before a relaunch. This only waits for a save write in progress to finish:
 * the card layer (port3ds/game/card.c) opens, writes, fsyncs and closes each
 * card file per operation, so once its queue is empty no card file is open.
 * It never deletes, truncates, reformats or recreates save files, and the
 * CIA install leaves sdmc:/3ds/ttyd/ (card/ and everything else) untouched. */
static bool on_ready(void* user) {
    (void)user;
    int waited = 0;
    while (((s_save_busy != NULL && s_save_busy()) || crash_wd_held() > 1) && waited < 1000) {
        svcSleepThread(10000000LL); /* up to 10 s */
        waited++;
    }
    if (waited >= 1000) {
        PLOG_W("devupdate: a save was still running after 10 s");
    } else if (waited > 0) {
        PLOG_I("devupdate: waited %d ms for a save to finish", waited * 10);
    }
    return true;
}

static void on_relaunch(void* user) {
    (void)user;
    s_relaunch = 1;
    s_pause_req = 1; /* a game thread that has not paused yet stops at its next frame and leaves */
    crash_wd_hold(false);
    LightEvent_Signal(&s_resume);
}

static void on_recovery_exit(bool relaunch, void* user) {
    (void)user;
    crash_recovery_exit(relaunch);
}

static void log_line(int level, const char* text, void* user) {
    (void)user;
    plat_log(level, "%s", text);
}

static bool recovery_hook(const char* report, void* user) {
    (void)user;
    return devupdate_enter_recovery(report);
}

PlatRecoveryHook plat_devupdate_recovery_hook(void) { return plat_devupdate_enabled() ? recovery_hook : NULL; }

void plat_devupdate_init(void) {
    if (!plat_devupdate_enabled()) {
        return;
    }
    LightEvent_Init(&s_resume, RESET_ONESHOT);
    PLOG_I("devupdate: launch args \"%s\"%s", devupdate_launch_args(),
           devupdate_launched() ? " (relaunched by devupdate)" : "");
    DevUpdateConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = (uint16_t)plat_switch_int("devupdateport", DEVUPDATE_DEFAULT_PORT);
    if (plat_switch_value("devupdatetoken", s_token, sizeof(s_token))) {
        cfg.token = s_token;
    }
    cfg.version = plat_build_string();
    cfg.log_path = "log.txt";
    cfg.on_begin = on_begin;
    cfg.on_ready = on_ready;
    cfg.on_resume = on_resume;
    cfg.on_relaunch = on_relaunch;
    cfg.on_recovery_exit = on_recovery_exit;
    cfg.log = log_line;
    int32_t rc = devupdate_start(&cfg);
    if (rc != 0) {
        PLOG_E("devupdate: not started (%08lx)", (unsigned long)rc);
    }
}

void plat_devupdate_stop(void) { devupdate_stop(); }
