/* ctr-devkit gpuprobe: see include/ctrdk/gpuprobe.h.
 *
 * Layout of one case: clear a 64 KiB block, record a command list that draws into the 64x64
 * RGBA8 target at block+16 KiB (16 KiB of sentinel before it, 32 KiB after it, to catch writes
 * beside the target), submit with GX_ProcessCommandList, wait for the P3D event with a
 * timeout, copy the block back and compare. citro3d records state and the draw; the harness
 * submits the list itself (no citro3d frame queue), so a hang is a timeout here and not a
 * blocked frame. Lists are patched at word level where a case needs bytes citro3d would not
 * write.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <3ds.h>
#include <citro3d.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ctrdk/gpuprobe.h"
#include "gpuprobe_shbin.h"

#define RTW 64
#define RT_BYTES (RTW * RTW * 4)
#define BLK_BYTES (64 * 1024)
#define RT_OFF (16 * 1024)
#define SENT 0x11223344u
#define RED 0xFF0000FFu
#define CMD_BYTES (64 * 1024)
#define TEX_SMALL (8 * 1024)
#define NSHADER 6
#define NVERT 12
#define TICKS_PER_MS 268111.856

typedef enum { R_OK, R_WRONG, R_HANG, R_FAULT, R_ERR, R_SKIP, R_NONE } ResCode;
static const char* const k_res_name[] = {"OK", "WRONG", "HANG", "FAULT", "ERR", "SKIP", "-"};

typedef struct {
    ResCode code;
    char note[96];
} Res;

typedef enum {
    K_DRAW, K_SCISSOR, K_VIEWPORT, K_SHADER, K_NOSHADER, K_VBUF, K_IBUF, K_ATTR13, K_TEX, K_CMD, K_RAWREG, K_RT,
    K_RTDEL, K_SELFTEST
} Kind;

typedef enum { EX_RED, EX_CLEAR, EX_LEFT, EX_RIGHT } Expect;

typedef struct {
    const char* id;
    const char* group;
    int danger; /* 0 control, 1 unlikely, 2 hang likely, 3 memory faults possible */
    const char* predict; /* OK / WRONG / HANG / ? and where the rule comes from */
    Kind kind;
    int a;
    int p[4];
    Expect exp;
    const char* desc;
} Case;

typedef struct {
    float x, y, z;
    u8 c[4];
    float u, v;
} Vtx;

/* ---- the cases ---- */

#define SC_NORMAL 1
#define DRAW_ARR 0
#define DRAW_E16 1
#define DRAW_E8 2
#define DRAW_STRIP 3
#define DRAW_FAN 4
#define TF_565 0
#define TF_RGBA8 1
#define TF_ETC1 2

#define CASE(id, grp, dng, pred, kind, a, p0, p1, p2, p3, ex, desc) \
    {id, grp, dng, pred, kind, a, {p0, p1, p2, p3}, ex, desc}

