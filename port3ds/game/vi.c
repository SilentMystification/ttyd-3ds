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

#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "event.h"
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

void port_debug_frame(u32 frame);

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
    PlatHudStats st;
    memset(&st, 0, sizeof(st));
    PlatGpuStats g;
    plat_gpu_stats(&g);
    u64 now = plat_ticks();
    st.frames = s_frames;
    st.frame_us = s_frame_start ? (u32)((now - s_frame_start) / (PLAT_TICKS_PER_SEC / 1000000u)) : 0;
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
