/* qtmbench: GPU tests. The GPU reads vertex buffers, index buffers,
 * textures and command lists from each memory, and writes into each memory
 * (memory fill, texture copy, display transfer, render target). Every
 * result is read back by the CPU and checked against a known pattern.
 *
 * citro3d is used for state and draws only: the harness records its own
 * command lists (GPUCMD_SetBuffer/GPUCMD_Split) and submits them with
 * GX_ProcessCommandList, without citro3d's frame queue, so each operation
 * is timed alone: CPU ticks from the submit to the GSP completion event,
 * plus GSP's own perf log (op start to IRQ, in us) when it is available.
 * Waits have a timeout, so a command GSP refuses is reported, not a hang.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "qtmbench_shbin.h"

#define W 256
#define H 256
#define RT_BYTES (W * H * 4)
#define GRID 32
#define CELL (W / GRID)
#define GRID_VERTS (GRID * GRID * 6)
#define CMD_BYTES (1u * QB_MIB)
/* Draws per timing list (quick: few, for emulators, whose software renderer is slow). */
#define R_VTX (g_opt.quick ? 4 : 64)
#define R_IDX (g_opt.quick ? 4 : 128)
#define R_TEX (g_opt.quick ? 2 : 16)
#define TIMEOUT_NS 2000000000LL

typedef struct {
    float x, y, z;
    u8 c[4];
    float u, v;
} Vtx;

#define VB_BYTES (GRID_VERTS * sizeof(Vtx))
#define IVB_BYTES (GRID * GRID * 4 * sizeof(Vtx))
#define IB_BYTES (GRID * GRID * 6 * sizeof(u16))

static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uproj = -1;
static C3D_AttrInfo s_attr;
static C3D_Tex s_tex;
static void* s_tex_orig;
static LightEvent s_ev[GSPGPU_EVENT_MAX];
static bool s_perflog, s_ready;
static int s_timeouts;
static u32* s_cmd;      /* linear: where lists are recorded */
static u8* s_stage;     /* linear 1 MiB: g_lin_aux */
static u8* s_rt_vram;   /* render target in VRAM */
static u8* s_rt_lin;    /* render target in linear (for transfers) */
static u8* s_tex_vram;  /* the texture in VRAM, for the render-target tests */
static Vtx* s_quad_lin; /* full-target textured quad, linear */
static Vtx* s_grid_lin; /* the grid (arrays) in linear */
static Vtx* s_ivb_lin;  /* the grid (indexed) vertices in linear */
static u16* s_ib_lin;
static Vtx* s_grid_cpu; /* CPU copies for placing into other memories */
static u32* s_tex_cpu;  /* tiled texture pattern */
static u32* s_img;      /* readback */
static u16* s_ib_cpu;
static Vtx* s_ivb_cpu;

typedef struct {
    double wall_us;
    double gpu_us;
    bool ok;
    Result rc;
} GxT;

static void ev_cb(void* p) { LightEvent_Signal((LightEvent*)p); }

static u64 gx_start(GSPGPU_Event ev) {
    LightEvent_Clear(&s_ev[ev]);
    return tick();
}

static GxT gx_finish(GSPGPU_Event ev, Result rc, u64 t0) {
    GxT r = {NAN, NAN, false, rc};
    if (R_FAILED(rc)) {
        return r;
    }
    r.ok = LightEvent_WaitTimeout(&s_ev[ev], TIMEOUT_NS) == 0;
    r.wall_us = t2us(tick() - t0);
    if (!r.ok) {
        s_timeouts++;
        logf_("gpu: timeout waiting for event %d", (int)ev);
        return r;
    }
    if (s_perflog) {
        GSPGPU_PerfLog log;
        if (R_SUCCEEDED(GSPGPU_GetPerfLog(&log))) {
            r.gpu_us = log.entries[ev].lastDurationUs;
        }
    }
    return r;
}

static bool gpu_dead(void) { return s_timeouts >= 3; }

static GxT gx_texcopy(void* dst, const void* src, u32 n) {
    u64 t0 = gx_start(GSPGPU_EVENT_PPF);
    Result rc = GX_TextureCopy((u32*)src, 0, (u32*)dst, 0, n, GX_TRANSFER_RAW_COPY(1));
    return gx_finish(GSPGPU_EVENT_PPF, rc, t0);
}

bool gx_copy(void* dst, const void* src, u32 n) { return gx_texcopy(dst, src, n).ok; }

