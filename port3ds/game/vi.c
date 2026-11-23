/* Dolphin VI on the 3DS: retrace timing and the per-frame hooks.
 *
 * The top screen's vertical blank (59.83 Hz) stands in for the GameCube's
 * 60 Hz retrace (PAL's 50 Hz is emulated on it, see vblank()): plat/vblank.c
 * calls vblank() on its thread, whose retrace() runs the
 * game's pre- and post-retrace callbacks as an interrupt and wakes the
 * threads waiting in VIWaitForRetrace. The frame itself is presented by GX
 * (GXCopyDisp); the external framebuffer the game hands VI is not used.
 *
 * VIWaitForRetrace is called once per game frame (DEMOSwapBuffers), so it
 * also runs the port's frame hooks: the system's HOME/sleep/close handling,
 * the bottom-screen HUD and warp requests from the debug menu.
 */
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "battle/battle.h"
#include "data/mapdata.h"
#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "event.h"
#include "pmario_sound.h"
#include "mario/mario.h"
#include "mario/mario_party.h"
#include "mario/mariost.h"
#include "memory.h"
#include "plat/gpu.h"
#include "plat/hud.h"
#include "plat/input.h"
#include "plat/plat.h"
#include "plat/sdio.h"
#include "plat/sweep.h"
#include "plat/thread.h"
#include "port3ds/loadprof.h"
#include "port3ds/os_internal.h"
#include "test_mode.h"
#include "version.h"

static VIRetraceCallback s_pre, s_post;
static volatile u32 s_retrace_count;
static OSThreadQueue s_retrace_queue;
static int s_black = 1;
static void* s_next_fb;
static u32 s_frames;
static u64 s_frame_start;

static void retrace(void) {
    port_irq_enter();
    /* determ (test mode): the callbacks run once per game frame, in VIWaitForRetrace */
    int callbacks = !tst_determ();
    if (s_pre != NULL && callbacks) {
        s_pre(s_retrace_count);
    }
    s_retrace_count++;
    if (s_post != NULL && callbacks) {
        s_post(s_retrace_count);
    }
    port_irq_leave();
    OSWakeupThread(&s_retrace_queue);
}

/* ---- Retrace rate: 60 Hz, or PAL's 50 Hz ----
 *
 * The TV format follows the render mode the game configures, as VIConfigure
 * sets it on the GameCube: a PAL console starts in VI_PAL (50 Hz) and the PAL
 * release's prompt either stays there or configures its EURGB60 mode (60 Hz).
 * NTSC, EURGB60 and MPAL retrace once per LCD vertical blank (59.83 Hz, about
 * 16.7 ms). PAL and DEBUG_PAL retrace every 20 ms: the LCD still refreshes at
 * 59.83 Hz, so the vblank thread runs the retrace on the LCD blank nearest to
 * each 20 ms tick (5 of every 6 blanks) and game frames are paced at 50 Hz by
 * this emulation, not by the LCD. A frame then stays up for one or two LCD
 * refreshes, the judder of 50 Hz content on a 60 Hz screen. */
static u32 s_tv_format =
#if VERSION == VERSION_EU
    VI_PAL;
#else
    VI_NTSC;
#endif
static u64 s_next_retrace; /* 50 Hz: tick of the next emulated retrace */

static int vi_50hz(void) {
    u32 f = __atomic_load_n(&s_tv_format, __ATOMIC_RELAXED);
    return f == VI_PAL || f == VI_DEBUG_PAL;
}

static void vblank(void) {
    if (!vi_50hz()) {
        s_next_retrace = 0;
        retrace();
        return;
    }
    const u64 period = PLAT_TICKS_PER_SEC / 50;
    u64 now = plat_ticks();
    if (s_next_retrace == 0 || now > s_next_retrace + period) {
        s_next_retrace = now; /* first 50 Hz blank, or after a stall (sleep, HOME Menu) */
    }
    if (now + PLAT_TICKS_PER_SEC / 240 < s_next_retrace) {
        return; /* the next LCD blank is nearer to the tick than this one */
    }
    s_next_retrace += period;
    retrace();
}

void VIInit(void) {
    static int ready;
    if (ready) {
        return;
    }
    ready = 1;
    if (tst_determ()) {
        /* test mode: game time from the frame count, sound paced by the frame */
        port_os_set_time_source(tst_game_time);
        port_ai_set_frame_paced(1);
    }
    OSInitThreadQueue(&s_retrace_queue);
    plat_vblank_start(vblank);
}

