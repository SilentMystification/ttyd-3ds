/* Stereoscopic 3D: render once, reproject the second view from depth.
 *
 * The owner's rule (docs/PORT_3DS_GPU_OFFLOAD.md, "Stereo 3D"): the 3D scene
 * is never rendered twice. A stereo frame replays the record's 3D part into
 * the EFB once; this file then
 *  1. copies the EFB colour (VRAM) into a texture in linear memory (one
 *     GX_TextureCopy of the shown rows, tiled as it is);
 *  2. samples the EFB depth buffer, read as an RGBA8 texture, into a small
 *     render target (4x4 samples per grid cell) and transfers it to linear
 *     memory; the CPU reads it on the next frame (no stall), takes the
 *     minimum depth per cell and turns it into a disparity;
 *  3. per eye, draws the colour snapshot back into the EFB through a grid
 *     mesh whose vertices are shifted horizontally by -/+ disparity/2.
 * gpu.c replays the 2D tail of the record on top of each eye's warped image
 * and presents the EFB into that eye's screen target.
 *
 * Depth to disparity. GX perspective depth is d = f (D - n) / ((f - n) D)
 * for eye distance D, so 1/D is linear in d: 1/D = 1/n - d (1/n - 1/f). The
 * usual disparity P(D) = sep (1 - Dc/D) (Dc the convergence distance) is
 * then, with f >> n,
 *      P(d) = sep * (d - dc) / (1 - dc)
 * with dc the convergence's depth value: no near or far plane is needed.
 * P > 0 is behind the screen (uncrossed), P = sep at infinity; the crossed
 * (pop-out) side is clamped at -sep (Star3D's "safe" budget, an idea only).
 *
 * Switches: stereoforce=<0..1> (simulates the slider), stereosep=<px> (total
 * parallax at infinity at the full slider, top-screen pixels, default 6),
 * stereoconv=<percentile> (convergence: that percentile of the grid's
 * depths, default 50 = median), stereogrid=<W>x<H> (default 40x24),
 * stereostats=<N> (cost log every N frames), scrshot (mono shots also save
 * the screen target). */
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/gpu.h"
#include "plat/options.h"
#include "plat_internal.h"
#include "stereo.h"

#define TARGET_W 512
#define TARGET_H 1024
#define REGION_W 480
#define REGION_H 800
#define EFB_W 608
#define EFB_H 480
#define SS 4 /* depth samples per cell side */
#define GRID_MAX_W 80
#define GRID_MAX_H 48

static StereoInit s_in;
static int s_ready;  /* resources made */
static int s_failed; /* could not make them: always mono */
static int s_gw = 40, s_gh = 24;
static int s_sw, s_sh; /* depth sample target size */
static float s_force = -1.0f;
static float s_sep_px = 6.0f;
static int s_conv_pct = 50;
static int s_stats_every;
static int s_scrshot;

static C3D_RenderTarget* s_right;
static C3D_Tex s_snap;      /* EFB colour snapshot, linear memory, tiled RGBA8 512x1024 */
static C3D_Tex s_depth_tex; /* the EFB depth buffer seen as an RGBA8 texture */
static C3D_RenderTarget* s_dtarget;
static u32* s_dlin; /* depth samples, linear rows (s_sw x s_sh) */
static int s_dvalid; /* s_dlin holds the last stereo frame's samples */
static PlatGpuVertex* s_mesh[2][2]; /* [buffer][eye] */
static int s_mesh_buf;
static u16* s_idx;
static u32 s_nidx, s_nverts;
static float s_cell_d[GRID_MAX_W * GRID_MAX_H];
static float s_base[(GRID_MAX_W + 1) * (GRID_MAX_H + 1)][2]; /* unshifted clip x, y per vertex */
static float s_dclip[2];                                     /* clip x, y per EFB pixel of shift */
static float s_dc = -1.0f; /* smoothed convergence depth */
static float s_slider;

