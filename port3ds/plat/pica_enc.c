/* PICA200 command words for the state classes (pica_enc.h). Read from libcitro3d.a 1.7.1 (C3Di_UpdateContext,
 * C3Di_EffectBind, C3Di_SetTex, C3Di_TexEnvBind and the setters) and libctru's GPUCMD_Add, f32tof24, f32tof31. */
#include "pica_enc.h"

#include <string.h>

#define HDR(inc, mask, reg) ((uint32_t)(inc) << 31 | (uint32_t)(mask) << 16 | (uint32_t)(reg))

/* GPUCMD_Add with 1..256 parameters: first parameter, header with the count, the rest, padded to an even length. */
static inline uint32_t* run(uint32_t* w, uint32_t hdr, const uint32_t* p, uint32_t n) {
    w[0] = p[0];
    w[1] = hdr | (n - 1) << 20;
    for (uint32_t i = 1; i < n; i++) {
        w[1 + i] = p[i];
    }
    w += n + 1;
    if (!(n & 1)) {
        *w++ = 0;
    }
    return w;
}

static inline uint32_t* put(uint32_t* w, uint32_t mask, uint32_t reg, uint32_t v) {
    w[0] = v;
    w[1] = HDR(0, mask, reg);
    return w + 2;
}

uint32_t pica_f24(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    int32_t e = (int32_t)((b << 1) >> 24) - 64;
    uint32_t s = (b >> 31) << 23;
    if (e < 0) {
        return s;
    }
    if (e > 127) {
        return s | 0x7F0000u;
    }
    return s | ((b << 9) >> 16) | (uint32_t)e << 16;
}

uint32_t pica_f31(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    int32_t e = (int32_t)((b << 1) >> 24) - 64;
    uint32_t s = (b >> 31) << 30;
    if (e < 0) {
        return s;
    }
    if (e > 127) {
        return s | 0x3F800000u;
    }
    return (b & 0x007FFFFFu) | s | (uint32_t)e << 23;
}

/* C3Di_UpdateContext: texture-environment buffer group (C3DiF_TexEnvBuf), then the stages (C3DiF_TexEnv(i)). */
uint32_t pica_enc_tev(uint32_t* out, const PicaTevStage st[6], uint32_t buf_bits, uint32_t buf_color, uint32_t fog_color,
                      const PicaEncFixed* f) {
    uint32_t* w = out;
    w = put(w, 0x7, 0x0E0, f->texenv_buf_hi | buf_bits);
    w = put(w, 0xF, 0x0FD, buf_color);
    w = put(w, 0xF, 0x0E1, fog_color);
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t reg = (i + 24 + (i > 3 ? 2 : 0)) << 3; /* C3Di_TexEnvBind: 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 */
        w = run(w, HDR(1, 0xF, reg), st[i].w, 5);
    }
    return (uint32_t)(w - out);
}

/* C3Di_UpdateContext: each bound unit through C3Di_SetTex (2D path), then GPUREG_TEXUNIT_CONFIG with the units and
 * the cache clear, the clear bit alone, and the shadow word (C3DiF_TexStatus). */
uint32_t pica_enc_tex(uint32_t* out, const PicaTexUnit u[3], const PicaEncFixed* f) {
    static const uint16_t k_reg[3][2] = {{0x081, 0x08E}, {0x091, 0x096}, {0x099, 0x09E}};
    uint32_t* w = out;
    for (int i = 0; i < 3; i++) {
        const uint32_t p[5] = {u[i].border, u[i].dim, u[i].param, u[i].lod, u[i].addr8};
        w = run(w, HDR(1, 0xF, k_reg[i][0]), p, 5);
        w = put(w, 0xF, k_reg[i][1], u[i].fmt & 0xF);
    }
    w = put(w, 0xB, 0x080, f->tex_cfg_hi | 0x7u | 0x10000u);
    w = put(w, 0x4, 0x080, 0x10000u);
    w = put(w, 0xF, 0x08B, f->tex_shadow);
    return (uint32_t)(w - out);
}

/* C3Di_UpdateContext: viewport (C3DiF_Viewport), scissor (C3DiF_Scissor), effect (C3DiF_Effect, C3Di_EffectBind). */
uint32_t pica_enc_pix(uint32_t* out, const PicaPixIn* in, const PicaEncFixed* f) {
    uint32_t* w = out;
    if (in->viewport) {
        float fw = (float)in->vw, fh = (float)in->vh;
        const uint32_t vp[4] = {pica_f24(fw * 0.5f), pica_f31(2.0f / fw) << 1, pica_f24(fh * 0.5f),
                                pica_f31(2.0f / fh) << 1};
        w = run(w, HDR(1, 0xF, 0x041), vp, 4);
        w = put(w, 0xF, 0x068, (in->vx & 0xFFFFu) | in->vy << 16);
    }
    const uint32_t sc[3] = {in->sc_mode, (in->sc_x0 & 0xFFFFu) | in->sc_y0 << 16,
                            ((in->sc_x1 - 1) & 0xFFFFu) | (in->sc_y1 - 1) << 16};
    w = run(w, HDR(1, 0xF, 0x065), sc, 3);
    w = put(w, 0xF, 0x06D, 1);  /* depth map on */
    w = put(w, 0xF, 0x040, 0);  /* culling off (C3D_CullFace(GPU_CULL_NONE)) */
    const uint32_t zm[2] = {pica_f24(in->zscale), pica_f24(in->zoffset)};
    w = run(w, HDR(1, 0xF, 0x04D), zm, 2);
    const uint32_t ts[4] = {in->alpha_test, f->stencil_test, f->stencil_op, in->depth_test};
    w = run(w, HDR(1, 0xF, 0x104), ts, 4);
    w = put(w, 0x8, 0x126, ((0xAF02u >> ((in->depth_test >> 3) & 14)) << 24) & 0x3000000u);
    w = put(w, 0xF, 0x103, f->blend_color);
    w = put(w, 0xF, 0x101, in->alpha_blend);
    w = put(w, 0xF, 0x102, in->logic_op & 0xFFu);
    w = put(w, 0x7, 0x100, f->fragop_lo | (in->blend_on ? 0x100u : 0));
    w = put(w, 0xF, 0x130, f->fragop_shadow);
    w = put(w, 0x1, 0x062, f->early_depth);
    w = put(w, 0xF, 0x118, f->early_depth);
    w = put(w, 0x1, 0x061, f->early_func);
    w = put(w, 0x7, 0x06A, f->early_ref);
    return (uint32_t)(w - out);
}

uint32_t pica_enc_list_end(uint32_t* out, uint32_t n_before, int fb_flush) {
    uint32_t* w = out;
    if (fb_flush) { /* C3Di_SplitFrame with a draw since the last split */
        w = put(w, 0xF, 0x111, 1); /* GPUREG_FRAMEBUFFER_FLUSH */
        w = put(w, 0xF, 0x110, 1); /* GPUREG_FRAMEBUFFER_INVALIDATE */
        w = put(w, 0xF, 0x063, 1); /* GPUREG_EARLYDEPTH_CLEAR */
    }
    /* GPUCMD_Split: GPUREG_FINALIZE, a second one when the list would not end on a 16-byte boundary */
    w = put(w, 0xF, 0x010, 0x12345678u);
    if ((n_before + (uint32_t)(w - out)) & 3u) {
        w = put(w, 0xF, 0x010, 0x12345678u);
    }
    return (uint32_t)(w - out);
}