#define TEXA(fmtname, fmt, k, pred) \
    CASE("tex_" fmtname "_a" #k, "texture", (k) == 256 ? 0 : 2, pred, K_TEX, fmt, (k) % 256, 8, 8, 0, EX_RED, \
         fmtname " 8x8 at a base aligned to exactly " #k " bytes")

static const Case k_cases[] = {
    /* controls */
    CASE("ctl_arrays", "control", 0, "OK", K_DRAW, DRAW_ARR, 3, 0, 0, 0, EX_RED, "DrawArrays 3 vertices, full-target triangle"),
    CASE("ctl_elems16", "control", 0, "OK", K_DRAW, DRAW_E16, 3, 0, 0, 0, EX_RED, "DrawElements, u16 indices"),
    CASE("ctl_elems8", "control", 0, "OK", K_DRAW, DRAW_E8, 3, 0, 0, 0, EX_RED, "DrawElements, u8 indices"),
    CASE("dr_arr6", "control", 0, "OK", K_DRAW, DRAW_ARR, 6, 0, 0, 0, EX_RED, "DrawArrays 6 vertices (two triangles)"),
    CASE("dr_strip3", "control", 0, "OK", K_DRAW, DRAW_STRIP, 3, 0, 0, 0, EX_RED, "triangle strip, 3 vertices"),
    CASE("sc_full", "control", 0, "OK", K_SCISSOR, SC_NORMAL, 0, 0, 64, 64, EX_RED, "scissor on, whole target"),
    CASE("sc_left32", "control", 0, "OK", K_SCISSOR, SC_NORMAL, 0, 0, 32, 64, EX_LEFT, "scissor on, left half"),
    CASE("vp_full", "control", 0, "OK", K_VIEWPORT, 0, 0, 0, 64, 64, EX_RED, "viewport 0,0,64,64"),
    CASE("sh_full", "control", 0, "OK", K_SHADER, 0, -1, 0, 0, 0, EX_RED, "shader writes every output in full"),
    CASE("sh_b14", "control", 0, "OK", K_SHADER, 5, 14, 0, 0, 0, EX_RED, "shader branches on bool uniform b14 (true)"),
    CASE("vb_off0", "control", 0, "OK", K_VBUF, 0, 0, 0, 0, 0, EX_RED, "vertex data at attribute offset 0"),
    CASE("vb_off16", "control", 0, "OK", K_VBUF, 0, 16, 0, 0, 0, EX_RED, "vertex data at attribute offset 16"),
    CASE("vb_base16", "control", 0, "OK", K_VBUF, 1, 16, 0, 0, 0, EX_RED, "vertex buffer base 16-byte aligned"),
    CASE("ib_off0", "control", 0, "OK", K_IBUF, 1, 0, 0, 0, 0, EX_RED, "u16 indices at offset 0"),
    CASE("ib_off16", "control", 0, "OK", K_IBUF, 1, 16, 0, 0, 0, EX_RED, "u16 indices at offset 16"),
    CASE("cb_aligned16", "control", 0, "OK", K_CMD, 0, 0, 0, 0, 0, EX_RED, "list size a multiple of 16 bytes"),
    CASE("rg_ctl", "control", 0, "OK", K_RAWREG, 0, 0x0040, 0xF, 1, 0, EX_RED, "write to a defined register (FACECULLING_CONFIG = 0)"),
    CASE("rg_mask0", "control", 0, "OK", K_RAWREG, 0, 0x0040, 0x0, 1, 0, EX_RED, "register write with byte mask 0 (does nothing)"),
    TEXA("rgba8", TF_RGBA8, 256, "OK"),
    TEXA("rgb565", TF_565, 256, "OK"),
    TEXA("etc1", TF_ETC1, 256, "OK"),
    CASE("tex_sz8", "control", 0, "OK", K_TEX, TF_RGBA8, 0, 8, 8, 0, EX_RED, "RGBA8 8x8"),

    /* scissor and viewport */
    CASE("sc_past", "scissor", 1, "OK?", K_SCISSOR, SC_NORMAL, 0, 0, 128, 128, EX_RED, "scissor larger than the target"),
    CASE("sc_off", "scissor", 1, "?", K_SCISSOR, SC_NORMAL, 100, 100, 200, 200, EX_CLEAR, "scissor entirely beside the target"),
    CASE("sc_w0", "scissor", 2, "HANG (documented)", K_SCISSOR, SC_NORMAL, 32, 0, 32, 64, EX_CLEAR, "scissor width 0 (left == right)"),
    CASE("sc_h0", "scissor", 2, "HANG (documented)", K_SCISSOR, SC_NORMAL, 0, 32, 64, 32, EX_CLEAR, "scissor height 0 (top == bottom)"),
    CASE("sc_inv", "scissor", 2, "HANG (documented)", K_SCISSOR, SC_NORMAL, 48, 0, 16, 64, EX_CLEAR, "scissor inverted (right < left)"),
    CASE("sc_zero", "scissor", 2, "HANG (documented)", K_SCISSOR, SC_NORMAL, 0, 0, 0, 0, EX_CLEAR, "scissor 0,0,0,0"),
    CASE("vp_right", "viewport", 1, "OK?", K_VIEWPORT, 0, 32, 0, 64, 64, EX_RIGHT, "viewport starts inside and ends past the target"),
    CASE("vp_big", "viewport", 1, "OK?", K_VIEWPORT, 0, 0, 0, 1024, 1024, EX_RED, "viewport 1024x1024 on a 64x64 target"),
    CASE("vp_off", "viewport", 2, "?", K_VIEWPORT, 0, 100, 0, 64, 64, EX_CLEAR, "viewport entirely right of the target"),
    CASE("vp_w0", "viewport", 2, "HANG (documented)", K_VIEWPORT, 0, 0, 0, 0, 64, EX_CLEAR, "viewport width 0"),
    CASE("vp_h0", "viewport", 2, "HANG (documented)", K_VIEWPORT, 0, 0, 0, 64, 0, EX_CLEAR, "viewport height 0"),
    CASE("vp_zero", "viewport", 2, "HANG (documented)", K_VIEWPORT, 0, 0, 0, 0, 0, EX_CLEAR, "viewport 0x0"),

    /* draw counts */
    CASE("dr_arr4", "draw", 1, "OK?", K_DRAW, DRAW_ARR, 4, 0, 0, 0, EX_RED, "DrawArrays, 4 vertices (not a multiple of 3)"),
    CASE("dr_arr5", "draw", 1, "OK?", K_DRAW, DRAW_ARR, 5, 0, 0, 0, EX_RED, "DrawArrays, 5 vertices"),
    CASE("dr_elem4", "draw", 1, "OK?", K_DRAW, DRAW_E16, 4, 0, 0, 0, EX_RED, "DrawElements, index count 4"),
    CASE("dr_elem5", "draw", 1, "OK?", K_DRAW, DRAW_E16, 5, 0, 0, 0, EX_RED, "DrawElements, index count 5"),
    CASE("dr_elem8_4", "draw", 1, "OK?", K_DRAW, DRAW_E8, 4, 0, 0, 0, EX_RED, "DrawElements u8, index count 4"),
    CASE("dr_1v", "draw", 2, "? (documented: zero-count)", K_DRAW, DRAW_ARR, 1, 0, 0, 0, EX_CLEAR, "DrawArrays, 1 vertex"),
    CASE("dr_2v", "draw", 2, "?", K_DRAW, DRAW_ARR, 2, 0, 0, 0, EX_CLEAR, "DrawArrays, 2 vertices"),
    CASE("dr_elem1", "draw", 2, "?", K_DRAW, DRAW_E16, 1, 0, 0, 0, EX_CLEAR, "DrawElements, 1 index"),
    CASE("dr_elem2", "draw", 2, "?", K_DRAW, DRAW_E16, 2, 0, 0, 0, EX_CLEAR, "DrawElements, 2 indices"),
    CASE("dr_strip2", "draw", 2, "?", K_DRAW, DRAW_STRIP, 2, 0, 0, 0, EX_CLEAR, "triangle strip, 2 vertices"),
    CASE("dr_fan2", "draw", 2, "?", K_DRAW, DRAW_FAN, 2, 0, 0, 0, EX_CLEAR, "triangle fan, 2 vertices"),
    CASE("dr_zero_arr", "draw", 2, "HANG (documented)", K_DRAW, DRAW_ARR, 0, 0, 0, 0, EX_CLEAR, "DrawArrays, 0 vertices"),
    CASE("dr_zero_elem", "draw", 2, "HANG (documented)", K_DRAW, DRAW_E16, 0, 0, 0, 0, EX_CLEAR, "DrawElements, 0 indices"),

    /* shaders */
    CASE("sh_b15", "shader", 1, "WRONG (documented)", K_SHADER, 4, 15, 0, 0, 0, EX_RED, "shader branches on bool uniform b15 (true)"),
    CASE("sh_part_clr", "shader", 2, "HANG (documented)", K_SHADER, 2, -1, 0, 0, 0, EX_RED, "colour output written as .xyz only"),
    CASE("sh_part_tc", "shader", 2, "HANG (documented)", K_SHADER, 3, -1, 0, 0, 0, EX_RED, "texcoord output written as .xy only"),
    CASE("sh_part_pos", "shader", 2, "HANG (documented)", K_SHADER, 1, -1, 0, 0, 0, EX_RED, "position output written as .xyz only"),

    /* vertex and index buffers */
    CASE("vb_off2", "buffer", 2, "?", K_VBUF, 0, 2, 0, 0, 0, EX_RED, "attribute buffer offset 2"),
    CASE("vb_off4", "buffer", 2, "?", K_VBUF, 0, 4, 0, 0, 0, EX_RED, "attribute buffer offset 4"),
    CASE("vb_off8", "buffer", 2, "?", K_VBUF, 0, 8, 0, 0, 0, EX_RED, "attribute buffer offset 8"),
    CASE("vb_base8", "buffer", 2, "?", K_VBUF, 1, 8, 0, 0, 0, EX_RED, "vertex buffer base only 8-byte aligned"),
    CASE("ib_off1", "buffer", 2, "?", K_IBUF, 1, 1, 0, 0, 0, EX_RED, "u16 indices at offset 1 (odd)"),
    CASE("ib_off2", "buffer", 2, "?", K_IBUF, 1, 2, 0, 0, 0, EX_RED, "u16 indices at offset 2"),
    CASE("ib_off4", "buffer", 2, "?", K_IBUF, 1, 4, 0, 0, 0, EX_RED, "u16 indices at offset 4"),
    CASE("ib_off8", "buffer", 2, "?", K_IBUF, 1, 8, 0, 0, 0, EX_RED, "u16 indices at offset 8"),
    CASE("ib8_off1", "buffer", 1, "OK?", K_IBUF, 0, 1, 0, 0, 0, EX_RED, "u8 indices at offset 1"),
    CASE("at_13", "buffer", 2, "?", K_ATTR13, 0, 0, 0, 0, 0, EX_RED, "attribute count field 13 (more than 12 attributes)"),

    /* textures (base alignment per format, sizes) */
    TEXA("rgba8", TF_RGBA8, 128, "?"),
    TEXA("rgba8", TF_RGBA8, 64, "?"),
    TEXA("rgba8", TF_RGBA8, 32, "?"),
    TEXA("rgba8", TF_RGBA8, 16, "?"),
    TEXA("rgba8", TF_RGBA8, 8, "?"),
    TEXA("rgb565", TF_565, 128, "?"),
    TEXA("rgb565", TF_565, 64, "? (Star3D: unaligned fail)"),
    TEXA("rgb565", TF_565, 32, "? (Star3D: unaligned fail)"),
    TEXA("rgb565", TF_565, 16, "? (Star3D: unaligned fail)"),
    TEXA("rgb565", TF_565, 8, "? (Star3D: unaligned fail)"),
    TEXA("etc1", TF_ETC1, 128, "?"),
    TEXA("etc1", TF_ETC1, 64, "?"),
    TEXA("etc1", TF_ETC1, 32, "?"),
    TEXA("etc1", TF_ETC1, 16, "?"),
    TEXA("etc1", TF_ETC1, 8, "?"),
    CASE("tex_sz4", "texture", 2, "?", K_TEX, TF_RGBA8, 0, 4, 4, 0, EX_RED, "RGBA8 4x4"),
    CASE("tex_np24", "texture", 1, "?", K_TEX, TF_RGBA8, 0, 24, 24, 0, EX_RED, "RGBA8 24x24 (not a power of two)"),
    CASE("tex_np100", "texture", 2, "?", K_TEX, TF_RGBA8, 0, 100, 100, 0, EX_RED, "RGBA8 100x100 (not a multiple of 8)"),
    CASE("tex_sz1024", "texture", 1, "OK?", K_TEX, TF_565, 0, 1024, 1024, 0, EX_RED, "RGB565 1024x1024 (largest documented)"),
    CASE("tex_sz2048", "texture", 3, "?", K_TEX, TF_ETC1, 0, 2048, 2048, 0, EX_RED, "ETC1 2048x2048 (past the 1024 limit)"),

    /* command list */
    CASE("cb_size8", "cmdlist", 2, "?", K_CMD, 1, 0, 0, 0, 0, EX_RED, "list size 8 bytes past a multiple of 16"),
    CASE("cb_trunc4", "cmdlist", 2, "?", K_CMD, 2, 0, 0, 0, 0, EX_RED, "list size 4 bytes short (no word padding)"),
    CASE("cb_nofinal", "cmdlist", 2, "HANG (no FINALIZE)", K_CMD, 3, 0, 0, 0, 0, EX_RED, "FINALIZE write cut off the end"),

    /* registers */
    CASE("rg_undef06", "register", 2, "?", K_RAWREG, 0, 0x0006, 0xF, 1, 0x12345678, EX_RED, "write to register 0x0006 (not defined)"),
    CASE("rg_undef2e0", "register", 2, "?", K_RAWREG, 0, 0x02E0, 0xF, 1, 0x12345678, EX_RED, "write to register 0x02E0 (not defined)"),
    CASE("rg_past", "register", 2, "?", K_RAWREG, 0, 0x03F0, 0xF, 1, 0x12345678, EX_RED, "write to register 0x03F0 (past the file)"),
    CASE("rg_span", "register", 2, "?", K_RAWREG, 0, 0x02FE, 0xF, 4, 0, EX_RED, "consecutive write from 0x02FE across the end of the file"),

    /* render target */
    CASE("rt_linear", "target", 1, "?", K_RT, 1, 64, 64, 0, 0, EX_RED, "render target in linear memory, not VRAM"),
    CASE("rt_dim60", "target", 2, "?", K_RT, 0, 60, 60, 0, 0, EX_RED, "frame buffer 60x60 (not a multiple of 8)"),
    CASE("rt_dim80", "target", 1, "WRONG (writes past the target)", K_RT, 0, 80, 80, 0, 0, EX_RED, "frame buffer 80x80 on a 64x64 allocation: must show the guard check works"),

    /* risky: heap memory, no shader */
    CASE("sh_none", "shader", 2, "?", K_NOSHADER, 0, 0, 0, 0, 0, EX_RED, "no vertex shader set up: every vertex-shader register write removed from the list"),
    CASE("vb_heap", "buffer", 3, "?", K_VBUF, 2, 0, 0, 0, 0, EX_RED, "attribute buffer in the normal heap (no physical address)"),
    CASE("ib_heap", "buffer", 3, "?", K_IBUF, 1, 0, 1, 0, 0, EX_RED, "index buffer in the normal heap"),
    CASE("tex_heap", "texture", 3, "?", K_TEX, TF_RGBA8, 0, 8, 8, 1, EX_RED, "texture in the normal heap"),
    CASE("rt_deleted", "frame", 3, "?", K_RTDEL, 0, 0, 0, 0, 0, EX_RED, "citro3d: render target deleted inside the frame (must stay last)"),
};
#define NCASES ((int)(sizeof(k_cases) / sizeof(k_cases[0])))

static const Case k_selftest = CASE("self_timeout", "selftest", 0, "HANG", K_SELFTEST, 0, 0, 0, 0, 0, EX_RED,
                                    "never submits: tests the timeout and the reset");

/* ---- state ---- */

typedef struct {
    ResCode code;
    int ms;
    char note[96];
    bool began;
    bool done;
} Journal;

static struct {
    GpuProbeConfig cfg;
    char dir[160];
    int timeout_ms;
    LightEvent ev[GSPGPU_EVENT_MAX];
    DVLB_s* dvlb;
    shaderProgram_s prog[NSHADER];
    int uproj[NSHADER];
    u32* cmd;
    u8* blk_v;  /* VRAM block */
    u8* blk_l;  /* linear block (render target in FCRAM) */
    u8* stage;  /* linear copy of a block */
    u8* vb;     /* linear, 64 KiB */
    u8* ib;     /* linear, 4 KiB */
    u8* tex;    /* linear, TEX_SMALL */
    bool dead;
    bool c3d_ready;
    int resets;
    char reset_note[64];
    Journal* jr;
} g;

static void plog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g.cfg.log != NULL) {
        g.cfg.log(line, g.cfg.user);
    } else {
        printf("%s\n", line);
    }
}

