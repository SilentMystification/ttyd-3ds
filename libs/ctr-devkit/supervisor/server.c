/* ctr-devkit supervisor: the TCP server (devupdate's CDU1 header and reply lines) and the watchdog.
 *
 * One client at a time, every receive bounded by a timeout, so a stalled PC never holds it.
 * Nothing here waits for the watched app: PM, NS and AM calls return within their own limits
 * (actions.c), and the watchdog runs between requests on this thread.
 *
 * Commands (docs/CTR_DEVKIT_PLAN.md "Supervisor"):
 *   1      ping                    one "pong supervisor ..." line
 *   0x101  status                  "status key=value" lines
 *   0x102  terminate [gpureset=N]  PM ends the game (then by force), then the GPU reset attempt
 *   0x103  launch <switches>       (terminate and GPU reset first if it runs) args file, then NS launch
 *   2      push (CIA payload)      terminate, AM install (64 KiB writes, CRC before the commit),
 *                                  GPU reset attempt, launch with the switches; the supervisor's own
 *                                  title: install to NAND, launch the game if it is not running, exit
 *   6      files get|ls <path>     read-only: the app's folder (dir=), "@sv/..." this module's folder
 *   0x104  watch N                 arm (N s) or disarm (0) the watchdog
 *   0x105  reboot [hard|dry]       NS_RebootToTitle(game), else APT_HardwareResetAsync
 *   0x106  exit                    the supervisor ends (its update, uninstall)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>

#include "../devupdate/proto.h"
#include "ctrdk/supervisor.h"
#include "policy.h"
#include "sv_internal.h"

#define MAX_ARGS 0x2F0
#define CHUNK 0x10000
#define RECV_TIMEOUT_MS 15000
#define LOG_MAX (256u << 10) /* supervisor.log is moved to supervisor.old.log past this */
#define SV_DIR "sdmc:/3ds/ctrdk"

volatile bool sv_exit_requested;

/* ---- Log ---- */

void (*sv_log_hook)(const char* line);
static bool s_log_file;
static LightLock s_log_lock = 1; /* LightLock_Init state */
static u64 s_start_tick;

void sv_log_enable_file(bool on) { s_log_file = on; }

static unsigned uptime_s(void) { return (unsigned)((svcGetSystemTick() - s_start_tick) / SYSCLOCK_ARM11); }

void sv_log(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svcOutputDebugString(buf, (s32)strlen(buf));
    if (sv_log_hook != NULL) {
        sv_log_hook(buf);
    }
    if (!s_log_file) {
        return;
    }
    LightLock_Lock(&s_log_lock);
    struct stat st;
    if (stat(CTRDK_SV_LOG, &st) == 0 && (u32)st.st_size > LOG_MAX) {
        remove(SV_DIR "/supervisor.old.log");
        rename(CTRDK_SV_LOG, SV_DIR "/supervisor.old.log");
    }
    FILE* f = fopen(CTRDK_SV_LOG, "a");
    if (f == NULL) {
        mkdir("sdmc:/3ds", 0777);
        mkdir(SV_DIR, 0777);
        f = fopen(CTRDK_SV_LOG, "a");
    }
    if (f != NULL) {
        fprintf(f, "[%6u] %s\n", uptime_s(), buf);
        fclose(f);
    }
    LightLock_Unlock(&s_log_lock);
}

/* ---- Configuration ---- */

void sv_config_defaults(SvConfig* cfg) {
    const char* kind = cfg->kind;
    const char* version = cfg->version;
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind = kind;
    cfg->version = version;
    cfg->port = CTRDK_SV_PORT;
    cfg->max_relaunches = 2;
    cfg->terminate_ms = 3000;
    cfg->gpureset = 2;
    cfg->launch_flags = 1; /* PMLAUNCHFLAG_NORMAL_APPLICATION */
    cfg->log_file = true;
}

