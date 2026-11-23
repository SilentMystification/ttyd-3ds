/* GX: init, FIFO objects, framebuffer copies, synchronisation, queries.
 *
 * The EFB is the PICA200 render target (EFB size x resolution scale).
 * GXCopyDisp ends the frame: the back end shows the EFB on the top screen.
 * GXCopyTex copies an EFB rectangle into a render-target texture that later
 * draws use where the game points a GXTexObj at the copy's destination.
 */
#include <stdio.h>
#include <string.h>

#include "gx_internal.h"

#pragma GCC diagnostic ignored "-Wmissing-braces" /* the SDK's render mode tables, as written */
#include "plat/plat.h"

/* ---- Render modes (the SDK's GXFrameBuf.c data) ---- */
GXRenderModeObj GXNtsc480IntDf = {0,   640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
                                  {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
                                  {8, 8, 10, 12, 10, 8, 8}};
GXRenderModeObj GXMpal480IntDf = {8,   640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
                                  {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
                                  {8, 8, 10, 12, 10, 8, 8}};
GXRenderModeObj GXPal528IntDf = {4,   640, 528, 528, 40, 23, 640, 528, 1, 0, 0,
                                 {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
                                 {8, 8, 10, 12, 10, 8, 8}};
GXRenderModeObj GXEurgb60Hz480IntDf = {20,  640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
                                       {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
                                       {8, 8, 10, 12, 10, 8, 8}};

void GXAdjustForOverscan(const GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
    u16 hor2 = hor * 2;
    u16 ver2 = ver * 2;
    u32 verf;
    u32 mode;
    if (rmin != rmout) {
        *rmout = *rmin;
    }
    mode = rmin->viTVmode & 3;
    rmout->fbWidth = rmin->fbWidth - hor2;
    verf = (ver2 * rmin->efbHeight) / (u32)rmin->xfbHeight;
    rmout->efbHeight = rmin->efbHeight - verf;
    if (rmin->xFBmode == VI_XFBMODE_SF && mode == 0) {
        rmout->xfbHeight = rmin->xfbHeight - ver2 / 2;
    } else {
        rmout->xfbHeight = rmin->xfbHeight - ver2;
    }
    rmout->viWidth = rmin->viWidth - hor2;
    if (mode == 1) {
        rmout->viHeight = rmin->viHeight - (ver2 * 2);
    } else {
        rmout->viHeight = rmin->viHeight - ver2;
    }
    rmout->viXOrigin = rmin->viXOrigin + hor;
    rmout->viYOrigin = rmin->viYOrigin + ver;
}

/* ---- Init and FIFO objects ----
 * GX commands go straight to the front end; the FIFO the game allocates is
 * kept only so the FIFO queries answer with what the game gave. */
typedef struct {
    void* base;
    u32 size;
} PortFifo;
_Static_assert(sizeof(PortFifo) <= sizeof(GXFifoObj), "PortFifo must fit GXFifoObj");

static GXFifoObj s_fifo;
static int s_ready;

GXFifoObj* GXInit(void* base, u32 size) {
    GXInitFifoBase(&s_fifo, base, size);
    if (!s_ready) {
        s_ready = 1;
        gx_tex_init();
        plat_gpu_init();
        gx_rec_init();
    }
    /* the state is set up directly, with any translator idle; the shadow starts from it */
    gx_rec_excl_begin(GXF_READBACK);
    gx_state_init(PLAT_GPU_EFB_MAX_W, PLAT_GPU_EFB_MAX_H); /* the GameCube EFB's physical size */
    plat_gpu_frame_begin();
    gx_draw_frame_begin();
    memcpy(&gx_game, &gx, sizeof(gx));
    gx_rec_excl_end();
    PLOG_I("GXInit: FIFO %p (%lu bytes)", base, (unsigned long)size);
    return &s_fifo;
}

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    PortFifo* f = (PortFifo*)fifo;
    f->base = base;
    f->size = size;
}

void GXSetCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSetGPFifo(GXFifoObj* fifo) { (void)fifo; }
GXFifoObj* GXGetCPUFifo(void) { return &s_fifo; }
void* GXGetFifoBase(const GXFifoObj* fifo) { return ((const PortFifo*)fifo)->base; }
u32 GXGetFifoSize(const GXFifoObj* fifo) { return ((const PortFifo*)fifo)->size; }

/* The port's GX never overflows its FIFO or hangs the GP: the queries say so. */
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    *overhi = GX_FALSE;
    *underlow = GX_TRUE;
    *readIdle = GX_TRUE;
    *cmdIdle = GX_TRUE;
    *brkpt = GX_FALSE;
}

static u32 s_overflow_count;
u32 GXGetOverflowCount(void) { return s_overflow_count; }
u32 GXResetOverflowCount(void) {
    u32 v = s_overflow_count;
    s_overflow_count = 0;
    return v;
}

void GXAbortFrame(void) {}

/* ---- Performance counters (used only by DEMOInit's hang diagnosis) ---- */
void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1) {
    (void)perf0;
    (void)perf1;
}
void GXClearGPMetric(void) {}
void GXReadXfRasMetric(u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy, u32* clocks) {
    static u32 tick;
    tick++;
    *xf_wait_in = tick;
    *xf_wait_out = tick;
    *ras_busy = 0;
    *clocks = tick;
}

/* ---- Synchronisation ---- */

void GXSetDrawSync(u16 token) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx_pipe_reset();
    if (gx_rec_on()) {
        const u32 w = token;
        gx_rec_call(REC_SetDrawSync, &w, 1);
        return;
    }
    gx.draw_sync_token = token;
}

