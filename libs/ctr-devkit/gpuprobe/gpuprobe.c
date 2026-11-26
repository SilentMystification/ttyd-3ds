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
#define MAIN_BYTES (64 * 1024)  /* hand-built main lists (K_CB, K_THRU) */
#define CHUNK_BYTES (32 * 1024) /* chunks they jump to or call */
#define NEST_B_OFF (16 * 1024)  /* cb_call_nest: chunk B inside the chunk buffer */
#define MAXCALL 512

typedef enum { R_OK, R_WRONG, R_HANG, R_FAULT, R_ERR, R_SKIP, R_NONE } ResCode;
static const char* const k_res_name[] = {"OK", "WRONG", "HANG", "FAULT", "ERR", "SKIP", "-"};

typedef struct {
    ResCode code;
    char note[96];
} Res;

typedef enum {
    K_DRAW, K_SCISSOR, K_VIEWPORT, K_SHADER, K_NOSHADER, K_VBUF, K_IBUF, K_ATTR13, K_TEX, K_CMD, K_RAWREG, K_RT,
    K_RTDEL, K_SELFTEST, K_CB, K_THRU, K_DT
} Kind;

/* EX_COUNT: exactly Scene.want_red red pixels in the target, the rest untouched */
typedef enum { EX_RED, EX_CLEAR, EX_LEFT, EX_RIGHT, EX_COUNT } Expect;

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