void sv_config_load(SvConfig* cfg, bool with_port) {
    FILE* f = fopen(CTRDK_SV_CONFIG, "r");
    if (f == NULL) {
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char* v = strchr(line, '=');
        if (v == NULL) {
            continue;
        }
        *v++ = '\0';
        long x = strtol(v, NULL, 0);
        if (strcmp(line, "port") == 0) {
            if (with_port && x > 0 && x < 65536) {
                cfg->port = (uint16_t)x;
            }
        } else if (strcmp(line, "game") == 0) {
            cfg->game_title_id = strtoull(v, NULL, 16);
        } else if (strcmp(line, "token") == 0) {
            size_t n = strnlen(v, sizeof(cfg->token) - 1); /* the header carries at most 31 characters */
            memcpy(cfg->token, v, n);
            cfg->token[n] = '\0';
        } else if (strcmp(line, "dir") == 0) {
            snprintf(cfg->dir, sizeof(cfg->dir), "%s", v);
        } else if (strcmp(line, "watch") == 0 && x >= 0) {
            cfg->watch_s = (uint32_t)x;
        } else if (strcmp(line, "max_relaunches") == 0 && x >= 0) {
            cfg->max_relaunches = (uint32_t)x;
        } else if (strcmp(line, "terminate_ms") == 0 && x > 0 && x <= 30000) {
            cfg->terminate_ms = (uint32_t)x;
        } else if (strcmp(line, "gpureset") == 0 && x >= 0 && x <= 2) {
            cfg->gpureset = (int)x;
        } else if (strcmp(line, "launch") == 0) {
            cfg->launch_pm = strcmp(v, "pm") == 0;
        } else if (strcmp(line, "launch_flags") == 0 && x >= 0) {
            cfg->launch_flags = (uint32_t)x;
        } else if (strcmp(line, "ndm_hold") == 0) {
            cfg->ndm_hold = x != 0;
        } else if (strcmp(line, "log") == 0) {
            cfg->log_file = x != 0;
        }
    }
    fclose(f);
}

/* ---- Socket helpers ---- */

static void send_all(int fd, const void* data, size_t n) {
    const char* p = data;
    while (n > 0) {
        int r = send(fd, p, n, 0);
        if (r <= 0) {
            return;
        }
        p += r;
        n -= (size_t)r;
    }
}