static void ev_cb(void* p) { LightEvent_Signal((LightEvent*)p); }

static void flush_dc(const void* p, u32 n) { GSPGPU_FlushDataCache(p, n); }
static void inval_dc(const void* p, u32 n) { GSPGPU_InvalidateDataCache(p, n); }

static void res_set(Res* r, ResCode c, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void res_set(Res* r, ResCode c, const char* fmt, ...) {
    r->code = c;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->note, sizeof(r->note), fmt, ap);
    va_end(ap);
}

/* ---- GX operations with a timeout ---- */

static bool ev_wait(GSPGPU_Event e, int ms) { return LightEvent_WaitTimeout(&g.ev[e], (s64)ms * 1000000LL) == 0; }

static bool gx_fill(void* dst, u32 n, u32 value) {
    LightEvent_Clear(&g.ev[GSPGPU_EVENT_PSC0]);
    Result rc = GX_MemoryFill((u32*)dst, value, (u32*)((u8*)dst + n), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL, 0, NULL, 0);
    return R_SUCCEEDED(rc) && ev_wait(GSPGPU_EVENT_PSC0, g.timeout_ms);
}

static bool gx_copy(void* dst, const void* src, u32 n) {
    LightEvent_Clear(&g.ev[GSPGPU_EVENT_PPF]);
    Result rc = GX_TextureCopy((u32*)src, 0, (u32*)dst, 0, n, GX_TRANSFER_RAW_COPY(1));
    return R_SUCCEEDED(rc) && ev_wait(GSPGPU_EVENT_PPF, g.timeout_ms);
}

