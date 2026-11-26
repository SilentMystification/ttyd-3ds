/* ctr-devkit supervisor: the in-app client (install, update, start, status, uninstall, heartbeat).
 *
 * Runs in the app's process, development builds only. Needs am:net (install, delete) and
 * pm:app (start, end); Luma3DS's sm does not check the service access list, so an app CIA can
 * open both. pm:app has only 4 sessions system-wide, so each call opens and closes its own.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>

#include "ctrdk/supervisor.h"

#define CHUNK 0x10000
#define END_TIMEOUT_NS 3000000000LL

static const FS_ProgramInfo s_prog = {CTRDK_SV_TITLE_ID, MEDIATYPE_NAND, {0}};

static uint32_t be32(const u8* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const u8* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t align64(uint32_t x) { return (x + 63u) & ~63u; }

/* The title ID from the CIA's TMD (the same walk as devupdate.py's cia_title_id). 0 = unreadable. */
static uint64_t cia_title_id(FILE* f) {
    u8 h[0x20];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) {
        return 0;
    }
    uint32_t off = align64(le32(h)) + align64(le32(h + 8)) + align64(le32(h + 12));
    u8 sig[4];
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(sig, 1, 4, f) != 4) {
        return 0;
    }
    uint32_t len;
    switch (be32(sig)) {
    case 0x10003: len = 0x200 + 0x3C; break;
    case 0x10004: len = 0x100 + 0x3C; break;
    case 0x10005: len = 0x3C + 0x40; break;
    default: return 0;
    }
    u8 id[8];
    if (fseek(f, (long)(off + 4 + len + 0x4C), SEEK_SET) != 0 || fread(id, 1, 8, f) != 8) {
        return 0;
    }
    return (uint64_t)be32(id) << 32 | be32(id + 4);
}

static bool is_running(bool* known) {
    static u32 pids[96];
    s32 n = 0;
    *known = false;
    if (R_FAILED(svcGetProcessList(&n, pids, (s32)(sizeof(pids) / sizeof(pids[0]))))) {
        return false;
    }
    for (s32 i = 0; i < n; i++) {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) {
            continue;
        }
        s64 tid = 0;
        Result r = svcGetProcessInfo(&tid, h, 0x10001); /* Luma3DS kernel extension: the title ID */
        svcCloseHandle(h);
        if (R_SUCCEEDED(r)) {
            *known = true;
            if ((uint64_t)tid == CTRDK_SV_TITLE_ID) {
                return true;
            }
        }
    }
    return false;
}

int32_t ctrdk_supervisor_status(CtrdkSvStatus* out) {
    memset(out, 0, sizeof(*out));
    Result r = amInit();
    if (R_SUCCEEDED(r)) {
        AM_TitleEntry e;
        u64 tid = CTRDK_SV_TITLE_ID;
        if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_NAND, 1, &tid, &e))) {
            out->installed = true;
            out->version = e.version;
        }
        amExit();
    }
    out->running = is_running(&out->running_known);
    return r;
}

static Result pm_end(void) {
    Result r = pmAppInit();
    if (R_FAILED(r)) {
        return r;
    }
    r = PMAPP_TerminateTitle(CTRDK_SV_TITLE_ID, END_TIMEOUT_NS);
    pmAppExit();
    return r;
}

static Result pm_start(void) {
    Result r = pmAppInit();
    if (R_FAILED(r)) {
        return r;
    }
    r = PMAPP_LaunchTitle(&s_prog, 0); /* no flags: not an application, not tied to the caller */
    pmAppExit();
    return r;
}

static void fail(CtrdkSvResult* out, const char* stage, Result r) {
    out->stage = stage;
    out->result = r;
}

/* ---- The configuration file: game=, port= and dir= are this app's; every other line is kept ---- */

#define CFG_MAX 2048

static uint64_t own_title_id(void) {
    s64 v = 0;
    if (R_SUCCEEDED(svcGetProcessInfo(&v, CUR_PROCESS_HANDLE, 0x10001)) && v != 0) { /* Luma3DS kernel extension */
        return (uint64_t)v;
    }
    u64 id = 0;
    return R_SUCCEEDED(APT_GetProgramID(&id)) ? id : 0;
}

/* This app's working directory without a trailing slash ("sdmc:/3ds/app"), or "" when it is not a
 * device path. */
