/* GX state calls.
 *
 * Every setter stores the GameCube register state in `gx` and bumps
 * gx.version so the emitter rebuilds the PICA state before the next draw.
 * Inside a display list being recorded, a setter instead appends a port
 * opcode with its arguments (0xC0, call id, word count, LEB128 words),
 * and GXCallDisplayList replays it through the same setter, as the GameCube
 * replays the register writes the SDK put in the list.
 */
#include <string.h>

#include "gx_internal.h"
#include "port_addr.h"
#include "plat/plat.h"

GxState gx;

/* ---- Recording ---- */

enum {
    REC_SetVtxDesc = 1,
    REC_ClearVtxDesc,
    REC_SetVtxAttrFmt,
    REC_SetArray,
    REC_LoadPosMtx,
    REC_LoadNrmMtx,
    REC_LoadTexMtx,
    REC_SetCurrentMtx,
    REC_SetTexCoordGen2,
    REC_SetNumTexGens,
    REC_SetNumChans,
    REC_SetChanCtrl,
    REC_SetChanAmbColor,
    REC_SetChanMatColor,
    REC_LoadLightObj,
    REC_SetNumTevStages,
    REC_SetTevOrder,
    REC_SetTevColorIn,
    REC_SetTevAlphaIn,
    REC_SetTevColorOp,
    REC_SetTevAlphaOp,
    REC_SetTevColor,
    REC_SetTevColorS10,
    REC_SetTevKColor,
    REC_SetTevKColorSel,
    REC_SetTevKAlphaSel,
    REC_SetTevSwapMode,
    REC_SetTevSwapModeTable,
    REC_SetTevDirect,
    REC_LoadTexObj,
    REC_LoadTlut,
    REC_SetAlphaCompare,
    REC_SetBlendMode,
    REC_SetZMode,
    REC_SetZCompLoc,
    REC_SetColorUpdate,
    REC_SetAlphaUpdate,
    REC_SetCullMode,
    REC_SetFog,
    REC_SetScissor,
    REC_SetViewport,
    REC_SetProjection,
    REC_SetLineWidth,
    REC_SetNumIndStages,
    REC_SetTevIndirect,
    REC_COUNT
};

/* Words are stored as unsigned LEB128 (7 bits a byte, low first): most
 * arguments are small enums and indices, so a recorded call is about as
 * long as the GameCube's register writes for it, and display lists the
 * game builds into fixed heaps (makeDisplayList's map lists) fit as there. */
static void rec(u32 id, const u32* words, u32 n) {
    u8 head[3] = {GXDL_OP_PORT, (u8)id, (u8)n};
    gx_dl_put(head, 3);
    for (u32 i = 0; i < n; i++) {
        u8 b[5];
        u32 k = 0, v = words[i];
        do {
            b[k++] = (u8)((v & 0x7F) | (v >= 0x80 ? 0x80 : 0));
            v >>= 7;
        } while (v != 0);
        gx_dl_put(b, k);
    }
}

#define F2U(x) (((union { f32 f; u32 u; }){.f = (x)}).u)
#define U2F(x) (((union { u32 u; f32 f; }){.u = (x)}).f)
/* Every setter starts with REC/REC0: a primitive still open (no GXEnd) is
 * drawn first, with the state it was given (see gx_end_open_primitive). */
#define REC(id, ...)                                         \
    do {                                                     \
        GX_CLOSE_PRIMITIVE();                                \
        if (gx_dl_recording()) {                             \
            const u32 w_[] = {__VA_ARGS__};                  \
            rec(REC_##id, w_, sizeof(w_) / sizeof(w_[0]));   \
            return;                                          \
        }                                                    \
    } while (0)
#define REC0(id)                         \
    do {                                 \
        GX_CLOSE_PRIMITIVE();            \
        if (gx_dl_recording()) {         \
            rec(REC_##id, NULL, 0);      \
            return;                      \
        }                                \
    } while (0)

static inline void touch(void) {
    if (gx_prim_open) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx.version++;
}

/* A matrix or the projection changed. build_state reads neither (the
 * transform reads them per vertex), so the PICA state is not rebuilt for them
 * (switch gxstatever=0: they bump the state version as every setter does). */
static inline void touch_xf(void) {
    if (gx_prim_open) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    if (!gx_opt_statever) {
        gx.version++;
    }
}

static GXColor u2c(u32 v) { return (GXColor){(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; }
static u32 c2u(GXColor c) { return (u32)c.r << 24 | (u32)c.g << 16 | (u32)c.b << 8 | c.a; }

/* ---- Vertex description ---- */

void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    GX_CLOSE_PRIMITIVE();
    REC(SetVtxDesc, attr, type);
    if (attr == GX_VA_NBT) {
        gx.vtx_desc[GX_VA_NBT] = (u8)type;
        gx.vtx_desc[GX_VA_NRM] = GX_NONE;
    } else if ((u32)attr < GX_VA_MAX_ATTR) {
        gx.vtx_desc[attr] = (u8)type;
        if (attr == GX_VA_NRM) {
            gx.vtx_desc[GX_VA_NBT] = GX_NONE;
        }
    }
}

void GXSetVtxDescv(const GXVtxDescList* list) {
    GX_CLOSE_PRIMITIVE();
    for (; list->attr != GX_VA_NULL; list++) {
        GXSetVtxDesc(list->attr, list->type);
    }
}

void GXGetVtxDesc(GXAttr attr, GXAttrType* type) {
    *type = (u32)attr < GX_VA_MAX_ATTR ? (GXAttrType)gx.vtx_desc[attr] : GX_NONE;
}

void GXGetVtxDescv(GXVtxDescList* list) {
    int n = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        list[n].attr = (GXAttr)a;
        list[n].type = (GXAttrType)gx.vtx_desc[a];
        n++;
    }
    list[n].attr = GX_VA_NULL;
    list[n].type = GX_NONE;
}

void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* list) {
    int n = 0;
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        list[n].attr = (GXAttr)a;
        list[n].cnt = (GXCompCnt)gx.vat[fmt][a].cnt;
        list[n].type = (GXCompType)gx.vat[fmt][a].type;
        list[n].frac = gx.vat[fmt][a].frac;
        n++;
    }
    list[n].attr = GX_VA_NULL;
}