static GxT gx_fill(void* dst, u32 n, u32 value) {
    u64 t0 = gx_start(GSPGPU_EVENT_PSC0);
    Result rc = GX_MemoryFill((u32*)dst, value, (u32*)((u8*)dst + n), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL, 0,
                              NULL, 0);
    return gx_finish(GSPGPU_EVENT_PSC0, rc, t0);
}

#define DT_FLAGS                                                                                                      \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                  \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |                   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static GxT gx_dispxfer(void* dst, const void* src, u32 w, u32 h) {
    u64 t0 = gx_start(GSPGPU_EVENT_PPF);
    Result rc = GX_DisplayTransfer((u32*)src, GX_BUFFER_DIM(w, h), (u32*)dst, GX_BUFFER_DIM(w, h), DT_FLAGS);
    return gx_finish(GSPGPU_EVENT_PPF, rc, t0);
}

/* ---- patterns and checks ---- */

static u32 tile_off(u32 x, u32 y, u32 w) {
    u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m;
}

static u32 cell_color(u32 cx, u32 cy) {
    u32 r = cx * 8, g = cy * 8, b = ((cx * 5 + cy * 3) & 31) * 8;
    return (r << 24) | (g << 16) | (b << 8) | 0xFF;
}

static u32 exp_grid(u32 x, u32 y) { return cell_color(x / CELL, y / CELL); }

static u32 exp_tex(u32 x, u32 y) {
    u32 h = (x | (y << 8)) * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    return h;
}

static bool close_px(u32 a, u32 b, int tol) {
    for (int s = 0; s < 32; s += 8) {
        int d = (int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF);
        if (d > tol || d < -tol) {
            return false;
        }
    }
    return true;
}

/* Mismatching pixels for the best of the four orientations (identity,
 * x flip, y flip, both); *orient gets which one. */
static u32 check_img(const u32* px, bool tiled, u32 (*f)(u32, u32), int tol, int* orient) {
    u32 best = 0xFFFFFFFFu;
    *orient = -1;
    for (int o = 0; o < 4; o++) {
        u32 bad = 0;
        for (u32 y = 0; y < H && bad < best; y++) {
            for (u32 x = 0; x < W; x++) {
                u32 xs = (o & 1) ? W - 1 - x : x;
                u32 ys = (o & 2) ? H - 1 - y : y;
                u32 got = px[tiled ? tile_off(x, y, W) : y * W + x];
                if (!close_px(got, f(xs, ys), tol)) {
                    bad++;
                }
            }
        }
        if (bad < best) {
            best = bad;
            *orient = o;
        }
    }
    return best;
}

static const char* const k_orient[4] = {"as-is", "x-flip", "y-flip", "xy-flip"};

/* Reads n bytes of GPU output at p (any memory) into s_img. */
static void readback(const void* p, u32 n) {
    cache_inval(p, n);
    memcpy(s_img, p, n);
}

/* Places n bytes at dst in memory m: CPU copy, or GPU copy for VRAM. */
static bool put(MemId m, void* dst, const void* src, u32 n) {
    if (m == MEM_VRAM || !g_mem[m].cpu_write) {
        memcpy(s_stage, src, n);
        cache_flush(s_stage, n);
        return gx_copy(dst, s_stage, n);
    }
    memcpy(dst, src, n);
    cache_flush(dst, n);
    return true;
}

static void vtx(Vtx* v, float x, float y, u32 col, float u, float t) {
    v->x = x;
    v->y = y;
    v->z = 0.0f;
    v->c[0] = col >> 24;
    v->c[1] = col >> 16;
    v->c[2] = col >> 8;
    v->c[3] = col;
    v->u = u;
    v->v = t;
}