void VIConfigure(const GXRenderModeObj* rm) {
    if (rm != NULL) {
        u32 format = (u32)rm->viTVmode >> 2;
        if (format != s_tv_format) {
            PLOG_I("VI: TV format %lu -> %lu (%s Hz)", (unsigned long)s_tv_format, (unsigned long)format,
                   format == VI_PAL || format == VI_DEBUG_PAL ? "50" : "60");
        }
        __atomic_store_n(&s_tv_format, format, __ATOMIC_RELAXED);
    }
}
void VIFlush(void) {}
void VISetNextFrameBuffer(void* fb) { s_next_fb = fb; }
void VISetBlack(BOOL black) { s_black = black; }
u32 VIGetRetraceCount(void) { return s_retrace_count; }
u32 VIGetNextField(void) { return 1; }
/* No component cable: the progressive-scan prompt never shows. */
u32 VIGetDTVStatus(void) { return 0; }

u32 VIGetTvFormat(void) { return __atomic_load_n(&s_tv_format, __ATOMIC_RELAXED); }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    BOOL level = OSDisableInterrupts();
    VIRetraceCallback old = s_pre;
    s_pre = callback;
    OSRestoreInterrupts(level);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    BOOL level = OSDisableInterrupts();
    VIRetraceCallback old = s_post;
    s_post = callback;
    OSRestoreInterrupts(level);
    return old;
}

/* A new file's state, as seq_load's "new game" sets it up, for a warp that
 * starts outside the game (the title, the opening): the prologue's story
 * set-up, no partner out, the save flags cleared, the story so far
 * (GSW 0, which map scripts branch on) and one partner joined and out. */
static void warp_new_game(int story, int party) {
    /* the file select's new game stops the title's (or the opening's) music
     * and sounds; the warp skips it */
    psndStopAllFadeOut();
    psndSFXAllOff();
    stg0_00_init();
    marioPartyKill();
    swInit();
    swByteSet(0, story);
    if (party > 0 && party < 8) {
        partyJoin((PartyId)party);
        marioPartyEntry((PartyId)party);
    }
}

/* Switch fastboot (tests, on by default in the tools): through the boot
 * sequences before the title (the warning screen, the logos) press A every 6
 * game frames, held for 2. Stops for good once the title starts or a warp
 * (map=, sweep=) has begun. */
static int s_fastboot = -1; /* -1 unread, 0 off or finished, 1 pressing */

static uint32_t fastboot_keys(void) {
    return s_fastboot == 1 && (s_frames % 6) < 2 ? PLAT_KEY_A : 0;
}

static void fastboot_frame(int warped) {
    if (s_fastboot < 0) {
        s_fastboot = plat_switch_has("fastboot") ? 1 : 0;
        if (s_fastboot) {
            plat_input_set_extra_keys(fastboot_keys);
            PLOG_I("fastboot: on");
        }
    }
    if (s_fastboot != 1) {
        return;
    }
    int seq = gp != NULL ? seqGetSeq() : SEQ_UNDEFINED;
    if (warped || (seq != SEQ_UNDEFINED && seq != SEQ_LOGO)) {
        s_fastboot = 0;
        plat_input_set_extra_keys(NULL);
        PLOG_I("fastboot: off at frame %u (sequence %d)", (unsigned)s_frames, seq);
    }
}

static void warp_to(const char* map, const char* bero) {
    if (seqGetSeq() != SEQ_GAME) {
        warp_new_game(plat_switch_int("story", 0), plat_switch_int("party", 1));
    }
    PLOG_I("warp: %s %s", map, bero);
    seqSetSeq(SEQ_MAP_CHANGE, map, bero[0] ? bero : NULL);
}

/* Switch map=<map>[,<bero>] (tests): once the title or the opening's map
 * is up, start a new game there (story=N, party=N as for the menu's warp). */
