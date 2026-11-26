/* PICA200 register-file model (pica_model.h). */
#include "pica_model.h"

#include <string.h>

enum {
    R_FINALIZE = 0x010,
    R_EARLYDEPTH_CLEAR = 0x063,
    R_TEXUNIT_CONFIG = 0x080,
    R_PROCTEX_LUT = 0x0AF,
    R_PROCTEX_LUT_DATA7 = 0x0B7,
    R_FOG_LUT_INDEX = 0x0E6,
    R_FOG_LUT_DATA0 = 0x0E8,
    R_FOG_LUT_DATA7 = 0x0EF,
    R_FB_INVALIDATE = 0x110,
    R_FB_FLUSH = 0x111,
    R_GAS_LUT_INDEX = 0x123,
    R_GAS_LUT_DATA = 0x124,
    R_LIGHTING_LUT_INDEX = 0x1C5,
    R_LIGHTING_LUT_DATA7 = 0x1CF,
    R_DRAWARRAYS = 0x22E,
    R_DRAWELEMENTS = 0x22F,
    R_VTX_FUNC = 0x231,
    R_FIXEDATTRIB_INDEX = 0x232,
    R_FIXEDATTRIB_DATA0 = 0x233,
    R_FIXEDATTRIB_DATA2 = 0x235,
    R_CMDBUF_JUMP0 = 0x23C,
    R_CMDBUF_JUMP1 = 0x23D,
    R_RESTART_PRIMITIVE = 0x25F,
    R_GSH_CODETRANSFER_END = 0x28F,
    R_GSH_FLOATUNIFORM_CONFIG = 0x290,
    R_GSH_OPDESCS_DATA7 = 0x2AD,
    R_VSH_CODETRANSFER_END = 0x2BF,
    R_VSH_FLOATUNIFORM_CONFIG = 0x2C0,
    R_VSH_FLOATUNIFORM_DATA0 = 0x2C1,
    R_VSH_FLOATUNIFORM_DATA7 = 0x2C8,
    R_VSH_CODETRANSFER_CONFIG = 0x2CB,
    R_VSH_OPDESCS_DATA7 = 0x2DD,
};

/* 1: the register holds state the model compares (not a trigger, port or port index) */
static int reg_is_state(uint32_t r) {
    switch (r) {
    case R_FINALIZE:
    case R_EARLYDEPTH_CLEAR:
    case R_FB_INVALIDATE:
    case R_FB_FLUSH:
    case R_GAS_LUT_INDEX:
    case R_GAS_LUT_DATA:
    case R_DRAWARRAYS:
    case R_DRAWELEMENTS:
    case R_VTX_FUNC:
    case R_CMDBUF_JUMP0:
    case R_CMDBUF_JUMP1:
    case R_RESTART_PRIMITIVE:
    case R_VSH_CODETRANSFER_END:
    case R_VSH_FLOATUNIFORM_CONFIG:
        return 0;
    default: break;
    }
    if ((r >= R_PROCTEX_LUT && r <= R_PROCTEX_LUT_DATA7) || (r >= R_FOG_LUT_INDEX && r <= R_FOG_LUT_DATA7) ||
        (r >= R_LIGHTING_LUT_INDEX && r <= R_LIGHTING_LUT_DATA7) ||
        (r >= R_FIXEDATTRIB_INDEX && r <= R_FIXEDATTRIB_DATA2) ||
        (r >= R_GSH_CODETRANSFER_END && r <= R_GSH_OPDESCS_DATA7 && r != 0x29A && r != 0x2A4 && r != 0x299) ||
        (r >= R_VSH_FLOATUNIFORM_DATA0 && r <= R_VSH_FLOATUNIFORM_DATA7) ||
        (r >= R_VSH_CODETRANSFER_CONFIG && r <= R_VSH_OPDESCS_DATA7 && r != 0x2D4)) {
        return 0;
    }
    return 1;
}

void pica_model_reset(PicaModel* m) { memset(m, 0, sizeof(*m)); }