static void build_patterns(void) {
    Vtx* g = s_grid_cpu;
    for (u32 cy = 0; cy < GRID; cy++) {
        for (u32 cx = 0; cx < GRID; cx++) {
            float x0 = cx * CELL, y0 = cy * CELL, x1 = x0 + CELL, y1 = y0 + CELL;
            u32 c = cell_color(cx, cy);
            vtx(g++, x0, y0, c, 0, 0);
            vtx(g++, x1, y0, c, 0, 0);
            vtx(g++, x1, y1, c, 0, 0);
            vtx(g++, x0, y0, c, 0, 0);
            vtx(g++, x1, y1, c, 0, 0);
            vtx(g++, x0, y1, c, 0, 0);
            u32 q = (cy * GRID + cx);
            Vtx* iv = &s_ivb_cpu[q * 4];
            vtx(&iv[0], x0, y0, c, 0, 0);
            vtx(&iv[1], x1, y0, c, 0, 0);
            vtx(&iv[2], x1, y1, c, 0, 0);
            vtx(&iv[3], x0, y1, c, 0, 0);
            u16* ib = &s_ib_cpu[q * 6];
            ib[0] = q * 4;
            ib[1] = q * 4 + 1;
            ib[2] = q * 4 + 2;
            ib[3] = q * 4;
            ib[4] = q * 4 + 2;
            ib[5] = q * 4 + 3;
        }
    }
    for (u32 y = 0; y < H; y++) {
        for (u32 x = 0; x < W; x++) {
            s_tex_cpu[tile_off(x, y, W)] = exp_tex(x, y);
        }
    }
    Vtx* q = s_quad_lin;
    vtx(&q[0], 0, 0, 0xFFFFFFFF, 0, 0);
    vtx(&q[1], W, 0, 0xFFFFFFFF, 1, 0);
    vtx(&q[2], W, H, 0xFFFFFFFF, 1, 1);
    vtx(&q[3], 0, 0, 0xFFFFFFFF, 0, 0);
    vtx(&q[4], W, H, 0xFFFFFFFF, 1, 1);
    vtx(&q[5], 0, H, 0xFFFFFFFF, 0, 1);
    memcpy(s_grid_lin, s_grid_cpu, VB_BYTES);
    memcpy(s_ivb_lin, s_ivb_cpu, IVB_BYTES);
    memcpy(s_ib_lin, s_ib_cpu, IB_BYTES);
    cache_flush(s_quad_lin, 6 * sizeof(Vtx));
    cache_flush(s_grid_lin, VB_BYTES);
    cache_flush(s_ivb_lin, IVB_BYTES);
    cache_flush(s_ib_lin, IB_BYTES);
}

/* ---- init ---- */

bool gpu_init(void) {
    for (int i = 0; i < GSPGPU_EVENT_MAX; i++) {
        LightEvent_Init(&s_ev[i], RESET_STICKY);
    }
    gspSetEventCallback(GSPGPU_EVENT_PSC0, ev_cb, &s_ev[GSPGPU_EVENT_PSC0], false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, ev_cb, &s_ev[GSPGPU_EVENT_PPF], false);
    gspSetEventCallback(GSPGPU_EVENT_P3D, ev_cb, &s_ev[GSPGPU_EVENT_P3D], false);
    Result pr = GSPGPU_SetPerfLogMode(true);
    s_perflog = R_SUCCEEDED(pr);
    info("gsp_perflog", "%s (rc %08lX)", s_perflog ? "on" : "unavailable", (unsigned long)pr);

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        info("gpu", "C3D_Init failed");
        return false;
    }
    s_dvlb = DVLB_ParseFile((u32*)qtmbench_shbin, qtmbench_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    s_uproj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
    AttrInfo_Init(&s_attr);
    AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&s_attr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 2);

    s_cmd = linearMemAlign(CMD_BYTES, 64);
    s_stage = g_lin_aux;
    s_rt_vram = vramMemAlign(RT_BYTES, 4096);
    s_tex_vram = vramMemAlign(RT_BYTES, 4096);
    s_rt_lin = linearMemAlign(RT_BYTES, 4096);
    s_quad_lin = linearMemAlign(6 * sizeof(Vtx), 64);
    s_grid_lin = linearMemAlign(VB_BYTES, 64);
    s_ivb_lin = linearMemAlign(IVB_BYTES, 64);
    s_ib_lin = linearMemAlign(IB_BYTES, 64);
    s_grid_cpu = malloc(VB_BYTES);
    s_ivb_cpu = malloc(IVB_BYTES);
    s_ib_cpu = malloc(IB_BYTES);
    s_tex_cpu = malloc(RT_BYTES);
    s_img = malloc(QB_MIB);
    if (!C3D_TexInit(&s_tex, W, H, GPU_RGBA8)) {
        info("gpu", "C3D_TexInit failed");
        return false;
    }
    s_tex_orig = s_tex.data;
    C3D_TexSetFilter(&s_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    if (s_cmd == NULL || s_rt_vram == NULL || s_tex_vram == NULL || s_rt_lin == NULL || s_quad_lin == NULL ||
        s_grid_lin == NULL || s_ivb_lin == NULL || s_ib_lin == NULL || s_grid_cpu == NULL || s_ivb_cpu == NULL ||
        s_ib_cpu == NULL || s_tex_cpu == NULL || s_img == NULL) {
        info("gpu", "allocation failed");
        return false;
    }
    build_patterns();
    /* the texture in VRAM for the render-target tests */
    memcpy(s_stage, s_tex_cpu, RT_BYTES);
    cache_flush(s_stage, RT_BYTES);
    s_ready = gx_copy(s_tex_vram, s_stage, RT_BYTES);
    info("gpu", "%s", s_ready ? "ready" : "first GPU copy timed out");
    return s_ready;
}

