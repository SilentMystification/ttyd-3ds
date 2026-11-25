/* ctr-devkit devupdate: over-the-air CIA install and relaunch for libctru
 * homebrew, and a recovery mode after a crash. DEVELOPMENT USE ONLY: anyone
 * on the network can install a title while it runs (an optional token
 * limits that to whoever knows it).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * A server thread listens on a TCP port (default 5050). The PC tool
 * (devupdate/devupdate.py) sends a 64-byte header and a payload:
 *   ping     title ID, version, free space, whether it runs as a CIA or 3dsx
 *   push     a CIA, streamed into AM (no copy in RAM), CRC32-checked before
 *            the install is committed; on any error the install is cancelled
 *   restart  relaunch this title with the given switches
 *   report   the crash report and the app's log tail (recovery mode)
 *   launch   jump into an installed title (the bootstrap)
 *   files    put/get/ls/rm in one app-chosen SD folder (files_dir), CRC32-checked
 *   queue    add/list/clear/start/status: a list of test runs that the title
 *            works through by relaunching itself (see "Queue" below)
 * The device answers with text lines and ends each request with "done" or
 * "error <code> <text>".
 *
 * Switches for the next launch travel as APT deliver arguments
 * (aptSetChainloaderArgs); devupdate_launch_args() reads them at boot. They
 * belong to that one launch: a cold start from the HOME Menu gets none.
 */
#ifndef CTRDK_DEVUPDATE_H
#define CTRDK_DEVUPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVUPDATE_DEFAULT_PORT 5050
#define DEVUPDATE_MAX_ARGS 0x2F0 /* switch text per launch (APT allows 0x300 with the prefix) */

/* Where the files and queue commands put bytes on the SD card. All paths are
 * relative to the app's working directory (or absolute "sdmc:/..."). Every
 * callback may be NULL: a plain stdio default is used (fine for an app with
 * no SD layer of its own). An app that keeps SD access on one core passes
 * callbacks that queue the work there. All return 0 on success. */
typedef struct {
    /* offset 0 creates or truncates the file; later chunks follow in order */
    int (*write)(const char* path, uint64_t offset, const void* data, size_t n, void* user);
    int (*append)(const char* path, const void* data, size_t n, void* user);
    int (*rename)(const char* from, const char* to, void* user); /* replaces an existing target */
    int (*remove)(const char* path, void* user);
    int (*mkdir)(const char* path, void* user); /* 0 also if it exists */
    void (*flush)(void* user);                  /* everything queued so far has reached the card */
    /* whole file: malloc'd block with a NUL after the n bytes, NULL if absent */
    void* (*read)(const char* path, size_t* n, void* user);
} DevUpdateStore;

/* What an update is doing, for a progress screen (DevUpdateConfig.on_status). */
typedef enum {
    DEVUPDATE_PH_RECEIVING = 1, /* a CIA is arriving: done and total are bytes */
    DEVUPDATE_PH_CHECKING,      /* the CRC32 of the whole payload */
    DEVUPDATE_PH_INSTALLING,    /* AM_FinishCiaInstall (the commit) */
    DEVUPDATE_PH_RELAUNCHING,   /* the next launch is set; the process ends next. text: its switches */
    DEVUPDATE_PH_ERROR          /* the update stopped; text says why; on_resume follows */
} DevUpdatePhase;

typedef struct {
    uint16_t port;          /* 0 = 5050 */
    const char* token;       /* shared secret, up to 31 characters; NULL = none (LAN dev use only) */
    int core;                /* server and recovery threads; 0 = core 1 (falls back to core 0) */
    const char* version;     /* the app's version string, for ping */
    /* A 3dsx under the Homebrew Launcher refuses CIA installs unless this is
     * set (the generic bootstrap sets it). */
    bool allow_3dsx_install;
    const char* log_path;    /* the app's log, whose tail "report" sends; NULL = none */
    const char* report_path; /* the app's crash report file (ctrdk crash: report_path): "report" sends its tail when no
                                crash of this run left an in-memory report (a crash that never reached recovery, or the
                                launch after one); NULL = none */
    int recovery_net_seconds;    /* recovery ends the process if the network is not up in time; 0 = 30 */
    uint32_t soc_buffer_size;    /* 0 = 0x80000; reserved at start if the app has not set up sockets */

    /* A push or restart begins: pause the app at a safe point, quiet audio. */
    void (*on_begin)(void* user);
    void (*on_progress)(uint32_t done, uint32_t total, void* user);
    /* Phase changes, and during RECEIVING about every 64 KiB or 100 ms. Called on the server thread
     * between on_begin and on_resume (or the relaunch), so an app may draw a progress screen there.
     * text is NULL or a short message that is only valid during the call. NULL: not called. */
    void (*on_status)(DevUpdatePhase phase, uint32_t done, uint32_t total, const char* text, void* user);
    /* Installed (or restart): let running saves finish, close files. true =
     * relaunch now; false = the app calls devupdate_relaunch() later. */
    bool (*on_ready)(void* user);
    /* The request ended without a relaunch (it failed, or a push asked for
     * none) after on_begin: resume the app. */
    void (*on_resume)(void* user);
    /* Relaunch now: the chainloader and the arguments are set; the app ends
     * through its own exit path (exit() runs aptExit, which starts the next
     * title). NULL: the library only sets devupdate_relaunch_requested(),
     * which the app's main loop checks. */
    void (*on_relaunch)(void* user);
    /* Recovery mode ends the process: relaunch = the chainloader is set.
     * NULL: aptExit() when relaunching, then svcExitProcess(). */
    void (*on_recovery_exit)(bool relaunch, void* user);
    /* files and queue. files_dir (NULL = no files or queue commands) is the one
     * folder they may touch; the queue keeps queue.txt, state.txt, results.txt
     * and out/ in it. The store callbacks get the app's `user` pointer. */
    const char* files_dir;
    /* A second folder the PC may only read: "files get @/<path>", "files ls @/<dir>".
     * put, rm and queue refuse it (error 20). Read with stdio on the server
     * thread. NULL = none. */
    const char* files_ro_dir;
    const DevUpdateStore* store;     /* NULL = stdio */
    const char* queue_padfile_key;   /* switch word that gets an entry's padfile; NULL = "padfile" */
    /* Informational messages: level 0 error, 1 warning, 2 info. */
    void (*log)(int level, const char* text, void* user);
    void* user;
} DevUpdateConfig;

