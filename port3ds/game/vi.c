/* Dolphin VI on the 3DS: retrace timing and the per-frame hooks.
 *
 * The top screen's vertical blank (59.83 Hz) stands in for the GameCube's
 * 60 Hz retrace: plat/vblank.c calls retrace() on its thread, which runs the
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
#include <string.h>
#include <sys/stat.h>

#include "data/mapdata.h"
#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "event.h"
#include "pmario_sound.h"
#include "mario/mario_party.h"
#include "mario/mariost.h"
#include "memory.h"
#include "plat/gpu.h"
#include "plat/hud.h"
#include "plat/input.h"
#include "plat/plat.h"
#include "port3ds/os_internal.h"
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
    if (s_pre != NULL) {
        s_pre(s_retrace_count);
    }
    s_retrace_count++;
    if (s_post != NULL) {
        s_post(s_retrace_count);
    }
    port_irq_leave();
    OSWakeupThread(&s_retrace_queue);
}

void VIInit(void) {
    static int ready;
    if (ready) {
        return;
    }
    ready = 1;
    OSInitThreadQueue(&s_retrace_queue);
    plat_vblank_start(retrace);
}

void VIConfigure(const GXRenderModeObj* rm) { (void)rm; }
void VIFlush(void) {}
void VISetNextFrameBuffer(void* fb) { s_next_fb = fb; }
void VISetBlack(BOOL black) { s_black = black; }
u32 VIGetRetraceCount(void) { return s_retrace_count; }
u32 VIGetNextField(void) { return 1; }
/* No component cable: the progressive-scan prompt never shows. */
u32 VIGetDTVStatus(void) { return 0; }

u32 VIGetTvFormat(void) {
#if VERSION == VERSION_EU
    return VI_PAL;
#else
    return VI_NTSC;
#endif
}

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

static void warp_to(const char* map, const char* bero) {
    if (seqGetSeq() != SEQ_GAME) {
        warp_new_game(plat_switch_int("story", 0), plat_switch_int("party", 1));
    }
    PLOG_I("warp: %s %s", map, bero);
    seqSetSeq(SEQ_MAP_CHANGE, map, bero[0] ? bero : NULL);
}

/* Switch map=<map>[,<bero>] (tests): once the title or the opening's map
 * is up, start a new game there (story=N, party=N as for the menu's warp). */
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
        PLOG_I("warp: %s %s (switch)", map, bero);
        seqSetSeq(SEQ_MAP_CHANGE, map, bero[0] ? bero : NULL);
        state = 2;
    }
}

/* Switch sweep=N (tests, tools/port3ds/map_sweep.py): visits each map listed
 * in sweep.txt ("<map> [<story>]" per line) once the title or the opening is
 * up, stays N game frames after the map is up, saves sweep/<map>.ppm and logs
 * "sweep: <map> ok". A map not up within 1800 frames is logged as "timeout";
 * a sequence that cannot be left (a battle the map starts) stops the sweep. */
#define SWEEP_MAX 512
static char (*s_sweep_maps)[16];
static u8* s_sweep_story;
static int s_sweep_n, s_sweep_i, s_sweep_frames, s_sweep_wait, s_sweep_state = -1;

static void sweep_read(void) {
    s_sweep_state = 0;
    s_sweep_frames = plat_switch_int("sweep", 0);
    if (s_sweep_frames <= 0) {
        return;
    }
    FILE* f = fopen("sweep.txt", "r");
    if (f == NULL) {
        PLOG_E("sweep: no sweep.txt");
        return;
    }
    static char maps[SWEEP_MAX][16];
    static u8 story[SWEEP_MAX];
    s_sweep_maps = maps;
    s_sweep_story = story;
    char line[64];
    while (s_sweep_n < SWEEP_MAX && fgets(line, sizeof(line), f) != NULL) {
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
    fclose(f);
    mkdir("sweep", 0777);
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
        if (mapDataPtr(s_sweep_maps[s_sweep_i]) == NULL) {
            /* seq_mapchange would read through the NULL (the storybook's stg maps) */
            PLOG_I("sweep: %s not in the map table", s_sweep_maps[s_sweep_i]);
            s_sweep_i++;
            return;
        }
        if (seq == SEQ_TITLE || (s_sweep_i == 0 && seq == SEQ_GAME)) {
            warp_new_game(s_sweep_story[s_sweep_i], plat_switch_int("party", 1));
        } else if (seq == SEQ_GAME) {
            swByteSet(0, s_sweep_story[s_sweep_i]);
        } else {
            return; /* the logos, a map change in progress */
        }
        PLOG_I("sweep: %s (story %d)", s_sweep_maps[s_sweep_i], s_sweep_story[s_sweep_i]);
        seqSetSeq(SEQ_MAP_CHANGE, s_sweep_maps[s_sweep_i], NULL);
        s_sweep_state = 2;
        s_sweep_wait = 0;
        return;
    }
    s_sweep_wait++;
    const char* map = s_sweep_maps[s_sweep_i];
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

void port_debug_frame(u32 frame);
void plat_memory_log(const char* when);

static void frame_hooks(void) {
    s_frames++;
    port_debug_frame(s_frames);
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

void VIWaitForRetrace(void) {
    frame_hooks();
    u32 target = s_retrace_count;
    BOOL level = OSDisableInterrupts();
    while (s_retrace_count == target) {
        OSSleepThread(&s_retrace_queue);
    }
    OSRestoreInterrupts(level);
    s_frame_start = plat_ticks();
    plat_set_phase("game frame");
}
