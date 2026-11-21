/* GX front end for the 3DS port: internal state.
 *
 * The front end keeps the GameCube's GX state in C structs (the register
 * state the SDK would have written to the FIFO), assembles vertices from the
 * write-gather pipe and display lists, transforms, lights and texture-maps
 * them on the CPU exactly as the GX hardware does, compiles the TEV setup
 * into PICA200 combiner stages, and records draws for the back end
 * (include/plat/gpu.h).
 */
#ifndef PORT3DS_GX_INTERNAL_H
#define PORT3DS_GX_INTERNAL_H

#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include "plat/gpu.h"

#define GX_MAX_TEV 16
#define GX_MAX_TEXGEN 8
#define GX_MAX_TEXMAP 8
#define GX_MAX_LIGHT 8
#define GX_POS_MTX_COUNT 10   /* GX_PNMTX0..9 */
#define GX_TEX_MTX_COUNT 10   /* GX_TEXMTX0..9 */
#define GX_PTTEX_MTX_COUNT 20 /* GX_PTTEXMTX0..19 */

/* Our layout inside the SDK's opaque objects (sizes fixed by the SDK). */
typedef struct {
    void* image;
    u16 width, height;
    u8 format;        /* GXTexFmt, or GXCITexFmt with is_ci */
    u8 is_ci;
    u8 wrap_s, wrap_t;
    u8 mipmap;
    u8 min_filter, mag_filter;
    u8 bias_clamp, edge_lod, max_aniso;
    s8 lod_bias_q;    /* LOD bias in 1/32 units */
    u8 min_lod_q, max_lod_q; /* in 1/16 units */
    u32 tlut_name;
    u32 copy_handle;  /* texture is the result of a GXCopyTex (render target) */
} GxTexObj;

typedef struct {
    void* data;
    u16 n_entries;
    u8 format;
    u8 pad;
} GxTlutObj;

typedef struct {
    f32 pos[3];
    f32 dir[3];
    f32 a[3]; /* angle attenuation a0, a1, a2 */
    f32 k[3]; /* distance attenuation k0, k1, k2 */
    GXColor color;
} GxLightObj;

/* Vertex format of one attribute (GXSetVtxAttrFmt). */
typedef struct {
    u8 cnt;   /* GXCompCnt */
    u8 type;  /* GXCompType */
    u8 frac;
} GxAttrFmt;

/* One assembled vertex in GX's terms, before transform. */
typedef struct {
    f32 pos[3];
    f32 nrm[3];
    u8 has_nrm;
    u8 pnmtx;          /* position/normal matrix row index (0..9) */
    u8 texmtx[8];      /* per-coordinate texture matrix index */
    GXColor clr[2];
    f32 tex[8][2];
} GxVertex;