void gpu_exit(void) {
    if (s_tex_orig != NULL) {
        s_tex.data = s_tex_orig;
        C3D_TexDelete(&s_tex);
    }
    if (s_dvlb != NULL) {
        shaderProgramFree(&s_prog);
        DVLB_Free(s_dvlb);
    }
    C3D_Fini();
}

/* ---- command lists ---- */

static void set_proj(bool tiny) {
    C3D_Mtx m;
    float k = tiny ? 32.0f : 1.0f;
    Mtx_Ortho(&m, 0.0f, W * k, 0.0f, H * k, -1.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uproj, &m);
}

/* Starts a list that sets every piece of state it depends on, so lists can
 * be replayed in any order. */
static void list_begin(void* target, bool textured) {
    GPUCMD_SetBuffer(s_cmd, CMD_BYTES / 4, 0);
    C3D_BindProgram(&s_prog);
    C3D_SetAttrInfo(&s_attr);
    C3D_FrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    C3D_FrameBufAttrib(&fb, W, H, false);
    C3D_FrameBufColor(&fb, target, GPU_RB_RGBA8);
    C3D_FrameBufDepth(&fb, NULL, GPU_RB_DEPTH24);
    C3D_SetFrameBuf(&fb);
    C3D_SetViewport(0, 0, W, H);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    set_proj(false);
}

static u32 list_end(u32** out) {
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    u32* b;
    u32 n;
    GPUCMD_Split(&b, &n);
    *out = b;
    return n;
}

static GxT list_run(u32* b, u32 words) {
    cache_flush(b, words * 4);
    u64 t0 = gx_start(GSPGPU_EVENT_P3D);
    Result rc = GX_ProcessCommandList(b, words * 4, 0);
    return gx_finish(GSPGPU_EVENT_P3D, rc, t0);
}

static void bind_vbo(const void* vb) {
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vb, sizeof(Vtx), 3, 0x210);
}

static void bind_tex(void* data) {
    s_tex.data = data;
    C3D_TexBind(0, &s_tex);
}

/* Best-of-n runs of a recorded list. */
static GxT list_time(u32* b, u32 words, int n) {
    GxT best = {NAN, NAN, false, 0};
    for (int i = 0; i < n; i++) {
        GxT t = list_run(b, words);
        if (!t.ok) {
            return t;
        }
        if (!best.ok || t.wall_us < best.wall_us) {
            best = t;
        }
    }
    return best;
}

static void clear_rt(void* rt) { gx_fill(rt, RT_BYTES, 0x11223344u); }

static void rec_check(const char* test, MemId m, u32 bad, int orient, const GxT* t) {
    char note[100];
    snprintf(note, sizeof(note), "%s; %s; rc=%08lX", bad == 0 ? "ok" : "MISMATCH", orient >= 0 ? k_orient[orient] : "-",
             (unsigned long)t->rc);
    rec("gpu_check", test, g_mem[m].name, -1, 0, t->ok ? (double)bad : NAN, "badpx", NAN,
        t->ok ? note : "GPU op failed or timed out");
}

static void rec_time(const char* suite, const char* test, MemId m, const GxT* t, u64 bytes) {
    char note[100];
    snprintf(note, sizeof(note), "aux=GSP us; %.0f MB/s", t->ok ? (double)bytes / t->wall_us : 0.0);
    rec(suite, test, g_mem[m].name, -1, (u32)bytes, t->ok ? t->wall_us : NAN, "us", t->gpu_us,
        t->ok ? note : "failed or timed out");
}

/* ---- GPU reads from memory m ---- */