/* stats */
static u32 s_n_st, s_n_mono, s_prep_us_sum;
static u64 s_render_st, s_render_mono, s_gpu_st, s_gpu_mono, s_tail_sum, s_tailz_sum;
static u32 s_prep_us;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void stereo_init(const StereoInit* in) {
    s_in = *in;
    char buf[32];
    if (plat_switch_value("stereoforce", buf, sizeof(buf))) {
        s_force = clampf(strtof(buf, NULL), 0.0f, 1.0f);
    }
    if (plat_switch_value("stereosep", buf, sizeof(buf))) {
        s_sep_px = clampf(strtof(buf, NULL), 0.0f, 20.0f);
    }
    s_conv_pct = plat_switch_int("stereoconv", 50);
    if (s_conv_pct < 1) s_conv_pct = 1;
    if (s_conv_pct > 99) s_conv_pct = 99;
    if (plat_switch_value("stereogrid", buf, sizeof(buf))) {
        int w = 0, h = 0;
        if (sscanf(buf, "%dx%d", &w, &h) == 2 && w >= 2 && h >= 2 && w <= GRID_MAX_W && h <= GRID_MAX_H) {
            s_gw = w;
            s_gh = h;
        }
    }
    s_stats_every = plat_switch_int("stereostats", 0);
    s_scrshot = plat_switch_has("scrshot");
    if (s_force >= 0.0f || s_stats_every) {
        PLOG_I("stereo: force %.2f, sep %.1f px, convergence at the %d%% depth, grid %dx%d", (double)s_force,
               (double)s_sep_px, s_conv_pct, s_gw, s_gh);
    }
}

int stereo_wanted(void) {
    if (s_failed || !opt_get_stereo3d()) {
        return 0;
    }
    s_slider = s_force >= 0.0f ? s_force : osGet3DSliderState();
    return s_slider > 0.0f && s_sep_px > 0.0f;
}

static int make_resources(void) {
    s_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB8, -1);
    if (s_right == NULL) {
        PLOG_W("stereo: no VRAM for the right eye's screen target: mono");
        return 0;
    }
    C3D_RenderTargetSetOutput(s_right, GFX_TOP, GFX_RIGHT, s_in.screen_flags);
    if (!C3D_TexInit(&s_snap, TARGET_W, TARGET_H, GPU_RGBA8)) {
        PLOG_W("stereo: no linear memory for the %d KB colour snapshot: mono", TARGET_W * TARGET_H * 4 / 1024);
        return 0;
    }
    memset(s_snap.data, 0, s_snap.size);
    GSPGPU_FlushDataCache(s_snap.data, s_snap.size);
    C3D_TexSetFilter(&s_snap, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_snap, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    s_sw = (s_gw * SS + 7) & ~7;
    s_sh = (s_gh * SS + 7) & ~7;
    if (s_sw < 64) s_sw = 64; /* display transfers want at least 64 */
    if (s_sh < 64) s_sh = 64;
    s_dtarget = C3D_RenderTargetCreate(s_sw, s_sh, GPU_RB_RGBA8, -1);
    s_dlin = linearAlloc((u32)(s_sw * s_sh * 4));
    s_nverts = (u32)((s_gw + 1) * (s_gh + 1));
    s_nidx = (u32)(s_gw * s_gh * 6);
    s_idx = linearAlloc(s_nidx * sizeof(u16));
    PlatGpuVertex* mesh = linearAlloc(4 * s_nverts * sizeof(PlatGpuVertex));
    for (int b = 0; b < 2 && mesh != NULL; b++) {
        for (int e = 0; e < 2; e++) {
            s_mesh[b][e] = mesh + (2 * b + e) * s_nverts;
        }
    }
    if (s_dtarget == NULL || s_dlin == NULL || s_idx == NULL || mesh == NULL) {
        PLOG_W("stereo: no memory for the depth grid or the meshes: mono");
        return 0;
    }
    u32 k = 0;
    for (int j = 0; j < s_gh; j++) {
        for (int i = 0; i < s_gw; i++) {
            u16 a = (u16)(j * (s_gw + 1) + i), b = (u16)(a + 1), c = (u16)(a + s_gw + 1), d = (u16)(c + 1);
            s_idx[k++] = a; s_idx[k++] = b; s_idx[k++] = c;
            s_idx[k++] = b; s_idx[k++] = d; s_idx[k++] = c;
        }
    }
    GSPGPU_FlushDataCache(s_idx, s_nidx * sizeof(u16));
    /* the meshes' fixed parts: texture coordinates (the unshifted EFB point), clip position without a shift;
     * EFB -> target is affine, so a shift of x EFB pixels moves the clip position by x * s_dclip */
    float ox, oy, ax, ay;
    s_in.efb_point(0, 0, &ox, &oy);
    s_in.efb_point(1, 0, &ax, &ay);
    s_dclip[0] = 2.0f * (ax - ox) / REGION_W;
    s_dclip[1] = 2.0f * (ay - oy) / REGION_H;
    for (int j = 0; j <= s_gh; j++) {
        for (int i = 0; i <= s_gw; i++) {
            int k = j * (s_gw + 1) + i;
            float tx, ty;
            s_in.efb_point((float)i * EFB_W / s_gw, (float)j * EFB_H / s_gh, &tx, &ty);
            s_base[k][0] = 2.0f * tx / REGION_W - 1.0f;
            s_base[k][1] = 2.0f * ty / REGION_H - 1.0f;
            for (int b = 0; b < 2; b++) {
                for (int e = 0; e < 2; e++) {
                    PlatGpuVertex* o = &s_mesh[b][e][k];
                    memset(o, 0, sizeof(*o));
                    o->pos[0] = s_base[k][0];
                    o->pos[1] = s_base[k][1];
                    o->pos[2] = -0.5f;
                    o->pos[3] = 1.0f;
                    o->color[0] = o->color[1] = o->color[2] = o->color[3] = 255;
                    o->tc0[0] = tx / TARGET_W;
                    o->tc0[1] = ty / TARGET_H;
                }
            }
        }
    }
    memset(&s_depth_tex, 0, sizeof(s_depth_tex));
    s_depth_tex.data = s_in.efb->frameBuf.depthBuf;
    s_depth_tex.fmt = GPU_RGBA8; /* D24S8 bytes: A = depth bits 0-7, B = 8-15, G = 16-23, R = stencil */
    s_depth_tex.width = TARGET_W;
    s_depth_tex.height = TARGET_H;
    s_depth_tex.size = TARGET_W * TARGET_H * 4;
    s_depth_tex.param = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
                        GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
    PLOG_I("stereo: on (grid %dx%d, depth samples %dx%d); VRAM free %lu KB, linear free %lu KB", s_gw, s_gh, s_sw,
           s_sh, (unsigned long)(vramSpaceFree() / 1024), (unsigned long)(linearSpaceFree() / 1024));
    return 1;
}

