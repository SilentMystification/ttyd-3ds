/* ctr-devkit devupdate: see include/ctrdk/devupdate.h and README.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * Threads: the server (accepts one client at a time, blocks in poll) and
 * the recovery thread (dormant on an event until devupdate_enter_recovery),
 * both made at devupdate_start on the configured core. Nothing is
 * allocated after devupdate_start: the transfer buffer is static, the
 * socket buffer is reserved at start.
 *
 * Protocol v1 (little-endian), client to device:
 *   0  char magic[4] "CDU1"
 *   4  u16 version (1)
 *   6  u16 command: 1 ping, 2 push, 3 restart, 4 report, 5 launch, 6 files,
 *      7 queue (6 and 7: the argument text is "put <path>", "get <path>",
 *      "ls <dir>", "rm <path>" or "add", "list", "clear", "start", "status";
 *      put and add carry a payload with size and CRC32 as for push; see
 *      files.c and queue.c)
 *   8  u32 flags: bit 0 relaunch this title after a push, bit 1 jump to the
 *      pushed (or given) title after a push
 *      bit 2: the argument area starts with a version text for the incoming build
 *      (one length byte, then that many characters; args_len counts it; see proto.c)
 *      bit 3: push through the SD card: the CIA is written to files_dir/stage.cia first
 *      (through the app's store), read back and CRC-checked, then installed from there
 *  12  u32 payload size (the CIA)
 *  16  u32 payload CRC32 (zlib's)
 *  20  u32 argument length (switch text for the next launch)
 *  24  u64 title ID (push: the CIA's, from the PC tool; launch: the target)
 *  32  char token[32] (zero padded)
 *  then the arguments, then the payload.
 * Device to client: text lines; the last is "done" or "error <code> <text>".
 */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/iosupport.h>
#include <sys/socket.h>

#include <3ds.h>

#include "ctrdk/devupdate.h"
#include "internal.h"
#include "proto.h"

/* Azahar cannot do an APT application jump. A test harness that wants to
 * emulate the relaunch creates EMU_MARKER; the library then also writes the
 * next launch's arguments to EMU_ARGS when it relaunches, and the harness
 * starts the title again, which reads (and deletes) that file when APT gave it
 * no arguments. On a console neither file exists. */
#define EMU_MARKER "sdmc:/3ds/ctrdk-emulate-relaunch"
#define EMU_ARGS "sdmc:/3ds/ctrdk-launch-args.txt"

#define ARG_PREFIX "ctrdk:"
#define ARG_PREFIX_LEN 6
#define DELIVER_BYTES 0x300
#define CHUNK 0x10000
#define LOG_TAIL 8192
#define THREAD_STACK 0x6000
#define THREAD_PRIO 0x30
#define RELAUNCH_GRACE_S 8 /* a relaunch must have ended the process this soon, or the server thread ends it */

typedef DuHeader Header;
#define CMD_PING DU_CMD_PING
#define CMD_PUSH DU_CMD_PUSH
#define CMD_RESTART DU_CMD_RESTART
#define CMD_REPORT DU_CMD_REPORT
#define CMD_LAUNCH DU_CMD_LAUNCH
#define CMD_FILES DU_CMD_FILES
#define CMD_QUEUE DU_CMD_QUEUE
#define FLAG_RELAUNCH DU_FLAG_RELAUNCH
#define FLAG_LAUNCH DU_FLAG_LAUNCH
#define FLAG_VERSION DU_FLAG_VERSION
#define FLAG_STAGE DU_FLAG_STAGE

static DevUpdateConfig s_cfg;
static bool s_running;
static Thread s_server, s_recovery_thread;
static LightEvent s_recovery_ev;
static volatile int s_quit, s_recovery, s_paused, s_sleeping, s_reopen;
static int s_listen = -1;
static u32* s_soc_buf;
static bool s_own_soc;
static bool s_am_ok;
static Result s_am_rc;
static u64 s_title_id;
static aptHookCookie s_cookie;
static const char* s_report;
static char s_incoming[DU_VERSION_MAX + 1]; /* the PC tool's version text of the build being pushed ("" = none) */
static char s_from[48];     /* the build that relaunched us (deliver argument ctrdk_from=) */

static u8 s_buf[CHUNK] __attribute__((aligned(64)));
static u32 s_crc_table[256];

/* The next launch: target and deliver arguments ("ctrdk:" + switch text). */
static char s_next_args[DELIVER_BYTES];
static u64 s_jump_tid;
static bool s_jump_self = true;
static volatile bool s_pending, s_relaunch_req;
static volatile u64 s_relaunch_deadline; /* queue relaunches: the process must have ended by then */

static void log_msg(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_msg(int level, const char* fmt, ...) {
    if (s_cfg.log == NULL) {
        return;
    }
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_cfg.log(level, buf, s_cfg.user);
}

/* ---- CRC32 (zlib's polynomial) ---- */

static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        s_crc_table[i] = c;
    }
}