/* ---- init, reset ---- */

static void events_up(void) {
    for (int i = 0; i < GSPGPU_EVENT_MAX; i++) {
        LightEvent_Init(&g.ev[i], RESET_STICKY);
    }
    gspSetEventCallback(GSPGPU_EVENT_PSC0, ev_cb, &g.ev[GSPGPU_EVENT_PSC0], false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, ev_cb, &g.ev[GSPGPU_EVENT_PPF], false);
    gspSetEventCallback(GSPGPU_EVENT_P3D, ev_cb, &g.ev[GSPGPU_EVENT_P3D], false);
}

static bool c3d_up(bool keep_queue) {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return false;
    }
    if (!keep_queue) {
        /* citro3d binds its own GX queue, emptied only by C3D_FrameBegin; every GX call here goes to GSP. */
        GX_BindQueue(NULL);
    }
    g.c3d_ready = true;
    return true;
}

static void c3d_down(void) {
    if (g.c3d_ready) {
        C3D_Fini();
        g.c3d_ready = false;
    }
}

static bool gpu_setup(void) {
    events_up();
    if (!c3d_up(false)) {
        return false;
    }
    g.dvlb = DVLB_ParseFile((u32*)gpuprobe_shbin, gpuprobe_shbin_size);
    if (g.dvlb == NULL || g.dvlb->numDVLE < NSHADER) {
        plog("gpuprobe: shader binary has %d entries, need %d", g.dvlb ? (int)g.dvlb->numDVLE : 0, NSHADER);
        return false;
    }
    for (int i = 0; i < NSHADER; i++) {
        shaderProgramInit(&g.prog[i]);
        shaderProgramSetVsh(&g.prog[i], &g.dvlb->DVLE[i]);
        g.uproj[i] = shaderInstanceGetUniformLocation(g.prog[i].vertexShader, "projection");
    }
    g.cmd = linearMemAlign(CMD_BYTES, 64);
    g.blk_v = vramMemAlign(BLK_BYTES, 4096);
    g.blk_l = linearMemAlign(BLK_BYTES, 4096);
    g.stage = linearMemAlign(BLK_BYTES, 4096);
    g.vb = linearMemAlign(64 * 1024, 256);
    g.ib = linearMemAlign(4096, 256);
    g.tex = linearMemAlign(TEX_SMALL, 256);
    return g.cmd && g.blk_v && g.blk_l && g.stage && g.vb && g.ib && g.tex;
}

static void gpu_teardown(void) {
    c3d_down();
    for (int i = 0; i < NSHADER && g.dvlb != NULL; i++) {
        shaderProgramFree(&g.prog[i]);
    }
    if (g.dvlb != NULL) {
        DVLB_Free(g.dvlb);
        g.dvlb = NULL;
    }
    if (g.cmd) linearFree(g.cmd);
    if (g.blk_v) vramFree(g.blk_v);
    if (g.blk_l) linearFree(g.blk_l);
    if (g.stage) linearFree(g.stage);
    if (g.vb) linearFree(g.vb);
    if (g.ib) linearFree(g.ib);
    if (g.tex) linearFree(g.tex);
}

static Res run_scene_ctl(void);

/* After a hang: GSPGPU_ResetGpuCore, then a control draw; if the GPU is still dead, restart gfx and citro3d. */
static bool gpu_recover(void) {
    g.resets++;
    Result rc = GSPGPU_ResetGpuCore();
    LightEvent_Clear(&g.ev[GSPGPU_EVENT_P3D]);
    Res r = run_scene_ctl();
    if (r.code == R_OK) {
        snprintf(g.reset_note, sizeof(g.reset_note), "reset rc=%08lX ok", (unsigned long)rc);
        return true;
    }
    plog("gpuprobe: GPU did not answer after ResetGpuCore (rc=%08lX); restarting gfx and citro3d", (unsigned long)rc);
    c3d_down();
    gfxExit();
    gfxInitDefault();
    events_up();
    bool up = c3d_up(false);
    r = up ? run_scene_ctl() : r;
    snprintf(g.reset_note, sizeof(g.reset_note), "reset rc=%08lX, gfx restart %s", (unsigned long)rc,
             r.code == R_OK ? "ok" : "failed");
    return r.code == R_OK;
}

/* ---- scene ---- */

typedef struct {
    int shader;
    int boolreg;
    bool textured;
    bool rt_linear;
    int fbw, fbh;
    /* viewport */
    u32 vp[4];
    /* scissor: mode -1 leaves citro3d's default */
    int sc_mode;
    u32 sc[4];
    /* vertex buffer */
    int vb_mode; /* 0 offset register, 1 base address, 2 heap */
    int vb_k;
    bool attr13;
    /* draw */
    int draw; /* DRAW_* */
    int count;
    int ib_off;
    bool ib_heap;
    /* texture */
    void* tdata;
    int tfmt;
    int tw, th;
    u32 tbytes;
    /* list tail / patches */
    int raw_reg, raw_mask, raw_n;
    u32 raw_val;
    int cmd_mode;
    bool noshader;
    Expect exp;
    const char* id;
} Scene;