void GXClearVtxDesc(void) {
    GX_CLOSE_PRIMITIVE();
    REC0(ClearVtxDesc);
    memset(gx.vtx_desc, GX_NONE, sizeof(gx.vtx_desc));
    gx.vtx_desc[GX_VA_POS] = GX_DIRECT;
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    GX_CLOSE_PRIMITIVE();
    REC(SetVtxAttrFmt, vtxfmt, attr, cnt, type, frac);
    if ((u32)vtxfmt >= GX_MAX_VTXFMT) {
        return;
    }
    if (attr == GX_VA_NBT) {
        attr = GX_VA_NRM;
        cnt = GX_NRM_NBT;
    }
    if ((u32)attr >= GX_VA_MAX_ATTR) {
        return;
    }
    gx.vat[vtxfmt][attr] = (GxAttrFmt){(u8)cnt, (u8)type, frac};
}

void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) {
    GX_CLOSE_PRIMITIVE();
    for (; list->attr != GX_VA_NULL; list++) {
        GXSetVtxAttrFmt(vtxfmt, list->attr, list->cnt, list->type, list->frac);
    }
}

void GXSetArray(GXAttr attr, void* base_ptr, u8 stride) {
    GX_CLOSE_PRIMITIVE();
    REC(SetArray, attr, (u32)base_ptr, stride);
    if (attr == GX_VA_NBT) {
        attr = GX_VA_NRM;
    }
    if ((u32)attr < GX_VA_MAX_ATTR) {
        gx.array_base[attr] = base_ptr;
        gx.array_stride[attr] = stride;
    }
}

void GXInvalidateVtxCache(void) {
    GX_CLOSE_PRIMITIVE();
}

/* ---- Transform ---- */

void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id) {
    REC(LoadPosMtx, id, F2U(mtx[0][0]), F2U(mtx[0][1]), F2U(mtx[0][2]), F2U(mtx[0][3]), F2U(mtx[1][0]),
        F2U(mtx[1][1]), F2U(mtx[1][2]), F2U(mtx[1][3]), F2U(mtx[2][0]), F2U(mtx[2][1]), F2U(mtx[2][2]),
        F2U(mtx[2][3]));
    u32 i = id / 3;
    if (i < GX_POS_MTX_COUNT) {
        memcpy(gx.pos_mtx[i], mtx, sizeof(gx.pos_mtx[i]));
        touch_xf();
    }
}

void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id) {
    REC(LoadNrmMtx, id, F2U(mtx[0][0]), F2U(mtx[0][1]), F2U(mtx[0][2]), F2U(mtx[1][0]), F2U(mtx[1][1]),
        F2U(mtx[1][2]), F2U(mtx[2][0]), F2U(mtx[2][1]), F2U(mtx[2][2]));
    u32 i = id / 3;
    if (i < GX_POS_MTX_COUNT) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                gx.nrm_mtx[i][r][c] = mtx[r][c];
            }
        }
        touch_xf();
    }
}

void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, GXTexMtxType type) {
    GX_CLOSE_PRIMITIVE();
    int rows = type == GX_MTX3x4 ? 3 : 2;
    REC(LoadTexMtx, id, type, F2U(mtx[0][0]), F2U(mtx[0][1]), F2U(mtx[0][2]), F2U(mtx[0][3]), F2U(mtx[1][0]),
        F2U(mtx[1][1]), F2U(mtx[1][2]), F2U(mtx[1][3]), F2U(rows == 3 ? mtx[2][0] : 0.0f),
        F2U(rows == 3 ? mtx[2][1] : 0.0f), F2U(rows == 3 ? mtx[2][2] : 0.0f), F2U(rows == 3 ? mtx[2][3] : 1.0f));
    f32 (*dst)[4];
    if (id >= GX_PTTEXMTX0) {
        u32 i = (id - GX_PTTEXMTX0) / 3;
        if (i >= GX_PTTEX_MTX_COUNT) {
            return;
        }
        dst = gx.pttex_mtx[i];
    } else if (id >= GX_TEXMTX0) {
        u32 i = (id - GX_TEXMTX0) / 3;
        if (i >= GX_TEX_MTX_COUNT) {
            return;
        }
        dst = gx.tex_mtx[i];
        gx.tex_mtx_is_3x4[i] = type == GX_MTX3x4;
    } else {
        return;
    }
    memcpy(dst, mtx, sizeof(f32) * 4 * (size_t)rows);
    if (rows == 2) {
        dst[2][0] = dst[2][1] = dst[2][3] = 0.0f;
        dst[2][2] = 1.0f;
    }
    touch_xf();
}

void GXSetCurrentMtx(u32 id) {
    GX_CLOSE_PRIMITIVE();
    REC(SetCurrentMtx, id);
    gx.cur_pnmtx = id / 3;
}