static void send_line(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_line(int fd, const char* fmt, ...) {
    char buf[320];
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

/* Up to n bytes: >0 got, 0 closed, -1 timeout or error. */
static int recv_some(int fd, void* buf, size_t n, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 250) {
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
    char* p = buf;
    while (n > 0) {
        int r = recv_some(fd, p, n, timeout_ms);
        if (r <= 0) {
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 2) < 0) {
        sv_log("supervisor: bind/listen on %u failed, errno %d", (unsigned)port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- CRC32 (zlib's polynomial) ---- */

static u32 s_crc_table[256];

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

/* ---- State ---- */

static u8 s_buf[CHUNK] __attribute__((aligned(64))); /* push and files: this thread only */
static SvWatch s_wd;
static uint32_t s_beats_seen;
static char s_switches[MAX_ARGS + 1]; /* the last launch's switches: the watchdog relaunches with them */
static char s_last[200] = "none";     /* the last action, for status */
static uint64_t s_game_override;      /* a push named the game when game= was 0 */

static u64 now_ms(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000u); }

static uint64_t game_tid(const SvConfig* cfg) { return cfg->game_title_id != 0 ? cfg->game_title_id : s_game_override; }

static void set_last(const char* text) { snprintf(s_last, sizeof(s_last), "[%u] %.180s", uptime_s(), text); }

static void pull_beat(void) {
    SvBeat b;
    sv_hb_get(&b);
    if (b.count != s_beats_seen) {
        s_beats_seen = b.count;
        sv_wd_heartbeat(&s_wd, b.frame, b.flags, b.tick / (SYSCLOCK_ARM11 / 1000u));
    }
}

static const char* state_text(int s) { return s == SV_GAME_RUNNING ? "running" : s == SV_GAME_STOPPED ? "stopped" : "unknown"; }

/* terminate -> GPU reset -> launch, each line to fd when fd >= 0 */
static int32_t relaunch(int fd, const SvConfig* cfg, uint64_t tid, const char* switches, int gpureset) {
    char rep[200];
    sv_terminate(cfg, tid, rep, sizeof(rep));
    if (fd >= 0) {
        send_line(fd, "%s", rep);
    }
    sv_gpu_reset(gpureset, rep, sizeof(rep));
    if (fd >= 0) {
        send_line(fd, "%s", rep);
    }
    int32_t r = sv_launch(cfg, tid, switches, rep, sizeof(rep));
    if (fd >= 0) {
        send_line(fd, "%s", rep);
    }
    snprintf(s_switches, sizeof(s_switches), "%s", switches);
    sv_wd_launched(&s_wd, now_ms());
    set_last(rep);
    return r;
}

/* ---- Commands ---- */

static void do_ping(int fd, const SvConfig* cfg) {
    uint32_t pid = 0;
    int st = sv_game_state(game_tid(cfg), &pid);
    pull_beat();
    send_line(fd, "pong supervisor version=%s kind=%s uptime=%u game=%016llx state=%s pid=%lu proto=%d hb=%lu watch=%lu",
              cfg->version, cfg->kind, uptime_s(), (unsigned long long)game_tid(cfg), state_text(st), (unsigned long)pid,
              DU_PROTO_VERSION, (unsigned long)s_beats_seen, (unsigned long)s_wd.watch_s);
    send_line(fd, "done");
}

static void do_status(int fd, const SvConfig* cfg) {
    uint32_t pid = 0;
    int st = sv_game_state(game_tid(cfg), &pid);
    pull_beat();
    u64 now = now_ms();
    struct in_addr ip = {(u32)gethostid()};
    send_line(fd, "status kind=%s", cfg->kind);
    send_line(fd, "status version=%s", cfg->version);
    send_line(fd, "status uptime_s=%u", uptime_s());
    send_line(fd, "status ip=%s port=%u", inet_ntoa(ip), (unsigned)cfg->port);
    send_line(fd, "status game_title=%016llx", (unsigned long long)game_tid(cfg));
    send_line(fd, "status game_state=%s", state_text(st));
    send_line(fd, "status game_pid=%lu", (unsigned long)pid);
    send_line(fd, "status game_dir=%s", cfg->dir[0] ? cfg->dir : "none");
    if (s_beats_seen == 0) {
        send_line(fd, "status heartbeat=none");
    } else {
        send_line(fd, "status heartbeat=%lu frame=%lu flags=%lu age_ms=%llu stale_ms=%llu since_launch=%lu",
                  (unsigned long)s_beats_seen, (unsigned long)s_wd.frame, (unsigned long)s_wd.flags,
                  (unsigned long long)(now > s_wd.beat_ms ? now - s_wd.beat_ms : 0),
                  (unsigned long long)sv_wd_stale_ms(&s_wd, now), (unsigned long)s_wd.beats);
    }
    send_line(fd, "status service=%s rc=%08lx sessions=%lu", sv_hb_result() == 0 ? "registered" : "failed",
              (unsigned long)sv_hb_result(), (unsigned long)sv_hb_sessions());
    send_line(fd, "status watch=%lu relaunches=%lu max_relaunches=%lu", (unsigned long)s_wd.watch_s,
              (unsigned long)s_wd.relaunches, (unsigned long)s_wd.max_relaunches);
    send_line(fd, "status switches=%s", s_switches);
    send_line(fd, "status last=%s", s_last);
    send_line(fd, "status config gpureset=%d launch=%s launch_flags=%lx terminate_ms=%lu ndm_hold=%d log=%d", cfg->gpureset,
              cfg->launch_pm ? "pm" : "ns", (unsigned long)cfg->launch_flags, (unsigned long)cfg->terminate_ms,
              cfg->ndm_hold ? 1 : 0, cfg->log_file ? 1 : 0);
    send_line(fd, "status commands=ping,status,terminate,launch,push,files,watch,reboot,exit");
    send_line(fd, "done");
}

static bool need_game(int fd, const SvConfig* cfg) {
    if (game_tid(cfg) != 0) {
        return true;
    }
    send_line(fd, "error 22 no game title (game= in %s, or push the game once)", CTRDK_SV_CONFIG);
    return false;
}

static void do_terminate(int fd, const SvConfig* cfg, const char* args) {
    if (!need_game(fd, cfg)) {
        return;
    }
    char rep[200];
    sv_terminate(cfg, game_tid(cfg), rep, sizeof(rep));
    send_line(fd, "%s", rep);
    set_last(rep);
    sv_gpu_reset((int)sv_kv_int(args, "gpureset", cfg->gpureset), rep, sizeof(rep));
    send_line(fd, "%s", rep);
    send_line(fd, "done");
}

static void do_launch(int fd, const SvConfig* cfg, const char* switches) {
    if (!need_game(fd, cfg)) {
        return;
    }
    /* A launch of a running title does nothing (F3) and would leave the arguments file behind: end it first. */
    int32_t r = relaunch(fd, cfg, game_tid(cfg), switches, cfg->gpureset);
    sv_wd_pushed(&s_wd, now_ms()); /* a launch by the PC counts as a push: the relaunch budget is full again */
    if (R_FAILED(r)) {
        send_line(fd, "error 23 launch failed: rc=%08lx", (unsigned long)r);
        return;
    }
    send_line(fd, "done");
}

static void do_watch(int fd, const char* args) {
    long n = strtol(args, NULL, 0);
    if (n < 0 || n > 86400) {
        send_line(fd, "error 3 watch: seconds 0..86400");
        return;
    }
    sv_wd_arm(&s_wd, (uint32_t)n, now_ms());
    sv_hb_set_watch((uint32_t)n);
    char t[80];
    snprintf(t, sizeof(t), "watch %ld", n);
    set_last(t);
    sv_log("supervisor: %s", t);
    send_line(fd, "watch armed=%lu relaunches=%lu max_relaunches=%lu", (unsigned long)s_wd.watch_s,
              (unsigned long)s_wd.relaunches, (unsigned long)s_wd.max_relaunches);
    send_line(fd, "done");
}

static void push_fail(int fd, int code, const char* what, const char* stage, u32 at, Result rc) {
    char t[200];
    snprintf(t, sizeof(t), "error %d %s: stage=%s at=%lu rc=%08lx (install cancelled, old install kept)", code, what, stage,
             (unsigned long)at, (unsigned long)rc);
    sv_log("supervisor: push %s", t);
    set_last(t);
    send_line(fd, "%s", t);
}

static void do_push(int fd, const SvConfig* cfg, const DuHeader* h, const char* switches, const char* incoming) {
    uint64_t tid = h->title_id;
    bool self = tid == CTRDK_SV_TITLE_ID;
    if (!self) {
        if (game_tid(cfg) == 0) {
            s_game_override = tid;
        } else if (tid != game_tid(cfg)) {
            send_line(fd, "error 24 title %016llx is not the watched game %016llx", (unsigned long long)tid,
                      (unsigned long long)game_tid(cfg));
            return;
        }
    }
    sv_log("supervisor: push %016llx (%lu bytes, %s)%s", (unsigned long long)tid, (unsigned long)h->size,
           incoming[0] ? incoming : "no version text", self ? " = self-update" : "");
    char rep[200];
    if (!self) {
        sv_terminate(cfg, tid, rep, sizeof(rep)); /* always: whatever state the game is in */
        send_line(fd, "%s", rep);
    }
    Result rc = amInit();
    if (R_FAILED(rc)) {
        push_fail(fd, 13, "amInit failed", "am-start", 0, rc);
        goto relaunch_old;
    }
    Handle cia = 0;
    rc = AM_StartCiaInstall(self ? MEDIATYPE_NAND : MEDIATYPE_SD, &cia);
    if (R_FAILED(rc)) {
        amExit();
        push_fail(fd, 13, "AM_StartCiaInstall failed", "am-start", 0, rc);
        goto relaunch_old;
    }
    send_line(fd, "ok receiving %lu", (unsigned long)h->size);
    u32 done = 0, fill = 0, woff = 0, crc = 0xFFFFFFFFu;
    while (done < h->size) {
        u32 want = CHUNK - fill;
        if (want > h->size - done) {
            want = h->size - done;
        }
        int r = recv_some(fd, s_buf + fill, want, RECV_TIMEOUT_MS);
        if (r <= 0) {
            AM_CancelCIAInstall(cia);
            amExit();
            push_fail(fd, r == 0 ? 14 : 15, r == 0 ? "connection closed" : "receive timeout", "receive", done, 0);
            goto relaunch_old;
        }
        crc = crc_update(crc, s_buf + fill, (u32)r);
        fill += (u32)r;
        done += (u32)r;
        if (fill == CHUNK || done == h->size) { /* whole 64 KiB writes at 64 KiB offsets (devupdate's finding) */
            u32 w = 0;
            rc = FSFILE_Write(cia, &w, woff, s_buf, fill, 0);
            if (R_FAILED(rc) || w != fill) {
                AM_CancelCIAInstall(cia);
                amExit();
                push_fail(fd, 17, "AM write failed", "am-write", woff, R_FAILED(rc) ? rc : (Result)(0xE0000000u | w));
                goto relaunch_old;
            }
            woff += fill;
            fill = 0;
        }
    }
    crc = ~crc;
    if (crc != h->crc) {
        AM_CancelCIAInstall(cia);
        amExit();
        push_fail(fd, 5, "crc mismatch", "crc", done, (Result)crc);
        goto relaunch_old;
    }
    send_line(fd, "installing");
    rc = AM_FinishCiaInstall(cia);
    amExit();
    if (R_FAILED(rc)) {
        push_fail(fd, 18, "AM_FinishCiaInstall failed", "am-commit", done, rc);
        goto relaunch_old;
    }
    send_line(fd, "installed %016llx", (unsigned long long)tid);
    sv_log("supervisor: installed %016llx", (unsigned long long)tid);
    if (self) {
        uint64_t g = game_tid(cfg);
        if (g != 0 && sv_game_state(g, NULL) == SV_GAME_STOPPED) {
            sv_launch(cfg, g, s_switches, rep, sizeof(rep)); /* its ensure() starts the new supervisor */
            send_line(fd, "%s", rep);
        }
        send_line(fd, "exiting: the app's ensure() (at its start, or its heartbeat thread within 10 s) starts the new supervisor");
        set_last("self-update installed, exiting");
        send_line(fd, "done");
        sv_exit_requested = true;
        return;
    }
    sv_gpu_reset(cfg->gpureset, rep, sizeof(rep));
    send_line(fd, "%s", rep);
    rc = sv_launch(cfg, tid, switches, rep, sizeof(rep));
    send_line(fd, "%s", rep);
    snprintf(s_switches, sizeof(s_switches), "%s", switches);
    sv_wd_pushed(&s_wd, now_ms());
    set_last(rep);
    if (R_FAILED(rc)) {
        send_line(fd, "error 23 installed, but the launch failed: rc=%08lx", (unsigned long)rc);
        return;
    }
    send_line(fd, "done");
    return;

relaunch_old:
    if (!self) {
        /* No rollback, but the old build runs again, so the PC can retry here or on devupdate's port. */
        sv_gpu_reset(cfg->gpureset, rep, sizeof(rep));
        sv_launch(cfg, tid, s_switches, rep, sizeof(rep));
        sv_wd_launched(&s_wd, now_ms());
        sv_log("supervisor: after the failed push: %s", rep);
    }
}

/* ---- files (read-only) ---- */

/* "@sv/x" -> this module's folder, "@/x" or "x" -> the app's folder. false: bad path or no folder. */
static bool ro_path(const SvConfig* cfg, const char* rel, char* out, size_t n, int fd) {
    const char* root = cfg->dir;
    if (strncmp(rel, "@sv/", 4) == 0 || strcmp(rel, "@sv") == 0) {
        root = SV_DIR;
        rel += rel[3] == '/' ? 4 : 3;
    } else if (strncmp(rel, "@/", 2) == 0) {
        rel += 2;
    } else if (strcmp(rel, "@") == 0) {
        rel += 1;
    }
    if (root[0] == '\0') {
        send_line(fd, "error 19 no app folder (dir= in %s; the app's ensure() writes it)", CTRDK_SV_CONFIG);
        return false;
    }
    if (rel[0] != '\0' && !sv_rel_ok(rel)) {
        send_line(fd, "error 20 bad path");
        return false;
    }
    int w = rel[0] ? snprintf(out, n, "%s/%s", root, rel) : snprintf(out, n, "%s", root);
    if (w <= 0 || (size_t)w >= n) {
        send_line(fd, "error 20 bad path");
        return false;
    }
    return true;
}

static void files_get(int fd, const SvConfig* cfg, const char* rel) {
    char path[200];
    if (!ro_path(cfg, rel, path, sizeof(path), fd)) {
        return;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        send_line(fd, "error 21 no such file %s", rel);
        return;
    }
    /* Two passes over the first n bytes: a log the game appends to keeps those bytes, so the CRC holds. */
    struct stat st;
    u32 n = fstat(fileno(f), &st) == 0 ? (u32)st.st_size : 0;
    u32 crc = 0xFFFFFFFFu, got = 0;
    while (got < n) {
        size_t want = n - got < CHUNK ? n - got : CHUNK;
        size_t r = fread(s_buf, 1, want, f);
        if (r == 0) {
            break;
        }
        crc = crc_update(crc, s_buf, (u32)r);
        got += (u32)r;
    }
    n = got;
    send_line(fd, "file %lu %08lx", (unsigned long)n, (unsigned long)~crc);
    fseek(f, 0, SEEK_SET);
    for (u32 sent = 0; sent < n;) {
        size_t want = n - sent < CHUNK ? n - sent : CHUNK;
        size_t r = fread(s_buf, 1, want, f);
        if (r == 0) {
            memset(s_buf, 0, want); /* the file shrank: the CRC on the PC fails, the framing holds */
            r = want;
        }
        send_all(fd, s_buf, r);
        sent += (u32)r;
    }
    fclose(f);
    send_line(fd, "%s", "");
    send_line(fd, "done");
}

static void files_ls(int fd, const SvConfig* cfg, const char* rel) {
    char path[200];
    if (!ro_path(cfg, rel, path, sizeof(path), fd)) {
        return;
    }
    DIR* d = opendir(path);
    if (d == NULL) {
        send_line(fd, "error 21 no such folder %s", rel);
        return;
    }
    struct dirent* e;
    int count = 0;
    while ((e = readdir(d)) != NULL && count < 500) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char full[460];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        bool dir = false;
        long size = 0;
        if (stat(full, &st) == 0) {
            dir = S_ISDIR(st.st_mode);
            size = (long)st.st_size;
        }
        send_line(fd, "entry %c %ld %s", dir ? 'd' : 'f', size, e->d_name);
        count++;
    }
    closedir(d);
    send_line(fd, "done");
}

static void do_files(int fd, const SvConfig* cfg, const char* args) {
    const char* rel = strchr(args, ' ');
    rel = rel != NULL ? rel + 1 : "";
    if (strncmp(args, "get ", 4) == 0) {
        files_get(fd, cfg, rel);
    } else if (strncmp(args, "ls", 2) == 0 && (args[2] == '\0' || args[2] == ' ')) {
        files_ls(fd, cfg, rel);
    } else if (strncmp(args, "put", 3) == 0 || strncmp(args, "rm", 2) == 0) {
        send_line(fd, "error 20 read-only (the supervisor's files are get and ls only)");
    } else {
        send_line(fd, "error 3 unknown files command");
    }
}

/* ---- reboot, exit ---- */

static void do_reboot(int fd, const SvConfig* cfg, const char* args) {
    bool hard = sv_has_word(args, "hard");
    uint64_t tid = game_tid(cfg);
    if (sv_has_word(args, "dry")) {
        send_line(fd, "reboot dry: would call %s", hard || tid == 0 ? "APT_HardwareResetAsync" : "NS_RebootToTitle(SD, game), then APT_HardwareResetAsync");
        send_line(fd, "done");
        return;
    }
    send_line(fd, "rebooting via=%s game=%016llx", hard || tid == 0 ? "apt" : "ns", (unsigned long long)tid);
    send_line(fd, "done");
    shutdown(fd, SHUT_RDWR);
    set_last("reboot");
    char rep[160];
    sv_reboot(tid, hard, rep, sizeof(rep)); /* returns only if neither path rebooted */
    sv_log("supervisor: %s", rep);
    set_last(rep);
}

/* ---- Watchdog ---- */

static void watchdog_tick(const SvConfig* cfg) {
    static u64 s_next;
    u64 now = now_ms();
    if (now < s_next) {
        return;
    }
    s_next = now + 1000;
    pull_beat();
    if (s_wd.watch_s == 0 || game_tid(cfg) == 0) {
        return;
    }
    int st = sv_game_state(game_tid(cfg), NULL);
    uint64_t stale = sv_wd_stale_ms(&s_wd, now);
    int act = sv_wd_tick(&s_wd, st, now);
    if (act == SV_WD_RELAUNCH) {
        sv_log("supervisor: watchdog: no heartbeat change for %llu ms (flags %lu, state %s): relaunch %lu of %lu",
               (unsigned long long)stale, (unsigned long)s_wd.flags, state_text(st), (unsigned long)s_wd.relaunches,
               (unsigned long)s_wd.max_relaunches);
        relaunch(-1, cfg, game_tid(cfg), s_switches, cfg->gpureset);
    } else if (act == SV_WD_GIVE_UP) {
        sv_hb_set_watch(0);
        sv_log("supervisor: watchdog: still no heartbeat after %lu relaunches: disarmed, waiting for the PC",
               (unsigned long)s_wd.relaunches);
        set_last("watchdog gave up (disarmed)");
    }
}

/* ---- Requests ---- */

static bool token_ok(const SvConfig* cfg, const char* got) {
    if (cfg->token[0] == '\0') {
        return true;
    }
    char t[33];
    memcpy(t, got, 32);
    t[32] = '\0';
    return strcmp(t, cfg->token) == 0;
}

static void handle_client(int fd, SvConfig* cfg) {
    DuHeader h;
    if (recv_all(fd, &h, sizeof(h), 10000) != 0) {
        return;
    }
    sv_config_load(cfg, false); /* game=, dir=, token= and the switches may have changed */
    s_wd.max_relaunches = cfg->max_relaunches;
    int bad = du_proto_check_header(&h, MAX_ARGS);
    if (bad == 1) {
        send_line(fd, "error 1 bad header (protocol version %d expected)", DU_PROTO_VERSION);
        return;
    }
    if (!token_ok(cfg, h.token)) {
        send_line(fd, "error 2 bad token");
        return;
    }
    if (bad == 3) {
        send_line(fd, "error 3 arguments too long");
        return;
    }
    static char args[MAX_ARGS + 1];
    if (h.args_len && recv_all(fd, args, h.args_len, 10000) != 0) {
        return;
    }
    char incoming[DU_VERSION_MAX + 1];
    if (du_proto_split_args(args, h.args_len, h.flags, incoming, sizeof(incoming)) < 0) {
        incoming[0] = '\0'; /* malformed version area: the whole area is switch text */
    }
    switch (h.cmd) {
    case CTRDK_SV_CMD_PING: do_ping(fd, cfg); break;
    case CTRDK_SV_CMD_STATUS: do_status(fd, cfg); break;
    case CTRDK_SV_CMD_TERMINATE: do_terminate(fd, cfg, args); break;
    case CTRDK_SV_CMD_LAUNCH: do_launch(fd, cfg, args); break;
    case CTRDK_SV_CMD_PUSH: do_push(fd, cfg, &h, args, incoming); break;
    case CTRDK_SV_CMD_FILES: do_files(fd, cfg, args); break;
    case CTRDK_SV_CMD_WATCH: do_watch(fd, args); break;
    case CTRDK_SV_CMD_REBOOT: do_reboot(fd, cfg, args); break;
    case CTRDK_SV_CMD_EXIT:
        send_line(fd, "exiting");
        send_line(fd, "done");
        set_last("exit requested");
        sv_log("supervisor: exit requested by the PC");
        sv_exit_requested = true;
        break;
    default:
        send_line(fd, "error 4 unknown command %u (supervisor: ping status terminate launch push files watch reboot exit)",
                  (unsigned)h.cmd);
        break;
    }
}

void sv_serve(SvConfig* cfg, volatile bool* quit) {
    crc_init();
    sv_wd_init(&s_wd, cfg->max_relaunches);
    if (cfg->watch_s != 0) {
        sv_wd_arm(&s_wd, cfg->watch_s, now_ms());
        sv_hb_set_watch(cfg->watch_s);
    }
    int listen_fd = -1;
    bool announced = false;
    while (!*quit && !sv_exit_requested) {
        watchdog_tick(cfg);
        if (listen_fd < 0) {
            listen_fd = open_listener(cfg->port);
            if (listen_fd < 0) {
                svcSleepThread(1000000000LL);
                continue;
            }
            announced = false;
        }
        if (!announced) {
            long ip = gethostid();
            if (ip != 0 && ip != -1) {
                sv_log("supervisor: listening on %s:%u", inet_ntoa((struct in_addr){(u32)ip}), (unsigned)cfg->port);
                announced = true;
            }
        }
        struct pollfd p = {listen_fd, POLLIN, 0};
        int r = poll(&p, 1, 500);
        if (r < 0) {
            /* The network went away (sleep, Wi-Fi lost): open the listener again. */
            close(listen_fd);
            listen_fd = -1;
            svcSleepThread(1000000000LL);
            continue;
        }
        if (r == 0 || !(p.revents & POLLIN)) {
            continue;
        }
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        handle_client(fd, cfg);
        close(fd);
    }
    if (listen_fd >= 0) {
        close(listen_fd);
    }
}

void sv_server_init_clock(void) { s_start_tick = svcGetSystemTick(); }
