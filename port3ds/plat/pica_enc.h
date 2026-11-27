/* PICA200 command words for the three state classes (GX native step 2, switch gxone; docs/PORT_3DS_GX_NATIVE.md).
 *
 * The port's own encoders. Each writes the words citro3d 1.7.1's C3Di_UpdateContext emits for the class after the
 * renderer's setters (plat/gpu.c apply_tev, tex_prepare + tex_bind3, apply_pix with `det` set), read from the
 * disassembly of the installed libcitro3d.a: the same registers, values, order, run headers and padding as
 * GPUCMD_Add. No libctru or citro3d call: the file builds on the host too (tools/port3ds/pica_enc_test).
 *
 * Register values the renderer never sets (stencil, early depth, the shadow and fragment-mode bits, ...) come from
 * PicaEncFixed, read once from citro3d's context after C3D_Init. Floats go through pica_f24/pica_f31, which are
 * libctru's f32tof24/f32tof31 (checked against libctru at start-up on the console build).
 *
 * Game-agnostic (docs: portability rule): only PICA registers and plain numbers. */
#ifndef PICA_ENC_H
#define PICA_ENC_H

#include <stdint.h>

/* Values of citro3d's context that the state classes do not set (after C3D_Init). */
typedef struct {
    uint32_t texenv_buf_hi; /* GPUREG_TEXENV_UPDATE_BUFFER (0xE0) bits 17..31 */
    uint32_t tex_cfg_hi;    /* GPUREG_TEXUNIT_CONFIG (0x80) without bits 0..2 and 16 */
    uint32_t tex_shadow;    /* GPUREG_TEXUNIT0_SHADOW (0x8B) */
    uint32_t stencil_test, stencil_op, blend_color;
    uint32_t fragop_lo;     /* GPUREG_COLOR_OPERATION (0x100) without bits 8..15 */
    uint32_t fragop_shadow; /* GPUREG_FRAGOP_SHADOW (0x130) */
    uint32_t early_depth, early_func, early_ref;
} PicaEncFixed;

/* One combiner stage as C3D_TexEnv's five words: sources, operands, functions, constant, scales. */
typedef struct {
    uint32_t w[5];
} PicaTevStage;

/* The texture-environment block: GPUREG_TEXENV_UPDATE_BUFFER (fog mode, buffer update bits), buffer colour,
 * fog colour, then the six stages. `buf_bits` = fog mode (bits 0..2) | rgb update (8..11) | alpha update
 * (12..15). 42 words. */
#define PICA_TEV_WORDS 42u
uint32_t pica_enc_tev(uint32_t* out, const PicaTevStage st[6], uint32_t buf_bits, uint32_t buf_color, uint32_t fog_color,
                      const PicaEncFixed* f);

/* One texture unit as C3Di_SetTex reads C3D_Tex: border, dim, param, lodParam, physical address >> 3, format. */
typedef struct {
    uint32_t border, dim, param, lod, addr8, fmt;
} PicaTexUnit;

/* The texture block: the three units' parameters and formats, then GPUREG_TEXUNIT_CONFIG with the cache clear and
 * the shadow word. All three units bound (the renderer binds a blank texture to an unused unit). 30 words. */
#define PICA_TEX_WORDS 30u
uint32_t pica_enc_tex(uint32_t* out, const PicaTexUnit u[3], const PicaEncFixed* f);

/* The pixel block's inputs, as the arguments of citro3d's setters. */
typedef struct {
    uint32_t viewport;              /* 1: C3D_SetViewport(vx, vy, vw, vh) ran (the target rectangle is at least 1x1) */
    uint32_t vx, vy, vw, vh;
    uint32_t sc_mode;               /* GPU_SCISSORMODE of the last C3D_SetScissor */
    uint32_t sc_x0, sc_y0, sc_x1, sc_y1; /* its left, top, right, bottom (corners kept as set by NORMAL, 0 0 1 1) */
    float zscale, zoffset;          /* C3D_DepthMap(true, zscale, zoffset) */
    uint32_t alpha_test;            /* C3D_AlphaTest's word: enable | func << 4 | ref << 8 */
    uint32_t depth_test;            /* C3D_DepthTest's word: enable | func << 4 | mask << 8 */
    uint32_t alpha_blend;           /* C3D_AlphaBlend's word */
    uint32_t logic_op;              /* GPU_LOGICOP */
    uint32_t blend_on;              /* 1: the last of AlphaBlend / ColorLogicOp was AlphaBlend */
} PicaPixIn;

