/* ctr-devkit supervisor: the in-app client (install, update, start, status, uninstall).
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

static void write_config(uint64_t game, uint16_t port) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ctrdk", 0777);
    FILE* f = fopen(CTRDK_SV_CONFIG, "w");
    if (f != NULL) {
        fprintf(f, "port=%u\ngame=%016llx\n", (unsigned)(port ? port : CTRDK_SV_PORT), (unsigned long long)game);
        fclose(f);
    }
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

int32_t ctrdk_supervisor_ensure(void) {
    CtrdkSvStatus st;
    ctrdk_supervisor_status(&st);
    if (!st.installed || (st.running_known && st.running)) {
        return 0;
    }
    return pm_start(); /* PM returns at once for a title that already runs (Luma3DS pm, launch.c) */
}