void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) {
    GX_CLOSE_PRIMITIVE();
    gx.proj[0] = (f32)type;
    gx.proj[1] = mtx[0][0];
    gx.proj[3] = mtx[1][1];
    gx.proj[5] = mtx[2][2];
    gx.proj[6] = mtx[2][3];
    if (type == GX_PERSPECTIVE) {
        gx.proj[2] = mtx[0][2];
        gx.proj[4] = mtx[1][2];
    } else {
        gx.proj[2] = mtx[0][3];
        gx.proj[4] = mtx[1][3];
    }
    REC(SetProjection, F2U(gx.proj[0]), F2U(gx.proj[1]), F2U(gx.proj[2]), F2U(gx.proj[3]), F2U(gx.proj[4]),
        F2U(gx.proj[5]), F2U(gx.proj[6]));
    touch_xf();
}

void GXSetProjectionv(const f32* ptr) {
    GX_CLOSE_PRIMITIVE();
    memcpy(gx.proj, ptr, sizeof(gx.proj));
    touch_xf();
}

void GXGetProjectionv(f32* ptr) { memcpy(ptr, gx.proj, sizeof(gx.proj)); }

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    REC(SetViewport, F2U(left), F2U(top), F2U(wd), F2U(ht), F2U(nearz), F2U(farz));
    gx.vp[0] = left;
    gx.vp[1] = top;
    gx.vp[2] = wd;
    gx.vp[3] = ht;
    gx.vp[4] = nearz;
    gx.vp[5] = farz;
    touch();
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    GX_CLOSE_PRIMITIVE();
    /* Field jitter moves the viewport half a line for interlaced output;
     * the 3DS screen is progressive. */
    (void)field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXGetViewportv(f32* vp) { memcpy(vp, gx.vp, sizeof(gx.vp)); }

void GXSetZScaleOffset(f32 scale, f32 offset) {
    GX_CLOSE_PRIMITIVE();
    gx.zscale = scale;
    gx.zoffset = offset;
    touch_xf();
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    REC(SetScissor, left, top, wd, ht);
    gx.scissor[0] = left;
    gx.scissor[1] = top;
    gx.scissor[2] = wd;
    gx.scissor[3] = ht;
    touch();
}

void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    *left = gx.scissor[0];
    *top = gx.scissor[1];
    *wd = gx.scissor[2];
    *ht = gx.scissor[3];
}

void GXSetScissorBoxOffset(s32 x_off, s32 y_off) {
    GX_CLOSE_PRIMITIVE();
    gx.scissor_off[0] = x_off;
    gx.scissor_off[1] = y_off;
    touch();
}

/* ---- Texture coordinate generation ---- */

void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize,
                       u32 pt_texmtx) {
    REC(SetTexCoordGen2, dst_coord, func, src_param, mtx, normalize, pt_texmtx);
    if ((u32)dst_coord >= GX_MAX_TEXGEN) {
        return;
    }
    gx.texgen[dst_coord].func = (u8)func;
    gx.texgen[dst_coord].src = (u8)src_param;
    gx.texgen[dst_coord].mtx = mtx;
    gx.texgen[dst_coord].normalize = normalize;
    gx.texgen[dst_coord].pt_mtx = pt_texmtx;
    gx.cur_texmtx[dst_coord] = (u8)mtx;
    touch();
}

void GXSetNumTexGens(u8 nTexGens) {
    REC(SetNumTexGens, nTexGens);
    gx.num_texgens = nTexGens;
    touch();
}

/* ---- Lighting ---- */

void GXSetNumChans(u8 nChans) {
    REC(SetNumChans, nChans);
    gx.num_chans = nChans;
    touch();
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
                   GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    REC(SetChanCtrl, chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn);
    int first, last;
    switch (chan) {
    case GX_COLOR0A0: first = 0; last = 2; break; /* COLOR0 and ALPHA0 */
    case GX_COLOR1A1: first = 1; last = 3; break;
    default: first = last = (int)chan; break;
    }
    for (int c = first; c <= last; c += 2) {
        if (c > 3) {
            break;
        }
        gx.chan[c].enable = enable;
        gx.chan[c].amb_src = (u8)amb_src;
        gx.chan[c].mat_src = (u8)mat_src;
        gx.chan[c].light_mask = light_mask;
        gx.chan[c].diff_fn = (u8)diff_fn;
        gx.chan[c].attn_fn = (u8)attn_fn;
        if (first == last) {
            break;
        }
    }
    touch();
}

static void set_chan_color(GXColor* dst, GXChannelID chan, GXColor c) {
    switch (chan) {
    case GX_COLOR0: dst[0].r = c.r; dst[0].g = c.g; dst[0].b = c.b; break;
    case GX_COLOR1: dst[1].r = c.r; dst[1].g = c.g; dst[1].b = c.b; break;
    case GX_ALPHA0: dst[0].a = c.a; break;
    case GX_ALPHA1: dst[1].a = c.a; break;
    case GX_COLOR0A0: dst[0] = c; break;
    case GX_COLOR1A1: dst[1] = c; break;
    default: break;
    }
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    REC(SetChanAmbColor, chan, c2u(amb_color));
    set_chan_color(gx.amb, chan, amb_color);
    touch();
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    REC(SetChanMatColor, chan, c2u(mat_color));
    set_chan_color(gx.mat, chan, mat_color);
    touch();
}