static int s_warp_switch_done;
static void warp_switch(void) {
    static int state = -1; /* -1 unread, 0 off, 1 waiting, 2 done */
    static char map[16], bero[16];
    if (state < 0) {
        char v[40];
        state = 0;
        if (plat_switch_value("map", v, sizeof(v)) && v[0] != '\0') {
            char* comma = strchr(v, ',');
            if (comma != NULL) {
                *comma = '\0';
                snprintf(bero, sizeof(bero), "%.15s", comma + 1);
            }
            snprintf(map, sizeof(map), "%.15s", v);
            state = 1;
        }
    }
    if (state != 1 || gp == NULL) {
        return;
    }
    if (seqGetSeq() == SEQ_TITLE || (seqGetSeq() == SEQ_GAME && strcmp(gp->currentMapName, map) != 0)) {
        warp_new_game(plat_switch_int("story", 0), plat_switch_int("party", 1));
        PLOG_I("warp: %s %s (switch) at game frame %u", map, bero, (unsigned)s_frames);
        seqSetSeq(SEQ_MAP_CHANGE, map, bero[0] ? bero : NULL);
        state = 2;
        s_warp_switch_done = 1;
    }
}

/* Switch sweep=N (tests, tools/port3ds/map_sweep.py): visits each map listed
 * in sweep.txt ("<map> [<story>]" per line) once the title or the opening is
 * up, stays N game frames after the map is up, saves sweep/<map>.ppm and logs
 * "sweep: <map> ok". A map not up within 1800 frames is logged as "timeout";
 * a battle (the map starts one, or one begins while it is up) counts as a pass
 * ("ok (battle on entry)") and is left by raising BATTLE_SEQ_END, the flag the
 * game sets itself when BattleMain finishes: seq_battleMain then runs its normal
 * exit (battle_exit, back to SEQ_GAME). Only the victory screens are skipped. */
#define SWEEP_MAX 512
static char (*s_sweep_maps)[16];
static u8* s_sweep_story;
static const char* s_sweep_warp; /* the map the warp goes to (a stg stage's host map) */
static int s_sweep_n, s_sweep_i, s_sweep_frames, s_sweep_wait, s_sweep_state = -1;

static void sweep_read(void) {
    s_sweep_state = 0;
    s_sweep_frames = plat_switch_int("sweep", 0);
    if (s_sweep_frames <= 0) {
        return;
    }
    /* the game's map names for the sweep: stg battle-background hosts (test_gallery.c) */
    plat_sweep_register_entry_hook(tst_stage_host);
    size_t text_n = 0;
    char* text = plat_sd_read_file("sweep.txt", &text_n); /* through the SD service: core 0 makes no SD call */
    if (text == NULL) {
        PLOG_E("sweep: no sweep.txt");
        return;
    }
    static char maps[SWEEP_MAX][16];
    static u8 story[SWEEP_MAX];
    s_sweep_maps = maps;
    s_sweep_story = story;
    char line[64];
    for (char* p = text; s_sweep_n < SWEEP_MAX && *p != '\0';) {
        char* nl = strchr(p, '\n');
        size_t ll = nl != NULL ? (size_t)(nl - p) : strlen(p);
        if (ll >= sizeof(line)) {
            ll = sizeof(line) - 1;
        }
        memcpy(line, p, ll);
        line[ll] = '\0';
        p = nl != NULL ? nl + 1 : p + strlen(p);
        char name[16];
        int st = 0;
        int got = sscanf(line, "%15s %d", name, &st);
        if (got < 1 || name[0] == '#') {
            continue;
        }
        snprintf(maps[s_sweep_n], sizeof(maps[0]), "%s", name);
        story[s_sweep_n] = (u8)(got == 2 && st >= 0 && st <= 255 ? st : 0);
        s_sweep_n++;
    }
    free(text);
    plat_sd_mkdir("sweep");
    PLOG_I("sweep: %d maps, %d frames each", s_sweep_n, s_sweep_frames);
    s_sweep_state = s_sweep_n > 0 ? 1 : 0;
}

