/* ctr-devkit supervisor: the watchdog policy and the small text helpers. Plain C with no libctru,
 * so `devupdate.py selftest` builds the same file with gcc (supervisor/selftest_host.c) and runs the
 * policy through its cases. Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_SV_POLICY_H
#define CTRDK_SV_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Heartbeat flags (the same bits as CTRDK_SV_HB_* in ctrdk/supervisor.h). */
#define SV_HB_PAUSED 1u   /* a menu or an update holds the game loop: never hung */
#define SV_HB_LOADING 2u  /* a load: the limit is SV_LOADING_FACTOR times longer */
#define SV_HB_RECOVERY 4u /* the crash screen waits for the PC: never hung */
#define SV_LOADING_FACTOR 4u

enum { SV_GAME_UNKNOWN = -1, SV_GAME_STOPPED = 0, SV_GAME_RUNNING = 1 };
enum { SV_WD_NONE = 0, SV_WD_RELAUNCH = 1, SV_WD_GIVE_UP = 2 };

typedef struct {
    uint32_t watch_s;        /* 0 = off */
    uint32_t max_relaunches; /* automatic relaunches allowed per push (or per arm) */
    uint32_t relaunches;     /* done since the last push or arm */
    uint32_t frame;          /* the last heartbeat's frame value */
    uint32_t flags;          /* the last heartbeat's flags */
    uint32_t beats;          /* heartbeats since the last launch */
    uint64_t change_ms;      /* when the frame value last changed (or the last launch / arm) */
    uint64_t beat_ms;        /* when the last heartbeat came */
} SvWatch;

void sv_wd_init(SvWatch* w, uint32_t max_relaunches);
/* watch N (0 = off). Arming resets the relaunch count and the timer. */
void sv_wd_arm(SvWatch* w, uint32_t secs, uint64_t now_ms);
/* After any launch by the supervisor: the timer starts again, the heartbeat count is 0. */
void sv_wd_launched(SvWatch* w, uint64_t now_ms);
/* After a push: the relaunch count is 0 again, then as sv_wd_launched. */
void sv_wd_pushed(SvWatch* w, uint64_t now_ms);
void sv_wd_heartbeat(SvWatch* w, uint32_t frame, uint32_t flags, uint64_t now_ms);
/* Called a few times a second. game: SV_GAME_*. Returns SV_WD_RELAUNCH (the caller terminates,
 * resets the GPU, relaunches, then calls sv_wd_launched; the count is already raised), SV_WD_GIVE_UP
 * (the limit was reached: the watchdog has disarmed itself), or SV_WD_NONE.
 * Rules: off -> none; the game stopped -> none (it ended; the PC decides); paused or recovery ->
 * the timer restarts; otherwise no frame change for watch_s (x SV_LOADING_FACTOR while loading) ->
 * relaunch, or give up after max_relaunches. "Unknown" counts as running (emulators: no process list). */
int sv_wd_tick(SvWatch* w, int game, uint64_t now_ms);
/* Milliseconds since the frame value changed (for status). */
uint64_t sv_wd_stale_ms(const SvWatch* w, uint64_t now_ms);

/* "key=value" words in a command's argument text: the value as an integer (decimal, or hex with 0x),
 * or def if the key is missing or bad. */
long sv_kv_int(const char* args, const char* key, long def);
/* The word "word" stands alone in args. */
bool sv_has_word(const char* args, const char* word);
/* The launch arguments file's text: "ctrdk:" + switches + " ctrdk_from=supervisor" (no leading
 * space when switches is empty). Returns the length, or -1 if it does not fit in n (out is then ""). */
int sv_launch_args_text(char* out, size_t n, const char* switches);
/* A path below a root for the read-only files command: false for "", absolute paths, "." and ".."
 * components, "//", and characters outside letters, digits and . _ - + /. */
bool sv_rel_ok(const char* rel);

#endif