/* GXLightObj: our GxLightObj inside the SDK's 64 bytes. */
_Static_assert(sizeof(GxLightObj) <= sizeof(GXLightObj), "GxLightObj must fit GXLightObj");

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    GxLightObj* l = (GxLightObj*)lt_obj;
    l->a[0] = a0;
    l->a[1] = a1;
    l->a[2] = a2;
    l->k[0] = k0;
    l->k[1] = k1;
    l->k[2] = k2;
}

void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func) {
    /* The SDK's coefficients (GXLight.c). */
    f32 a0, a1, a2, r, d, cr;
    if (cutoff <= 0.0f || cutoff > 90.0f) {
        spot_func = GX_SP_OFF;
    }
    r = cutoff * 3.1415927f / 180.0f;
    cr = cosf(r);
    switch (spot_func) {
    case GX_SP_FLAT:
        a0 = -1000.0f * cr;
        a1 = 1000.0f;
        a2 = 0.0f;
        break;
    case GX_SP_COS:
        a0 = -cr / (1.0f - cr);
        a1 = 1.0f / (1.0f - cr);
        a2 = 0.0f;
        break;
    case GX_SP_COS2:
        a0 = 0.0f;
        a1 = -cr / (1.0f - cr);
        a2 = 1.0f / (1.0f - cr);
        break;
    case GX_SP_SHARP:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = cr * (cr - 2.0f) / d;
        a1 = 2.0f / d;
        a2 = -1.0f / d;
        break;
    case GX_SP_RING1:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = -4.0f * cr / d;
        a1 = 4.0f * (1.0f + cr) / d;
        a2 = -4.0f / d;
        break;
    case GX_SP_RING2:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = 1.0f - 2.0f * cr * cr / d;
        a1 = 4.0f * cr / d;
        a2 = -2.0f / d;
        break;
    default:
        a0 = 1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        break;
    }
    GxLightObj* l = (GxLightObj*)lt_obj;
    l->a[0] = a0;
    l->a[1] = a1;
    l->a[2] = a2;
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func) {
    f32 k0, k1, k2;
    if (ref_dist < 0.0f || ref_br <= 0.0f || ref_br >= 1.0f) {
        dist_func = GX_DA_OFF;
    }
    switch (dist_func) {
    case GX_DA_GENTLE:
        k0 = 1.0f;
        k1 = (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.0f;
        break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_STEEP:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    default:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = 0.0f;
        break;
    }
    GxLightObj* l = (GxLightObj*)lt_obj;
    l->k[0] = k0;
    l->k[1] = k1;
    l->k[2] = k2;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) {
    GxLightObj* l = (GxLightObj*)lt_obj;
    l->pos[0] = x;
    l->pos[1] = y;
    l->pos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    GxLightObj* l = (GxLightObj*)lt_obj;
    l->dir[0] = -nx;
    l->dir[1] = -ny;
    l->dir[2] = -nz;
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color) { ((GxLightObj*)lt_obj)->color = color; }

void GXLoadLightObjImm(const GXLightObj* lt_obj, GXLightID light) {
    GX_CLOSE_PRIMITIVE();
    const GxLightObj* l = (const GxLightObj*)lt_obj;
    int idx = 31 - __builtin_clz((u32)light & 0xFF);
    if (gx_dl_recording()) {
        const u32* w = (const u32*)l;
        u32 words[1 + sizeof(GxLightObj) / 4];
        words[0] = (u32)light;
        memcpy(words + 1, w, sizeof(GxLightObj));
        rec(REC_LoadLightObj, words, sizeof(words) / 4);
        return;
    }
    if (idx >= 0 && idx < GX_MAX_LIGHT) {
        gx.light[idx] = *l;
        touch();
    }
}

/* ---- TEV ---- */

void GXSetNumTevStages(u8 nStages) {
    REC(SetNumTevStages, nStages);
    gx.num_tev = nStages;
    touch();
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    REC(SetTevOrder, stage, coord, map, color);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    gx.tev[stage].coord = (u8)coord;
    gx.tev[stage].map = (u8)(map == GX_TEXMAP_NULL ? 0xFF : (map & 0xFF));
    gx.tev[stage].color = (u8)color;
    touch();
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    REC(SetTevColorIn, stage, a, b, c, d);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    gx.tev[stage].c_in[0] = (u8)a;
    gx.tev[stage].c_in[1] = (u8)b;
    gx.tev[stage].c_in[2] = (u8)c;
    gx.tev[stage].c_in[3] = (u8)d;
    touch();
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    REC(SetTevAlphaIn, stage, a, b, c, d);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    gx.tev[stage].a_in[0] = (u8)a;
    gx.tev[stage].a_in[1] = (u8)b;
    gx.tev[stage].a_in[2] = (u8)c;
    gx.tev[stage].a_in[3] = (u8)d;
    touch();
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                     GXTevRegID out_reg) {
    REC(SetTevColorOp, stage, op, bias, scale, clamp, out_reg);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    gx.tev[stage].c_op = (u8)op;
    gx.tev[stage].c_bias = (u8)bias;
    gx.tev[stage].c_scale = (u8)scale;
    gx.tev[stage].c_clamp = clamp;
    gx.tev[stage].c_out = (u8)out_reg;
    touch();
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                     GXTevRegID out_reg) {
    REC(SetTevAlphaOp, stage, op, bias, scale, clamp, out_reg);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    gx.tev[stage].a_op = (u8)op;
    gx.tev[stage].a_bias = (u8)bias;
    gx.tev[stage].a_scale = (u8)scale;
    gx.tev[stage].a_clamp = clamp;
    gx.tev[stage].a_out = (u8)out_reg;
    touch();
}

/* GXSetTevOp: the SDK's presets (GXTev.c). */
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    GX_CLOSE_PRIMITIVE();
    GXTevColorArg carg = id == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    GXTevAlphaArg aarg = id == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(id, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
        break;
    case GX_BLEND:
        GXSetTevColorIn(id, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    case GX_PASSCLR:
    default:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
        break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GXSetTevColor(GXTevRegID id, GXColor color) {
    REC(SetTevColor, id, c2u(color));
    if ((u32)id < 4) {
        gx.tev_reg[id] = (GXColorS10){color.r, color.g, color.b, color.a};
        touch();
    }
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    REC(SetTevColorS10, id, (u32)(u16)color.r << 16 | (u16)color.g, (u32)(u16)color.b << 16 | (u16)color.a);
    if ((u32)id < 4) {
        gx.tev_reg[id] = color;
        touch();
    }
}

void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    REC(SetTevKColor, id, c2u(color));
    if ((u32)id < 4) {
        gx.tev_kcolor[id] = color;
        touch();
    }
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    REC(SetTevKColorSel, stage, sel);
    if ((u32)stage < GX_MAX_TEV) {
        gx.tev[stage].kc_sel = (u8)sel;
        touch();
    }
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    REC(SetTevKAlphaSel, stage, sel);
    if ((u32)stage < GX_MAX_TEV) {
        gx.tev[stage].ka_sel = (u8)sel;
        touch();
    }
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    REC(SetTevSwapMode, stage, ras_sel, tex_sel);
    if ((u32)stage < GX_MAX_TEV) {
        gx.tev[stage].ras_swap = (u8)ras_sel;
        gx.tev[stage].tex_swap = (u8)tex_sel;
        touch();
    }
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                           GXTevColorChan alpha) {
    REC(SetTevSwapModeTable, table, red, green, blue, alpha);
    if ((u32)table < 4) {
        gx.swap_table[table][0] = (u8)red;
        gx.swap_table[table][1] = (u8)green;
        gx.swap_table[table][2] = (u8)blue;
        gx.swap_table[table][3] = (u8)alpha;
        touch();
    }
}

void GXSetTevDirect(GXTevStageID tev_stage) {
    REC(SetTevDirect, tev_stage);
    if ((u32)tev_stage < GX_MAX_TEV) {
        gx.tev[tev_stage].ind_enabled = 0;
        touch();
    }
}

void GXSetTevIndWarp(GXTevStageID tev_stage, GXIndTexStageID ind_stage, u8 signed_offset, u8 replace_mode,
                     GXIndTexMtxID matrix_sel) {
    REC(SetTevIndirect, tev_stage, ind_stage, signed_offset, replace_mode, matrix_sel);
    if ((u32)tev_stage >= GX_MAX_TEV) {
        return;
    }
    gx.tev[tev_stage].ind_enabled = 1;
    gx.tev[tev_stage].ind_stage = (u8)ind_stage;
    gx.tev[tev_stage].ind_fmt = GX_ITF_8;
    gx.tev[tev_stage].ind_bias = signed_offset ? GX_ITB_STU : GX_ITB_NONE;
    gx.tev[tev_stage].ind_mtx = (u8)matrix_sel;
    gx.tev[tev_stage].ind_wrap_s = replace_mode ? 6 /* GX_ITW_0 */ : 0;
    gx.tev[tev_stage].ind_wrap_t = replace_mode ? 6 : 0;
    gx.tev[tev_stage].ind_add_prev = 0;
    touch();
}

void GXSetNumIndStages(u8 nIndStages) {
    REC(SetNumIndStages, nIndStages);
    gx.num_ind = nIndStages;
    touch();
}

void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    GX_CLOSE_PRIMITIVE();
    if ((u32)ind_stage < 4) {
        gx.ind[ind_stage].coord = (u8)tex_coord;
        gx.ind[ind_stage].map = (u8)tex_map;
        touch();
    }
}

void GXSetIndTexCoordScale(GXIndTexStageID ind_state, GXIndTexScale scale_s, GXIndTexScale scale_t) {
    GX_CLOSE_PRIMITIVE();
    if ((u32)ind_state < 4) {
        gx.ind[ind_state].scale_s = (u8)scale_s;
        gx.ind[ind_state].scale_t = (u8)scale_t;
        touch();
    }
}

void GXSetIndTexMtx(GXIndTexMtxID mtx_id, const f32 offset[2][3], s8 scale_exp) {
    GX_CLOSE_PRIMITIVE();
    int i = (int)mtx_id - GX_ITM_0;
    if (i >= 0 && i < 3) {
        memcpy(gx.ind_mtx[i], offset, sizeof(gx.ind_mtx[i]));
        gx.ind_mtx_scale[i] = scale_exp;
        touch();
    }
}

void __GXSetIndirectMask(u32 mask) { (void)mask; }

/* ---- Pixel engine ---- */

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    REC(SetAlphaCompare, comp0, ref0, op, comp1, ref1);
    gx.alpha_comp0 = (u8)comp0;
    gx.alpha_ref0 = ref0;
    gx.alpha_op = (u8)op;
    gx.alpha_comp1 = (u8)comp1;
    gx.alpha_ref1 = ref1;
    touch();
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    REC(SetBlendMode, type, src_factor, dst_factor, op);
    gx.blend_type = (u8)type;
    gx.blend_src = (u8)src_factor;
    gx.blend_dst = (u8)dst_factor;
    gx.blend_logic = (u8)op;
    touch();
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    REC(SetZMode, compare_enable, func, update_enable);
    gx.z_enable = compare_enable;
    gx.z_func = (u8)func;
    gx.z_update = update_enable;
    touch();
}

