/* The pause look of the player Options menu: the game is paused, so the top screen shows its last
 * frame, greyed and darker, and the sound is muted (plat/audio.c).
 *
 * The render thread has nothing queued while the game thread is paused (plat_gpu_sync), so the
 * frame buffer that was shown last can be rewritten on the CPU and swapped in. With the 3D slider
 * up both eyes get the grey frame. Nothing needs undoing on close: the game's next frame is drawn
 * and swapped as usual and replaces it.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "plat/audio.h"
#include "plat/gpu.h"
#include "plat_internal.h"

static u8* s_grey_fb; /* the buffer shown while frozen, for the test dump */
static u16 s_grey_w, s_grey_h;
static int s_grey_bpp;

static void grey_buffer(u8* fb, u16 w, u16 h, int bpp) {
    for (u32 i = 0, n = (u32)w * h; i < n; i++) {
        u8* p = fb + (size_t)i * (size_t)bpp;
        u32 avg = ((u32)p[0] + p[1] + p[2]) / 3;
        u8 v = (u8)(avg * 5 / 8); /* grey, and darker */
        p[0] = p[1] = p[2] = v;
    }
}

/* The grey itself. The caller has made sure that the render thread and the GPU are idle. */
static void pause_grey_idle(void) {
    gspWaitForVBlank();
    gspWaitForVBlank(); /* the last frame's transfer has landed and been shown */
    GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(GFX_TOP);
    int bpp = fmt == GSP_BGR8_OES ? 3 : fmt == GSP_RGBA8_OES ? 4 : 0;
    if (bpp == 0) {
        return;
    }
    int eyes = gfxIs3D() ? 2 : 1;
    for (int e = 0; e < eyes; e++) {
        u16 w, h;
        u8* fb = gfxGetFramebuffer(GFX_TOP, e ? GFX_RIGHT : GFX_LEFT, &w, &h);
        if (fb == NULL) {
            continue;
        }
        grey_buffer(fb, w, h, bpp);
        if (e == 0) {
            s_grey_fb = fb;
            s_grey_w = w;
            s_grey_h = h;
            s_grey_bpp = bpp;
        }
    }
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, gfxIs3D());
    PLOG_I("pause: top screen greyed (%d eye%s)", eyes, eyes > 1 ? "s" : "");
}

void plat_pause_grey(void) {
    plat_gpu_sync();
    pause_grey_idle();
}

void plat_pause_release(void) { s_grey_fb = NULL; }

/* An intentional pause of the game thread (a menu, a devupdate transfer): the watchdog is held, the
 * top screen is greyed and the sound muted (menus), and the time spent is added up so the game's frame
 * timing can leave it out (plat_paused_ticks). */
static uint64_t s_pause_t0;
static volatile uint64_t s_paused_total;
uint64_t plat_paused_ticks(void) { return s_paused_total; }
void plat_pause_add_ticks(uint64_t ticks) { s_paused_total += ticks; }

void plat_pause_begin(void) {
    plat_wd_hold(1);
    plat_audio_set_muted(1);
    plat_pause_grey();
    s_pause_t0 = svcGetSystemTick();
}

/* The updater's pause, from its server thread. idle: the game thread is parked at a frame boundary after its own
 * plat_gpu_sync, so the GPU is idle and the top screen is greyed without another sync. Not idle (the game
 * thread did not get there: a long load, or a hung GPU): no sync, no grey. plat_gpu_sync waits for the GPU
 * without a limit and would stop the update behind a hung one. */
void plat_pause_begin_updater(int idle) {
    plat_wd_hold(1);
    plat_audio_set_muted(1);
    if (idle) {
        pause_grey_idle();
    } else {
        PLOG_W("pause: the game thread is not at a frame boundary, the top screen is not greyed");
    }
    s_pause_t0 = svcGetSystemTick();
}

void plat_pause_end(void) {
    plat_pause_release();
    plat_audio_set_muted(0);
    plat_pause_add_ticks(svcGetSystemTick() - s_pause_t0);
    plat_wd_hold(0);
}

/* Test aid (shotbottom): the frozen top screen as shots/pNNNNN.ppm. */
void plat_pause_dump(int tick) {
    if (s_grey_fb == NULL) {
        return;
    }
    char path[48];
    snprintf(path, sizeof(path), "shots/p%05d.ppm", tick);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)s_grey_h, (unsigned)s_grey_w);
    for (unsigned y = 0; y < s_grey_w; y++) {
        for (unsigned x = 0; x < s_grey_h; x++) {
            const u8* px = s_grey_fb + ((size_t)x * s_grey_w + (s_grey_w - 1 - y)) * (size_t)s_grey_bpp;
            fputc(px[2], f);
            fputc(px[1], f);
            fputc(px[0], f);
        }
    }
    fclose(f);
}