static u32 crc_update(u32 crc, const u8* p, u32 n) {
    for (u32 i = 0; i < n; i++) {
        crc = s_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

/* ---- Sockets ---- */

static void send_all(int fd, const char* s, size_t n) {
    while (n > 0) {
        int r = send(fd, s, n, 0);
        if (r <= 0) {
            return;
        }
        s += r;
        n -= (size_t)r;
    }
}

static void send_line(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_line(int fd, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    send_all(fd, buf, (size_t)n);
}

/* Up to n bytes: >0 got, 0 closed, -1 timeout or error, -2 the console
 * went to sleep. */
static int recv_some(int fd, void* buf, size_t n, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 250) {
        if (s_sleeping) {
            return -2;
        }
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, 250);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            continue;
        }
        int got = recv(fd, buf, n, 0);
        return got < 0 ? -1 : got;
    }
    return -1;
}

static int recv_all(int fd, void* buf, size_t n, int timeout_ms) {
    u8* p = buf;
    while (n > 0) {
        int r = recv_some(fd, p, n, timeout_ms);
        if (r <= 0) {
            return r == 0 ? -1 : r;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static bool open_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s_cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 2) < 0) {
        log_msg(0, "devupdate: bind/listen on port %u failed, errno %d", (unsigned)s_cfg.port, errno);
        close(fd);
        return false;
    }
    s_listen = fd;
    return true;
}

static void close_listener(void) {
    int fd = s_listen;
    s_listen = -1;
    if (fd >= 0) {
        close(fd);
    }
}

/* ---- The next launch ---- */

static void set_next(bool self, u64 tid, const char* args) {
    memset(s_next_args, 0, sizeof(s_next_args));
    memcpy(s_next_args, ARG_PREFIX, ARG_PREFIX_LEN);
    strncpy(s_next_args + ARG_PREFIX_LEN, args, sizeof(s_next_args) - ARG_PREFIX_LEN - 1);
    s_jump_self = self;
    s_jump_tid = tid;
}

static void emu_write_args(void) {
    if (access(EMU_MARKER, F_OK) != 0) {
        return;
    }
    FILE* f = fopen(EMU_ARGS, "w");
    if (f != NULL) {
        fwrite(s_next_args, 1, strnlen(s_next_args, sizeof(s_next_args)), f);
        fclose(f);
    }
}

void devupdate_prepare_relaunch(void) {
    emu_write_args();
    if (s_jump_self) {
        aptSetChainloaderToSelf();
    } else {
        aptSetChainloader(s_jump_tid, MEDIATYPE_SD);
    }
    aptSetChainloaderArgs(s_next_args, sizeof(s_next_args), NULL);
}

void devupdate_relaunch(void) {
    s_relaunch_req = false;
    if (envIsHomebrew() && !s_jump_self) {
        /* Under the Homebrew Launcher: jump as FBI does; the system then
         * closes this process (aptMainLoop returns false). */
        u8 hmac[0x20];
        memset(hmac, 0, sizeof(hmac));
        Result r = APT_PrepareToDoApplicationJump(0, s_jump_tid, MEDIATYPE_SD);
        if (R_SUCCEEDED(r)) {
            r = APT_DoApplicationJump(s_next_args, sizeof(s_next_args), hmac);
        }
        log_msg(R_SUCCEEDED(r) ? 2 : 0, "devupdate: application jump to %016llx: %08lx", (unsigned long long)s_jump_tid,
                (unsigned long)r);
        return;
    }
    devupdate_prepare_relaunch();
    devupdate_stop();
    exit(0);
}

bool devupdate_pending(void) { return s_pending; }
bool devupdate_relaunch_requested(void) { return s_relaunch_req; }
bool devupdate_running(void) { return s_running; }
bool devupdate_in_recovery(void) { return s_recovery != 0; }

static bool s_launched;

const char* devupdate_incoming_version(void) { return s_incoming; }

const char* devupdate_launch_from(void) {
    const char* a = devupdate_launch_args();
    const char* p = strstr(a, "ctrdk_from=");
    s_from[0] = '\0';
    if (p != NULL) {
        size_t i = 0;
        for (p += 11; *p != '\0' && *p != ' ' && i + 1 < sizeof(s_from); p++) {
            s_from[i++] = *p == '_' ? ' ' : *p;
        }
        s_from[i] = '\0';
    }
    return s_from;
}

bool devupdate_launched(void) {
    devupdate_launch_args();
    return s_launched;
}

const char* devupdate_launch_args(void) {
    static char args[DELIVER_BYTES + 1];
    static bool read;
    if (read) {
        return args;
    }
    read = true;
    static u8 buf[DELIVER_BYTES];
    u8 hmac[0x20];
    u64 sender = 0;
    bool received = false;
    memset(buf, 0, sizeof(buf));
    Result r = APT_ReceiveDeliverArg(buf, sizeof(buf), hmac, &sender, &received);
    if (R_SUCCEEDED(r) && received && memcmp(buf, ARG_PREFIX, ARG_PREFIX_LEN) == 0) {
        size_t n = strnlen((const char*)buf + ARG_PREFIX_LEN, sizeof(buf) - ARG_PREFIX_LEN);
        memcpy(args, buf + ARG_PREFIX_LEN, n);
        args[n] = '\0';
        s_launched = true;
    } else {
        FILE* f = fopen(EMU_ARGS, "r"); /* the Azahar harness's stand-in for deliver arguments */
        if (f != NULL) {
            size_t got = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            remove(EMU_ARGS);
            buf[got] = '\0';
            if (got >= ARG_PREFIX_LEN && memcmp(buf, ARG_PREFIX, ARG_PREFIX_LEN) == 0) {
                memcpy(args, buf + ARG_PREFIX_LEN, got - ARG_PREFIX_LEN + 1);
                s_launched = true;
            }
        }
    }
    return args;
}