int stereo_begin(void) {
    if (s_failed) {
        return 0;
    }
    if (!s_ready) {
        if (!make_resources()) {
            s_failed = 1;
            return 0;
        }
        s_ready = 1;
        gfxSet3D(true); /* a frame without the right target still shows the left image to both eyes */
    }
    return 1;
}

static int cmp_float(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}

/* The last stereo frame's depth samples -> per-cell minimum depth; the
 * convergence depth from them. */
static void read_grid(void) {
    static float sorted[GRID_MAX_W * GRID_MAX_H];
    int n = 0;
    if (!s_dvalid) {
        for (int c = 0; c < s_gw * s_gh; c++) {
            s_cell_d[c] = 1.0f;
        }
        return;
    }
    GSPGPU_InvalidateDataCache(s_dlin, (u32)(s_sw * s_sh * 4));
    for (int cj = 0; cj < s_gh; cj++) {
        for (int ci = 0; ci < s_gw; ci++) {
            u32 m = 0xFFFFFF;
            for (int sj = 0; sj < SS; sj++) {
                int vy = cj * SS + sj; /* viewport row; linear row = s_sh - 1 - vy */
                const u32* row = s_dlin + (s_sh - 1 - vy) * s_sw;
                for (int si = 0; si < SS; si++) {
                    u32 d = row[ci * SS + si] & 0xFFFFFF;
                    if (d < m) m = d;
                }
            }
            float d = (float)m / 16777215.0f;
            s_cell_d[cj * s_gw + ci] = d;
            if (m < 0xFFFFF0) {
                sorted[n++] = d; /* cleared (sky) cells do not pick the convergence */
            }
        }
    }
    if (n == 0) {
        return; /* keep the last convergence */
    }
    qsort(sorted, (size_t)n, sizeof(float), cmp_float);
    float dc = sorted[(n - 1) * s_conv_pct / 100];
    s_dc = s_dc < 0.0f ? dc : s_dc + 0.2f * (dc - s_dc); /* eases over a few frames */
}