static void own_dir(char* out, size_t n) {
    if (getcwd(out, n) == NULL) {
        out[0] = '\0';
        return;
    }
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    if (len == 0 || strchr(out, ':') == NULL) {
        out[0] = '\0';
    }
}

/* Rewrites CTRDK_SV_CONFIG with game= (and port= when port is not 0, dir= when dir is not "")
 * first and every other line kept. Writes only when the text changes. */
static void update_config(uint64_t game, uint16_t port, const char* dir) {
    static char old[CFG_MAX], out[CFG_MAX + 256];
    size_t n = 0;
    FILE* f = fopen(CTRDK_SV_CONFIG, "r");
    if (f != NULL) {
        n = fread(old, 1, sizeof(old) - 1, f);
        fclose(f);
    }
    old[n] = '\0';
    size_t o = (size_t)snprintf(out, sizeof(out), "game=%016llx\n", (unsigned long long)game);
    if (port != 0) {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "port=%u\n", (unsigned)port);
    }
    if (dir[0] != '\0') {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "dir=%s\n", dir);
    }
    for (char* line = old; *line != '\0';) {
        char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        bool drop = len == 0 || strncmp(line, "game=", 5) == 0 || (port != 0 && strncmp(line, "port=", 5) == 0) ||
                    (dir[0] != '\0' && strncmp(line, "dir=", 4) == 0);
        if (!drop && o + len + 2 < sizeof(out)) {
            memcpy(out + o, line, len);
            o += len;
            out[o++] = '\n';
        }
        line += len + (end != NULL ? 1 : 0);
    }
    out[o] = '\0';
    if (strcmp(old, out) == 0) {
        return; /* the order written is stable: no SD write once it is right */
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ctrdk", 0777);
    f = fopen(CTRDK_SV_CONFIG, "w");
    if (f != NULL) {
        fwrite(out, 1, o, f);
        fclose(f);
    }
}

static void write_config(uint64_t game, uint16_t port) {
    char dir[128];
    own_dir(dir, sizeof(dir));
    update_config(game, port ? port : CTRDK_SV_PORT, dir);
}

int32_t ctrdk_supervisor_install(const char* cia_path, uint64_t game_title_id, uint16_t port, CtrdkSvResult* out) {
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(cia_path, "rb");
    if (f == NULL) {
        fail(out, "open", -1);
        return out->result;
    }
    if (cia_title_id(f) != CTRDK_SV_TITLE_ID) {
        fclose(f);
        fail(out, "open", -2); /* not the supervisor: never install anything else to NAND from here */
        return out->result;
    }
    bool known;
    if (is_running(&known) || !known) {
        pm_end(); /* the update path: the old one ends before its title is replaced */
    }
    u8* buf = malloc(CHUNK);
    Result r = amInit();
    if (buf == NULL || R_FAILED(r)) {
        free(buf);
        fclose(f);
        fail(out, "am-start", buf == NULL ? -3 : r);
        return out->result;
    }
    Handle h = 0;
    r = AM_StartCiaInstall(MEDIATYPE_NAND, &h);
    if (R_FAILED(r)) {
        fail(out, "am-start", r);
    } else {
        fseek(f, 0, SEEK_SET);
        u64 off = 0;
        size_t n;
        while ((n = fread(buf, 1, CHUNK, f)) > 0) {
            u32 w = 0;
            r = FSFILE_Write(h, &w, off, buf, (u32)n, 0);
            if (R_FAILED(r) || w != n) {
                fail(out, "am-write", R_FAILED(r) ? r : -4);
                break;
            }
            off += n;
        }
        if (out->stage == NULL && ferror(f)) {
            fail(out, "am-write", -5);
        }
        if (out->stage != NULL) {
            AM_CancelCIAInstall(h);
        } else {
            r = AM_FinishCiaInstall(h);
            if (R_FAILED(r)) {
                fail(out, "am-finish", r);
            }
        }
    }
    amExit();
    free(buf);
    fclose(f);
    if (out->stage != NULL) {
        return out->result;
    }
    write_config(game_title_id, port);
    r = pm_start();
    if (R_FAILED(r)) {
        fail(out, "launch", r);
        out->reboot_needed = true;
        return out->result;
    }
    out->started = true;
    return 0;
}

int32_t ctrdk_supervisor_uninstall(CtrdkSvResult* out) {
    memset(out, 0, sizeof(*out));
    pm_end(); /* not running is fine */
    Result r = amInit();
    if (R_SUCCEEDED(r)) {
        r = AM_DeleteTitle(MEDIATYPE_NAND, CTRDK_SV_TITLE_ID);
        amExit();
    }
    remove(CTRDK_SV_CONFIG);
    if (R_FAILED(r)) {
        fail(out, "delete", r);
    }
    return out->result;
}