/* Recovery ends the process. The chainloader is set when relaunching. */
static void recovery_exit(bool relaunch) __attribute__((noreturn));
static void recovery_exit(bool relaunch) {
    if (relaunch) {
        devupdate_prepare_relaunch();
    }
    log_msg(1, "devupdate: recovery ends the process (%s)", relaunch ? "relaunch" : "exit");
    if (s_cfg.on_recovery_exit != NULL) {
        s_cfg.on_recovery_exit(relaunch, s_cfg.user);
        for (;;) {
            svcSleepThread(1000000000LL); /* the app's exit path ends the process */
        }
    }
    if (relaunch) {
        aptExit();
    }
    svcExitProcess();
    for (;;) {
    }
}

/* ---- Requests ---- */

static void status(DevUpdatePhase phase, u32 done, u32 total, const char* text) {
    if (s_cfg.on_status != NULL) {
        s_cfg.on_status(phase, done, total, text, s_cfg.user);
    }
}

static volatile bool s_busy; /* between begin and resume: the update holds the app */

bool devupdate_busy(void) { return s_busy; }

static void begin(bool recovery) {
    s_busy = true;
    if (!recovery && s_cfg.on_begin != NULL) {
        s_cfg.on_begin(s_cfg.user);
    }
}

static void resume(bool recovery) {
    if (!recovery && s_cfg.on_resume != NULL) {
        s_cfg.on_resume(s_cfg.user);
    }
    s_busy = false;
}