void stereo_prepare(void) {
    u64 t0 = svcGetSystemTick();
    read_grid();
    float dc = s_dc < 0.0f ? 1.0f : s_dc;
    /* P top-screen pixels of parallax are +-P/2 per eye, and a top-screen
     * pixel is two EFB pixels: each eye shifts by +-P EFB pixels */
    float sep_efb = s_sep_px * s_slider;
    /* the disparity at each vertex (from the nearest of the up to four cells around it: foreground edges
     * stay sharp), the same for both eyes; the image's left and right edges stay put */
    static float pv[(GRID_MAX_W + 1) * (GRID_MAX_H + 1)];
    for (int j = 0; j <= s_gh; j++) {
        for (int i = 0; i <= s_gw; i++) {
            float p = 0.0f;
            if (i > 0 && i < s_gw && dc < 1.0f) {
                int c0 = (j > 0 ? j - 1 : 0) * s_gw, c1 = (j < s_gh ? j : s_gh - 1) * s_gw;
                float d = s_cell_d[c0 + i - 1];
                if (s_cell_d[c0 + i] < d) d = s_cell_d[c0 + i];
                if (s_cell_d[c1 + i - 1] < d) d = s_cell_d[c1 + i - 1];
                if (s_cell_d[c1 + i] < d) d = s_cell_d[c1 + i];
                p = sep_efb * clampf((d - dc) / (1.0f - dc), -1.0f, 1.0f);
            }
            pv[j * (s_gw + 1) + i] = p;
        }
    }
    s_mesh_buf ^= 1;
    const float cell = (float)EFB_W / s_gw;
    for (int e = 0; e < 2; e++) {
        PlatGpuVertex* v = s_mesh[s_mesh_buf][e];
        float sign = e == 0 ? -1.0f : 1.0f; /* far points move left in the left eye */
        for (int j = 0; j <= s_gh; j++) {
            float last = -1e9f; /* the previous vertex's x, shifted */
            for (int i = 0; i <= s_gw; i++) {
                int k = j * (s_gw + 1) + i;
                float x = (float)i * cell, sx = x + sign * pv[k];
                if (sx < last + 0.5f) {
                    sx = last + 0.5f; /* never fold the mesh */
                }
                last = sx;
                v[k].pos[0] = s_base[k][0] + (sx - x) * s_dclip[0];
                v[k].pos[1] = s_base[k][1] + (sx - x) * s_dclip[1];
            }
        }
    }
    GSPGPU_FlushDataCache(s_mesh[s_mesh_buf][0], 2 * s_nverts * sizeof(PlatGpuVertex)); /* both eyes: one block */
    s_prep_us = (u32)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000u));
}

static void replace_tex0(C3D_Tex* t) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_TexBind(0, t);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_in.uloc_rot0, 1, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_in.uloc_rot1, 0, 1, 0, 0);
}