static void sweep_frame(void) {
    if (s_sweep_state < 0) {
        sweep_read();
    }
    if (s_sweep_state <= 0 || gp == NULL) {
        return;
    }
    int seq = seqGetSeq();
    if (s_sweep_state == 1) { /* start the next map */
        if (s_sweep_i >= s_sweep_n) {
            PLOG_I("sweep: done");
            s_sweep_state = 0;
            return;
        }
        s_sweep_warp = s_sweep_maps[s_sweep_i];
        if (mapDataPtr(s_sweep_warp) == NULL) {
            /* names that are no map-change targets (the registered sweep entry hook) are entered through a battle on a host map */
            int story = s_sweep_story[s_sweep_i];
            s_sweep_warp = plat_sweep_entry_host(s_sweep_maps[s_sweep_i], &story);
            if (s_sweep_warp == NULL) {
                PLOG_I("sweep: %s not in the map table", s_sweep_maps[s_sweep_i]);
                s_sweep_i++;
                return;
            }
            s_sweep_story[s_sweep_i] = (u8)story;
        }
        if (seq == SEQ_TITLE || (s_sweep_i == 0 && seq == SEQ_GAME)) {
            warp_new_game(s_sweep_story[s_sweep_i], plat_switch_int("party", 1));
        } else if (seq == SEQ_GAME) {
            swByteSet(0, s_sweep_story[s_sweep_i]);
        } else {
            return; /* the logos, a map change in progress */
        }
        PLOG_I("sweep: %s (story %d) at frame %u", s_sweep_maps[s_sweep_i], s_sweep_story[s_sweep_i], (unsigned)s_frames);
        /* A normal map exit leaves the system level at 0. The previous map's event may still hold a level
         * (hei_12 sets 2), which would start the next map's entry event stopped and hang SEQ_MAP_CHANGE. */
        if (_mariostSystemLevel != 0 || (gp->systemLevelFlags & 0xF) != 0) {
            PLOG_I("sweep: reset system level %d -> 0", (int)_mariostSystemLevel);
            marioStSystemLevel(0);
        }
        /* Likewise the previous map may have left Mario as Peach or Bowser, or in the second pose set (a
         * Peach scene's event sets it): Peach has no third pose group, so marioPreDisp would run
         * animPoseMain(-1) on the memory in front of the pose table (NULL vertex buffers, writes at 0x0 to 0x13C,
         * a fault on hardware; the GameCube writes into the heap there). A real play-through never leaves a map
         * in this state; peach.c clears the same two flags on the next pose change. */
        {
            MarioWork* mw = marioGetPtr();
            if (mw->characterId != 0 || (mw->dispFlags & (MARIO_DISP_FLAG_ANIM_SET_2 | MARIO_DISP_FLAG_POSE_UNUSED))) {
                PLOG_I("sweep: reset character %d (dispFlags %x) to Mario", (int)mw->characterId, (unsigned)mw->dispFlags);
                mw->dispFlags &= ~(MARIO_DISP_FLAG_ANIM_SET_2 | MARIO_DISP_FLAG_POSE_UNUSED);
                if (mw->characterId != 0) {
                    marioSetCharMode(0);
                }
            }
        }
        seqSetSeq(SEQ_MAP_CHANGE, s_sweep_warp, NULL);
        s_sweep_state = 2;
        s_sweep_wait = 0;
        return;
    }
    s_sweep_wait++;
    const char* map = s_sweep_warp;
    if (s_sweep_state == 6) { /* a stg stage's battle (test_gallery.c) */
        int r = tst_stage_poll(s_sweep_frames);
        if (r == 1) {
            char path[48];
            snprintf(path, sizeof(path), "sweep/%s.ppm", s_sweep_maps[s_sweep_i]);
            plat_gpu_shot(path);
            PLOG_I("sweep: %s ok (battle stage)", s_sweep_maps[s_sweep_i]);
        } else if (r == 2) {
            s_sweep_i++;
            s_sweep_state = 1;
        } else if (r < 0) {
            PLOG_W("sweep: %s timeout (battle stage, sequence %d)", s_sweep_maps[s_sweep_i], seq);
            s_sweep_state = 4;
            s_sweep_wait = 0;
        }
        return;
    }
    if (seq == SEQ_BATTLE && (s_sweep_state == 2 || s_sweep_state == 3 || s_sweep_state == 4)) {
        PLOG_I("sweep: %s ok (battle on entry)", map);
        s_sweep_state = 5;
        s_sweep_wait = 0;
        return;
    }
    if (s_sweep_state == 5) { /* in a battle: end it the way the game does, once it is set up */
        if (seq == SEQ_GAME) {
            s_sweep_i++;
            s_sweep_state = 1;
        } else if (seq == SEQ_BATTLE && _battleWorkPointer != NULL && gp->inBattle && s_sweep_wait > 20) {
            _battleWorkPointer->flags |= BATTLE_SEQ_END;
        } else if (s_sweep_wait > 900) {
            PLOG_W("sweep: stopped after %s (battle did not end, sequence %d)", map, seq);
            s_sweep_state = 0;
        }
        return;
    }
    if (s_sweep_state == 2) { /* waiting for the map */
        if (seq == SEQ_GAME && strcmp(gp->currentMapName, map) == 0) {
            s_sweep_state = 3;
            s_sweep_wait = 0;
        } else if (s_sweep_wait > 1800) {
            PLOG_W("sweep: %s timeout (sequence %d, map %s)", map, seq, gp->currentMapName);
            s_sweep_state = 4;
        }
        return;
    }
    if (s_sweep_state == 3 && s_sweep_wait == s_sweep_frames && s_sweep_warp != s_sweep_maps[s_sweep_i]) {
        s_sweep_state = tst_stage_start(s_sweep_maps[s_sweep_i]) ? 6 : 4;
        s_sweep_wait = 0;
        return;
    }
    if (s_sweep_state == 3 && s_sweep_wait == s_sweep_frames) {
        char path[48];
        snprintf(path, sizeof(path), "sweep/%s.ppm", map);
        plat_gpu_shot(path);
        PLOG_I("sweep: %s ok (sequence %d, map %s)", map, seq, gp->currentMapName);
        s_sweep_state = 4;
    }
    if (s_sweep_state == 4) { /* leave for the next map from the field */
        if (seq == SEQ_GAME) {
            s_sweep_i++;
            s_sweep_state = 1;
        } else if (s_sweep_wait > s_sweep_frames + 1800) {
            PLOG_W("sweep: stopped after %s (sequence %d)", map, seq);
            s_sweep_state = 0;
        }
    }
}