/* After a push, a restart or a launch: hands over to the next launch. */
static void hand_over(int fd, bool self, u64 tid, const char* args, bool recovery) {
    char with_from[DEVUPDATE_MAX_ARGS + 64];
    if (self && s_cfg.version != NULL && strlen(args) + strlen(s_cfg.version) + 16 < sizeof(with_from)) {
        /* the running build's stamp travels to the next launch, spaces as '_' */
        char* o = with_from + snprintf(with_from, sizeof(with_from), "%s%sctrdk_from=", args, args[0] ? " " : "");
        for (const char* c = s_cfg.version; *c != '\0'; c++) {
            *o++ = *c == ' ' ? '_' : *c;
        }
        *o = '\0';
        args = with_from;
    }
    set_next(self, tid, args);
    if (recovery) {
        if (fd >= 0) {
            send_line(fd, self ? "relaunching" : "launching %016llx", (unsigned long long)tid);
            send_line(fd, "done");
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        recovery_exit(true);
    }
    bool now = s_cfg.on_ready != NULL ? s_cfg.on_ready(s_cfg.user) : true;
    if (!now) {
        if (fd >= 0) {
            send_line(fd, "relaunch deferred");
            send_line(fd, "done");
        }
        return;
    }
    if (fd >= 0) {
        if (self) {
            send_line(fd, "relaunching");
        } else {
            send_line(fd, "launching %016llx", (unsigned long long)tid);
        }
        send_line(fd, "done");
        shutdown(fd, SHUT_RDWR);
    }
    if (fd < 0 || s_cfg.on_relaunch != NULL) {
        /* No state of the app may prevent an update: if the app's exit path does not end the process
         * in time (its game thread is stuck in a load, a menu loop that does not poll), the server
         * thread ends it. */
        s_relaunch_deadline = svcGetSystemTick() + (u64)RELAUNCH_GRACE_S * SYSCLOCK_ARM11;
    }
    log_msg(2, "devupdate: relaunch requested (%s, args \"%s\")", self ? "this title" : "another title",
            s_next_args + ARG_PREFIX_LEN);
    status(DEVUPDATE_PH_RELAUNCHING, 0, 0, s_next_args + ARG_PREFIX_LEN);
    s_relaunch_req = true;
    if (s_cfg.on_relaunch != NULL) {
        if (!(envIsHomebrew() && !s_jump_self)) {
            devupdate_prepare_relaunch(); /* the app's exit() then runs aptExit, which chainloads */
        }
        s_cfg.on_relaunch(s_cfg.user);
    }
}

/* Where a chunk of the CIA goes: AM's install handle, or the staging file on the card. */
typedef struct {
    Handle cia;       /* AM */
    const char* part; /* staging: the .part file (through the app's store) */
    Result rc;        /* the last AM or store result */
} Sink;

static bool sink_am(Sink* k, u32 off, const u8* p, u32 n) {
    u32 written = 0;
    k->rc = FSFILE_Write(k->cia, &written, off, p, n, 0);
    if (R_SUCCEEDED(k->rc) && written != n) {
        k->rc = 0xE0000000u | written; /* a short write: report the length that went in */
    }
    return R_SUCCEEDED(k->rc);
}

static bool sink_store(Sink* k, u32 off, const u8* p, u32 n) {
    k->rc = (Result)du_store_write(k->part, off, p, n);
    return k->rc == 0;
}

/* Exactly n bytes from the socket into buf: 0, or the failure of recv_some (0 = closed -> -4). */
static int recv_fill(int fd, u8* buf, u32 n) {
    u32 got = 0;
    while (got < n) {
        int r = recv_some(fd, buf + got, n - got, 15000);
        if (r <= 0) {
            return r == 0 ? -4 : r;
        }
        got += (u32)r;
    }
    return 0;
}

enum { PUMP_OK = 0, PUMP_SLEEP = -2, PUMP_CLOSED = -4, PUMP_TIMEOUT = -1, PUMP_SINK = -5 };

/* Receives the payload into the sink. Every write is a full CHUNK (64 KiB) except the last: AM sees a
 * fixed, aligned write pattern whatever the network's segments do. Returns a PUMP_ code; *done is the
 * offset reached, *crc the running CRC. */
static int pump_socket(int fd, u32 size, Sink* k, bool (*sink)(Sink*, u32, const u8*, u32), u32* done, u32* crc) {
    u32 next_report = 1u << 20;
    u64 last_status = 0;
    *done = 0;
    *crc = 0xFFFFFFFFu;
    status(DEVUPDATE_PH_RECEIVING, 0, size, NULL);
    while (*done < size) {
        u32 want = size - *done < CHUNK ? size - *done : CHUNK;
        int r = recv_fill(fd, s_buf, want);
        if (r != 0) {
            return r;
        }
        *crc = crc_update(*crc, s_buf, want);
        if (!sink(k, *done, s_buf, want)) {
            return PUMP_SINK;
        }
        *done += want;
        if (*done >= next_report || *done == size) {
            send_line(fd, "progress %lu %lu", (unsigned long)*done, (unsigned long)size);
            next_report = *done + (1u << 20);
        }
        if (s_cfg.on_progress != NULL) {
            s_cfg.on_progress(*done, size, s_cfg.user);
        }
        u64 now = svcGetSystemTick();
        if (*done == size || now - last_status >= SYSCLOCK_ARM11 / 10) {
            last_status = now;
            status(DEVUPDATE_PH_RECEIVING, *done, size, NULL);
        }
    }
    return PUMP_OK;
}

/* Staged CIA: reads the file in chunks, into the sink (AM) or just for the CRC (sink NULL). */
static int pump_file(FILE* f, u32 size, Sink* k, u32* done, u32* crc, DevUpdatePhase phase) {
    u64 last_status = 0;
    *done = 0;
    *crc = 0xFFFFFFFFu;
    while (*done < size) {
        u32 want = size - *done < CHUNK ? size - *done : CHUNK;
        if (fread(s_buf, 1, want, f) != want) {
            return PUMP_CLOSED;
        }
        *crc = crc_update(*crc, s_buf, want);
        if (k != NULL && !sink_am(k, *done, s_buf, want)) {
            return PUMP_SINK;
        }
        *done += want;
        u64 now = svcGetSystemTick();
        if (*done == size || now - last_status >= SYSCLOCK_ARM11 / 10) {
            last_status = now;
            status(phase, *done, size, NULL);
        }
    }
    return PUMP_OK;
}

/* One failed push: the status screen, the reply line (error code, what failed, the AM or store result
 * and the stage), and the log. The install is already cancelled and the old install is untouched. */
static void push_fail(int fd, int code, const char* stage, u32 at, Result rc, const char* what) {
    status(DEVUPDATE_PH_ERROR, at, 0, what);
    send_line(fd, "error %d %s: stage=%s at=%lu rc=%08lx (install cancelled, old install kept)", code, what, stage,
              (unsigned long)at, (unsigned long)rc);
    log_msg(0, "devupdate: push failed: %s, stage %s, at %lu, rc %08lx", what, stage, (unsigned long)at,
            (unsigned long)rc);
}

static void crc_fail(int fd, const char* stage, u32 done, u32 got, u32 want) {
    char what[64];
    snprintf(what, sizeof(what), "crc mismatch, received %08lx header %08lx", (unsigned long)got, (unsigned long)want);
    push_fail(fd, 5, stage, done, 0, what);
}

/* The error for a PUMP_ code from the receiving stage. */
static void pump_fail(int fd, int pr, const char* stage, u32 done, Result rc) {
    switch (pr) {
    case PUMP_SLEEP: push_fail(fd, 16, stage, done, rc, "sleep"); break;
    case PUMP_CLOSED: push_fail(fd, 14, stage, done, rc, "connection closed"); break;
    case PUMP_TIMEOUT: push_fail(fd, 15, stage, done, rc, "receive timeout"); break;
    default: push_fail(fd, 17, stage, done, rc, "write failed"); break;
    }
}

static void stage_path(char* path, size_t n, const char* suffix) {
    char rel[24];
    snprintf(rel, sizeof(rel), "stage.cia%s", suffix);
    du_files_path(path, n, rel);
}

/* Stage: the CIA goes to files_dir/stage.cia on the card, is read back and CRC-checked, and only then
 * is AM fed from the file. A slow or broken network can then not break AM's stream. Returns the open
 * file (or NULL after an error reply). */
static FILE* stage_receive(int fd, const Header* h, char* path, size_t path_n) {
    char part[200];
    stage_path(path, path_n, "");
    stage_path(part, sizeof(part), ".part");
    du_files_mkdirs("stage.cia");
    Sink k = {0, part, 0};
    send_line(fd, "staging %lu", (unsigned long)h->size);
    if (!sink_store(&k, 0, s_buf, 0)) { /* creates or empties it */
        push_fail(fd, 17, "stage-create", 0, k.rc, "cannot create the staging file");
        return NULL;
    }
    u32 done, crc;
    int pr = pump_socket(fd, h->size, &k, sink_store, &done, &crc);
    if (pr != PUMP_OK) {
        du_store_remove(part);
        pump_fail(fd, pr, "stage-receive", done, k.rc);
        return NULL;
    }
    crc = ~crc;
    if (crc != h->crc) {
        du_store_remove(part);
        crc_fail(fd, "stage-receive", done, crc, h->crc);
        return NULL;
    }
    du_store_flush();
    if (du_store_rename(part, path) != 0) {
        du_store_remove(part);
        push_fail(fd, 17, "stage-rename", done, 0, "cannot rename the staging file");
        return NULL;
    }
    du_store_flush();
    status(DEVUPDATE_PH_CHECKING, 0, h->size, NULL);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        push_fail(fd, 17, "stage-open", 0, (Result)errno, "cannot read the staging file back");
        return NULL;
    }
    pr = pump_file(f, h->size, NULL, &done, &crc, DEVUPDATE_PH_CHECKING);
    crc = ~crc;
    if (pr != PUMP_OK || crc != h->crc) {
        fclose(f);
        du_store_remove(path);
        du_store_flush();
        crc_fail(fd, "stage-verify", done, crc, h->crc);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    send_line(fd, "staged %lu crc %08lx", (unsigned long)h->size, (unsigned long)crc);
    log_msg(2, "devupdate: staged %lu bytes on the card, crc ok", (unsigned long)h->size);
    return f;
}

static void do_push(int fd, const Header* h, const char* args, bool recovery) {
    if (envIsHomebrew() && !s_cfg.allow_3dsx_install) {
        send_line(fd, "error 10 this app runs as a 3dsx: CIA installs are off (use 3dslink, or the bootstrap)");
        return;
    }
    if (!s_am_ok) {
        send_line(fd, "error 11 am:net is not available (amInit %08lx)", (unsigned long)s_am_rc);
        return;
    }
    if (h->size == 0) {
        send_line(fd, "error 12 empty payload");
        return;
    }
    if ((h->flags & FLAG_LAUNCH) && h->title_id == 0) {
        send_line(fd, "error 3 launch after push needs the title ID");
        return;
    }
    bool stage = (h->flags & FLAG_STAGE) != 0;
    if (stage && !du_files_enabled()) {
        send_line(fd, "error 19 this app has no files folder: --stage needs one");
        return;
    }
    send_line(fd, "ok receiving %lu", (unsigned long)h->size);
    log_msg(2, "devupdate: receiving %lu bytes for %016llx%s", (unsigned long)h->size, (unsigned long long)h->title_id,
            stage ? " (staged on the card)" : "");
    begin(recovery);
    FILE* sf = NULL;
    char spath[200] = "";
    if (stage) {
        sf = stage_receive(fd, h, spath, sizeof(spath));
        if (sf == NULL) {
            resume(recovery);
            return;
        }
    }
    Handle cia = 0;
    Sink k = {0, NULL, 0};
    u32 done = 0, crc = 0;
    int pr;
    Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    if (R_FAILED(rc)) {
        push_fail(fd, 13, "am-start", 0, rc, "AM_StartCiaInstall failed");
        goto fail_cleanup;
    }
    k.cia = cia;
    if (stage) {
        status(DEVUPDATE_PH_INSTALLING, 0, h->size, NULL);
        pr = pump_file(sf, h->size, &k, &done, &crc, DEVUPDATE_PH_INSTALLING);
        fclose(sf);
        sf = NULL;
    } else {
        pr = pump_socket(fd, h->size, &k, sink_am, &done, &crc);
    }
    if (pr != PUMP_OK) {
        AM_CancelCIAInstall(cia);
        pump_fail(fd, pr, stage ? "am-write-staged" : "am-write", done, k.rc);
        log_msg(0, "devupdate: transfer failed (%d) after %lu bytes, install cancelled", pr, (unsigned long)done);
        goto fail_cleanup;
    }
    crc = ~crc;
    status(DEVUPDATE_PH_CHECKING, done, h->size, NULL);
    if (crc != h->crc) {
        AM_CancelCIAInstall(cia);
        crc_fail(fd, "crc", done, crc, h->crc);
        goto fail_cleanup;
    }
    send_line(fd, "installing");
    status(DEVUPDATE_PH_INSTALLING, done, h->size, NULL);
    rc = AM_FinishCiaInstall(cia); /* moves the handle to AM: nothing to cancel after this, a failed commit leaves the old install */
    if (R_FAILED(rc)) {
        push_fail(fd, 18, "am-commit", done, rc, "AM_FinishCiaInstall failed");
        goto fail_cleanup;
    }
    if (stage) {
        du_store_remove(spath);
        du_store_flush();
    }
    send_line(fd, "installed %016llx", (unsigned long long)h->title_id);
    log_msg(2, "devupdate: installed %016llx", (unsigned long long)h->title_id);
    if (h->flags & FLAG_LAUNCH) {
        hand_over(fd, h->title_id == s_title_id, h->title_id, args, recovery);
    } else if (h->flags & FLAG_RELAUNCH) {
        s_pending = true;
        hand_over(fd, true, 0, args, recovery);
    } else {
        if (h->title_id == s_title_id) {
            s_pending = true;
        }
        resume(recovery);
        send_line(fd, "done");
    }
    return;

fail_cleanup:
    if (sf != NULL) {
        fclose(sf);
    }
    if (stage && spath[0] != '\0') {
        du_store_remove(spath);
        du_store_flush();
    }
    resume(recovery);
}

static void send_file_tail(int fd, const char* path) {
    int lf = path != NULL ? open(path, O_RDONLY) : -1;
    if (lf < 0) {
        send_line(fd, "log 0");
        return;
    }
    off_t end = lseek(lf, 0, SEEK_END);
    off_t from = end > LOG_TAIL ? end - LOG_TAIL : 0;
    lseek(lf, from, SEEK_SET);
    int n = read(lf, s_buf, LOG_TAIL);
    close(lf);
    if (n < 0) {
        n = 0;
    }
    send_line(fd, "log %d", n);
    send_all(fd, (const char*)s_buf, (size_t)n);
    send_line(fd, "%s", "");
}

static void do_ping(int fd) {
    FS_ArchiveResource res;
    u64 free_bytes = 0;
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD))) {
        free_bytes = (u64)res.freeClusters * res.clusterSize;
    }
    send_line(fd, "pong title=%016llx version=%s kind=%s free=%llu recovery=%d pending=%d am=%s",
              (unsigned long long)s_title_id, s_cfg.version != NULL ? s_cfg.version : "?",
              envIsHomebrew() ? "3dsx" : "cia", (unsigned long long)free_bytes, s_recovery ? 1 : 0, s_pending ? 1 : 0,
              s_am_ok ? "ok" : "off");
    send_line(fd, "args %s", devupdate_launch_args());
    if (du_files_enabled()) {
        send_line(fd, "files %s", s_cfg.files_dir);
    }
    send_line(fd, "done");
}