void stereo_snapshot(void) {
    /* 1. the colour: memory rows 224..1023 hold viewport rows 0..799 (the region) */
    const u32 tile_row = TARGET_W * 8 * 4;
    const u32 first = (TARGET_H - REGION_H) / 8, bytes = (REGION_H / 8) * tile_row;
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    GX_TextureCopy((u32*)((u8*)s_in.efb->frameBuf.colorBuf + first * tile_row), GX_BUFFER_DIM(bytes, 0),
                   (u32*)((u8*)s_snap.data + first * tile_row), GX_BUFFER_DIM(bytes, 0), bytes, 8);
    /* 2. the depth grid's samples: viewport pixel (i, j) of the sample target is EFB point
     * ((i + 0.5) * 608 / W', (j + 0.5) * 480 / H') with W' = gw * SS, H' = gh * SS */
    C3D_FrameDrawOn(s_dtarget);
    replace_tex0(&s_depth_tex);
    C3D_SetViewport(0, 0, (u32)s_sw, (u32)s_sh);
    C3D_SetAttrInfo(s_in.attr);
    float gw = (float)(s_gw * SS), gh = (float)(s_gh * SS);
    float cx[4] = {0, 1, 0, 1}, cy[4] = {0, 0, 1, 1};
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    for (int k = 0; k < 4; k++) {
        float tx, ty;
        s_in.efb_point(cx[k] * EFB_W, cy[k] * EFB_H, &tx, &ty);
        C3D_ImmSendAttrib(-1.0f + 2.0f * cx[k] * gw / s_sw, -1.0f + 2.0f * cy[k] * gh / s_sh, -0.5f, 1.0f);
        C3D_ImmSendAttrib(255, 255, 255, 255);
        C3D_ImmSendAttrib(tx / TARGET_W, ty / TARGET_H, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
    }
    C3D_ImmDrawEnd();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    GX_DisplayTransfer((u32*)s_dtarget->frameBuf.colorBuf, GX_BUFFER_DIM(s_sw, s_sh), s_dlin,
                       GX_BUFFER_DIM(s_sw, s_sh),
                       GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    s_dvalid = 1;
    C3D_FrameDrawOn(s_in.efb);
}

void stereo_warp(int eye) {
    C3D_FrameDrawOn(s_in.efb);
    replace_tex0(&s_snap);
    C3D_SetViewport(0, 0, REGION_W, REGION_H);
    C3D_SetAttrInfo(s_in.attr);
    static C3D_BufInfo bi;
    BufInfo_Init(&bi);
    BufInfo_Add(&bi, s_mesh[s_mesh_buf][eye], sizeof(PlatGpuVertex), 5, 0x43210);
    C3D_SetBufInfo(&bi);
    C3D_DrawElements(GPU_TRIANGLES, (int)s_nidx, C3D_UNSIGNED_SHORT, s_idx);
}

C3D_RenderTarget* stereo_eye_target(int eye) { return eye == 0 ? s_in.screen : s_right; }

void stereo_frame_done(int stereo, u32 render_us, u32 gpu_us, u32 tail_cmds, u32 tail_zwrites) {
    if (!s_stats_every) {
        return;
    }
    if (stereo) {
        s_n_st++;
        s_render_st += render_us;
        s_gpu_st += gpu_us;
        s_prep_us_sum += s_prep_us;
        s_tail_sum += tail_cmds;
        s_tailz_sum += tail_zwrites;
    } else {
        s_n_mono++;
        s_render_mono += render_us;
        s_gpu_mono += gpu_us;
    }
    if (s_n_st + s_n_mono < (u32)s_stats_every) {
        return;
    }
    u32 ns = s_n_st ? s_n_st : 1, nm = s_n_mono ? s_n_mono : 1;
    PLOG_I("stereostats: %lu stereo frames: render %lu us gpu %lu us (mesh %lu us), 2D tail %lu commands (%lu draws "
           "writing depth); %lu mono frames: render %lu us gpu %lu us; convergence depth %.5f",
           (unsigned long)s_n_st, (unsigned long)(s_render_st / ns), (unsigned long)(s_gpu_st / ns),
           (unsigned long)(s_prep_us_sum / ns), (unsigned long)(s_tail_sum / ns), (unsigned long)(s_tailz_sum / ns),
           (unsigned long)s_n_mono, (unsigned long)(s_render_mono / nm), (unsigned long)(s_gpu_mono / nm), (double)s_dc);
    s_n_st = s_n_mono = s_prep_us_sum = 0;
    s_render_st = s_render_mono = s_gpu_st = s_gpu_mono = s_tail_sum = s_tailz_sum = 0;
}

/* One screen target (240x400 RGB8, as the framebuffer) to a 400x240 PPM. */
static void shot_target(C3D_RenderTarget* t, const char* path) {
    static u8* buf;
    const u32 bytes = 240 * 400 * 3;
    if (buf == NULL) {
        buf = linearAlloc(bytes);
        if (buf == NULL) {
            return;
        }
    }
    GSPGPU_InvalidateDataCache(buf, bytes);
    C3D_SyncDisplayTransfer((u32*)t->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), (u32*)buf, GX_BUFFER_DIM(240, 400),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(buf, bytes);
    u8* out = malloc(16 + 400 * 240 * 3); /* queued to the SD service (core 1); no SD call here */
    if (out == NULL) {
        return;
    }
    size_t out_n = (size_t)sprintf((char*)out, "P6\n400 240\n255\n");
    static u8 row[400 * 3];
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 400; x++) {
            const u8* p = buf + (x * 240 + (239 - y)) * 3; /* the framebuffer's layout: columns, bottom up; BGR */
            row[x * 3 + 0] = p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[0];
        }
        memcpy(out + out_n, row, sizeof(row));
        out_n += sizeof(row);
    }
    plat_sd_write_file_own(path, out, out_n, 0);
}

void stereo_shot(const char* path, int stereo) {
    if (!stereo && !s_scrshot) {
        return;
    }
    char p[64];
    size_t n = strlen(path);
    if (n < 4 || n + 3 > sizeof(p)) {
        return;
    }
    memcpy(p, path, n - 4);
    if (stereo) {
        memcpy(p + n - 4, ".L.ppm", 7);
        shot_target(s_in.screen, p);
        memcpy(p + n - 4, ".R.ppm", 7);
        shot_target(s_right, p);
        PLOG_I("stereo shot %s: convergence depth %.5f, slider %.2f", path, (double)s_dc, (double)s_slider);
    } else {
        memcpy(p + n - 4, ".M.ppm", 7);
        shot_target(s_in.screen, p);
    }
}