u32 port_game_frame(void) {
    return s_frames;
}

/* Game-time budget per game frame: the retrace callback (vblank thread) takes
 * game time from it, the game thread refills it once per frame. Below 30 fps
 * (25 fps in PAL's 50 Hz mode) a frame then advances the game clocks by at
 * most 2 retraces: 33 ms at 60 Hz, 2 x 20 ms at 50 Hz. */
#define GAME_TIME_BUDGET_MS (vi_50hz() ? 40 : 33)
static u32 s_time_budget = 0;

OSTime port_game_time_take(OSTime want) {
    u32 left = __atomic_load_n(&s_time_budget, __ATOMIC_RELAXED);
    u32 take;
    do {
        take = want < (OSTime)left ? (u32)want : left;
    } while (!__atomic_compare_exchange_n(&s_time_budget, &left, left - take, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return take;
}

void port_debug_frame(u32 frame);
void port_card_frame(u32 frame);
void plat_memory_log(const char* when);

static void frame_hooks(void) {
    s_frames++;
    /* determ: exactly one retrace of game time per frame */
    u32 budget = tst_determ() ? (u32)tst_retrace_ticks() : (u32)OSMillisecondsToTicks(GAME_TIME_BUDGET_MS);
    __atomic_store_n(&s_time_budget, budget, __ATOMIC_RELAXED);
    port_debug_frame(s_frames);
    tst_frame(s_frames);
    port_card_frame(s_frames); /* determ: modelled card completions */
    plat_input_frame();
    plat_set_phase("frame hooks");
    if (!plat_frame_poll()) {
        plat_shutdown();
    }
    char map[16], bero[16];
    if (plat_take_warp_request(map, sizeof(map), bero, sizeof(bero))) {
        warp_to(map, bero);
    }
    warp_switch();
    sweep_frame();
    if (port_lp_active()) {
        int seq = gp != NULL ? seqGetSeq() : SEQ_UNDEFINED;
        int ready = (seq == SEQ_BATTLE && _battleWorkPointer != NULL && gp->inBattle);
        port_lp_frame(s_frames, seq, ready, gp != NULL ? gp->currentMapName : NULL,
                      s_frame_start ? plat_ticks() - s_frame_start : 0);
    }
    fastboot_frame(s_sweep_state > 1 || s_warp_switch_done);
    if (s_frames % 1800 == 0) {
        plat_memory_log("now");
    }
    PlatHudStats st;
    memset(&st, 0, sizeof(st));
    PlatGpuStats g;
    plat_gpu_stats(&g);
    u64 now = plat_ticks();
    st.frames = s_frames;
    st.frame_us = s_frame_start ? (u32)((now - s_frame_start) / (PLAT_TICKS_PER_SEC / 1000000u)) : 0;
    if (st.frame_us > 2000000u) {
        PLOG_W("slow frame %lu: %lu ms (map %s)", (unsigned long)s_frames, (unsigned long)(st.frame_us / 1000u),
               gp != NULL ? gp->currentMapName : "-");
    }
    st.render_us = g.render_us;
    st.gpu_us = g.gpu_us;
    st.draws = g.draws;
    st.vertices = g.vertices;
    if (gp != NULL) {
        memcpy(st.map, gp->currentMapName, sizeof(st.map) - 1);
        st.map[sizeof(st.map) - 1] = '\0';
    }
    plat_hud_frame(&st);
}

static void wait_retrace_raw(void) {
    u32 target = s_retrace_count;
    BOOL level = OSDisableInterrupts();
    while (s_retrace_count == target) {
        OSSleepThread(&s_retrace_queue);
    }
    OSRestoreInterrupts(level);
}

static void wait_retrace(void) {
    if (!port_lp_active()) {
        wait_retrace_raw();
        return;
    }
    u64 t0 = plat_ticks();
    wait_retrace_raw();
    port_lp_retrace(plat_ticks() - t0);
}

/* determ (test mode): DVD manager reads queued this frame finish first, then
 * the frame's one retrace runs the game's callbacks with a virtual count and
 * the frame's 1/60 s (1/50 s) of sound interrupts run. */
static void determ_retrace(void) {
    static u32 s_audio_acc;
    int waits = 0;
    while (tst_dvd_busy() && waits < 600) {
        wait_retrace();
        waits++;
    }
    if (waits == 600) {
        PLOG_W("determ: DVD manager still busy after 600 retraces");
    }
    u32 count = tst_vretrace();
    port_irq_enter();
    if (s_pre != NULL) {
        s_pre(count - 1);
    }
    if (s_post != NULL) {
        s_post(count);
    }
    port_irq_leave();
    u32 fps = VIGetTvFormat() == VI_PAL ? 50 : 60;
    int quanta = 0;
    s_audio_acc += 32000; /* samples per second, 160 per quantum */
    while (s_audio_acc >= fps * 160) {
        s_audio_acc -= fps * 160;
        quanta++;
    }
    port_ai_pump(quanta);
}

/* Switch dvdsettle=1 (outside determ): while a map change or a battle start loads, a frame does not end until
 * the DVD manager's queued reads have finished (up to SETTLE_MAX_MS), as determ's frames do. The DVD manager
 * thread only runs while the game thread sleeps, so with one slow frame per read the load took one frame per read
 * (the GameCube's frames are short and its drive slow: reads overlapped frames; a determ run of the same load
 * takes the frame count the game's own logic needs). The wait runs only in the loading sequences, so a read
 * queued in the field (music, streaming) never stalls a frame. */
#define SETTLE_MAX_MS 250
static int s_settle = -1;

static void dvd_settle(void) {
    if (s_settle < 0) {
        s_settle = plat_switch_has("dvdsettle");
    }
    if (!s_settle || gp == NULL) {
        return;
    }
    int seq = seqGetSeq();
    if ((seq != SEQ_MAP_CHANGE && seq != SEQ_BATTLE) || !tst_dvd_busy()) {
        return;
    }
    u64 t0 = plat_ticks();
    u64 limit = t0 + (u64)SETTLE_MAX_MS * (PLAT_TICKS_PER_SEC / 1000u);
    while (tst_dvd_busy() && plat_ticks() < limit) {
        OSYieldThread(); /* the DVD manager thread runs; returns at once while it waits for the file thread */
        if (tst_dvd_busy()) {
            plat_thread_sleep_ns(200000);
        }
    }
    if (port_lp_active()) {
        port_lp_retrace(plat_ticks() - t0);
    }
}

void VIWaitForRetrace(void) {
    frame_hooks();
    wait_retrace();
    if (tst_determ()) {
        determ_retrace();
    } else {
        dvd_settle();
    }
    s_frame_start = plat_ticks();
    plat_set_phase("game frame");
}