/* The token is reached once the front end has taken the commands before it;
 * the GPU work behind it is finished by GXDrawDone. With a translator, that
 * is once it has translated them: the read is a fence (the game waits on the
 * token before it reuses memory the GPU may read, as smartGarbage does). */
u16 GXReadDrawSync(void) {
    if (gx_rec_on()) {
        gx_rec_fence(GXF_SYNC);
        return gx_game.draw_sync_token;
    }
    gx_rec_sync_mark();
    return gx.draw_sync_token;
}

void GXDrawDone(void) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx_pipe_reset();
    if (gx_rec_on()) {
        gx_rec_call(REC_DrawDone, NULL, 0); /* the translator waits for the GPU */
        gx_rec_fence(GXF_DRAWDONE);
        return;
    }
    u64 t0 = GX_PERF_T();
    plat_gpu_sync();
    if (gx_perf_on > 0) {
        gx_perf.t_sync += plat_ticks() - t0;
    }
}

void GXPixModeSync(void) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx_pipe_reset();
}

/* ---- Bounding box ---- */

void GXClearBoundingBox(void) {
    GX_CLOSE_PRIMITIVE();
    if (gx_rec_on()) {
        gx_rec_call(REC_ClearBoundingBox, NULL, 0);
        return;
    }
    gx_bbox_clear();
}

void GXReadBoundingBox(u16* left, u16* top, u16* right, u16* bottom) {
    GX_CLOSE_PRIMITIVE();
    if (gx_rec_on()) {
        gx_rec_fence(GXF_READBACK); /* the box is the translator's, complete once it caught up */
    }
    u16 b[4];
    gx_bbox_read(b); /* the translator is idle here (fenced, or this thread translates) */
    *left = b[0];
    *top = b[1];
    *right = b[2];
    *bottom = b[3];
}

/* ---- Pixel peek ---- */

void GXPeekARGB(u16 x, u16 y, u32* color) {
    GX_CLOSE_PRIMITIVE();
    gx_pipe_reset();
    if (gx_rec_on()) {
        gx_rec_excl_begin(GXF_READBACK); /* the back end's record is the translator's */
        *color = plat_gpu_peek_argb(x, y);
        gx_rec_excl_end();
        return;
    }
    *color = plat_gpu_peek_argb(x, y);
}