static void scene_default(Scene* s, const Case* c) {
    memset(s, 0, sizeof(*s));
    s->shader = 0;
    s->boolreg = -1;
    s->fbw = s->fbh = RTW;
    s->vp[2] = s->vp[3] = RTW;
    s->sc_mode = -1;
    s->draw = DRAW_ARR;
    s->count = 3;
    s->exp = c->exp;
    s->id = c->id;
}

static void make_vertices(Vtx* v) {
    /* a triangle that covers the whole 64x64 target, repeated, so any count is a full or partial copy of it */
    for (int i = 0; i < NVERT; i++) {
        Vtx t = {0.0f, 0.0f, 0.0f, {255, 0, 0, 255}, 0.0f, 0.0f};
        switch (i % 3) {
        case 1:
            t.x = 160.0f;
            t.u = 2.5f;
            break;
        case 2:
            t.y = 160.0f;
            t.v = 2.5f;
            break;
        }
        v[i] = t;
    }
}

static u32 tile_off(u32 x, u32 y, u32 w) {
    u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m;
}

static bool near_px(u32 a, u32 b, int tol) {
    for (int s = 0; s < 32; s += 8) {
        int d = (int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF);
        if (d > tol || d < -tol) {
            return false;
        }
    }
    return true;
}

static void set_proj(int shader) {
    C3D_Mtx m;
    Mtx_Ortho(&m, 0.0f, (float)RTW, 0.0f, (float)RTW, -1.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uproj[shader], &m);
}

/* Words of a command at i, and the index of the next one (list format: see gpu_cmd_diff.py). */
static u32 cmd_next(const u32* w, u32 i) {
    u32 extra = (w[i + 1] >> 20) & 0xFF;
    return i + 2 + extra + ((extra + 2) & 1);
}

static void kill_vsh_writes(u32* w, u32 n) {
    for (u32 i = 0; i + 1 < n; i = cmd_next(w, i)) {
        u32 reg = w[i + 1] & 0xFFFF;
        if ((reg >= 0x2B9 && reg <= 0x2DD) || reg == 0x242) {
            w[i + 1] &= ~(0xFu << 16);
        }
    }
}

static void dump_list(const char* id, const u32* w, u32 bytes) {
    char path[220];
    snprintf(path, sizeof(path), "%s/gpuprobe/%s.bin", g.dir, id);
    FILE* f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(w, 1, bytes, f);
        fclose(f);
    }
}

static Res classify(const Scene* s, bool timed_out, Result rc) {
    Res r = {R_OK, ""};
    if (R_FAILED(rc)) {
        res_set(&r, R_ERR, "GX_ProcessCommandList rc=%08lX", (unsigned long)rc);
        return r;
    }
    if (timed_out) {
        res_set(&r, R_HANG, "no P3D completion in %d ms", g.timeout_ms);
        return r;
    }
    u8* blk = s->rt_linear ? g.blk_l : g.blk_v;
    if (!gx_copy(g.stage, blk, BLK_BYTES)) {
        res_set(&r, R_HANG, "readback copy timed out");
        return r;
    }
    inval_dc(g.stage, BLK_BYTES);
    const u32* w = (const u32*)g.stage;
    u32 guard_bad = 0, first_bad = 0;
    for (u32 i = 0; i < BLK_BYTES / 4; i++) {
        if (i >= RT_OFF / 4 && i < (RT_OFF + RT_BYTES) / 4) {
            continue;
        }
        if (w[i] != SENT) {
            if (!guard_bad) {
                first_bad = i * 4;
            }
            guard_bad++;
        }
    }
    static const u32 colx[4] = {8, 24, 40, 56};
    u32 want[4];
    for (int i = 0; i < 4; i++) {
        bool red = s->exp == EX_RED || (s->exp == EX_LEFT && i < 2) || (s->exp == EX_RIGHT && i >= 2);
        want[i] = red ? RED : SENT;
    }
    int bad = 0;
    u32 got0 = 0, want0 = 0;
    for (int i = 0; i < 4; i++) {
        for (u32 y = 16; y <= 48; y += 32) {
            u32 px = w[RT_OFF / 4 + tile_off(colx[i], y, RTW)];
            if (!near_px(px, want[i], 6)) {
                if (!bad) {
                    got0 = px;
                    want0 = want[i];
                }
                bad++;
            }
        }
    }
    if (guard_bad || bad) {
        char a[60] = "", b[48] = "";
        if (bad) {
            snprintf(a, sizeof(a), "%d/8 px wrong, first %08lX want %08lX", bad, (unsigned long)got0, (unsigned long)want0);
        }
        if (guard_bad) {
            snprintf(b, sizeof(b), "%s%lu guard words changed from +0x%lX", bad ? "; " : "", (unsigned long)guard_bad,
                     (unsigned long)first_bad);
        }
        res_set(&r, R_WRONG, "%s%s", a, b);
    }
    return r;
}