typedef struct {
    /* vertex description */
    u8 vtx_desc[GX_VA_MAX_ATTR];          /* GXAttrType per GXAttr */
    GxAttrFmt vat[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
    const u8* array_base[GX_VA_MAX_ATTR];
    u8 array_stride[GX_VA_MAX_ATTR];

    /* transform */
    f32 pos_mtx[GX_POS_MTX_COUNT][3][4];
    f32 nrm_mtx[GX_POS_MTX_COUNT][3][3];
    f32 tex_mtx[GX_TEX_MTX_COUNT][3][4];
    u8 tex_mtx_is_3x4[GX_TEX_MTX_COUNT];
    f32 pttex_mtx[GX_PTTEX_MTX_COUNT][3][4];
    u32 cur_pnmtx;          /* GXSetCurrentMtx: index 0..9 */
    u8 cur_texmtx[8];       /* default texture matrix per coordinate */
    f32 proj[7];            /* GXSetProjectionv layout: type, p0..p5 */
    f32 vp[6];              /* left, top, width, height, near, far */
    f32 zscale, zoffset;

    /* texgen */
    u8 num_texgens;
    struct {
        u8 func, src, normalize;
        u32 mtx;
        u32 pt_mtx;
    } texgen[GX_MAX_TEXGEN];

    /* lighting */
    u8 num_chans;
    struct {
        u8 enable, amb_src, mat_src;
        u8 diff_fn, attn_fn;
        u32 light_mask;
    } chan[4]; /* COLOR0, COLOR1, ALPHA0, ALPHA1 */
    GXColor amb[2], mat[2];
    GxLightObj light[GX_MAX_LIGHT];

    /* TEV */
    u8 num_tev;
    struct {
        u8 coord, map, color;          /* order */
        u8 c_in[4], a_in[4];
        u8 c_op, c_bias, c_scale, c_clamp, c_out;
        u8 a_op, a_bias, a_scale, a_clamp, a_out;
        u8 ras_swap, tex_swap;
        u8 kc_sel, ka_sel;
        u8 ind_stage, ind_fmt, ind_bias, ind_mtx, ind_wrap_s, ind_wrap_t, ind_add_prev, ind_utc_lod, ind_alpha;
        u8 ind_enabled;
    } tev[GX_MAX_TEV];
    GXColorS10 tev_reg[4];   /* PREV, REG0, REG1, REG2 */
    GXColor tev_kcolor[4];
    u8 swap_table[4][4];

    /* indirect */
    u8 num_ind;
    struct {
        u8 coord, map, scale_s, scale_t;
    } ind[4];
    f32 ind_mtx[3][2][3];
    s8 ind_mtx_scale[3];

    /* textures */
    GxTexObj tex_map[GX_MAX_TEXMAP];
    u8 tex_map_valid[GX_MAX_TEXMAP];
    GxTlutObj tlut[20];

    /* pixel engine */
    u8 alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    u8 blend_type, blend_src, blend_dst, blend_logic;
    u8 z_enable, z_func, z_update, z_before_tex;
    u8 color_update, alpha_update;
    u8 dst_alpha_enable, dst_alpha;
    u8 cull;
    u8 pixel_fmt, z_fmt;
    u8 fog_type;
    f32 fog_start, fog_end, fog_near, fog_far;
    GXColor fog_color;
    u8 ztex_op, ztex_fmt;
    u32 ztex_bias;
    u32 scissor[4];        /* left, top, width, height */
    s32 scissor_off[2];
    u8 line_width, point_size;

    /* copies */
    u16 copy_src[4];       /* left, top, w, h */
    u16 copy_tex_dst[2];
    u8 copy_tex_fmt, copy_tex_mip;
    u16 copy_disp_dst[2];
    GXColor copy_clear_color;
    u32 copy_clear_z;
    f32 disp_yscale;

    /* sync */
    u16 draw_sync_token;
    u16 efb_w, efb_h;

    /* bounding box (GXReadBoundingBox) */
    u16 bbox[4]; /* left, top, right, bottom */

    /* state version: bumped by every setter, so the emitter knows when to
     * rebuild the PICA state */
    u32 version;
} GxState;

extern GxState gx;

/* ---- gx_pipe.c ---- */
/* Write-gather pipe: 8-byte slots (GXVert.h on ports). */
extern volatile PPCWGPipe* port_gx_wg_ptr;
extern volatile PPCWGPipe* port_gx_wg_end;
void gx_pipe_reset(void);
/* Display list recording (GXBeginDisplayList .. GXEndDisplayList). */
int gx_dl_recording(void);
void gx_dl_put(const void* bytes, u32 n);
/* Port opcodes for state calls inside a display list (0xC0..0xFF). */
enum {
    GXDL_OP_PORT = 0xC0,
};

/* ---- gx_draw.c ---- */
/* Transforms, lights and texture-maps n assembled vertices of primitive
 * `prim` and records them as triangles. */
void gx_draw_vertices(u8 prim, const GxVertex* v, u32 n);
void gx_draw_frame_begin(void);
void gx_draw_frame_end(void);
extern u32 gx_stat_draws, gx_stat_vertices;

/* ---- gx_tex.c ---- */
/* Converted texture for texture map slot `map` (handle 0 if none). */
u32 gx_tex_handle_for(int map, u16* out_w, u16* out_h);
void gx_tex_invalidate_all(void);
void gx_tex_frame_end(void);
u32 gx_tex_uploads_this_frame(void);
u32 gx_tex_copy_target(const void* addr, u16 w, u16 h, u8 fmt);
void gx_tex_init(void);
void gx_state_init(u16 efb_w, u16 efb_h);
extern int gx_bbox_active;
/* Decodes a GameCube-format texture to RGBA8 (row-major). */
void gx_tex_decode_rgba8(const u8* src, u32 w, u32 h, u32 fmt, const u16* tlut, u32 tlut_fmt, u8* out);

/* ---- gx_tev.c ---- */
/* Fills the combiner part of `st` from the TEV state. */
void gx_tev_compile(PlatGpuState* st);

/* ---- dl encoding helpers (gx_state.c) ---- */
void gx_dl_replay_port_op(const u8** p, const u8* end);

/* gx_pipe.c: a GXBegin not yet closed by GXEnd (see gx_end_open_primitive). */
extern int gx_prim_open;
void gx_end_open_primitive(const void* caller);
#define GX_CLOSE_PRIMITIVE()                                     \
    do {                                                         \
        if (gx_prim_open) {                                      \
            gx_end_open_primitive(__builtin_return_address(0)); \
        }                                                        \
    } while (0)

extern void* gx_begin_caller;

#endif