static void test_vbo(MemId m) {
    u8* vb = g_mem[m].base;
    if (!put(m, vb, s_grid_cpu, VB_BYTES)) {
        rec("gpu_check", "vbo", g_mem[m].name, -1, 0, NAN, "badpx", NAN, "placing the buffer failed");
        return;
    }
    clear_rt(s_rt_vram);
    list_begin(s_rt_vram, false);
    bind_vbo(vb);
    C3D_DrawArrays(GPU_TRIANGLES, 0, GRID_VERTS);
    u32* b;
    u32 n = list_end(&b);
    GxT t = list_run(b, n);
    readback(s_rt_vram, RT_BYTES);
    int o;
    u32 bad = t.ok ? check_img(s_img, true, exp_grid, 2, &o) : 0;
    rec_check("vbo", m, bad, o, &t);

    list_begin(s_rt_vram, false);
    bind_vbo(vb);
    set_proj(true);
    for (int r = 0; r < R_VTX; r++) {
        C3D_DrawArrays(GPU_TRIANGLES, 0, GRID_VERTS);
    }
    n = list_end(&b);
    t = list_time(b, n, 3);
    rec_time("gpu_read", "vbo_rep", m, &t, (u64)VB_BYTES * R_VTX);
}

static void test_ibo(MemId m) {
    u8* ib = g_mem[m].base;
    if (!put(m, ib, s_ib_cpu, IB_BYTES)) {
        rec("gpu_check", "ibo", g_mem[m].name, -1, 0, NAN, "badpx", NAN, "placing the buffer failed");
        return;
    }
    clear_rt(s_rt_vram);
    list_begin(s_rt_vram, false);
    bind_vbo(s_ivb_lin);
    C3D_DrawElements(GPU_TRIANGLES, GRID * GRID * 6, C3D_UNSIGNED_SHORT, ib);
    u32* b;
    u32 n = list_end(&b);
    GxT t = list_run(b, n);
    readback(s_rt_vram, RT_BYTES);
    int o;
    u32 bad = t.ok ? check_img(s_img, true, exp_grid, 2, &o) : 0;
    rec_check("ibo", m, bad, o, &t);

    list_begin(s_rt_vram, false);
    bind_vbo(s_ivb_lin);
    set_proj(true);
    for (int r = 0; r < R_IDX; r++) {
        C3D_DrawElements(GPU_TRIANGLES, GRID * GRID * 6, C3D_UNSIGNED_SHORT, ib);
    }
    n = list_end(&b);
    t = list_time(b, n, 3);
    rec_time("gpu_read", "ibo_rep", m, &t, (u64)IB_BYTES * R_IDX);
}

static void test_tex(MemId m) {
    u8* tex = g_mem[m].base;
    if (!put(m, tex, s_tex_cpu, RT_BYTES)) {
        rec("gpu_check", "tex", g_mem[m].name, -1, 0, NAN, "badpx", NAN, "placing the texture failed");
        return;
    }
    clear_rt(s_rt_vram);
    list_begin(s_rt_vram, true);
    bind_vbo(s_quad_lin);
    bind_tex(tex);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    u32* b;
    u32 n = list_end(&b);
    GxT t = list_run(b, n);
    readback(s_rt_vram, RT_BYTES);
    int o;
    u32 bad = t.ok ? check_img(s_img, true, exp_tex, 0, &o) : 0;
    rec_check("tex", m, bad, o, &t);

    list_begin(s_rt_vram, true);
    bind_vbo(s_quad_lin);
    bind_tex(tex);
    for (int r = 0; r < R_TEX; r++) {
        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    }
    n = list_end(&b);
    t = list_time(b, n, 3);
    rec_time("gpu_read", "tex_rep", m, &t, (u64)RT_BYTES * R_TEX);
}

/* The command list itself in memory m: an immediate-mode grid (the vertex
 * data is inside the list), recorded in linear memory and copied to m. */
static void test_cmdlist(MemId m) {
    list_begin(s_rt_vram, false);
    C3D_ImmDrawBegin(GPU_TRIANGLES);
    for (u32 i = 0; i < GRID_VERTS; i++) {
        const Vtx* v = &s_grid_cpu[i];
        C3D_ImmSendAttrib(v->x, v->y, v->z, 1.0f);
        C3D_ImmSendAttrib(v->c[0], v->c[1], v->c[2], v->c[3]);
        C3D_ImmSendAttrib(v->u, v->v, 0.0f, 1.0f);
    }
    C3D_ImmDrawEnd();
    u32* b;
    u32 n = list_end(&b);
    u32 bytes = n * 4;
    u32* dst = (u32*)g_mem[m].base;
    if (bytes > QB_MIB || !put(m, dst, b, bytes)) {
        rec("gpu_check", "cmdlist", g_mem[m].name, -1, bytes, NAN, "badpx", NAN, "placing the list failed");
        return;
    }
    clear_rt(s_rt_vram);
    GxT t = list_run(dst, n);
    readback(s_rt_vram, RT_BYTES);
    int o;
    u32 bad = t.ok ? check_img(s_img, true, exp_grid, 2, &o) : 0;
    rec_check("cmdlist", m, bad, o, &t);
    t = list_time(dst, n, 3);
    rec_time("gpu_read", "cmdlist", m, &t, bytes);
}