/* Builds the list for the scene, submits it, classifies. */
static Res run_scene(Scene* s) {
    Res r = {R_OK, ""};
    u8* blk = s->rt_linear ? g.blk_l : g.blk_v;
    if (!gx_fill(blk, BLK_BYTES, SENT)) {
        res_set(&r, R_HANG, "clearing the target timed out");
        return r;
    }

    memset(g.vb, 0, 64 * 1024);
    Vtx* heapv = NULL;
    if (s->vb_mode == 2) {
        heapv = malloc(NVERT * sizeof(Vtx) + 64);
        make_vertices(heapv);
    } else {
        /* mode 0: data at vb+k, attribute offset register k; mode 1: data and base at vb+k */
        make_vertices((Vtx*)(g.vb + s->vb_k));
    }
    flush_dc(g.vb, 64 * 1024);

    memset(g.ib, 0, 4096);
    u8* ibp = g.ib + 16 + s->ib_off; /* aligned data area starts at +16 */
    u8* ibheap = NULL;
    if (s->ib_heap) {
        ibheap = malloc(256);
        memset(ibheap, 0, 256);
    }
    u8* ibdst = s->ib_heap ? ibheap : ibp;
    if (s->draw == DRAW_E8) {
        for (int i = 0; i < 12; i++) ibdst[i] = (u8)(i % 3);
    } else {
        for (int i = 0; i < 12; i++) ((u16*)ibdst)[i] = (u16)(i % 3);
    }
    flush_dc(g.ib, 4096);

    GPUCMD_SetBuffer(g.cmd, CMD_BYTES / 4, 0);
    C3D_BindProgram(&g.prog[s->shader]);
    C3D_AttrInfo ai;
    AttrInfo_Init(&ai);
    AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai, 2, GPU_FLOAT, 2);
    if (s->attr13) {
        /* the attribute count field is attrs - 1 in bits 28..31 of the high format word */
        ai.attrCount = 13;
        ai.flags[1] = (ai.flags[1] & 0x0FFFFFFFu) | (12u << 28);
    }
    C3D_SetAttrInfo(&ai);

    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    if (s->vb_mode == 2) {
        BufInfo_Add(bi, heapv, sizeof(Vtx), 3, 0x210);
    } else if (s->vb_mode == 1) {
        BufInfo_Add(bi, g.vb + s->vb_k, sizeof(Vtx), 3, 0x210);
    } else {
        BufInfo_Add(bi, g.vb, sizeof(Vtx), 3, 0x210);
        bi->buffers[0].offset = (u32)s->vb_k;
    }

    C3D_FrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    C3D_FrameBufAttrib(&fb, (u16)s->fbw, (u16)s->fbh, false);
    C3D_FrameBufColor(&fb, blk + RT_OFF, GPU_RB_RGBA8);
    C3D_FrameBufDepth(&fb, NULL, GPU_RB_DEPTH24);
    C3D_SetFrameBuf(&fb);
    C3D_SetViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    if (s->sc_mode >= 0) {
        C3D_SetScissor((GPU_SCISSORMODE)s->sc_mode, s->sc[0], s->sc[1], s->sc[2], s->sc[3]);
    } else {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, RTW, RTW);
    }
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, s->textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    set_proj(s->shader);
    if (s->boolreg >= 0) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, s->boolreg, true);
    }
    C3D_Tex tex;
    if (s->textured) {
        memset(&tex, 0, sizeof(tex));
        tex.data = s->tdata;
        tex.fmt = s->tfmt == TF_565 ? GPU_RGB565 : s->tfmt == TF_ETC1 ? GPU_ETC1 : GPU_RGBA8;
        tex.size = s->tbytes;
        tex.width = (u16)s->tw;
        tex.height = (u16)s->th;
        tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
        C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &tex);
    }

    switch (s->draw) {
    case DRAW_ARR:
        C3D_DrawArrays(GPU_TRIANGLES, 0, s->count);
        break;
    case DRAW_STRIP:
        C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, s->count);
        break;
    case DRAW_FAN:
        C3D_DrawArrays(GPU_TRIANGLE_FAN, 0, s->count);
        break;
    case DRAW_E8:
        C3D_DrawElements(GPU_TRIANGLES, s->count, C3D_UNSIGNED_BYTE, ibdst);
        break;
    default:
        C3D_DrawElements(GPU_TRIANGLES, s->count, C3D_UNSIGNED_SHORT, ibdst);
        break;
    }

    if (s->raw_n > 0) {
        u32 vals[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        vals[0] = s->raw_val;
        if (s->raw_n > 1) {
            GPUCMD_AddMaskedIncrementalWrites((u32)s->raw_reg, (u32)s->raw_mask, vals, (u32)s->raw_n);
        } else {
            GPUCMD_AddMaskedWrite((u32)s->raw_reg, (u32)s->raw_mask, s->raw_val);
        }
    }
    if (s->cmd_mode == 1) {
        /* a dummy write moves the size between 16-byte multiples (below) */
        GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, 0);
    }
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    u32* b;
    u32 nw;
    GPUCMD_Split(&b, &nw);
    if (s->noshader) {
        kill_vsh_writes(b, nw);
    }
    u32 bytes = nw * 4;
    /* nw is a multiple of 4 words from GPUCMD_Split in libctru; cmd_mode picks the variants */
    switch (s->cmd_mode) {
    case 1:
        if ((bytes & 15) == 0) {
            bytes -= 8;
        }
        break;
    case 2:
        bytes -= 4;
        break;
    case 3:
        bytes -= 8; /* the FINALIZE write is the last command */
        break;
    default:
        break;
    }
    dump_list(s->id, b, bytes);
    flush_dc(b, nw * 4);

    LightEvent_Clear(&g.ev[GSPGPU_EVENT_P3D]);
    Result rc = GX_ProcessCommandList(b, bytes, 0);
    bool timed_out = R_SUCCEEDED(rc) && !ev_wait(GSPGPU_EVENT_P3D, g.timeout_ms);
    r = classify(s, timed_out, rc);
    if (r.code == R_OK && (s->cmd_mode == 1 || s->cmd_mode == 2)) {
        char n[48];
        snprintf(n, sizeof(n), "size %lu (mod 16 = %lu)", (unsigned long)bytes, (unsigned long)(bytes & 15));
        res_set(&r, R_OK, "%s", n);
    }
    free(heapv);
    free(ibheap);
    return r;
}

static Res run_scene_ctl(void) {
    Scene s;
    Case c = {"ctl_recover", "control", 0, "OK", K_DRAW, 0, {3, 0, 0, 0}, EX_RED, ""};
    scene_default(&s, &c);
    return run_scene(&s);
}

/* ---- textures ---- */

static u32 tex_bpp_x8(int fmt) { return fmt == TF_RGBA8 ? 32 : fmt == TF_565 ? 16 : 4; } /* bits per pixel */

static void fill_color(int fmt, u8* p, u32 bytes, bool red) {
    if (fmt == TF_RGBA8) {
        u32 w = red ? 0xFF0000FFu : 0x00FF00FFu;
        for (u32 i = 0; i + 4 <= bytes; i += 4) memcpy(p + i, &w, 4);
    } else if (fmt == TF_565) {
        u16 w = red ? 0xF800 : 0x07E0;
        for (u32 i = 0; i + 2 <= bytes; i += 2) memcpy(p + i, &w, 2);
    } else {
        /* ETC1, differential mode, all pixel indices 0: base colour + 2. PICA stores the 8 bytes reversed. */
        static const u8 r_blk[8] = {0, 0, 0, 0, 0x02, 0, 0, 0xF8};
        static const u8 g_blk[8] = {0, 0, 0, 0, 0x02, 0, 0xF8, 0};
        for (u32 i = 0; i + 8 <= bytes; i += 8) memcpy(p + i, red ? r_blk : g_blk, 8);
    }
}

/* ---- case dispatch ---- */

static Res run_citro3d_frame(void);