/* ---- GXProject (the SDK's math) ---- */

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz) {
    f32 peye_x = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    f32 peye_y = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    f32 peye_z = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];
    f32 xc, yc, zc, wc;
    if (pm[0] == (f32)GX_PERSPECTIVE) {
        xc = peye_x * pm[1] + peye_z * pm[2];
        yc = peye_y * pm[3] + peye_z * pm[4];
        zc = pm[6] + peye_z * pm[5];
        wc = 1.0f / -peye_z;
    } else {
        xc = pm[2] + peye_x * pm[1];
        yc = pm[4] + peye_y * pm[3];
        zc = pm[6] + peye_z * pm[5];
        wc = 1.0f;
    }
    *sx = vp[2] / 2.0f + vp[0] + wc * xc * vp[2] / 2.0f;
    *sy = vp[3] / 2.0f + vp[1] + -wc * yc * vp[3] / 2.0f;
    *sz = vp[5] + wc * zc * (vp[5] - vp[4]);
}

/* ---- Copies ---- */

#define F2U(x) (((union { f32 f; u32 u; }){.f = (x)}).u)
/* Recorded for a translator (gx_rec.c): the call by value, then return. */
#define RECM(id, ...)                                              \
    do {                                                           \
        if (gx_rec_on()) {                                         \
            const u32 w_[] = {__VA_ARGS__};                        \
            gx_rec_call(REC_##id, w_, sizeof(w_) / sizeof(w_[0])); \
            return;                                                \
        }                                                          \
    } while (0)

void GXSetCopyClear(GXColor clear_clr, u32 clear_z) {
    GX_CLOSE_PRIMITIVE();
    RECM(SetCopyClear, (u32)clear_clr.r << 24 | (u32)clear_clr.g << 16 | (u32)clear_clr.b << 8 | clear_clr.a, clear_z);
    gx.copy_clear_color = clear_clr;
    gx.copy_clear_z = clear_z & 0xFFFFFF;
}

/* Anti-aliasing sample pattern and the vertical (deflicker) filter shape the
 * copy to the TV's interlaced frame; the port's display transfer scales the
 * EFB to the 3DS screen instead. */
void GXSetCopyFilter(GXBool aa, const u8 sample_pattern[12][2], GXBool vf, const u8 vfilter[7]) {
    (void)aa;
    (void)sample_pattern;
    (void)vf;
    (void)vfilter;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    GX_CLOSE_PRIMITIVE();
    RECM(SetDispCopySrc, left, top, wd, ht);
    gx.copy_src[0] = left;
    gx.copy_src[1] = top;
    gx.copy_src[2] = wd;
    gx.copy_src[3] = ht;
}

void GXSetDispCopyDst(u16 wd, u16 ht) {
    RECM(SetDispCopyDst, wd, ht);
    gx.copy_disp_dst[0] = wd;
    gx.copy_disp_dst[1] = ht;
}

u32 GXSetDispCopyYScale(f32 vscale) {
    if (gx_rec_on()) {
        const u32 w = F2U(vscale);
        gx_rec_call(REC_SetDispCopyYScale, &w, 1);
        gx_rec_getter(GXG_COPYSRC);
        return (u32)(gx_game.copy_src[3] * vscale);
    }
    gx.disp_yscale = vscale;
    return (u32)(gx.copy_src[3] * vscale);
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { return (f32)xfbHeight / (f32)efbHeight; }

void GXSetDispCopyGamma(GXGamma gamma) { (void)gamma; }

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    GX_CLOSE_PRIMITIVE();
    RECM(SetTexCopySrc, left, top, wd, ht);
    gx.copy_src[0] = left;
    gx.copy_src[1] = top;
    gx.copy_src[2] = wd;
    gx.copy_src[3] = ht;
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    GX_CLOSE_PRIMITIVE();
    RECM(SetTexCopyDst, wd, ht, fmt, mipmap);
    gx.copy_tex_dst[0] = wd;
    gx.copy_tex_dst[1] = ht;
    gx.copy_tex_fmt = (u8)fmt;
    gx.copy_tex_mip = mipmap;
}

static u32 clear_rgba(void) {
    GXColor c = gx.copy_clear_color;
    return (u32)c.r | (u32)c.g << 8 | (u32)c.b << 16 | (u32)c.a << 24;
}

void GXCopyTex(void* dest, GXBool clear) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx_pipe_reset();
    RECM(CopyTex, (u32)dest, clear);
    u16 w = gx.copy_tex_dst[0], h = gx.copy_tex_dst[1];
    if (w == 0 || h == 0 || gx.copy_src[2] == 0 || gx.copy_src[3] == 0) {
        return;
    }
    gx_stat_copies++;
    if (gx_draw_dumping()) {
        PLOG_I("gxdump copy %u,%u %ux%u to %p %ux%u fmt %u clear %d from %p after %lu draws", gx.copy_src[0],
               gx.copy_src[1], gx.copy_src[2], gx.copy_src[3], dest, w, h, gx.copy_tex_fmt, clear,
               __builtin_return_address(0), (unsigned long)gx_stat_draws);
    }
    u32 handle = gx_tex_copy_target(dest, w, h, gx.copy_tex_fmt);
    /* a texture at `dest` now samples the copy: the next draw looks its textures up again */
    gx.version++;
    gx_draw_state_lost();
    int half = gx.copy_tex_mip; /* mipmap flag: box-filter by 2 */
    plat_gpu_copy_tex(gx.copy_src[0], gx.copy_src[1], gx.copy_src[2], gx.copy_src[3], half, handle, w, h);
    if (clear) {
        plat_gpu_clear(gx.copy_src[0], gx.copy_src[1], gx.copy_src[2], gx.copy_src[3], clear_rgba(),
                       gx.copy_clear_z, gx.color_update, gx.z_update);
    }
}