void GXSetZCompLoc(GXBool before_tex) {
    REC(SetZCompLoc, before_tex);
    gx.z_before_tex = before_tex;
    touch();
}

void GXSetColorUpdate(GXBool update_enable) {
    REC(SetColorUpdate, update_enable);
    gx.color_update = update_enable;
    touch();
}

void GXSetAlphaUpdate(GXBool update_enable) {
    REC(SetAlphaUpdate, update_enable);
    gx.alpha_update = update_enable;
    touch();
}

void GXSetDstAlpha(GXBool enable, u8 alpha) {
    GX_CLOSE_PRIMITIVE();
    gx.dst_alpha_enable = enable;
    gx.dst_alpha = alpha;
    touch();
}

void GXSetCullMode(GXCullMode mode) {
    REC(SetCullMode, mode);
    gx.cull = (u8)mode;
    touch();
}

void GXGetCullMode(GXCullMode* mode) { *mode = (GXCullMode)gx.cull; }

void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) {
    GX_CLOSE_PRIMITIVE();
    gx.pixel_fmt = (u8)pix_fmt;
    gx.z_fmt = (u8)z_fmt;
    touch();
}

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    REC(SetFog, type, F2U(startz), F2U(endz), F2U(nearz), F2U(farz), c2u(color));
    gx.fog_type = (u8)type;
    gx.fog_start = startz;
    gx.fog_end = endz;
    gx.fog_near = nearz;
    gx.fog_far = farz;
    gx.fog_color = color;
    touch();
}