static void model_write(PicaModel* m, uint32_t r, uint32_t v, uint32_t mask) {
    if (r >= PICA_NREG) {
        return;
    }
    if (r >= R_VSH_FLOATUNIFORM_DATA0 && r <= R_VSH_FLOATUNIFORM_DATA7) {
        if (!m->uidx_known) {
            return;
        }
        /* f32 mode: 4 words a row; f24 mode (the shader's own constants): 3 words, kept as written with a mark in the
         * fourth (a row is compared as written, not as a value) */
        m->urow[m->usub++] = v;
        if (m->usub == (m->uf32 ? 4u : 3u)) {
            if (!m->uf32) {
                m->urow[3] = 0xF24F24F2u;
            }
            m->usub = 0;
            if (m->uidx < 96) {
                memcpy(m->unif[m->uidx], m->urow, 16);
                m->unif_known[m->uidx] = 1;
            }
            m->uidx++;
        }
        return;
    }
    switch (r) {
    case R_VSH_FLOATUNIFORM_CONFIG:
        m->uidx = v & 0xFFu;
        m->uf32 = v >> 31;
        m->usub = 0;
        m->uidx_known = 1;
        return;
    case R_FIXEDATTRIB_INDEX:
        m->fxidx = v & 0xFu;
        m->fxsub = 0;
        m->fxidx_known = 1;
        return;
    case R_FIXEDATTRIB_DATA0:
    case R_FIXEDATTRIB_DATA0 + 1:
    case R_FIXEDATTRIB_DATA2:
        m->fxdata[r - R_FIXEDATTRIB_DATA0] = v;
        if (r == R_FIXEDATTRIB_DATA2 && m->fxidx_known) {
            memcpy(m->fixed[m->fxidx], m->fxdata, 12);
            m->fixed_known[m->fxidx] = 1;
        }
        return;
    case R_FOG_LUT_INDEX:
        m->fogidx = v & 0x7Fu;
        m->fogidx_known = 1;
        return;
    case R_FINALIZE: m->finals++; return;
    case R_DRAWARRAYS:
    case R_DRAWELEMENTS: m->draws++; return;
    default: break;
    }
    if (r >= R_FOG_LUT_DATA0 && r <= R_FOG_LUT_DATA7) {
        if (m->fogidx_known) {
            m->fog[m->fogidx & 127u] = v;
            m->fog_known[m->fogidx & 127u] = 1;
            m->fogidx++;
        }
        return;
    }
    if (!reg_is_state(r)) {
        if ((r >= R_PROCTEX_LUT && r <= R_PROCTEX_LUT_DATA7) || (r >= R_LIGHTING_LUT_INDEX && r <= R_LIGHTING_LUT_DATA7) ||
            r == R_GAS_LUT_INDEX || r == R_GAS_LUT_DATA) {
            m->luts++;
        }
        return;
    }
    if (r == R_TEXUNIT_CONFIG && (mask & 4u) && (v & 0x10000u)) {
        m->texclears++; /* the clear bit is an event: the register keeps the rest */
        v &= ~0x10000u;
    }
    uint32_t bm = (mask & 1u ? 0xFFu : 0) | (mask & 2u ? 0xFF00u : 0) | (mask & 4u ? 0xFF0000u : 0) |
                  (mask & 8u ? 0xFF000000u : 0);
    m->reg[r] = (m->reg[r] & ~bm) | (v & bm);
    m->known[r] |= (uint8_t)(mask & 0xFu);
}

void pica_model_feed(PicaModel* m, const uint32_t* w, uint32_t n) {
    uint32_t i = 0;
    while (i + 1 < n) {
        uint32_t hdr = w[i + 1];
        uint32_t r = hdr & 0xFFFFu, mask = hdr >> 16 & 0xFu, extra = hdr >> 20 & 0xFFu, inc = hdr >> 31;
        if (i + 2 + extra > n) {
            m->bad++;
            return;
        }
        model_write(m, r, w[i], mask);
        for (uint32_t k = 1; k <= extra; k++) {
            model_write(m, inc ? r + k : r, w[i + 1 + k], mask);
        }
        i += 2 + extra;
        i += i & 1u; /* GPUCMD_Add pads a command to an even word count */
    }
}

static void note(PicaModelDiff* d, uint32_t id, uint32_t a, uint32_t b) {
    if (d->differ++ == 0) {
        d->first_reg = id;
        d->first_a = a;
        d->first_b = b;
    }
}

void pica_model_cmp(const PicaModel* a, const PicaModel* b, PicaModelDiff* d) {
    memset(d, 0, sizeof(*d));
    for (uint32_t r = 0; r < PICA_NREG; r++) {
        uint32_t ka = a->known[r], kb = b->known[r];
        if ((ka | kb) == 0 || !reg_is_state(r)) {
            continue;
        }
        uint32_t both = ka & kb;
        uint32_t bm = (both & 1u ? 0xFFu : 0) | (both & 2u ? 0xFF00u : 0) | (both & 4u ? 0xFF0000u : 0) |
                      (both & 8u ? 0xFF000000u : 0);
        if (((a->reg[r] ^ b->reg[r]) & bm) != 0) {
            note(d, r, a->reg[r], b->reg[r]);
        }
        if (ka != kb) {
            d->one_sided++;
        }
    }
    for (uint32_t i = 0; i < 96; i++) {
        if (a->unif_known[i] && b->unif_known[i]) {
            if (memcmp(a->unif[i], b->unif[i], 16) != 0) {
                note(d, 0x1000u + i, a->unif[i][0], b->unif[i][0]);
            }
        } else if (a->unif_known[i] != b->unif_known[i]) {
            d->one_sided++;
        }
    }
    for (uint32_t i = 0; i < 16; i++) {
        if (a->fixed_known[i] && b->fixed_known[i]) {
            if (memcmp(a->fixed[i], b->fixed[i], 12) != 0) {
                note(d, 0x2000u + i, a->fixed[i][0], b->fixed[i][0]);
            }
        } else if (a->fixed_known[i] != b->fixed_known[i]) {
            d->one_sided++;
        }
    }
    for (uint32_t i = 0; i < 128; i++) {
        if (a->fog_known[i] && b->fog_known[i]) {
            if (a->fog[i] != b->fog[i]) {
                note(d, 0x3000u + i, a->fog[i], b->fog[i]);
            }
        } else if (a->fog_known[i] != b->fog_known[i]) {
            d->one_sided++;
        }
    }
}