/* TextureCopy and DisplayTransfer reading from m. */
static void test_xfer_src(MemId m) {
    u8* src = g_mem[m].base;
    if (!put(m, src, s_tex_cpu, RT_BYTES)) {
        return;
    }
    memset(s_rt_lin, 0, RT_BYTES);
    cache_flush(s_rt_lin, RT_BYTES);
    GxT t = gx_texcopy(s_rt_lin, src, RT_BYTES);
    readback(s_rt_lin, RT_BYTES);
    u32 bad = t.ok ? (u32)(memcmp(s_img, s_tex_cpu, RT_BYTES) != 0) : 0;
    rec_check("texcopy_src", m, bad, 0, &t);
    GxT best = t;
    for (int i = 0; i < 3 && t.ok; i++) {
        t = gx_texcopy(s_rt_lin, src, RT_BYTES);
        if (t.ok && t.wall_us < best.wall_us) {
            best = t;
        }
    }
    rec_time("gpu_read", "texcopy_src", m, &best, RT_BYTES);

    t = gx_dispxfer(s_rt_lin, src, W, H);
    readback(s_rt_lin, RT_BYTES);
    int o;
    bad = t.ok ? check_img(s_img, false, exp_tex, 0, &o) : 0;
    rec_check("dispxfer_src", m, bad, o, &t);
    best = t;
    for (int i = 0; i < 3 && t.ok; i++) {
        t = gx_dispxfer(s_rt_lin, src, W, H);
        if (t.ok && t.wall_us < best.wall_us) {
            best = t;
        }
    }
    rec_time("gpu_read", "dispxfer_src", m, &best, RT_BYTES);
}

/* ---- GPU writes into memory m ---- */

static u32 count_ne(const u32* p, u32 words, u32 v) {
    u32 bad = 0;
    for (u32 i = 0; i < words; i++) {
        bad += p[i] != v;
    }
    return bad;
}

static void test_writes(MemId m) {
    u8* dst = g_mem[m].base;
    const u32 n = QB_MIB;
    /* memory fill */
    GxT best = {NAN, NAN, false, 0}, t;
    for (int i = 0; i < 3; i++) {
        t = gx_fill(dst, n, 0xC0FFEE00u + i);
        if (!t.ok) {
            best = t;
            break;
        }
        if (!best.ok || t.wall_us < best.wall_us) {
            best = t;
        }
    }
    readback(dst, n);
    u32 bad = best.ok ? count_ne(s_img, n / 4, 0xC0FFEE02u) : 0;
    rec_check("fill_dst", m, bad, 0, &best);
    rec_time("gpu_write", "fill", m, &best, n);

    /* texture copy */
    fill_pattern((u32*)s_stage, n / 4, 0xABCD0000u);
    cache_flush(s_stage, n);
    best.ok = false;
    for (int i = 0; i < 3; i++) {
        t = gx_texcopy(dst, s_stage, n);
        if (!t.ok) {
            best = t;
            break;
        }
        if (!best.ok || t.wall_us < best.wall_us) {
            best = t;
        }
    }
    readback(dst, n);
    bad = best.ok ? (u32)(memcmp(s_img, s_stage, n) != 0) : 0;
    rec_check("texcopy_dst", m, bad, 0, &best);
    rec_time("gpu_write", "texcopy", m, &best, n);

    /* display transfer (tiled -> linear) */
    memcpy(s_stage, s_tex_cpu, RT_BYTES);
    cache_flush(s_stage, RT_BYTES);
    best.ok = false;
    for (int i = 0; i < 3; i++) {
        t = gx_dispxfer(dst, s_stage, W, H);
        if (!t.ok) {
            best = t;
            break;
        }
        if (!best.ok || t.wall_us < best.wall_us) {
            best = t;
        }
    }
    readback(dst, RT_BYTES);
    int o = -1;
    bad = best.ok ? check_img(s_img, false, exp_tex, 0, &o) : 0;
    rec_check("dispxfer_dst", m, bad, o, &best);
    rec_time("gpu_write", "dispxfer", m, &best, RT_BYTES);

    /* render target in m (texture in VRAM) */
    u8* rt = m == MEM_VRAM ? s_rt_vram : dst;
    clear_rt(rt);
    list_begin(rt, true);
    bind_vbo(s_quad_lin);
    bind_tex(s_tex_vram);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    u32* b;
    u32 words = list_end(&b);
    t = list_run(b, words);
    readback(rt, RT_BYTES);
    bad = t.ok ? check_img(s_img, true, exp_tex, 0, &o) : 0;
    rec_check("rendertarget", m, bad, o, &t);
    list_begin(rt, true);
    bind_vbo(s_quad_lin);
    bind_tex(s_tex_vram);
    for (int r = 0; r < R_TEX; r++) {
        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    }
    words = list_end(&b);
    t = list_time(b, words, 3);
    rec_time("gpu_write", "rendertarget_rep", m, &t, (u64)RT_BYTES * R_TEX);
}

