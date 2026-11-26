/* ctr-devkit supervisor: the TCP server (devupdate's CDU1 header; ping and status in phase 1).
 *
 * One client at a time, every receive bounded by a timeout, so a stalled PC never holds it.
 * Nothing here waits for the watched app.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>

#include "../devupdate/proto.h"
#include "ctrdk/supervisor.h"
#include "sv_internal.h"

void (*sv_log_hook)(const char* line);

void sv_log(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svcOutputDebugString(buf, (s32)strlen(buf));
    if (sv_log_hook != NULL) {
        sv_log_hook(buf);
    }
}

void sv_config_load(SvConfig* cfg) {
    FILE* f = fopen(CTRDK_SV_CONFIG, "r");
    if (f == NULL) {
        return;
    }
    char line[96];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0 && p < 65536) {
                cfg->port = (uint16_t)p;
            }
        } else if (strncmp(line, "game=", 5) == 0) {
            cfg->game_title_id = strtoull(line + 5, NULL, 16);
        } else if (strncmp(line, "token=", 6) == 0) {
            size_t n = strnlen(line + 6, sizeof(cfg->token) - 1); /* the header carries at most 31 characters */
            memcpy(cfg->token, line + 6, n);
            cfg->token[n] = '\0';
        }
    }
    fclose(f);
}

/* ---- The watched title ---- */

typedef struct {
    bool known;    /* the process list and title IDs could be read */
    bool running;
    u32 pid;
} GameState;

/* svcGetProcessInfo type 0x10001 (the title ID) is a Luma3DS kernel extension; on anything else
 * every call fails and the state is "unknown". */
static void game_state(uint64_t tid, GameState* g) {
    static u32 pids[96];
    s32 n = 0;
    memset(g, 0, sizeof(*g));
    if (R_FAILED(svcGetProcessList(&n, pids, (s32)(sizeof(pids) / sizeof(pids[0]))))) {
        return;
    }
    for (s32 i = 0; i < n; i++) {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) {
            continue;
        }
        s64 value = 0;
        Result r = svcGetProcessInfo(&value, h, 0x10001);
        svcCloseHandle(h);
        if (R_FAILED(r)) {
            continue;
        }
        g->known = true;
        if (tid != 0 && (uint64_t)value == tid) {
            g->running = true;
            g->pid = pids[i];
        }
    }
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

static int recv_all(int fd, void* buf, size_t n, int timeout_ms) {
    char* p = buf;
    int waited = 0;
    while (n > 0) {
        struct pollfd pf = {fd, POLLIN, 0};
        int r = poll(&pf, 1, 250);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            waited += 250;
            if (waited >= timeout_ms) {
                return -1;
            }
            continue;
        }
        int got = recv(fd, p, n, 0);
        if (got <= 0) {
            return -1;
        }
        p += got;
        n -= (size_t)got;
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

/* ---- Commands ---- */

static u64 s_start_tick;

static unsigned uptime_s(void) { return (unsigned)((svcGetSystemTick() - s_start_tick) / SYSCLOCK_ARM11); }

static const char* state_text(const GameState* g) { return !g->known ? "unknown" : g->running ? "running" : "stopped"; }

static void do_ping(int fd, const SvConfig* cfg) {
    GameState g;
    game_state(cfg->game_title_id, &g);
    send_line(fd, "pong supervisor version=%s kind=%s uptime=%u game=%016llx state=%s pid=%lu proto=%d", cfg->version,
              cfg->kind, uptime_s(), (unsigned long long)cfg->game_title_id, state_text(&g), (unsigned long)g.pid,
              DU_PROTO_VERSION);
    send_line(fd, "done");
}

static void do_status(int fd, const SvConfig* cfg) {
    GameState g;
    game_state(cfg->game_title_id, &g);
    struct in_addr ip = {(u32)gethostid()};
    send_line(fd, "status kind=%s", cfg->kind);
    send_line(fd, "status version=%s", cfg->version);
    send_line(fd, "status uptime_s=%u", uptime_s());
    send_line(fd, "status ip=%s port=%u", inet_ntoa(ip), (unsigned)cfg->port);
    send_line(fd, "status game_title=%016llx", (unsigned long long)cfg->game_title_id);
    send_line(fd, "status game_state=%s", state_text(&g));
    send_line(fd, "status game_pid=%lu", (unsigned long)g.pid);
    send_line(fd, "status heartbeat=none"); /* phase 2: the app's heartbeat service */
    send_line(fd, "status commands=ping,status");
    send_line(fd, "done");
}

static bool token_ok(const SvConfig* cfg, const char* got) {
    if (cfg->token[0] == '\0') {
        return true;
    }
    char t[33];
    memcpy(t, got, 32);
    t[32] = '\0';
    return strcmp(t, cfg->token) == 0;
}

static void handle_client(int fd, const SvConfig* cfg) {
    DuHeader h;
    if (recv_all(fd, &h, sizeof(h), 10000) != 0) {
        return;
    }
    int bad = du_proto_check_header(&h, 0x2F0);
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
    static char args[0x2F0 + 1];
    if (h.args_len && recv_all(fd, args, h.args_len, 10000) != 0) {
        return;
    }
    switch (h.cmd) {
    case CTRDK_SV_CMD_PING: do_ping(fd, cfg); break;
    case CTRDK_SV_CMD_STATUS: do_status(fd, cfg); break;
    default: send_line(fd, "error 4 unknown command %u (supervisor phase 1: ping, status)", (unsigned)h.cmd); break;
    }
}

void sv_serve(const SvConfig* cfg, volatile bool* quit) {
    s_start_tick = svcGetSystemTick();
    int listen_fd = -1;
    bool announced = false;
    while (!*quit) {
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