#define SC_NORMAL 3 /* GPU_SCISSOR_NORMAL: draw inside the rectangle */
#define DRAW_ARR 0
#define DRAW_E16 1
#define DRAW_E8 2
#define DRAW_STRIP 3
#define DRAW_FAN 4
#define TF_565 0
#define TF_RGBA8 1
#define TF_ETC1 2
/* K_CB modes (a) */
#define CB_JUMP0 1
#define CB_JUMP1 2
#define CB_CALL 3
#define CB_NEST 4
#define CB_IRQ 5
/* K_CB chunk placement (p[3]) */
#define PL_LIN 0
#define PL_VRAM 1
#define PL_QTM 2
/* K_VBUF / K_IBUF buffer in QTM */
#define VB_QTM 3
#define IB_QTM 2

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

    /* command processor: jump and two-channel call (GX native plan step 0). Call: the caller sets ADDR0/SIZE0
     * to its own continuation and ADDR1/SIZE1 to the chunk, writes JUMP1; the chunk ends with a JUMP0 write.
     * p: calls, chunk address offset (bytes past a 64-byte boundary), chunk size mode, placement */
    CASE("cb_jump0", "cmdlist", 2, "?", K_CB, CB_JUMP0, 0, 0, 0, PL_LIN, EX_RED, "main list jumps (JUMP0) to a chunk holding state, draw and FINALIZE"),
    CASE("cb_jump1", "cmdlist", 2, "?", K_CB, CB_JUMP1, 0, 0, 0, PL_LIN, EX_RED, "main list jumps (JUMP1) to a chunk holding state, draw and FINALIZE"),
    CASE("cb_call1", "cmdlist", 2, "?", K_CB, CB_CALL, 1, 0, 0, PL_LIN, EX_RED, "call: channel 1 call of a draw chunk, channel 0 return to the main list, 1 call"),
    CASE("cb_call500", "cmdlist", 2, "?", K_CB, CB_CALL, 500, 0, 0, PL_LIN, EX_COUNT, "500 calls of one static draw chunk, a 1x1 scissor per call: exactly 500 pixels"),
    CASE("cb_call_nest", "cmdlist", 2, "?", K_CB, CB_NEST, 1, 0, 0, PL_LIN, EX_RED, "main calls A, A calls the draw chunk B (return into A's tail), A's tail jumps back to main"),
    CASE("cb_call_a8", "cmdlist", 2, "?", K_CB, CB_CALL, 1, 8, 0, PL_LIN, EX_RED, "call: chunk address 8 bytes past a 16-byte boundary, size a multiple of 16"),
    CASE("cb_call_a16", "cmdlist", 2, "?", K_CB, CB_CALL, 1, 16, 0, PL_LIN, EX_RED, "call: chunk address 16 bytes past a 64-byte boundary"),
    CASE("cb_call_size8", "cmdlist", 2, "?", K_CB, CB_CALL, 1, 0, 1, PL_LIN, EX_RED, "call: chunk size 8 bytes past a multiple of 16"),
    CASE("cb_call_size16", "cmdlist", 2, "?", K_CB, CB_CALL, 1, 0, 2, PL_LIN, EX_RED, "call: chunk size 16 bytes past a multiple of 32"),
    CASE("cb_vram", "cmdlist", 2, "?", K_CB, CB_CALL, 1, 0, 0, PL_VRAM, EX_RED, "call: chunk in VRAM, written there by TextureCopy"),
    CASE("cb_irq", "cmdlist", 2, "?", K_CB, CB_IRQ, 0, 0, 0, PL_LIN, EX_RED, "draw in the main list, then JUMP0 to a chunk with only flush and FINALIZE: one P3D"),
    CASE("cp_thru", "thru", 1, "OK (timing)", K_THRU, 0, 0, 0, 0, 0, EX_RED, "command-processor time: 8 K words as single writes, bursts, and 8 calls of a 1 K chunk"),

    /* DisplayTransfer linear -> tiled, same format in and out, sizes 8..256, vs the app's CPU Morton tiler */
    CASE("dt_tile_rgba8", "dt", 2, "?", K_DT, 0, 0, 0, 0, 0, EX_RED, "DisplayTransfer RGBA8 linear to tiled, byte compare with the CPU tiler"),
    CASE("dt_tile_rgb8", "dt", 2, "?", K_DT, 1, 0, 0, 0, 0, EX_RED, "DisplayTransfer RGB8 linear to tiled (probe's own tiler as reference)"),
    CASE("dt_tile_rgb565", "dt", 2, "?", K_DT, 2, 0, 0, 0, 0, EX_RED, "DisplayTransfer RGB565 linear to tiled, byte compare with the CPU tiler"),
    CASE("dt_tile_rgba5551", "dt", 2, "?", K_DT, 3, 0, 0, 0, 0, EX_RED, "DisplayTransfer RGBA5551 linear to tiled, byte compare with the CPU tiler"),
    CASE("dt_tile_rgba4", "dt", 2, "?", K_DT, 4, 0, 0, 0, 0, EX_RED, "DisplayTransfer RGBA4 linear to tiled, byte compare with the CPU tiler"),

    /* New 3DS extra RAM (QTM, physical 0x1F000000): does each GPU unit fetch from it (Azahar: no evidence) */
    CASE("cb_qtm", "qtm", 3, "?", K_CB, CB_CALL, 1, 0, 0, PL_QTM, EX_RED, "call: chunk in QTM (address 0x1F000000>>3 range), CPU written, GSP flush"),
    CASE("vb_qtm", "qtm", 3, "?", K_VBUF, VB_QTM, 0, 0, 0, 0, EX_RED, "vertex buffer in QTM"),
    CASE("ib_qtm", "qtm", 3, "?", K_IBUF, 1, 0, IB_QTM, 0, 0, EX_RED, "u16 index buffer in QTM"),
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
    u32* main;  /* linear, MAIN_BYTES */
    u32* chunk; /* linear, CHUNK_BYTES */
    u32* cvram; /* VRAM, CHUNK_BYTES (cb_vram) */
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
    g.main = linearMemAlign(MAIN_BYTES, 64);
    g.chunk = linearMemAlign(CHUNK_BYTES, 64);
    g.cvram = vramMemAlign(CHUNK_BYTES, 64);
    plog("gpuprobe: memory (virt/phys): cmd %p/%08lX vram %p/%08lX lin-target %p/%08lX vb %p/%08lX ib %p/%08lX tex %p/%08lX",
         (void*)g.cmd, (unsigned long)osConvertVirtToPhys(g.cmd), (void*)g.blk_v,
         (unsigned long)osConvertVirtToPhys(g.blk_v), (void*)g.blk_l, (unsigned long)osConvertVirtToPhys(g.blk_l),
         (void*)g.vb, (unsigned long)osConvertVirtToPhys(g.vb), (void*)g.ib, (unsigned long)osConvertVirtToPhys(g.ib),
         (void*)g.tex, (unsigned long)osConvertVirtToPhys(g.tex));
    return g.cmd && g.blk_v && g.blk_l && g.stage && g.vb && g.ib && g.tex && g.main && g.chunk && g.cvram;
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
    if (g.main) linearFree(g.main);
    if (g.chunk) linearFree(g.chunk);
    if (g.cvram) vramFree(g.cvram);
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
    int vb_mode; /* 0 offset register, 1 base address, 2 heap, VB_QTM */
    int vb_k;
    bool attr13;
    /* draw */
    int draw; /* DRAW_* */
    int count;
    int ib_off;
    bool ib_heap;
    bool ib_qtm;
    /* command-processor cases (K_CB): mode CB_*, calls, chunk offset, size mode, placement PL_* */
    int cb, cb_calls, cb_off, cb_size, cb_place;
    int want_red; /* EX_COUNT */
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
            t.x = 128.0f;
            t.u = 2.0f;
            break;
        case 2:
            t.y = 128.0f;
            t.v = 2.0f;
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
    static bool ctl_done;
    if (strcmp(id, "ctl_recover") == 0) {
        if (ctl_done) {
            return; /* the first control list (it carries the shader upload) is enough */
        }
        ctl_done = true;
    }
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
    /* CPU read of the block (VRAM is readable): the emulator's render cache is flushed by a CPU read, not by a GX copy. */
    inval_dc(blk, BLK_BYTES);
    memcpy(g.stage, blk, BLK_BYTES);
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
    if (s->exp == EX_COUNT) {
        u32 red = 0, other = 0;
        for (u32 i = RT_OFF / 4; i < (RT_OFF + RT_BYTES) / 4; i++) {
            if (near_px(w[i], RED, 6)) {
                red++;
            } else if (w[i] != SENT) {
                other++;
            }
        }
        if (red != (u32)s->want_red || other != 0 || guard_bad != 0) {
            res_set(&r, R_WRONG, "%lu red px (want %d), %lu other px, %lu guard words changed", (unsigned long)red,
                    s->want_red, (unsigned long)other, (unsigned long)guard_bad);
        } else {
            res_set(&r, R_OK, "%lu red px", (unsigned long)red);
        }
        return r;
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
        char a[80] = "", b[48] = "";
        if (bad) {
            u32 changed = 0;
            for (u32 i = RT_OFF / 4; i < (RT_OFF + RT_BYTES) / 4; i++) {
                changed += w[i] != SENT;
            }
            snprintf(a, sizeof(a), "%d/8 px wrong, first %08lX want %08lX, %lu/%d words drawn", bad,
                     (unsigned long)got0, (unsigned long)want0, (unsigned long)changed, RT_BYTES / 4);
        }
        if (guard_bad) {
            snprintf(b, sizeof(b), "%s%lu guard words changed from +0x%lX", bad ? "; " : "", (unsigned long)guard_bad,
                     (unsigned long)first_bad);
        }
        res_set(&r, R_WRONG, "%s%s", a, b);
    }
    return r;
}