static bool token_ok(const char* got) {
    char want[32];
    memset(want, 0, sizeof(want));
    if (s_cfg.token != NULL) {
        strncpy(want, s_cfg.token, sizeof(want) - 1);
    }
    u8 diff = 0;
    for (size_t i = 0; i < sizeof(want); i++) {
        diff |= (u8)(want[i] ^ got[i]);
    }
    return s_cfg.token == NULL || diff == 0;
}

static void handle_client(int fd, bool recovery) {
    Header h;
    if (recv_all(fd, &h, sizeof(h), 10000) != 0) {
        return;
    }
    int bad = du_proto_check_header(&h, DEVUPDATE_MAX_ARGS);
    if (bad == 1) {
        send_line(fd, "error 1 bad header (protocol version %d expected)", DU_PROTO_VERSION);
        return;
    }
    if (!token_ok(h.token)) {
        send_line(fd, "error 2 bad token");
        log_msg(1, "devupdate: request with a bad token refused");
        return;
    }
    if (bad == 3) {
        send_line(fd, "error 3 arguments longer than %d bytes", DEVUPDATE_MAX_ARGS);
        return;
    }
    char args[DEVUPDATE_MAX_ARGS + 1];
    if (h.args_len && recv_all(fd, args, h.args_len, 10000) != 0) {
        return;
    }
    if (du_proto_split_args(args, h.args_len, h.flags, s_incoming, sizeof(s_incoming)) < 0) {
        log_msg(1, "devupdate: the version text is longer than the argument area (%lu bytes); ignored",
                (unsigned long)h.args_len);
    }
    switch (h.cmd) {
    case CMD_PING: do_ping(fd); break;
    case CMD_REPORT:
        if (s_report != NULL && s_report[0] != '\0') {
            size_t n = strlen(s_report);
            send_line(fd, "report %u", (unsigned)n);
            send_all(fd, s_report, n);
            send_line(fd, "%s", "");
        } else {
            /* no in-memory report (no crash in this run, or it never reached recovery): the report file's tail */
            int rf = s_cfg.report_path != NULL ? open(s_cfg.report_path, O_RDONLY) : -1;
            int n = 0;
            if (rf >= 0) {
                off_t end = lseek(rf, 0, SEEK_END);
                lseek(rf, end > LOG_TAIL ? end - LOG_TAIL : 0, SEEK_SET);
                n = read(rf, s_buf, LOG_TAIL);
                close(rf);
                if (n < 0) {
                    n = 0;
                }
            }
            send_line(fd, "report %d", n);
            send_all(fd, (const char*)s_buf, (size_t)n);
            send_line(fd, "%s", "");
        }
        send_file_tail(fd, s_cfg.log_path);
        send_line(fd, "done");
        break;
    case CMD_RESTART:
        send_line(fd, "ok restarting");
        begin(recovery);
        hand_over(fd, true, 0, args, recovery);
        break;
    case CMD_LAUNCH:
        if (h.title_id == 0) {
            send_line(fd, "error 3 launch needs a title ID");
            break;
        }
        send_line(fd, "ok launching");
        begin(recovery);
        hand_over(fd, h.title_id == s_title_id, h.title_id, args, recovery);
        break;
    case CMD_PUSH: do_push(fd, &h, args, recovery); break;
    case CMD_FILES: du_cmd_files(fd, args, h.size, h.crc); break;
    case CMD_QUEUE: du_cmd_queue(fd, args, h.size, h.crc, recovery); break;
    default: send_line(fd, "error 4 unknown command %u", (unsigned)h.cmd); break;
    }
}