void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    GX_CLOSE_PRIMITIVE();
    gx.ztex_op = (u8)op;
    gx.ztex_fmt = (u8)fmt;
    gx.ztex_bias = bias;
    touch();
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets) {
    REC(SetLineWidth, width, texOffsets);
    gx.line_width = width;
    touch();
}

/* ---- Textures (objects live in gx_tex.c; loading a map is state) ---- */

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    GX_CLOSE_PRIMITIVE();
    if (gx_dl_recording()) {
        u32 words[1 + sizeof(GxTexObj) / 4];
        words[0] = (u32)id;
        memcpy(words + 1, obj, sizeof(GxTexObj));
        rec(REC_LoadTexObj, words, sizeof(words) / 4);
        return;
    }
    if ((u32)id < GX_MAX_TEXMAP) {
        const GxTexObj* t = (const GxTexObj*)obj;
        const int fmt_ok = t->format <= GX_TF_CMPR || t->format == GX_TF_Z8 || t->format == GX_TF_Z16 ||
                           t->format == GX_TF_Z24X8;
        if (!PORT_IS_ADDR(t->image) || !fmt_ok) {
            /* never initialised by GXInitTexObj; logged once per caller */
            static void* seen[16];
            void* ra = __builtin_return_address(0);
            int i;
            for (i = 0; i < 16 && seen[i] != NULL && seen[i] != ra; i++) {
            }
            if (i < 16 && seen[i] == NULL) {
                seen[i] = ra;
                PLOG_W("GX: GXLoadTexObj(%p) from %p: not a texture object (image %p, format %u)", obj, ra, t->image,
                       t->format);
            }
        }
        gx.tex_map[id] = *(GxTexObj*)obj;
        gx.tex_map_valid[id] = 1;
        touch();
    }
}

void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name) {
    GX_CLOSE_PRIMITIVE();
    if (gx_dl_recording()) {
        u32 words[1 + sizeof(GxTlutObj) / 4];
        words[0] = tlut_name;
        memcpy(words + 1, tlut_obj, sizeof(GxTlutObj));
        rec(REC_LoadTlut, words, sizeof(words) / 4);
        return;
    }
    if (tlut_name < 20) {
        gx.tlut[tlut_name] = *(GxTlutObj*)tlut_obj;
        touch();
    }
}

/* ---- Display list replay of recorded state calls ---- */