static Res cb_run(Scene* s, const u32* rec, u32 m, u32 e);

/* QTM bytes used by the buffer cases: vertices at +0, indices at +QTM_IB_OFF, chunks at +QTM_CB_OFF */
#define QTM_IB_OFF 4096u
#define QTM_CB_OFF 8192u
#define QTM_NEED (QTM_CB_OFF + CHUNK_BYTES)

static u32 qtm_flush(const void* p, u32 n, char* note, size_t nn) {
    /* GSP flushes only what it accepts as process memory; the SVC is the fallback, both results are reported */
    Result a = GSPGPU_FlushDataCache(p, n);
    Result b = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, n);
    snprintf(note, nn, "flush gsp %08lX svc %08lX pa %08lX", (unsigned long)a, (unsigned long)b,
             (unsigned long)osConvertVirtToPhys(p));
    return (u32)a;
}

/* Builds the list for the scene, submits it, classifies. */
static Res run_scene(Scene* s) {
    Res r = {R_OK, ""};
    char qnote[80] = "";
    bool qtm_used = s->vb_mode == VB_QTM || s->ib_qtm || s->cb_place == PL_QTM;
    if (qtm_used && (g.cfg.qtm == NULL || g.cfg.qtm_bytes < QTM_NEED)) {
        res_set(&r, R_SKIP, "no QTM given (3dsx, Old 3DS, or not mapped)");
        return r;
    }
    u8* qtm = (u8*)g.cfg.qtm;
    u8* blk = s->rt_linear ? g.blk_l : g.blk_v;
    /* CPU fill: a GX fill of the whole block overlaps the render target surface in an emulator's cache. */
    {
        u32* p = (u32*)blk;
        for (u32 i = 0; i < BLK_BYTES / 4; i++) {
            p[i] = SENT;
        }
        flush_dc(blk, BLK_BYTES);
    }

    memset(g.vb, 0, 64 * 1024);
    Vtx* heapv = NULL;
    if (s->vb_mode == 2) {
        heapv = malloc(NVERT * sizeof(Vtx) + 64);
        make_vertices(heapv);
    } else if (s->vb_mode == VB_QTM) {
        make_vertices((Vtx*)qtm);
        qtm_flush(qtm, 4096, qnote, sizeof(qnote));
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
    u8* ibfill = s->ib_qtm ? qtm + QTM_IB_OFF : ibdst;
    if (s->draw == DRAW_E8) {
        for (int i = 0; i < 12; i++) ibfill[i] = (u8)(i % 3);
    } else {
        for (int i = 0; i < 12; i++) ((u16*)ibfill)[i] = (u16)(i % 3);
    }
    flush_dc(g.ib, 4096);
    if (s->ib_qtm) {
        qtm_flush(qtm + QTM_IB_OFF, 256, qnote, sizeof(qnote));
    }

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
    } else if (s->vb_mode == VB_QTM) {
        /* the buffer offset is relative to citro3d's base 0x18000000; QTM is physical 0x1F000000 + n */
        BufInfo_Add(bi, g.vb, sizeof(Vtx), 3, 0x210);
        bi->buffers[0].offset = g.cfg.qtm_phys - bi->base_paddr;
    } else if (s->vb_mode == 1) {
        /* citro3d's base is the constant 0x18000000 and each buffer carries its offset from it; here the base
         * register (ATTRIBBUFFERS_LOC) is moved to the data address and the buffer offset is 0 */
        BufInfo_Add(bi, g.vb + s->vb_k, sizeof(Vtx), 3, 0x210);
        bi->base_paddr += bi->buffers[0].offset;
        bi->buffers[0].offset = 0;
    } else {
        BufInfo_Add(bi, g.vb, sizeof(Vtx), 3, 0x210);
        bi->buffers[0].offset += (u32)s->vb_k;
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
    /* K_CB: the first draw writes nothing (it only carries the state); the second draw is the chunk */
    C3D_DepthTest(false, GPU_ALWAYS, s->cb ? 0 : GPU_WRITE_COLOR);
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
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, 0x68 + s->boolreg, true); /* shader bool ids start at 0x68 */
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

    if (s->cb) {
        u32 *base, sz, m, e;
        GPUCMD_GetBuffer(&base, &sz, &m);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3); /* state unchanged except the write mask: only the draw packet */
        GPUCMD_GetBuffer(&base, &sz, &e);
        r = cb_run(s, base, m, e);
        free(heapv);
        free(ibheap);
        return r;
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
    if (s->ib_qtm) {
        /* citro3d wrote the linear index address; point INDEXBUFFER_CONFIG at the QTM copy */
        for (u32 i = 0; i + 1 < nw; i = cmd_next(b, i)) {
            if ((b[i + 1] & 0xFFFF) == GPUREG_INDEXBUFFER_CONFIG && ((b[i + 1] >> 20) & 0xFF) == 0) {
                b[i] = (b[i] & 0x80000000u) | ((g.cfg.qtm_phys + QTM_IB_OFF - 0x18000000u) & 0x0FFFFFFFu);
            }
        }
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
    if (qnote[0] != '\0') {
        size_t l = strlen(r.note);
        snprintf(r.note + l, sizeof(r.note) - l, "%s%s", l ? "; " : "", qnote);
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

/* ---- command processor: hand-built main lists and chunks (K_CB, K_THRU) ----
 * The PICA command processor fetches SIZEn*8 bytes at physical ADDRn*8 when CMDBUF_JUMPn is written; it has no
 * return stack. A call uses both channels: the caller sets ADDR0/SIZE0 to its own continuation and ADDR1/SIZE1
 * to the chunk and writes JUMP1; the chunk ends with a JUMP0 write. Every jump write here is the last command of
 * a 16-byte block, so the continuation starts on a 16-byte boundary. */

typedef struct {
    u32* w;
    u32 n, cap; /* words */
    bool ovf;
} LB;

typedef struct {
    u32 at[MAXCALL];  /* word index of the SIZE0 value of a call */
    u32 ret[MAXCALL]; /* word index of its return point */
    int n;
} Patch;

static Patch s_pm, s_pa;

static void lb_init(LB* l, u32* w, u32 bytes) {
    l->w = w;
    l->n = 0;
    l->cap = bytes / 4;
    l->ovf = false;
}

static void lb_put(LB* l, u32 reg, u32 val) {
    if (l->n + 2 > l->cap) {
        l->ovf = true;
        return;
    }
    l->w[l->n++] = val;
    l->w[l->n++] = reg | 0x000F0000u;
}

static void lb_copy(LB* l, const u32* src, u32 nw) {
    if (l->n + nw > l->cap) {
        l->ovf = true;
        return;
    }
    memcpy(l->w + l->n, src, nw * 4);
    l->n += nw;
}

/* a defined write that changes nothing in these lists: FACECULLING_CONFIG = 0 (cull none, set by every case) */
static void lb_pad(LB* l) { lb_put(l, GPUREG_FACECULLING_CONFIG, 0); }

/* pads so that `more` words added next end on a 16-byte boundary (n and more are even) */
static void lb_align(LB* l, u32 more) {
    if (((l->n + more) & 3) != 0) {
        lb_pad(l);
    }
}

static void lb_finalize(LB* l, bool flush) {
    if (flush) {
        lb_put(l, GPUREG_FRAMEBUFFER_FLUSH, 1);
        lb_put(l, GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    }
    lb_align(l, 2);
    lb_put(l, GPUREG_FINALIZE, 0x12345678);
}

static void lb_jump(LB* l, int ch, u32 phys, u32 bytes) {
    lb_put(l, GPUREG_CMDBUF_SIZE0 + (u32)ch, bytes >> 3);
    lb_put(l, GPUREG_CMDBUF_ADDR0 + (u32)ch, phys >> 3);
    lb_align(l, 2);
    lb_put(l, GPUREG_CMDBUF_JUMP0 + (u32)ch, 1);
}

/* Call of the chunk at cphys from list l (at physical lphys); the return size is patched by lb_patch. */
static void lb_call(LB* l, u32 lphys, u32 cphys, u32 cbytes, Patch* p) {
    lb_put(l, GPUREG_CMDBUF_SIZE1, cbytes >> 3);
    lb_put(l, GPUREG_CMDBUF_ADDR1, cphys >> 3);
    u32 at = l->n;
    lb_put(l, GPUREG_CMDBUF_SIZE0, 0);
    lb_put(l, GPUREG_CMDBUF_ADDR0, 0);
    lb_align(l, 2);
    lb_put(l, GPUREG_CMDBUF_JUMP1, 1);
    if (l->ovf || p->n >= MAXCALL) {
        l->ovf = true;
        return;
    }
    l->w[at + 2] = (lphys + l->n * 4) >> 3;
    p->at[p->n] = at;
    p->ret[p->n] = l->n;
    p->n++;
}

/* Each return runs from its return point to the end of the list. */
static void lb_patch(LB* l, const Patch* p) {
    for (int i = 0; i < p->n && !l->ovf; i++) {
        l->w[p->at[i]] = ((l->n - p->ret[i]) * 4) >> 3;
    }
}

/* End of a called chunk: pads, then JUMP0 (back to the caller). size_mode 0: size a multiple of 16;
 * 1: 8 past a multiple of 16; 2: 16 past a multiple of 32. */
static void lb_ret(LB* l, int size_mode) {
    u32 mask = size_mode == 2 ? 7 : 3, want = size_mode == 0 ? 0 : size_mode == 1 ? 2 : 4;
    while (((l->n + 2) & mask) != want && !l->ovf) {
        lb_pad(l);
    }
    lb_put(l, GPUREG_CMDBUF_JUMP0, 1);
}

static void dump_named(const char* id, const char* ext, const void* w, u32 bytes) {
    char path[220];
    snprintf(path, sizeof(path), "%s/gpuprobe/%s%s", g.dir, id, ext);
    FILE* f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(w, 1, bytes, f);
        fclose(f);
    }
}

static void note_add(Res* r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void note_add(Res* r, const char* fmt, ...) {
    size_t l = strlen(r->note);
    if (l + 3 >= sizeof(r->note)) {
        return;
    }
    if (l > 0) {
        r->note[l++] = ';';
        r->note[l++] = ' ';
        r->note[l] = '\0';
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->note + l, sizeof(r->note) - l, fmt, ap);
    va_end(ap);
}

static u32 ticks_us(u64 t) { return (u32)(t * 1000ull / 268112ull); } /* 268.111856 ticks per us */

/* rec: the scene list; [0, m) state plus a draw that writes nothing, [m, e) the draw packet (write mask + draw). */
static Res cb_run(Scene* s, const u32* rec, u32 m, u32 e) {
    Res r = {R_OK, ""};
    const u32* P = rec;
    const u32* D = rec + m;
    u32 nd = e - m;
    LB mn, ck;
    lb_init(&mn, g.main, MAIN_BYTES);
    lb_init(&ck, g.chunk, CHUNK_BYTES);
    u32 mphys = osConvertVirtToPhys(g.main);
    u32 kphys = osConvertVirtToPhys(g.chunk);
    u32 cphys = kphys, cbytes = 0, dump_bytes = 0;
    char extra[80] = "";
    s_pm.n = 0;
    s_pa.n = 0;
    switch (s->cb) {
    case CB_JUMP0:
    case CB_JUMP1:
        lb_copy(&ck, P, m);
        lb_copy(&ck, D, nd);
        lb_finalize(&ck, true);
        lb_jump(&mn, s->cb == CB_JUMP1, kphys, ck.n * 4);
        dump_bytes = ck.n * 4;
        break;
    case CB_IRQ:
        lb_copy(&mn, P, m);
        lb_copy(&mn, D, nd);
        lb_finalize(&ck, true); /* the chunk: flush, invalidate, FINALIZE */
        lb_jump(&mn, 0, kphys, ck.n * 4);
        dump_bytes = ck.n * 4;
        break;
    case CB_NEST: {
        LB b;
        lb_init(&b, g.chunk + NEST_B_OFF / 4, CHUNK_BYTES - NEST_B_OFF);
        lb_copy(&b, D, nd);
        lb_ret(&b, 0);
        u32 bphys = kphys + NEST_B_OFF;
        /* A: call B with the return into A's tail; the tail sets channel 0 to main's continuation and jumps */
        lb_call(&ck, kphys, bphys, b.n * 4, &s_pa);
        u32 tail_at = ck.n;
        lb_put(&ck, GPUREG_CMDBUF_SIZE0, 0);
        lb_put(&ck, GPUREG_CMDBUF_ADDR0, 0);
        lb_align(&ck, 2);
        lb_put(&ck, GPUREG_CMDBUF_JUMP0, 1);
        lb_patch(&ck, &s_pa);
        lb_copy(&mn, P, m);
        lb_call(&mn, mphys, kphys, ck.n * 4, &s_pm);
        lb_finalize(&mn, true);
        lb_patch(&mn, &s_pm);
        if (!ck.ovf && !mn.ovf && s_pm.n == 1) {
            ck.w[tail_at] = ((mn.n - s_pm.ret[0]) * 4) >> 3;
            ck.w[tail_at + 2] = (mphys + s_pm.ret[0] * 4) >> 3;
        }
        dump_bytes = NEST_B_OFF + b.n * 4;
        ck.ovf |= b.ovf;
        break;
    }
    default: { /* CB_CALL */
        u32 off = (u32)s->cb_off & ~3u;
        lb_init(&ck, g.chunk + off / 4, CHUNK_BYTES - off);
        lb_copy(&ck, D, nd);
        lb_ret(&ck, s->cb_size);
        cbytes = ck.n * 4;
        dump_bytes = off + cbytes;
        cphys = kphys + off;
        if (s->cb_place == PL_VRAM) {
            /* TextureCopy linear -> VRAM, then a CPU read back to check the copy */
            u32 nb = (off + cbytes + 15) & ~15u;
            flush_dc(g.chunk, nb);
            LightEvent_Clear(&g.ev[GSPGPU_EVENT_PPF]);
            Result rc = GX_TextureCopy(g.chunk, GX_BUFFER_DIM(nb >> 4, 0), g.cvram, GX_BUFFER_DIM(nb >> 4, 0), nb,
                                       GX_TRANSFER_RAW_COPY(1));
            if (R_FAILED(rc)) {
                res_set(&r, R_ERR, "GX_TextureCopy rc=%08lX", (unsigned long)rc);
                return r;
            }
            if (!ev_wait(GSPGPU_EVENT_PPF, g.timeout_ms)) {
                res_set(&r, R_HANG, "TextureCopy to VRAM: no PPF in %d ms", g.timeout_ms);
                return r;
            }
            inval_dc(g.cvram, nb);
            bool same = memcmp(g.cvram, g.chunk, nb) == 0;
            snprintf(extra, sizeof(extra), "VRAM copy %s", same ? "equal" : "differs (CPU read)");
            cphys = osConvertVirtToPhys(g.cvram) + off;
        } else if (s->cb_place == PL_QTM) {
            u8* q = (u8*)g.cfg.qtm + QTM_CB_OFF;
            memcpy(q, g.chunk, off + cbytes);
            qtm_flush(q, (off + cbytes + 63) & ~63u, extra, sizeof(extra));
            cphys = g.cfg.qtm_phys + QTM_CB_OFF + off;
        }
        lb_copy(&mn, P, m);
        int calls = s->cb_calls < 1 ? 1 : s->cb_calls;
        if (calls > 1) {
            lb_put(&mn, GPUREG_SCISSORTEST_MODE, SC_NORMAL);
        }
        for (int i = 0; i < calls; i++) {
            if (calls > 1) {
                /* call i draws pixel (i & 63, i >> 6) only: left/top and right-1/bottom-1 */
                u32 xy = ((u32)(i >> 6) << 16) | (u32)(i & 63);
                lb_put(&mn, GPUREG_SCISSORTEST_POS, xy);
                lb_put(&mn, GPUREG_SCISSORTEST_DIM, xy);
            }
            lb_call(&mn, mphys, cphys, cbytes, &s_pm);
        }
        lb_finalize(&mn, true);
        lb_patch(&mn, &s_pm);
        if (calls > 1) {
            s->exp = EX_COUNT;
            s->want_red = calls;
        }
        break;
    }
    }
    if (mn.ovf || ck.ovf) {
        res_set(&r, R_ERR, "list buffer overflow (main %lu words)", (unsigned long)mn.n);
        return r;
    }
    dump_named(s->id, ".bin", g.main, mn.n * 4);
    dump_named(s->id, ".chunk", g.chunk, dump_bytes);
    flush_dc(g.main, mn.n * 4);
    flush_dc(g.chunk, CHUNK_BYTES);

    LightEvent_Clear(&g.ev[GSPGPU_EVENT_P3D]);
    u64 t0 = svcGetSystemTick();
    Result rc = GX_ProcessCommandList(g.main, mn.n * 4, 0);
    bool timed_out = R_SUCCEEDED(rc) && !ev_wait(GSPGPU_EVENT_P3D, g.timeout_ms);
    u32 us = ticks_us(svcGetSystemTick() - t0);
    r = classify(s, timed_out, rc);
    if (!timed_out && R_SUCCEEDED(rc)) {
        if (s->cb == CB_IRQ) {
            LightEvent_Clear(&g.ev[GSPGPU_EVENT_P3D]);
            bool again = ev_wait(GSPGPU_EVENT_P3D, 20);
            note_add(&r, "P3D after %lu us, second P3D in 20 ms: %s", (unsigned long)us, again ? "yes" : "no");
        } else {
            note_add(&r, "%lu us", (unsigned long)us);
        }
    }
    if (extra[0] != '\0') {
        note_add(&r, "%s", extra);
    }
    return r;
}

/* ---- cp_thru: command-processor time per list shape ---- */

#define THRU_REG 0x0081 /* TEXUNIT0_BORDER_COLOR: no case samples the border */
#define THRU_WORDS 8192

/* Submits w and waits; best of 3 in us. 0: no completion (hang), -1: GSP refused. */
static int thru_best(u32* w, u32 nw, u32* best) {
    *best = 0xFFFFFFFFu;
    flush_dc(w, nw * 4);
    for (int k = 0; k < 3; k++) {
        LightEvent_Clear(&g.ev[GSPGPU_EVENT_P3D]);
        u64 t0 = svcGetSystemTick();
        Result rc = GX_ProcessCommandList(w, nw * 4, 0);
        if (R_FAILED(rc)) {
            return -1;
        }
        if (!ev_wait(GSPGPU_EVENT_P3D, g.timeout_ms)) {
            return 0;
        }
        u32 us = ticks_us(svcGetSystemTick() - t0);
        if (us < *best) {
            *best = us;
        }
    }
    return 1;
}

/* words per us x10, net of the empty list's time */
static u32 per_us_x10(u32 words, u32 us, u32 base) { return us > base ? words * 10u / (us - base) : 0; }

static Res run_thru(void) {
    Res r = {R_OK, ""};
    static const char* const name[4] = {"empty", "single", "burst", "called"};
    u32 us[4] = {0, 0, 0, 0};
    for (int mode = 0; mode < 4; mode++) {
        LB l;
        lb_init(&l, g.main, MAIN_BYTES);
        s_pm.n = 0;
        if (mode == 1) {
            for (u32 i = 0; i < THRU_WORDS / 2; i++) lb_put(&l, THRU_REG, i);
        } else if (mode == 2) {
            /* 32 commands of one header + 255 values to the same register (extra = 254): 256 words each */
            for (u32 c = 0; c < THRU_WORDS / 256; c++) {
                l.w[l.n] = c;
                l.w[l.n + 1] = THRU_REG | 0x000F0000u | (254u << 20);
                for (u32 k = 0; k < 254; k++) l.w[l.n + 2 + k] = k;
                l.n += 256;
            }
        } else if (mode == 3) {
            LB c;
            lb_init(&c, g.chunk, CHUNK_BYTES);
            for (u32 i = 0; i < 1020 / 2; i++) lb_put(&c, THRU_REG, i);
            lb_ret(&c, 0); /* 1024 words with the return */
            flush_dc(g.chunk, c.n * 4);
            for (int k = 0; k < THRU_WORDS / 1024; k++) {
                lb_call(&l, osConvertVirtToPhys(g.main), osConvertVirtToPhys(g.chunk), c.n * 4, &s_pm);
            }
        }
        lb_finalize(&l, false);
        lb_patch(&l, &s_pm);
        int ok = thru_best(g.main, l.n, &us[mode]);
        if (ok <= 0) {
            res_set(&r, ok == 0 ? R_HANG : R_ERR, "%s list: %s", name[mode], ok == 0 ? "no P3D" : "GSP refused");
            return r;
        }
    }
    u32 v[4];
    for (int i = 1; i < 4; i++) v[i] = per_us_x10(THRU_WORDS, us[i], us[0]);
    plog("gpuprobe: cp_thru: %u words per list, best of 3: empty list %lu us; single writes %lu us (%lu.%lu w/us); "
         "bursts %lu us (%lu.%lu w/us); 8 calls of a 1 K chunk %lu us (%lu.%lu w/us); w/us net of the empty list",
         THRU_WORDS, (unsigned long)us[0], (unsigned long)us[1], (unsigned long)(v[1] / 10),
         (unsigned long)(v[1] % 10), (unsigned long)us[2], (unsigned long)(v[2] / 10), (unsigned long)(v[2] % 10),
         (unsigned long)us[3], (unsigned long)(v[3] / 10), (unsigned long)(v[3] % 10));
    res_set(&r, R_OK, "us per 8K words: empty %lu single %lu burst %lu called %lu", (unsigned long)us[0],
            (unsigned long)us[1], (unsigned long)us[2], (unsigned long)us[3]);
    return r;
}

/* ---- dt_tile: DisplayTransfer linear -> tiled vs a CPU Morton tiler ---- */

static const u16 k_dt_size[][2] = {{8, 8},   {16, 16}, {32, 32}, {64, 64},  {128, 128},
                                   {256, 256}, {64, 32}, {32, 64}, {128, 16}, {16, 128}};
#define NDT ((int)(sizeof(k_dt_size) / sizeof(k_dt_size[0])))
#define DT_MAX (256 * 256 * 4)

static u32 dt_bpp(int fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }
static const char* dt_name(int fmt) {
    return fmt == 0 ? "rgba8" : fmt == 1 ? "rgb8" : fmt == 2 ? "rgb565" : fmt == 3 ? "rgba5551" : "rgba4";
}

static u32 xs_next(u32* x) {
    u32 v = *x;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *x = v;
    return v;
}

/* one RGBA8 texel (bytes R,G,B,A) as format fmt, in PICA memory byte order */
static void dt_encode(int fmt, const u8* p, u8* o) {
    u16 v;
    switch (fmt) {
    case 0: {
        u32 w = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        memcpy(o, &w, 4);
        return;
    }
    case 1:
        o[0] = p[2];
        o[1] = p[1];
        o[2] = p[0];
        return;
    case 2: v = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)); break;
    case 3: v = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7)); break;
    default: v = (u16)((p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | (p[3] >> 4)); break;
    }
    memcpy(o, &v, 2);
}

/* the probe's own tiler: linear rows (row 0 first) into 8x8 Morton tiles, row 0 into the last tile row */
static void dt_tile_cpu(const u8* lin, u32 w, u32 h, u32 bpp, u8* out) {
    for (u32 my = 0; my < h; my++) {
        const u8* row = lin + (h - 1 - my) * w * bpp;
        for (u32 mx = 0; mx < w; mx++) {
            memcpy(out + tile_off(mx, my, w) * bpp, row + mx * bpp, bpp);
        }
    }
}

static Res run_dt(int fmt) {
    Res r = {R_OK, ""};
    u8* in = linearMemAlign(DT_MAX, 128);
    u8* out = linearMemAlign(DT_MAX, 128);
    u8* ref = malloc(DT_MAX);
    u8* rgba = malloc(DT_MAX);
    u32 bpp = dt_bpp(fmt);
    int match[2] = {0, 0}, app_ref = 0, first_bad = -1;
    u32 seed = 0x2545F491u;
    if (in == NULL || out == NULL || ref == NULL || rgba == NULL) {
        res_set(&r, R_SKIP, "allocation failed");
        goto done;
    }
    for (int si = 0; si < NDT; si++) {
        u32 w = k_dt_size[si][0], h = k_dt_size[si][1], n = w * h;
        for (u32 i = 0; i < n * 4; i++) {
            u8 v = (u8)xs_next(&seed);
            rgba[i] = fmt == 4 ? (u8)((v & 15) * 17) : v; /* RGBA4: exact nibbles (n * 17 >> 4 == n) */
        }
        for (u32 i = 0; i < n; i++) dt_encode(fmt, rgba + i * 4, in + i * bpp);
        if (g.cfg.tile_ref != NULL && g.cfg.tile_ref(fmt, rgba, w, h, ref, g.cfg.user)) {
            app_ref = 1;
        } else {
            dt_tile_cpu(in, w, h, bpp, ref);
        }
        flush_dc(in, n * bpp);
        int ok_flip[2] = {0, 0};
        for (int flip = 0; flip < 2; flip++) {
            memset(out, 0xA5, n * bpp);
            flush_dc(out, n * bpp);
            LightEvent_Clear(&g.ev[GSPGPU_EVENT_PPF]);
            Result rc = GX_DisplayTransfer((u32*)in, GX_BUFFER_DIM(w, h), (u32*)out, GX_BUFFER_DIM(w, h),
                                           GX_TRANSFER_FLIP_VERT(flip) | GX_TRANSFER_OUT_TILED(1) |
                                               GX_TRANSFER_IN_FORMAT(fmt) | GX_TRANSFER_OUT_FORMAT(fmt) |
                                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
            if (R_FAILED(rc)) {
                res_set(&r, R_ERR, "GX_DisplayTransfer %lux%lu rc=%08lX", (unsigned long)w, (unsigned long)h,
                        (unsigned long)rc);
                goto done;
            }
            if (!ev_wait(GSPGPU_EVENT_PPF, g.timeout_ms)) {
                res_set(&r, R_HANG, "%lux%lu flip %d: no PPF in %d ms", (unsigned long)w, (unsigned long)h, flip,
                        g.timeout_ms);
                goto done;
            }
            inval_dc(out, n * bpp);
            u32 bad = 0, at = 0;
            for (u32 i = 0; i < n * bpp; i++) {
                if (out[i] != ref[i] && bad++ == 0) {
                    at = i;
                }
            }
            ok_flip[flip] = bad == 0;
            match[flip] += bad == 0;
            plog("gpuprobe: dt %s %lux%lu flip %d: %s (%lu of %lu bytes differ, first at %lu)", dt_name(fmt),
                 (unsigned long)w, (unsigned long)h, flip, bad ? "differs" : "equal", (unsigned long)bad,
                 (unsigned long)(n * bpp), (unsigned long)at);
        }
        if (!ok_flip[0] && !ok_flip[1] && first_bad < 0) {
            first_bad = si;
        }
    }
    {
        const char* rs = app_ref ? "app tiler" : "probe tiler";
        if (match[1] == NDT || match[0] == NDT) {
            res_set(&r, R_OK, "all %d sizes equal to the %s with FLIP_VERT %d (other: %d/%d)", NDT, rs,
                    match[1] == NDT ? 1 : 0, match[1] == NDT ? match[0] : match[1], NDT);
        } else {
            res_set(&r, R_WRONG, "sizes equal: flip %d/%d, noflip %d/%d vs %s; first bad %ux%u", match[1], NDT,
                    match[0], NDT, rs, first_bad >= 0 ? k_dt_size[first_bad][0] : 0u,
                    first_bad >= 0 ? k_dt_size[first_bad][1] : 0u);
        }
    }
done:
    if (in) linearFree(in);
    if (out) linearFree(out);
    free(ref);
    free(rgba);
    return r;
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
        s.ib_heap = c->p[1] == 1;
        s.ib_qtm = c->p[1] == IB_QTM;
        break;
    case K_CB:
        s.cb = c->a;
        s.cb_calls = c->p[0];
        s.cb_off = c->p[1];
        s.cb_size = c->p[2];
        s.cb_place = c->p[3];
        break;
    case K_THRU:
        return run_thru();
    case K_DT:
        return run_dt(c->a);
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