void gpu_suite(void) {
    if (!s_ready || !suite_on("gpu")) {
        return;
    }
    static const MemId ms[] = {MEM_LINEAR, MEM_QTM, MEM_VRAM};
    for (unsigned i = 0; i < 3 && !gpu_dead(); i++) {
        MemId m = ms[i];
        if (!g_mem[m].gpu) {
            continue;
        }
        status("gpu reads from %s", g_mem[m].name);
        test_vbo(m);
        test_ibo(m);
        test_tex(m);
        test_cmdlist(m);
        test_xfer_src(m);
        status("gpu writes into %s", g_mem[m].name);
        test_writes(m);
        ui_poll();
    }
    if (gpu_dead()) {
        rec("gpu_check", "gpu", "-", -1, 0, s_timeouts, "timeouts", NAN, "GPU stopped answering; later GPU tests skipped");
    }
    for (int m = 0; m < MEM_COUNT; m++) {
        if (g_mem[m].cpu_write && g_mem[m].present) {
            memset(g_mem[m].base, 0, g_mem[m].size);
            cache_flush(g_mem[m].base, g_mem[m].size);
        }
    }
}

/* ---- coherency ---- */

static void count_ab(const u32* p, u32 words, u32 sa, u32 sb, u32* na, u32* nb) {
    *na = *nb = 0;
    for (u32 i = 0; i < words; i++) {
        u32 a = (i * 2654435761u) ^ sa, b = (i * 2654435761u) ^ sb;
        *na += p[i] == a;
        *nb += p[i] == b;
    }
}

static void coh_one(MemId m, u32 n) {
    u32* p = (u32*)g_mem[m].base;
    u32 words = n / 4, na, nb;
    char note[100];
    const u32 A = 0xAAAA0000u, B = 0xBBBB0000u;
    u32* dst = (u32*)s_stage;

    /* CPU writes, GPU reads, with and without a flush in between. */
    for (int flush = 0; flush < 2; flush++) {
        fill_pattern(p, words, B);
        cache_flush(p, n); /* memory holds B */
        fill_pattern(p, words, A); /* the cache holds A (write-back) */
        if (flush) {
            cache_flush(p, n);
        }
        memset(dst, 0, n);
        cache_flush(dst, n);
        GxT t = gx_texcopy(dst, p, n);
        cache_inval(dst, n);
        count_ab(dst, words, A, B, &na, &nb);
        snprintf(note, sizeof(note), "new %lu old %lu of %lu words%s", (unsigned long)na, (unsigned long)nb,
                 (unsigned long)words, t.ok ? "" : " (copy failed)");
        rec("coherency", flush ? "cpu_wr_flush_gpu_rd" : "cpu_wr_gpu_rd_noflush", g_mem[m].name, -1, n,
            t.ok ? 100.0 * nb / words : NAN, "%stale", NAN, note);
        cache_flush(p, n);
    }
    /* GPU writes, CPU reads, without and with an invalidate. */
    fill_pattern(p, words, A);
    cache_flush(p, n);
    k_read_ldm(p, n); /* A now in the cache, clean */
    GxT t = gx_fill(p, n, 0x5EED5EEDu);
    u32 stale = 0;
    for (u32 i = 0; i < words; i++) {
        stale += p[i] != 0x5EED5EEDu;
    }
    snprintf(note, sizeof(note), "%lu of %lu words still old%s", (unsigned long)stale, (unsigned long)words,
             t.ok ? "" : " (fill failed)");
    rec("coherency", "gpu_wr_cpu_rd_noinval", g_mem[m].name, -1, n, t.ok ? 100.0 * stale / words : NAN, "%stale",
        NAN, note);
    cache_inval(p, n);
    stale = count_ne(p, words, 0x5EED5EEDu);
    snprintf(note, sizeof(note), "%lu of %lu words old", (unsigned long)stale, (unsigned long)words);
    rec("coherency", "gpu_wr_inval_cpu_rd", g_mem[m].name, -1, n, t.ok ? 100.0 * stale / words : NAN, "%stale", NAN,
        note);
    memset(p, 0, n);
    cache_flush(p, n);
}

