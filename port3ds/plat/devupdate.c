/* TTYD's glue to the ctr-devkit updater (libs/ctr-devkit/devupdate/).
 * On in development builds (PORT3DS_VERSION contains "-dev") unless the switch
 * nodevupdate is given or the option "devupdate" (Select menu, options.txt) is off.
 * Release builds are off unless the switch devupdate is given, the option is on,
 * or this launch is a devupdate relaunch.
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
 *   - a crash hands over to the updater's recovery mode (plat/crash.c);
 *   - queue runs (devupdate.py queue ...): the library's files and queue
 *     commands keep their files in sdmc:/3ds/ttyd/queue/ and write the card
 *     through plat/sdio.c; each log line goes to the queue's `until` matcher
 *     and each game frame is counted. See docs/PORT_3DS_TESTING.md, "Console walk".
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#include "ctrdk/crash.h"
#include "ctrdk/devupdate.h"
#include "plat/audio.h"
#include "plat/gpu.h"
#include "plat/options.h"
#include "plat_internal.h"
#include "port3ds_build.h"

static volatile int s_pause_req, s_paused, s_relaunch;
static LightEvent s_resume;
static int (*s_save_busy)(void);
static char s_token[32];

void plat_set_save_busy(int (*fn)(void)) { s_save_busy = fn; }
/* On when this launch is a devupdate relaunch (which keeps the server on even
 * when the restart gave no switches), or the switch devupdate is given, or
 * this is a development build and the switch nodevupdate is not given. */
int plat_devupdate_enabled(void) {
    static int s_enabled = -1; /* decided once, at boot (the option is read from options.txt then) */
    if (s_enabled >= 0) {
        return s_enabled;
    }
    int opt = plat_opt_get(PLAT_OPT_KEY_DEVUPDATE, -1); /* -1: the player never set it */
    int dev_build = strstr(PORT3DS_VERSION, "-dev") != NULL;
    if (devupdate_launched() || plat_switch_has("devupdate") || opt == 1) {
        s_enabled = 1;
    } else {
        s_enabled = dev_build && !plat_switch_has("nodevupdate") && opt != 0;
    }
    return s_enabled;
}

/* The option's value as the Select menu shows it: what the next launch will do. */
int plat_devupdate_wanted(void) {
    int opt = plat_opt_get(PLAT_OPT_KEY_DEVUPDATE, -1);
    if (opt >= 0) {
        return opt;
    }
    return strstr(PORT3DS_VERSION, "-dev") != NULL && !plat_switch_has("nodevupdate");
}

/* "192.168.1.20:5050" while the server runs and the console has an address, else "". */
void plat_devupdate_address(char* out, size_t n) {
    out[0] = '\0';
    if (!devupdate_running()) {
        return;
    }
    unsigned long ip = (uint32_t)gethostid(); /* libctru's soc: the console's address, network byte order */
    if (ip == 0 || ip == 0x0100007f) {
        return;
    }
    snprintf(out, n, "%lu.%lu.%lu.%lu:%d", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
             plat_switch_int("devupdateport", DEVUPDATE_DEFAULT_PORT));
}

/* Restart the title now with the current switches (the way a devupdate restart does), so a
 * changed option or the server takes effect at once. */
void plat_relaunch(void) {
    PLOG_I("relaunch requested, args \"%s\"", plat_switches_text());
    plat_options_save();
    devupdate_set_next_args(plat_switches_text());
    devupdate_prepare_relaunch();
    plat_quit(1);
}

/* The files and queue commands write the card through plat/sdio.c (core 1), like every other writer. */
static int st_write(const char* path, uint64_t off, const void* data, size_t n, void* user) {
    (void)user;
    return off == 0 ? plat_sd_write_file(path, data, n, 0) : plat_sd_write_at(path, off, data, n);
}
static int st_append(const char* path, const void* data, size_t n, void* user) {
    (void)user;
    return plat_sd_append(path, data, n);
}
static int st_rename(const char* from, const char* to, void* user) {
    (void)user;
    plat_sd_remove(to); /* the queue runs in order: the remove is done before the rename */
    return plat_sd_rename(from, to);
}
static int st_remove(const char* path, void* user) {
    (void)user;
    return plat_sd_remove(path);
}
static int st_mkdir(const char* path, void* user) {
    (void)user;
    return plat_sd_mkdir(path);
}
static void st_flush(void* user) {
    (void)user;
    plat_sd_flush();
}
static void* st_read(const char* path, size_t* n, void* user) {
    (void)user;
    return plat_sd_read_file(path, n);
}
static const DevUpdateStore s_store = {st_write, st_append, st_rename, st_remove, st_mkdir, st_flush, st_read};

/* Game thread, once per frame (plat_frame_poll): the pause point. */
void plat_devupdate_frame(void) {
    devupdate_queue_frame(); /* a queue entry's frame count (a plain return when no queue runs) */
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
    cfg.files_dir = "queue"; /* sdmc:/3ds/ttyd/queue: uploaded padfiles, queue.txt, results.txt, out/ */
    cfg.store = &s_store;
    int32_t rc = devupdate_start(&cfg);
    if (rc != 0) {
        PLOG_E("devupdate: not started (%08lx)", (unsigned long)rc);
    } else if (devupdate_queue_active()) {
        plat_log_set_line_hook(devupdate_queue_log_line); /* the entry's `until` pattern is matched against the log */
    }
}

void plat_devupdate_stop(void) { devupdate_stop(); }