void gx_dl_replay_port_op(const u8** pp, const u8* end) {
    const u8* p = *pp;
    if (end - p < 3) {
        plat_fatal("GX: truncated port opcode in a display list");
    }
    u32 id = p[1];
    u32 n = p[2];
    p += 3;
    u32 w[32];
    if (n > 32) {
        plat_fatal("GX: port opcode %lu with %lu words", (unsigned long)id, (unsigned long)n);
    }
    for (u32 i = 0; i < n; i++) {
        u32 v = 0;
        for (u32 shift = 0;; shift += 7) {
            if (p >= end || shift > 28) {
                plat_fatal("GX: truncated port opcode %lu", (unsigned long)id);
            }
            u8 b = *p++;
            v |= (u32)(b & 0x7F) << shift;
            if (!(b & 0x80)) {
                break;
            }
        }
        w[i] = v;
    }
    *pp = p;
    switch (id) {
    case REC_SetVtxDesc: GXSetVtxDesc((GXAttr)w[0], (GXAttrType)w[1]); break;
    case REC_ClearVtxDesc: GXClearVtxDesc(); break;
    case REC_SetVtxAttrFmt:
        GXSetVtxAttrFmt((GXVtxFmt)w[0], (GXAttr)w[1], (GXCompCnt)w[2], (GXCompType)w[3], (u8)w[4]);
        break;
    case REC_SetArray: GXSetArray((GXAttr)w[0], (void*)w[1], (u8)w[2]); break;
    case REC_LoadPosMtx: {
        f32 m[3][4];
        for (int i = 0; i < 12; i++) {
            m[i / 4][i % 4] = U2F(w[1 + i]);
        }
        GXLoadPosMtxImm(m, w[0]);
        break;
    }
    case REC_LoadNrmMtx: {
        f32 m[3][4] = {{0}};
        for (int i = 0; i < 9; i++) {
            m[i / 3][i % 3] = U2F(w[1 + i]);
        }
        GXLoadNrmMtxImm(m, w[0]);
        break;
    }
    case REC_LoadTexMtx: {
        f32 m[3][4];
        for (int i = 0; i < 12; i++) {
            m[i / 4][i % 4] = U2F(w[2 + i]);
        }
        GXLoadTexMtxImm(m, w[0], (GXTexMtxType)w[1]);
        break;
    }
    case REC_SetCurrentMtx: GXSetCurrentMtx(w[0]); break;
    case REC_SetTexCoordGen2:
        GXSetTexCoordGen2((GXTexCoordID)w[0], (GXTexGenType)w[1], (GXTexGenSrc)w[2], w[3], (GXBool)w[4], w[5]);
        break;
    case REC_SetNumTexGens: GXSetNumTexGens((u8)w[0]); break;
    case REC_SetNumChans: GXSetNumChans((u8)w[0]); break;
    case REC_SetChanCtrl:
        GXSetChanCtrl((GXChannelID)w[0], (GXBool)w[1], (GXColorSrc)w[2], (GXColorSrc)w[3], w[4], (GXDiffuseFn)w[5],
                      (GXAttnFn)w[6]);
        break;
    case REC_SetChanAmbColor: GXSetChanAmbColor((GXChannelID)w[0], u2c(w[1])); break;
    case REC_SetChanMatColor: GXSetChanMatColor((GXChannelID)w[0], u2c(w[1])); break;
    case REC_LoadLightObj: {
        GXLightObj l;
        memcpy(&l, w + 1, sizeof(GxLightObj));
        GXLoadLightObjImm(&l, (GXLightID)w[0]);
        break;
    }
    case REC_SetNumTevStages: GXSetNumTevStages((u8)w[0]); break;
    case REC_SetTevOrder:
        GXSetTevOrder((GXTevStageID)w[0], (GXTexCoordID)w[1], (GXTexMapID)w[2], (GXChannelID)w[3]);
        break;
    case REC_SetTevColorIn:
        GXSetTevColorIn((GXTevStageID)w[0], (GXTevColorArg)w[1], (GXTevColorArg)w[2], (GXTevColorArg)w[3],
                        (GXTevColorArg)w[4]);
        break;
    case REC_SetTevAlphaIn:
        GXSetTevAlphaIn((GXTevStageID)w[0], (GXTevAlphaArg)w[1], (GXTevAlphaArg)w[2], (GXTevAlphaArg)w[3],
                        (GXTevAlphaArg)w[4]);
        break;
    case REC_SetTevColorOp:
        GXSetTevColorOp((GXTevStageID)w[0], (GXTevOp)w[1], (GXTevBias)w[2], (GXTevScale)w[3], (GXBool)w[4],
                        (GXTevRegID)w[5]);
        break;
    case REC_SetTevAlphaOp:
        GXSetTevAlphaOp((GXTevStageID)w[0], (GXTevOp)w[1], (GXTevBias)w[2], (GXTevScale)w[3], (GXBool)w[4],
                        (GXTevRegID)w[5]);
        break;
    case REC_SetTevColor: GXSetTevColor((GXTevRegID)w[0], u2c(w[1])); break;
    case REC_SetTevColorS10:
        GXSetTevColorS10((GXTevRegID)w[0],
                         (GXColorS10){(s16)(w[1] >> 16), (s16)w[1], (s16)(w[2] >> 16), (s16)w[2]});
        break;
    case REC_SetTevKColor: GXSetTevKColor((GXTevKColorID)w[0], u2c(w[1])); break;
    case REC_SetTevKColorSel: GXSetTevKColorSel((GXTevStageID)w[0], (GXTevKColorSel)w[1]); break;
    case REC_SetTevKAlphaSel: GXSetTevKAlphaSel((GXTevStageID)w[0], (GXTevKAlphaSel)w[1]); break;
    case REC_SetTevSwapMode: GXSetTevSwapMode((GXTevStageID)w[0], (GXTevSwapSel)w[1], (GXTevSwapSel)w[2]); break;
    case REC_SetTevSwapModeTable:
        GXSetTevSwapModeTable((GXTevSwapSel)w[0], (GXTevColorChan)w[1], (GXTevColorChan)w[2], (GXTevColorChan)w[3],
                              (GXTevColorChan)w[4]);
        break;
    case REC_SetTevDirect: GXSetTevDirect((GXTevStageID)w[0]); break;
    case REC_LoadTexObj: {
        GXTexObj obj;
        memcpy(&obj, w + 1, sizeof(GxTexObj));
        GXLoadTexObj(&obj, (GXTexMapID)w[0]);
        break;
    }
    case REC_LoadTlut: {
        GXTlutObj t;
        memcpy(&t, w + 1, sizeof(GxTlutObj));
        GXLoadTlut(&t, w[0]);
        break;
    }
    case REC_SetAlphaCompare:
        GXSetAlphaCompare((GXCompare)w[0], (u8)w[1], (GXAlphaOp)w[2], (GXCompare)w[3], (u8)w[4]);
        break;
    case REC_SetBlendMode:
        GXSetBlendMode((GXBlendMode)w[0], (GXBlendFactor)w[1], (GXBlendFactor)w[2], (GXLogicOp)w[3]);
        break;
    case REC_SetZMode: GXSetZMode((GXBool)w[0], (GXCompare)w[1], (GXBool)w[2]); break;
    case REC_SetZCompLoc: GXSetZCompLoc((GXBool)w[0]); break;
    case REC_SetColorUpdate: GXSetColorUpdate((GXBool)w[0]); break;
    case REC_SetAlphaUpdate: GXSetAlphaUpdate((GXBool)w[0]); break;
    case REC_SetCullMode: GXSetCullMode((GXCullMode)w[0]); break;
    case REC_SetFog:
        GXSetFog((GXFogType)w[0], U2F(w[1]), U2F(w[2]), U2F(w[3]), U2F(w[4]), u2c(w[5]));
        break;
    case REC_SetScissor: GXSetScissor(w[0], w[1], w[2], w[3]); break;
    case REC_SetViewport: GXSetViewport(U2F(w[0]), U2F(w[1]), U2F(w[2]), U2F(w[3]), U2F(w[4]), U2F(w[5])); break;
    case REC_SetProjection: {
        f32 v[7];
        for (int i = 0; i < 7; i++) {
            v[i] = U2F(w[i]);
        }
        GXSetProjectionv(v);
        break;
    }
    case REC_SetLineWidth: GXSetLineWidth((u8)w[0], (GXTexOffset)w[1]); break;
    case REC_SetNumIndStages: GXSetNumIndStages((u8)w[0]); break;
    case REC_SetTevIndirect:
        GXSetTevIndWarp((GXTevStageID)w[0], (GXIndTexStageID)w[1], (u8)w[2], (u8)w[3], (GXIndTexMtxID)w[4]);
        break;
    default: plat_fatal("GX: unknown port opcode %lu in a display list", (unsigned long)id);
    }
}