void coherency_suite(void) {
    if (!s_ready || gpu_dead() || !suite_on("coherency")) {
        return;
    }
    status("coherency");
    static const MemId ms[] = {MEM_LINEAR, MEM_QTM};
    static const u32 sz[] = {16u << 10, 1u << 20};
    for (int i = 0; i < 2; i++) {
        if (!g_mem[ms[i]].gpu || !g_mem[ms[i]].cpu_write) {
            continue;
        }
        for (int k = 0; k < 2; k++) {
            coh_one(ms[i], sz[k]);
        }
    }
}

/* ---- loads for the contention matrix ---- */

static u32* s_load[GLOAD_COUNT][MEM_COUNT];
static u32 s_load_words[GLOAD_COUNT][MEM_COUNT];

static void* load_ptr(GLoad k, MemId m) {
    /* TEX at the region's base, VTX 512 KiB in, so both can coexist. */
    return g_mem[m].base + (k == GLOAD_VTX ? 512u * 1024u : 0);
}

bool gpu_load_prepare(GLoad k, MemId m) {
    if (!s_ready || gpu_dead()) {
        return false;
    }
    if (k == GLOAD_DT) {
        return true; /* linear -> linear, 1 MiB, in g_lin_aux */
    }
    if (!g_mem[m].gpu) {
        return false;
    }
    void* p = load_ptr(k, m);
    u32* b;
    u32 n;
    if (k == GLOAD_TEX) {
        if (!put(m, p, s_tex_cpu, RT_BYTES)) {
            return false;
        }
        list_begin(s_rt_vram, true);
        bind_vbo(s_quad_lin);
        bind_tex(p);
        for (int r = 0; r < R_TEX; r++) {
            C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
        }
    } else {
        if (!put(m, p, s_grid_cpu, VB_BYTES)) {
            return false;
        }
        list_begin(s_rt_vram, false);
        bind_vbo(p);
        set_proj(true);
        for (int r = 0; r < R_VTX; r++) {
            C3D_DrawArrays(GPU_TRIANGLES, 0, GRID_VERTS);
        }
    }
    n = list_end(&b);
    if (s_load[k][m] != NULL) {
        linearFree(s_load[k][m]);
    }
    s_load[k][m] = linearMemAlign(n * 4, 64);
    if (s_load[k][m] == NULL) {
        return false;
    }
    memcpy(s_load[k][m], b, n * 4);
    cache_flush(s_load[k][m], n * 4);
    s_load_words[k][m] = n;
    return true;
}

u32 gpu_load_bytes(GLoad k) {
    switch (k) {
    case GLOAD_TEX: return RT_BYTES * R_TEX;
    case GLOAD_VTX: return VB_BYTES * R_VTX;
    default: return QB_MIB;
    }
}

double gpu_load_once(GLoad k, MemId m) {
    if (gpu_dead()) {
        return -1.0;
    }
    GxT t;
    if (k == GLOAD_DT) {
        t = gx_dispxfer(g_lin_aux + 2 * QB_MIB, g_lin_aux + QB_MIB, 512, 512);
    } else {
        if (s_load[k][m] == NULL) {
            return -1.0;
        }
        u64 t0 = gx_start(GSPGPU_EVENT_P3D);
        Result rc = GX_ProcessCommandList(s_load[k][m], s_load_words[k][m] * 4, 0);
        t = gx_finish(GSPGPU_EVENT_P3D, rc, t0);
    }
    return t.ok ? t.wall_us : -1.0;
}