/* Viewport group (when set), scissor group, effect group. At most 46 words. */
#define PICA_PIX_WORDS_MAX 46u
uint32_t pica_enc_pix(uint32_t* out, const PicaPixIn* in, const PicaEncFixed* f);

/* The end of a command list of n_before words (even), as citro3d's C3Di_SplitFrame and libctru's GPUCMD_Split write
 * it: with fb_flush the framebuffer flush, invalidate and early-depth clear (citro3d writes them when a draw came
 * since the last split), then GPUREG_FINALIZE, twice when the list would not end on a 16-byte boundary. At most
 * PICA_LIST_END_MAX words; n_before + the result is a multiple of 4 when n_before is even. */
#define PICA_LIST_END_MAX 10u
uint32_t pica_enc_list_end(uint32_t* out, uint32_t n_before, int fb_flush);

/* Command-buffer calls (GX native step 5, switch gxcall; docs/PORT_3DS_GPU_RULES.md, cases cb_call*). The command
 * processor fetches SIZEn * 8 bytes at physical ADDRn * 8 when CMDBUF_JUMPn is written; it has no return stack. A call
 * writes CMDBUF_SIZE1/ADDR1 (the chunk), CMDBUF_SIZE0/ADDR0 (the continuation: from the word after the call to the
 * end of the calling list) and CMDBUF_JUMP1, in the order of the gpuprobe cases; the chunk ends with CMDBUF_JUMP0.
 * Every jump write is the last command of a 16-byte block, so chunks and continuations start 16-byte aligned and
 * their sizes are multiples of 16 bytes. Pads are FACECULLING_CONFIG writes with byte mask 0 (they write nothing). */
#define PICA_CALL_WORDS_MAX 12u
#define PICA_RET_WORDS 4u
/* At out (physical out_phys, 8-byte aligned): a call of chunk_bytes at chunk_phys. ADDR0 is the word after the call
 * (16-byte aligned), SIZE0 is 0 until pica_enc_call_patch; *size0 gets the index of the SIZE0 value word. Returns the
 * words (10 or 12). */
uint32_t pica_enc_call(uint32_t* out, uint32_t out_phys, uint32_t chunk_phys, uint32_t chunk_bytes, uint32_t* size0);
/* The continuation's size: bytes from the end of the call to the end of the list (a multiple of 16) */
static inline void pica_enc_call_patch(uint32_t* size0_word, uint32_t bytes) { *size0_word = bytes >> 3; }
/* A chunk's return at out (16-byte aligned): a pad, then CMDBUF_JUMP0. PICA_RET_WORDS words. */
uint32_t pica_enc_ret(uint32_t* out);

typedef struct {
    uint32_t size[2], addr[2]; /* the CMDBUF values written (bytes >> 3) */
    uint32_t set;              /* bits 0-3: size0, size1, addr0, addr1 written */
} PicaCall;
/* Parses a call at w (n words at most): single CMDBUF size/address writes and pads, ending with the CMDBUF_JUMP1
 * write. Returns its words; 0 when anything else comes first (another register, a burst, a byte mask other than
 * 0xF, JUMP0) or the words end. */
uint32_t pica_call_parse(const uint32_t* w, uint32_t n, PicaCall* c);
/* 1 when the n words are pads, then CMDBUF_JUMP0 as the last command (a chunk's return). */
int pica_ret_check(const uint32_t* w, uint32_t n);

/* libctru's float conversions (f32tof24, f32tof31), bit for bit. */
uint32_t pica_f24(float v);
uint32_t pica_f31(float v);

#endif