static Res run_case(const Case* c) {
    Scene s;
    scene_default(&s, c);
    switch (c->kind) {
    case K_DRAW:
        s.draw = c->a;
        s.count = c->p[0];
        break;
    case K_SCISSOR:
        s.sc_mode = c->a;
        for (int i = 0; i < 4; i++) s.sc[i] = (u32)c->p[i];
        break;
    case K_VIEWPORT:
        for (int i = 0; i < 4; i++) s.vp[i] = (u32)c->p[i];
        break;
    case K_SHADER:
        s.shader = c->a;
        s.boolreg = c->p[0];
        break;
    case K_NOSHADER:
        s.noshader = true;
        break;
    case K_VBUF:
        s.vb_mode = c->a;
        s.vb_k = c->p[0];
        break;
    case K_IBUF:
        s.draw = c->a ? DRAW_E16 : DRAW_E8;
        s.ib_off = c->p[0];
        s.ib_heap = c->p[1] != 0;
        break;
    case K_ATTR13:
        s.attr13 = true;
        break;
    case K_CMD:
        s.cmd_mode = c->a;
        break;
    case K_RAWREG:
        s.raw_reg = c->p[0];
        s.raw_mask = c->p[1];
        s.raw_n = c->p[2];
        s.raw_val = (u32)c->p[3];
        break;
    case K_RT:
        s.rt_linear = c->a == 1;
        s.fbw = c->p[0];
        s.fbh = c->p[1];
        break;
    case K_RTDEL:
        return run_citro3d_frame();
    case K_SELFTEST: {
        Res r = {R_OK, ""};
        LightEvent_Clear(&g.ev[GSPGPU_EVENT_P3D]);
        if (!ev_wait(GSPGPU_EVENT_P3D, g.timeout_ms)) {
            res_set(&r, R_HANG, "nothing submitted, timeout as designed");
        }
        return r;
    }
    case K_TEX: {
        int fmt = c->a, k = c->p[0], w = c->p[1], h = c->p[2];
        s.textured = true;
        s.tfmt = fmt;
        s.tw = w;
        s.th = h;
        u32 w8 = ((u32)w + 7) & ~7u, h8 = ((u32)h + 7) & ~7u;
        u32 bytes = w8 * h8 * tex_bpp_x8(fmt) / 8;
        if (bytes < 8) bytes = 8;
        s.tbytes = bytes;
        u8* mem = NULL;
        bool own = false, heap = c->p[3] != 0;
        if (bytes + 512 <= TEX_SMALL && !heap) {
            mem = g.tex;
            fill_color(fmt, mem, TEX_SMALL, false);
            fill_color(fmt, mem + k, bytes, true);
            flush_dc(mem, TEX_SMALL);
            s.tdata = mem + k;
        } else {
            u32 total = bytes + 512;
            mem = heap ? malloc(total) : linearMemAlign(total, 256);
            own = true;
            if (mem == NULL) {
                Res r = {R_SKIP, "allocation failed"};
                return r;
            }
            fill_color(fmt, mem, total, true);
            if (!heap) flush_dc(mem, total);
            s.tdata = mem;
        }
        Res r = run_scene(&s);
        if (own) {
            if (heap) free(mem); else linearFree(mem);
        }
        return r;
    }
    }
    return run_scene(&s);
}

/* citro3d's own frame path: target created and deleted inside a frame. Runs on a thread so a hang
 * in citro3d's wait is a timeout here. Must be the last case: a stuck thread is never reclaimed. */
static volatile int s_frame_done;

static void frame_thread(void* arg) {
    (void)arg;
    C3D_RenderTarget* t = C3D_RenderTargetCreate(RTW, RTW, GPU_RB_RGBA8, -1);
    if (t == NULL) {
        s_frame_done = -1;
        return;
    }
    C3D_FrameBegin(0);
    C3D_FrameDrawOn(t);
    C3D_BindProgram(&g.prog[0]);
    C3D_AttrInfo ai;
    AttrInfo_Init(&ai);
    AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai, 2, GPU_FLOAT, 2);
    C3D_SetAttrInfo(&ai);
    make_vertices((Vtx*)g.vb);
    flush_dc(g.vb, 4096);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.vb, sizeof(Vtx), 3, 0x210);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    set_proj(0);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    C3D_RenderTargetDelete(t);
    C3D_FrameEnd(0);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW); /* waits for the GPU to finish the first frame */
    C3D_FrameEnd(0);
    C3D_FrameSync();
    s_frame_done = 1;
}

static Res run_citro3d_frame(void) {
    Res r = {R_OK, "no pixel check (target freed)"};
    c3d_down();
    if (!c3d_up(true)) {
        res_set(&r, R_ERR, "C3D_Init failed");
        return r;
    }
    s_frame_done = 0;
    Thread th = threadCreate(frame_thread, NULL, 64 * 1024, 0x30, -1, false);
    if (th == NULL) {
        res_set(&r, R_ERR, "thread failed");
        return r;
    }
    int limit = g.timeout_ms < 2000 ? 2000 : g.timeout_ms;
    for (int waited = 0; waited < limit && s_frame_done == 0; waited += 10) {
        svcSleepThread(10 * 1000000LL);
    }
    if (s_frame_done == 0) {
        res_set(&r, R_HANG, "frame did not finish in %d ms", limit);
        return r;
    }
    if (s_frame_done < 0) {
        res_set(&r, R_ERR, "render target creation failed");
    }
    threadJoin(th, U64_MAX);
    threadFree(th);
    return r;
}

/* ---- journal and table ---- */

static void path_of(char* out, size_t n, const char* name) { snprintf(out, n, "%s/%s", g.dir, name); }

static int case_index(const Case* list, int n, const char* id) {
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static void journal_load(const Case* list, int n) {
    char path[200];
    path_of(path, sizeof(path), "gpuprobe.journal");
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[220];
    while (fgets(line, sizeof(line), f) != NULL) {
        char kind = line[0];
        char id[40];
        if (kind == 'B' && sscanf(line + 2, "%39s", id) == 1) {
            int i = case_index(list, n, id);
            if (i >= 0) {
                g.jr[i].began = true;
                g.jr[i].done = false;
            }
        } else if (kind == 'R') {
            char res[12];
            int ms = 0, used = 0;
            if (sscanf(line + 2, "%39s %11s %d %n", id, res, &ms, &used) >= 3) {
                int i = case_index(list, n, id);
                if (i >= 0) {
                    g.jr[i].done = true;
                    g.jr[i].ms = ms;
                    g.jr[i].code = R_NONE;
                    for (int k = 0; k < 6; k++) {
                        if (strcmp(res, k_res_name[k]) == 0) {
                            g.jr[i].code = (ResCode)k;
                        }
                    }
                    const char* note = line + 2 + used;
                    snprintf(g.jr[i].note, sizeof(g.jr[i].note), "%s", note);
                    g.jr[i].note[strcspn(g.jr[i].note, "\r\n")] = '\0';
                }
            }
        }
    }
    fclose(f);
}

static void journal_append(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void journal_append(const char* fmt, ...) {
    char path[200], line[240];
    path_of(path, sizeof(path), "gpuprobe.journal");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    FILE* f = fopen(path, "a");
    if (f != NULL) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f); /* closed per line: a crash or freeze keeps the "B" line */
    }
}

static void table_write(const Case* list, int n) {
    char path[200];
    path_of(path, sizeof(path), "gpuprobe.txt");
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%-14s %-9s %-4s %-32s %-6s %6s  %s\n", "case", "group", "dng", "expected", "result", "ms", "note");
    for (int i = 0; i < n; i++) {
        const Journal* j = &g.jr[i];
        fprintf(f, "%-14s %-9s %-4d %-32s %-6s %6d  %s\n", list[i].id, list[i].group, list[i].danger, list[i].predict,
                j->done ? k_res_name[j->code] : "-", j->ms, j->note);
    }
    fclose(f);
}