static volatile int s_installed = -1; /* the last ensure()'s view: -1 not looked, 0 no, 1 yes */

int32_t ctrdk_supervisor_ensure(void) {
    CtrdkSvStatus st;
    ctrdk_supervisor_status(&st);
    s_installed = st.installed ? 1 : 0;
    if (!st.installed) {
        return 0;
    }
    uint64_t me = own_title_id();
    if (me != 0 && me != CTRDK_SV_TITLE_ID) {
        char dir[128];
        own_dir(dir, sizeof(dir));
        update_config(me, 0, dir);
    }
    if (st.running_known && st.running) {
        return 0;
    }
    return pm_start(); /* PM returns at once for a title that already runs (Luma3DS pm, launch.c) */
}

/* ---- Heartbeat ---- */

/* devupdate, when linked: an update in progress holds the app; recovery mode is the crash screen. */
extern bool devupdate_busy(void) __attribute__((weak));
extern bool devupdate_in_recovery(void) __attribute__((weak));

#define HB_PERIOD_NS 1000000000LL
#define HB_RETRY_S 10
#define HB_RETRY_NOT_INSTALLED_S 60 /* not installed (the usual case): look rarely, AM is not free */

static volatile uint32_t s_hb_frame, s_hb_flags, s_hb_sent;
static volatile int s_hb_open;
static int s_hb_started;

/* The client thread: the only place that talks to the service, so the app's frame loop never waits
 * for the supervisor. */
static void hb_main(void* arg) {
    (void)arg;
    Handle session = 0;
    u64 next_try = 0; /* ticks; 0 = at once */
    int looks = 0;
    for (;;) {
        u64 now = svcGetSystemTick();
        if (session == 0 && now >= next_try) {
            next_try = now + (u64)HB_RETRY_S * SYSCLOCK_ARM11;
            bool reg = false;
            if (R_SUCCEEDED(srvIsServiceRegistered(&reg, CTRDK_SV_SERVICE)) && reg) {
                if (R_FAILED(srvGetServiceHandle(&session, CTRDK_SV_SERVICE))) {
                    session = 0;
                }
            } else if (looks++ > 0) {
                /* Not there after the first look: the boot-time ensure() was too early, or the
                 * supervisor ended (its own update, a fault). Start it (nothing if not installed). */
                ctrdk_supervisor_ensure();
                if (s_installed == 0) {
                    next_try = now + (u64)HB_RETRY_NOT_INSTALLED_S * SYSCLOCK_ARM11;
                }
            }
        }
        if (session != 0) {
            uint32_t flags = s_hb_flags;
            if (devupdate_busy != NULL && devupdate_busy()) {
                flags |= CTRDK_SV_HB_PAUSED;
            }
            if (devupdate_in_recovery != NULL && devupdate_in_recovery()) {
                flags |= CTRDK_SV_HB_RECOVERY;
            }
            u32* cmd = getThreadCommandBuffer();
            cmd[0] = IPC_MakeHeader(1, 2, 0);
            cmd[1] = s_hb_frame;
            cmd[2] = flags;
            Result r = svcSendSyncRequest(session);
            if (R_FAILED(r) || R_FAILED((Result)cmd[1])) {
                svcCloseHandle(session); /* the supervisor ended: look again in HB_RETRY_S */
                session = 0;
                s_hb_open = 0;
                next_try = svcGetSystemTick() + (u64)HB_RETRY_S * SYSCLOCK_ARM11;
            } else {
                s_hb_sent++;
                s_hb_open = 1;
            }
        }
        svcSleepThread(HB_PERIOD_NS);
    }
}

void ctrdk_supervisor_heartbeat(uint32_t frame, uint32_t flags) {
    s_hb_frame = frame;
    s_hb_flags = flags;
    if (!s_hb_started) {
        s_hb_started = 1;
        /* Lowest priority on the app's own core: it runs while the game thread waits (vblank). */
        threadCreate(hb_main, NULL, 0x1000, 0x3F, -2, true);
    }
}

int ctrdk_supervisor_heartbeat_state(uint32_t* sent) {
    if (sent != NULL) {
        *sent = s_hb_sent;
    }
    return s_hb_open;
}