/* ---- Threads ---- */

static void serve(bool recovery) {
    for (;;) {
        if (s_quit || (!recovery && s_recovery)) {
            return;
        }
        if (!recovery) {
            du_queue_tick();
            if (s_relaunch_deadline != 0 && svcGetSystemTick() > s_relaunch_deadline) {
                s_relaunch_deadline = 0;
                log_msg(0, "devupdate: the relaunch did not end the process in %d s; ending it", RELAUNCH_GRACE_S);
                recovery_exit(true);
            }
        }
        if (s_reopen && !recovery) {
            s_reopen = 0;
            close_listener();
        }
        if (s_listen < 0 && !open_listener()) {
            svcSleepThread(1000000000LL);
            continue;
        }
        if (s_paused && !recovery) {
            svcSleepThread(100000000LL); /* HOME Menu or sleep: no new requests */
            continue;
        }
        struct pollfd p = {s_listen, POLLIN, 0};
        int r = poll(&p, 1, 500);
        if (r < 0) {
            close_listener();
            svcSleepThread(1000000000LL);
            continue;
        }
        if (r == 0 || !(p.revents & POLLIN)) {
            continue;
        }
        int fd = accept(s_listen, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        handle_client(fd, recovery);
        close(fd);
    }
}

static volatile int s_server_done;

static void server_main(void* arg) {
    (void)arg;
    serve(false);
    s_server_done = 1;
}

static void recovery_main(void* arg) {
    (void)arg;
    LightEvent_Wait(&s_recovery_ev);
    if (s_quit) {
        return;
    }
    log_msg(1, "devupdate: recovery mode");
    du_files_use_direct(true); /* the app's SD layer may be what crashed */
    du_queue_recovery(s_report); /* a queue run records the crash and relaunches; no return then */
    /* The server thread sees s_recovery within 0.5 s and returns; its
     * listener is then reused (closing and binding again can find the port
     * still taken). A server thread that does not return (it crashed, or it
     * is stuck in a request) loses its listener. */
    for (int i = 0; i < 20 && !s_server_done; i++) {
        svcSleepThread(100000000LL);
    }
    if (!s_server_done) {
        close_listener();
    }
    int limit = s_cfg.recovery_net_seconds > 0 ? s_cfg.recovery_net_seconds : 30;
    u64 start = svcGetSystemTick();
    for (;;) {
        if (FindDevice("soc:") < 0 && s_soc_buf != NULL) {
            close_listener();
            socInit(s_soc_buf, s_cfg.soc_buffer_size); /* sockets were closed: the reserved buffer */
        }
        long ip = gethostid();
        if (ip != 0 && ip != -1 && (s_listen >= 0 || open_listener())) {
            log_msg(1, "devupdate: recovery: waiting for the PC on %s:%u", inet_ntoa((struct in_addr){(u32)ip}),
                    (unsigned)s_cfg.port);
            break;
        }
        if (svcGetSystemTick() - start > (u64)limit * SYSCLOCK_ARM11) {
            log_msg(0, "devupdate: recovery: no network after %d s", limit);
            recovery_exit(false);
        }
        svcSleepThread(1000000000LL);
    }
    serve(true);
}

bool devupdate_enter_recovery(const char* report) {
    if (!s_running || s_recovery_thread == NULL) {
        return false;
    }
    s_report = report;
    s_recovery = 1;
    LightEvent_Signal(&s_recovery_ev);
    return true;
}

static void apt_hook(APT_HookType type, void* param) {
    (void)param;
    switch (type) {
    case APTHOOK_ONSUSPEND: s_paused = 1; break;
    case APTHOOK_ONRESTORE: s_paused = 0; break;
    case APTHOOK_ONSLEEP:
        s_sleeping = 1;
        s_paused = 1;
        break;
    case APTHOOK_ONWAKEUP:
        s_sleeping = 0;
        s_paused = 0;
        s_reopen = 1; /* the network came back: a fresh listener */
        break;
    default: break;
    }
}

static Thread make_thread(ThreadFunc fn) {
    int core = s_cfg.core != 0 ? s_cfg.core : 1;
    Thread t = threadCreate(fn, NULL, THREAD_STACK, THREAD_PRIO, core, false);
    if (t == NULL && core != 0) {
        t = threadCreate(fn, NULL, THREAD_STACK, THREAD_PRIO, 0, false);
    }
    return t;
}

int32_t devupdate_start(const DevUpdateConfig* cfg) {
    if (s_running) {
        return 0;
    }
    s_cfg = *cfg;
    if (s_cfg.port == 0) {
        s_cfg.port = DEVUPDATE_DEFAULT_PORT;
    }
    if (s_cfg.soc_buffer_size == 0) {
        s_cfg.soc_buffer_size = 0x80000;
    }
    s_quit = s_recovery = s_paused = s_sleeping = s_reopen = s_server_done = 0;
    crc_init();
    devupdate_launch_args(); /* read once, before anything can relaunch */
    if (R_FAILED(APT_GetProgramID(&s_title_id))) {
        s_title_id = 0;
    }
    s_am_rc = amInit();
    s_am_ok = R_SUCCEEDED(s_am_rc);
    du_queue_init(devupdate_launch_args());

    s_soc_buf = memalign(0x1000, s_cfg.soc_buffer_size);
    if (FindDevice("soc:") < 0) {
        Result rc = s_soc_buf != NULL ? socInit(s_soc_buf, s_cfg.soc_buffer_size) : -1;
        if (R_FAILED(rc)) {
            log_msg(0, "devupdate: socInit %08lx", (unsigned long)rc);
            free(s_soc_buf);
            s_soc_buf = NULL;
            return rc;
        }
        s_own_soc = true;
    }
    LightEvent_Init(&s_recovery_ev, RESET_ONESHOT);
    s_server = make_thread(server_main);
    s_recovery_thread = make_thread(recovery_main);
    if (s_server == NULL) {
        log_msg(0, "devupdate: no server thread");
        return -1;
    }
    aptHook(&s_cookie, apt_hook, NULL);
    s_running = true;
    struct in_addr ip = {(u32)gethostid()};
    log_msg(2, "devupdate: listening on %s:%u, title %016llx (%s), am:net %s, recovery %s", inet_ntoa(ip),
            (unsigned)s_cfg.port, (unsigned long long)s_title_id, envIsHomebrew() ? "3dsx" : "cia",
            s_am_ok ? "ok" : "off", s_recovery_thread != NULL ? "ready" : "OFF");
    return 0;
}

void devupdate_stop(void) {
    if (!s_running) {
        return;
    }
    s_running = false;
    s_quit = 1;
    if (s_server != NULL && threadGetCurrent() != s_server) {
        threadJoin(s_server, 2000000000ULL);
    }
    LightEvent_Signal(&s_recovery_ev);
    if (s_recovery_thread != NULL && threadGetCurrent() != s_recovery_thread) {
        threadJoin(s_recovery_thread, 1000000000ULL);
    }
    close_listener();
    aptUnhook(&s_cookie);
    if (s_own_soc) {
        socExit();
        s_own_soc = false;
    }
    if (s_am_ok) {
        amExit();
        s_am_ok = false;
    }
}

/* ---- Shared with files.c and queue.c ---- */

const DevUpdateConfig* du_cfg(void) { return &s_cfg; }

void du_log(int level, const char* fmt, ...) {
    if (s_cfg.log == NULL) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_cfg.log(level, buf, s_cfg.user);
}

void du_send_line(int fd, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    send_all(fd, buf, (size_t)n);
}

void du_send_all(int fd, const char* s, size_t n) { send_all(fd, s, n); }
int du_recv_some(int fd, void* buf, size_t n, int timeout_ms) { return recv_some(fd, buf, n, timeout_ms); }
uint8_t* du_buf(void) { return s_buf; }
uint32_t du_crc_update(uint32_t crc, const uint8_t* p, uint32_t n) { return crc_update(crc, p, n); }
void devupdate_set_next_args(const char* args) { set_next(true, 0, args); }
void du_set_next(bool self, uint64_t tid, const char* args) { set_next(self, tid, args); }
void du_begin(bool recovery) { begin(recovery); }
void du_hand_over(int fd, bool self, uint64_t tid, const char* args, bool recovery) {
    hand_over(fd, self, tid, args, recovery);
}
void du_recovery_exit(bool relaunch) { recovery_exit(relaunch); }
bool du_relaunch_requested(void) { return s_relaunch_req; }