static bool selected(const Case* c, const char* only) {
    if (only == NULL || only[0] == '\0') {
        return true;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", only);
    for (char* tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        size_t l = strlen(tok);
        if (l > 0 && tok[l - 1] == '*') {
            if (strncmp(c->id, tok, l - 1) == 0) {
                return true;
            }
        } else if (strcmp(c->id, tok) == 0 || strcmp(c->group, tok) == 0) {
            return true;
        }
    }
    return false;
}

static int cmp_danger(const void* a, const void* b) {
    const Case* x = *(const Case* const*)a;
    const Case* y = *(const Case* const*)b;
    return x->danger - y->danger;
}

int gpuprobe_run(const GpuProbeConfig* cfg, GpuProbeSummary* out) {
    memset(&g, 0, sizeof(g));
    g.cfg = *cfg;
    snprintf(g.dir, sizeof(g.dir), "%s", cfg->dir != NULL ? cfg->dir : "sdmc:/3ds/ctrdk");
    g.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : cfg->emulator ? 5000 : 200;
    mkdir("sdmc:/3ds", 0777);
    mkdir(g.dir, 0777);
    char sub[200];
    path_of(sub, sizeof(sub), "gpuprobe");
    mkdir(sub, 0777);

    GpuProbeSummary sum;
    memset(&sum, 0, sizeof(sum));

    /* the run list: selftest first, then by danger (stable), rt_deleted stays last */
    static const Case* order[NCASES + 1];
    int n = 0;
    if (cfg->selftest) {
        order[n++] = &k_selftest;
    }
    const Case* sorted[NCASES];
    for (int i = 0; i < NCASES; i++) sorted[i] = &k_cases[i];
    /* qsort is not stable: sort by (danger, index) */
    for (int i = 1; i < NCASES; i++) {
        const Case* t = sorted[i];
        int j = i - 1;
        while (j >= 0 && cmp_danger(&sorted[j], &t) > 0) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = t;
    }
    for (int i = 0; i < NCASES; i++) {
        order[n++] = sorted[i];
    }
    /* flat list for the journal and the table, in run order */
    static Case flat[NCASES + 1];
    for (int i = 0; i < n; i++) flat[i] = *order[i];
    g.jr = calloc((size_t)n, sizeof(Journal));
    if (g.jr == NULL) {
        return -1;
    }
    char jpath[200];
    path_of(jpath, sizeof(jpath), "gpuprobe.journal");
    if (cfg->fresh) {
        remove(jpath);
    } else {
        journal_load(flat, n);
    }

    plog("gpuprobe: %d cases, timeout %d ms, journal %s%s", n, g.timeout_ms, jpath, cfg->fresh ? " (fresh)" : "");
    if (!gpu_setup()) {
        plog("gpuprobe: GPU setup failed");
        gpu_teardown();
        free(g.jr);
        return -1;
    }
    {
        Res r = run_scene_ctl();
        plog("gpuprobe: warm-up control draw: %s %s", k_res_name[r.code], r.note);
        r = run_scene_ctl();
        if (r.code != R_OK) {
            plog("gpuprobe: the control draw does not give OK; results below are not reliable");
        }
    }

    for (int i = 0; i < n; i++) {
        const Case* c = &flat[i];
        Journal* j = &g.jr[i];
        sum.total++;
        if (!selected(c, cfg->only)) {
            continue;
        }
        if (j->began && !j->done) {
            /* the last run died inside this case */
            j->code = R_FAULT;
            j->done = true;
            snprintf(j->note, sizeof(j->note), "the process died or froze in this case (journal)");
            journal_append("R %s FAULT 0 %s", c->id, j->note);
            plog("gpuprobe: %-14s FAULT (from the journal; not run again)", c->id);
        }
        if (j->done) {
            sum.cached++;
            continue;
        }
        if (g.dead) {
            j->code = R_SKIP;
            j->done = true;
            snprintf(j->note, sizeof(j->note), "GPU did not come back after a reset");
            journal_append("R %s SKIP 0 %s", c->id, j->note);
            continue;
        }
        if (cfg->no_dangerous && c->danger >= 3) {
            j->code = R_SKIP;
            j->done = true;
            snprintf(j->note, sizeof(j->note), "danger 3 skipped by request");
            journal_append("R %s SKIP 0 %s", c->id, j->note);
            continue;
        }
        journal_append("B %s", c->id);
        j->began = true;
        u64 t0 = svcGetSystemTick();
        g.reset_note[0] = '\0';
        Res r = run_case(c);
        int ms = (int)((double)(svcGetSystemTick() - t0) / TICKS_PER_MS);
        if (r.code == R_HANG) {
            if (!gpu_recover()) {
                g.dead = true;
            }
            snprintf(r.note + strlen(r.note), sizeof(r.note) - strlen(r.note), "; %s", g.reset_note);
        }
        j->code = r.code;
        j->ms = ms;
        j->done = true;
        snprintf(j->note, sizeof(j->note), "%s", r.note);
        journal_append("R %s %s %d %s", c->id, k_res_name[r.code], ms, r.note);
        plog("gpuprobe: %-14s %-6s %5d ms  [%s] %s", c->id, k_res_name[r.code], ms, c->predict, r.note);
        if (g.dead) {
            plog("gpuprobe: the GPU is dead after %s; remaining cases are skipped", c->id);
        }
        table_write(flat, n);
    }
    table_write(flat, n);

    for (int i = 0; i < n; i++) {
        if (!g.jr[i].done) {
            continue;
        }
        switch (g.jr[i].code) {
        case R_OK: sum.ok++; break;
        case R_WRONG: sum.wrong++; break;
        case R_HANG: sum.hang++; break;
        case R_FAULT: sum.fault++; break;
        case R_ERR: sum.err++; break;
        case R_SKIP: sum.skip++; break;
        default: break;
        }
    }
    plog("gpuprobe: done: %d OK, %d WRONG, %d HANG, %d FAULT, %d ERR, %d SKIP (%d from the journal); GPU resets %d",
         sum.ok, sum.wrong, sum.hang, sum.fault, sum.err, sum.skip, sum.cached, g.resets);
    plog("gpuprobe: table %s/gpuprobe.txt", g.dir);
    if (out != NULL) {
        *out = sum;
    }
    int bad = sum.wrong + sum.hang + sum.fault + sum.err;
    /* the frame test may leave a stuck citro3d thread: leave the GPU objects alone then */
    if (!g.dead && s_frame_done != 0) {
        gpu_teardown();
    }
    free(g.jr);
    return bad;
}