/* Switch gxclearrgb=RRGGBB (test only: the seam test clears to a loud colour): the
 * colour of the frame clear, instead of the game's. -1 off. */
static s32 frame_clear_override(void) {
    static s32 v = -2;
    if (v == -2) {
        char buf[16];
        v = -1;
        if (plat_switch_value("gxclearrgb", buf, sizeof(buf))) {
            unsigned x = 0;
            if (sscanf(buf, "%x", &x) == 1) {
                v = (s32)(((x >> 16) & 0xFF) | (x & 0xFF00) | ((x & 0xFF) << 16)); /* RGBA8, R in the low byte */
                PLOG_I("gxclearrgb: frames clear to %06x", x & 0xFFFFFF);
            }
        }
    }
    return v;
}

/* Ends the frame: the EFB goes to the screen. */
void GXCopyDisp(void* dest, GXBool clear) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    if (gx_rec_on()) {
        gx_pipe_reset();
        gx_rec_frame();
        const u32 w[3] = {(u32)dest, clear, (u32)__builtin_return_address(0)};
        gx_rec_call(REC_CopyDisp, w, 3);
        return;
    }
    gx_rec_frame();
    gx_copy_disp(dest, clear, __builtin_return_address(0));
}

void gx_copy_disp(void* dest, GXBool clear, const void* caller) {
    if (gx_stat_draws == 0) {
        /* a frame shown with nothing drawn since the last one: logged once per caller */
        static void* seen[8];
        void* ra = (void*)caller;
        int i;
        for (i = 0; i < 8 && seen[i] != NULL && seen[i] != ra; i++) {
        }
        if (i < 8 && seen[i] == NULL) {
            seen[i] = ra;
            PLOG_W("GX: GXCopyDisp from %p with nothing drawn since the last one", ra);
        }
    }
    (void)dest;
    gx_pipe_reset();
    u64 t0 = GX_PERF_T();
    plat_gpu_frame_end(gx.copy_src[2] ? gx.copy_src[2] : 640, gx.copy_src[3] ? gx.copy_src[3] : 480);
    u64 t_present = gx_perf_on > 0 ? plat_ticks() - t0 : 0;
    gx_tex_frame_end();
    plat_gpu_frame_begin();
    gx_draw_frame_begin();
    gx_perf.t_present += t_present; /* lands in the next frame's sums (the counters were just logged and reset) */
    if (clear) {
        u32 rgba = clear_rgba();
        s32 ov = frame_clear_override();
        if (ov >= 0) {
            rgba = (u32)ov | (rgba & 0xFF000000u);
        }
        plat_gpu_clear(0, 0, gx.efb_w, gx.efb_h, gx.pixel_fmt == GX_PF_RGB8_Z24 ? (rgba | 0xFF000000u) : rgba,
                       gx.copy_clear_z, 1, 1);
    }
    if (gx_perf_on > 0 && !gx_rec_mode) {
        gx_perf_drawdone_end = plat_ticks(); /* the frame's end: the gap to the next frame's start is the retrace wait */
    }
}