/* Starts the server thread and prepares recovery mode (its thread, the
 * socket buffer, the AM session, the title ID). Returns 0 or an error. */
int32_t devupdate_start(const DevUpdateConfig* cfg);
void devupdate_stop(void);
bool devupdate_running(void);

/* An update is installed and waits for the relaunch. */
bool devupdate_pending(void);
/* A relaunch was requested and on_relaunch is NULL: call devupdate_relaunch(). */
bool devupdate_relaunch_requested(void);
/* From the app's main thread: sets the chainloader (this title, or the
 * title a "launch" or bootstrap push named) with the requested arguments
 * and exits through exit(0). Under the Homebrew Launcher it jumps with
 * APT_PrepareToDoApplicationJump/APT_DoApplicationJump and returns; the
 * app's aptMainLoop then ends. */
void devupdate_relaunch(void);
/* Sets the chainloader for the pending relaunch without exiting, for apps
 * with their own exit path. */
void devupdate_prepare_relaunch(void);
/* The switch text the next launch of this title gets (as for a restart), for an app that
 * relaunches itself without a PC: set it, then devupdate_prepare_relaunch() and exit. */
void devupdate_set_next_args(const char* args);

/* The switch text this launch received as APT deliver arguments; "" if
 * none. Read once (the first call), any time, also before devupdate_start. */
const char* devupdate_launch_args(void);
/* This launch was started by devupdate (a restart, a push's relaunch, the
 * bootstrap's jump), even with no switches. */
bool devupdate_launched(void);
/* The version text the PC tool sent with the push being handled ("" if none): valid from on_begin on. */
const char* devupdate_incoming_version(void);
/* The running build's version (DevUpdateConfig.version) that relaunched this launch ("" if none or a cold start). */
const char* devupdate_launch_from(void);

/* From the app's crash handler, after its report and crash screen: wakes
 * the recovery thread prepared by devupdate_start and returns true (false
 * if devupdate is not running). The caller keeps its screen up and waits;
 * recovery ends the process through on_recovery_exit. */
bool devupdate_enter_recovery(const char* report);
bool devupdate_in_recovery(void);

/* ---- Queue ----
 * `queue add` stores entries on the card; `queue start` relaunches the title
 * for entry 1 with its switches (APT deliver arguments) plus
 * "<padfile key>=<files_dir>/<padfile>" and "ctrdkq=N". The launch that finds
 * ctrdkq=N is running entry N, and the library ends it when
 *   - a log line matches the entry's `until` regular expression (the app feeds
 *     its log lines to devupdate_queue_log_line), or
 *   - the entry's frame count is reached (the app calls devupdate_queue_frame
 *     once per frame), or
 *   - the app calls devupdate_queue_finish, or
 *   - the entry's timeout (seconds from process start) runs out, or
 *   - the app crashes into recovery mode (result "crash", the report is kept).
 * It then records one line in results.txt, moves the entry's `collect` paths to
 * out/<N>_<name>/, logs "[QUEUE] entry N <name> done result <result> ..." and
 * relaunches for entry N+1. After the last entry it logs "[QUEUE] done" and
 * relaunches with no switches (the title idles, reachable). */
bool devupdate_queue_active(void);               /* this launch runs a queue entry */
int devupdate_queue_entry(void);                 /* its number (1-based), 0 if none */
void devupdate_queue_frame(void);                /* once per app frame */
void devupdate_queue_log_line(const char* line); /* each log line, no sequence prefix; calls must not overlap */
void devupdate_queue_finish(const char* result); /* the app ends the entry itself (result: one word) */

#ifdef __cplusplus
}
#endif

#endif