/* ---- Defaults (the SDK's GXInit state) ---- */

void gx_state_init(u16 efb_w, u16 efb_h) {
    gx_opts_init();
    memset(&gx, 0, sizeof(gx));
    gx.efb_w = efb_w;
    gx.efb_h = efb_h;
    GXClearVtxDesc();
    for (int f = 0; f < GX_MAX_VTXFMT; f++) {
        GXSetVtxAttrFmt((GXVtxFmt)f, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt((GXVtxFmt)f, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt((GXVtxFmt)f, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXSetVtxAttrFmt((GXVtxFmt)f, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
        for (int t = 0; t < 8; t++) {
            GXSetVtxAttrFmt((GXVtxFmt)f, (GXAttr)(GX_VA_TEX0 + t), GX_TEX_ST, GX_F32, 0);
        }
    }
    f32 ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    for (int i = 0; i < GX_POS_MTX_COUNT; i++) {
        GXLoadPosMtxImm(ident, (u32)i * 3);
        GXLoadNrmMtxImm(ident, (u32)i * 3);
    }
    for (int i = 0; i < GX_TEX_MTX_COUNT; i++) {
        GXLoadTexMtxImm(ident, GX_TEXMTX0 + (u32)i * 3, GX_MTX3x4);
    }
    for (int i = 0; i < GX_PTTEX_MTX_COUNT; i++) {
        GXLoadTexMtxImm(ident, GX_PTTEXMTX0 + (u32)i * 3, GX_MTX3x4);
    }
    GXSetCurrentMtx(GX_PNMTX0);
    for (int t = 0; t < GX_MAX_TEXGEN; t++) {
        GXSetTexCoordGen2((GXTexCoordID)t, GX_TG_MTX2x4, (GXTexGenSrc)(GX_TG_TEX0 + t), GX_IDENTITY, GX_FALSE,
                          GX_PTIDENTITY);
    }
    GXSetNumTexGens(1);
    GXSetNumChans(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR0A0, (GXColor){0, 0, 0, 255});
    GXSetChanAmbColor(GX_COLOR1A1, (GXColor){0, 0, 0, 255});
    GXSetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});
    GXSetChanMatColor(GX_COLOR1A1, (GXColor){255, 255, 255, 255});
    for (int s = 0; s < GX_MAX_TEV; s++) {
        GXSetTevOrder((GXTevStageID)s, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        GXSetTevOp((GXTevStageID)s, GX_REPLACE);
        GXSetTevKColorSel((GXTevStageID)s, GX_TEV_KCSEL_1_4);
        GXSetTevKAlphaSel((GXTevStageID)s, GX_TEV_KASEL_1);
        GXSetTevSwapMode((GXTevStageID)s, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevDirect((GXTevStageID)s);
    }
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetNumTevStages(1);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetNumIndStages(0);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZCompLoc(GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetCullMode(GX_CULL_BACK);
    GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0, 0, 0, 0});
    GXSetScissor(0, 0, efb_w, efb_h);
    GXSetScissorBoxOffset(0, 0);
    GXSetViewport(0.0f, 0.0f, (f32)efb_w, (f32)efb_h, 0.0f, 1.0f);
    gx.zscale = 1.0f;
    gx.line_width = 6;
    gx.copy_clear_color = (GXColor){0, 0, 0, 255};
    gx.copy_clear_z = 0xFFFFFF;
    gx.disp_yscale = 1.0f;
}
