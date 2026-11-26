/* ctr-devkit supervisor: the watchdog policy and text helpers (see policy.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "policy.h"

void sv_wd_init(SvWatch* w, uint32_t max_relaunches) {
    memset(w, 0, sizeof(*w));
    w->max_relaunches = max_relaunches;
}

void sv_wd_arm(SvWatch* w, uint32_t secs, uint64_t now_ms) {
    w->watch_s = secs;
    w->relaunches = 0;
    w->change_ms = now_ms;
}

void sv_wd_launched(SvWatch* w, uint64_t now_ms) {
    w->beats = 0;
    w->flags = 0;
    w->change_ms = now_ms;
}

void sv_wd_pushed(SvWatch* w, uint64_t now_ms) {
    w->relaunches = 0;
    sv_wd_launched(w, now_ms);
}

void sv_wd_heartbeat(SvWatch* w, uint32_t frame, uint32_t flags, uint64_t now_ms) {
    if (w->beats == 0 || frame != w->frame) {
        w->change_ms = now_ms;
    }
    w->frame = frame;
    w->flags = flags;
    w->beats++;
    w->beat_ms = now_ms;
}

uint64_t sv_wd_stale_ms(const SvWatch* w, uint64_t now_ms) { return now_ms > w->change_ms ? now_ms - w->change_ms : 0; }

int sv_wd_tick(SvWatch* w, int game, uint64_t now_ms) {
    if (w->watch_s == 0 || game == SV_GAME_STOPPED) {
        return SV_WD_NONE;
    }
    if (w->beats > 0 && (w->flags & (SV_HB_PAUSED | SV_HB_RECOVERY))) {
        w->change_ms = now_ms; /* a paused or recovering game is never hung */
        return SV_WD_NONE;
    }
    uint64_t limit = (uint64_t)w->watch_s * 1000u;
    if (w->beats > 0 && (w->flags & SV_HB_LOADING)) {
        limit *= SV_LOADING_FACTOR;
    }
    if (sv_wd_stale_ms(w, now_ms) < limit) {
        return SV_WD_NONE;
    }
    if (w->relaunches >= w->max_relaunches) {
        w->watch_s = 0;
        return SV_WD_GIVE_UP;
    }
    w->relaunches++;
    return SV_WD_RELAUNCH;
}

/* ---- Text ---- */

/* The value after "key=" when key starts a word, else NULL. */
static const char* kv_find(const char* args, const char* key) {
    size_t k = strlen(key);
    for (const char* p = args; p != NULL && *p != '\0';) {
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, key, k) == 0 && p[k] == '=') {
            return p + k + 1;
        }
        p = strchr(p, ' ');
    }
    return NULL;
}

long sv_kv_int(const char* args, const char* key, long def) {
    const char* v = kv_find(args, key);
    if (v == NULL || *v == '\0' || *v == ' ') {
        return def;
    }
    char* end = NULL;
    long x = strtol(v, &end, 0);
    if (end == v || (*end != '\0' && *end != ' ')) {
        return def;
    }
    return x;
}

bool sv_has_word(const char* args, const char* word) {
    size_t k = strlen(word);
    for (const char* p = args; p != NULL && *p != '\0';) {
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, word, k) == 0 && (p[k] == '\0' || p[k] == ' ')) {
            return true;
        }
        p = strchr(p, ' ');
    }
    return false;
}

int sv_launch_args_text(char* out, size_t n, const char* switches) {
    int w = snprintf(out, n, "ctrdk:%s%sctrdk_from=supervisor", switches, switches[0] != '\0' ? " " : "");
    if (w < 0 || (size_t)w >= n) {
        if (n > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return w;
}

bool sv_rel_ok(const char* rel) {
    size_t n = strlen(rel);
    if (n == 0 || n > 100 || rel[0] == '/') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = rel[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == '+' || c == '/';
        if (!ok) {
            return false;
        }
        if (c == '.' && (i == 0 || rel[i - 1] == '/') && (rel[i + 1] == '.' || rel[i + 1] == '/' || rel[i + 1] == '\0')) {
            return false;
        }
        if (c == '/' && rel[i + 1] == '/') {
            return false;
        }
    }
    return rel[n - 1] != '/';
}
