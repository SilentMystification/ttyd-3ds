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
extern u32 gx_vtx_version; /* gx_state.c: the vertex state same_state compares changed */
/* Bumped by every write to a state's vertex description or attribute formats: [0] for `gx`, [1] for the game
 * thread's shadow `gx_game`. gx_rec.c keeps the hash of that state per format while its counter stands still.
 * 64 bits: never wraps. Each counter is written and read only by the thread that owns its state at the time. */
extern u64 gx_vdesc_ver[2];

/* Switch gxdirty (default 0; PORT_3DS_GX_PERF.md, "Core-2 round 2", stage 2):
 *   1: a setter compares its arguments with the state it holds and returns at once when they are equal (no
 *      version change); a setter that changes something sets the dirty bit of its class. build_state rebuilds
 *      only the dirty classes into the kept PICA state and gives the back end the regions they fill
 *      (plat_gpu_set_state_dirty); the uniform block rebuilds its clip rows only after a matrix or projection
 *      change and its texture rows only after a texgen or texture matrix change;
 *   2: as 1, and every rebuild the switch-off path would make is also made in full and compared (the kept PICA
 *      state, the transform's flags, the uniform block): `gxdirty verify` lines. */
extern int gx_opt_dirty;
extern u32 gx_dirty;
enum {
    GXD_TEV = 1u << 0,    /* TEV stages, order, registers, konst colours, swap tables, indirect */
    GXD_TEX = 1u << 1,    /* texture maps, TLUTs (and texture copies, conversions dropped) */
    GXD_PIX = 1u << 2,    /* alpha test, blend, depth, colour and alpha update, pixel format, fog, cull, ... */
    GXD_SCI = 1u << 3,    /* scissor, scissor offset, viewport */
    GXD_CHAN = 1u << 4,   /* colour channels, material and ambient colours, lights, texgens */
    GXD_STATE = 0x1Fu,    /* the classes build_state reads */
    GXD_CLIP = 1u << 5,   /* position or normal matrix, projection: the uniform block's clip rows (and light) */
    GXD_TEXROW = 1u << 6, /* texture matrix: the uniform block's texture rows */
    GXD_ALL = 0x7Fu
};
void gx_dirty_log(u32 frames); /* the gxthreadstats gxdirty line */

/* ---- GX calls by value ----
 * One id per state call. Display lists the game records hold them as port
 * opcodes (gx_state.c, LEB128 words); the translator thread's record
 * (gx_rec.c) holds them as plain words. gx_apply_call runs one through the
 * public GX function, on the thread that translates. Ids up to
 * REC_SetTevIndirect are the ones display lists can hold; the rest are only
 * recorded for the translator. */
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
    /* translator record only */
    REC_SetZScaleOffset,
    REC_SetScissorBoxOffset,
    REC_SetIndTexOrder,
    REC_SetIndTexCoordScale,
    REC_SetIndTexMtx,
    REC_SetDstAlpha,
    REC_SetPixelFmt,
    REC_SetZTexture,
    REC_SetCopyClear,
    REC_SetDispCopySrc,
    REC_SetDispCopyDst,
    REC_SetDispCopyYScale,
    REC_SetTexCopySrc,
    REC_SetTexCopyDst,
    REC_CopyTex,
    REC_CopyDisp,
    REC_DrawDone,
    REC_SetDrawSync,
    REC_ClearBoundingBox,
    REC_COUNT
};
/* Runs call `id` with its words through the public GX function (the caller translates). */
void gx_apply_call(u32 id, const u32* w);
/* The game thread's shadow of what it reads back (gx_rec.c's gx_game). */
void gx_shadow_call(u32 id, const u32* w);

/* ---- gx_rec.c: recording GX calls for a translator (switch gxthread) ----
 *   0 (default): every GX call is translated where it is made (the game thread);
 *   1: the game thread records the calls and translates the record itself at
 *      the fences (the frame's draw sync, GXDrawDone, read-backs): proves the
 *      record without threads;
 *   2: a translator thread on core 2 translates the record while the game
 *      thread goes on; the same fences wait for it;
 *   3: verify: as 1, plus checks (getters against the translated state, list
 *      bytes and memory reports between a call's recording and its translation).
 * `gx` is the translator's state. The game thread reads back from `gx_game`,
 * a shadow kept from the calls it records. */
extern int gx_rec_mode;
extern __thread int gx_tls_translator; /* this thread translates (the translator thread, or a fence's inline replay) */
/* gxthread=2: the game thread is translating its calls itself (the start of a frame, gx_rec.c's split) */
extern int gx_rec_direct;
static inline int gx_rec_on(void) { return gx_rec_mode != 0 && !gx_tls_translator && !gx_rec_direct; }
void gx_rec_direct_list(void); /* a list translated directly: counts towards the split */
void gx_rec_switch(void);
extern GxState gx_game;
/* The state a getter reads: the shadow while recording, else the translated state. */
static inline GxState* gx_g(void) { return gx_rec_on() ? &gx_game : &gx; }
void gx_rec_init(void);
void gx_rec_call(u32 id, const u32* w, u32 n);
extern int gx_dl_has_ops;
void gx_rec_state_list(const void* list, u32 nbytes); /* a list the game built holds state calls */
/* Switch gxstatecache (default 1; PORT_3DS_GX_PERF.md, "Core-2 round 2", stage 2):
 *   1: a state call whose words equal the last ones applied for the same piece of state returns before it
 *      touches anything (no version change, so no rebuild), and compiled TEV setups are kept and reused;
 *   2: as 1, and every reused TEV result is compiled again and compared.
 * gx_dd_apply_same: 1 when `gx` already holds call `id` with these words (the caller returns); else the words are
 * noted and the caller applies them. Every setter that writes a piece of state the table covers asks first. */
extern int gx_opt_statecache;
extern int gx_opt_statededup; /* switch gxstatededup (default 0): the early return of unchanged state calls */
int gx_dd_apply_same(u32 id, const u32* w, u32 n);
void gx_dd_apply_forget(void); /* `gx` was changed behind the setters (gx_state_init) */
void gx_tev_memo_log(u32 frames);

/* Fences: everything recorded so far is translated when they return. */
enum { GXF_SYNC, GXF_DRAWDONE, GXF_READBACK, GXF_RELEASE, GXF_FULL, GXF_VERIFY, GXF_COUNT };
void gx_rec_fence(int why);
/* A fence, then the caller translates directly until gx_rec_excl_end (GXInit, read-backs). */
void gx_rec_excl_begin(int why);
void gx_rec_excl_end(void);
/* gx_pipe.c side */
void gx_rec_list(const void* list, u32 nbytes, const void* caller);
void gx_rec_prim(u8 primfmt, u16 nverts, const void* caller, const PPCWGPipe* slots, u32 nslots);
void gx_rec_raw(const PPCWGPipe* slots, u32 nslots);
int gx_rec_prim_direct(u8 primfmt, u16 nverts, const void* caller, const PPCWGPipe* slots); /* gximmdirect */
/* Switch gxphase=N (measuring, default 0; logs with gxthreadstats; meant for gxthread=2 gxpipeline=1): time and
 * calls per phase of the translator (inside consume). One outer call in N is timed (a list call of a group, a state
 * call, a primitive, any other command) and, inside it, every nested phase (batch open and close, the per-list slow
 * path, batch forming, the state build); time is exclusive (a nested phase stops its outer's clock). The cost of a
 * segment (an empty nested phase at the start of each timed call) is taken off. Off: one load and branch per hook.
 * The game thread's record path has no timers: a hook in gx_rec_call cost 0.3 ms and the timed path is not the
 * setters' inlined one (PORT_3DS_GX_PERF.md, "Where the record path's time goes"). */
enum {
    GPH_CAL, GPH_T_LIST, GPH_T_SET, GPH_T_PRIM, GPH_T_PRIMD, GPH_T_RAW, GPH_T_OTHER, GPH_T_BOPEN, GPH_T_BCLOSE,
    GPH_T_SLOW, GPH_T_FORM, GPH_T_STATE, GPH_COUNT
};
extern int gx_ph_on;
int gx_ph_enter_slow(int ph, int outer);
void gx_ph_pop(void);
/* An outer phase: counted on every call, timed on one in N (or always inside a timed outer one). */
static inline int gx_ph_enter(int ph) { return __builtin_expect(gx_ph_on, 0) ? gx_ph_enter_slow(ph, 1) : 0; }
/* A nested phase: counted on every call, timed only inside a timed outer one. */
static inline int gx_ph_nested(int ph) { return __builtin_expect(gx_ph_on, 0) ? gx_ph_enter_slow(ph, 0) : 0; }
static inline void gx_ph_leave(int pushed) {
    if (__builtin_expect(pushed, 0)) {
        gx_ph_pop();
    }
}
void gx_rec_frame(void); /* game thread, GXCopyDisp: per-frame counters */
void gx_rec_log(u32 frames);
void gx_rec_frame_start(void); /* game thread, GXInvalidateVtxCache: the retrace wait ended */
void gx_rec_sync_mark(void);   /* game thread, gxthread=0: GXReadDrawSync */
/* verify mode: the memory a translated call reads (what: GXV_READ_*) */
enum { GXV_READ_LIST, GXV_READ_ARRAY, GXV_READ_TEX, GXV_READ_TLUT, GXV_READ_COUNT };
void gx_rec_check_read(const void* p, u32 n, int what);
/* Getters: in verify mode the shadow they read is checked against the translated state. */
enum { GXG_VTXDESC, GXG_VAT, GXG_PROJ, GXG_VIEWPORT, GXG_SCISSOR, GXG_CULL, GXG_COPYSRC, GXG_COUNT };
void gx_rec_getter_check(int which);
static inline void gx_rec_getter(int which) {
    if (gx_rec_mode == 3 && !gx_tls_translator) {
        gx_rec_getter_check(which);
    }
}
/* gx_misc.c: GXCopyDisp's body, on the translating thread */
void gx_copy_disp(void* dest, GXBool clear, const void* caller);

/* ---- gx_pipe.c ---- */
/* Write-gather pipe: 8-byte slots (GXVert.h on ports). */
extern volatile PPCWGPipe* port_gx_wg_ptr;
void gx_pipe_alloc(void); /* the pipe tables (heap, not .bss); once, from gx_opts_init */
extern volatile PPCWGPipe* port_gx_wg_end;
void gx_pipe_reset(void);
/* Display list recording (GXBeginDisplayList .. GXEndDisplayList). */
int gx_dl_recording(void);
void gx_dl_put(const void* bytes, u32 n);
/* Translator side of the recorded pipe calls (gx_rec.c). */
void gx_call_list(const void* list, u32 nbytes, const void* caller);
void gx_draw_prim(u8 primfmt, u16 nverts, const void* caller, const u32* words, u32 nwords);
void gx_run_raw(const u32* words, u32 nwords);
/* Port opcodes for state calls inside a display list (0xC0..0xFF). */
enum {
    GXDL_OP_PORT = 0xC0,
    /* a primitive kept as its raw writes: prim|fmt u8, count u16, writes u32, then each write as u32 */
    GXDL_OP_RAW_PRIM = 0xC1,
};

/* ---- gx_draw.c ---- */
/* Transforms, lights and texture-maps n assembled vertices of primitive
 * `prim` and records them as triangles. */
void gx_draw_vertices(u8 prim, const GxVertex* v, u32 n);
void gx_draw_frame_begin(void);
void gx_draw_frame_end(void);
/* Something build_state reads changed outside GxState (a texture copy): the next draw rebuilds. */
void gx_draw_state_lost(void);
extern u32 gx_stat_draws, gx_stat_vertices, gx_stat_copies;
/* A/B switches of the state work (read once by gx_opts_init; default 1 unless noted):
 *   gxstatever (default 0): skip rebuilds whose state is unchanged;
 *   gxfogcache: the fog LUT is recomputed only when the fog parameters change;
 *   gxunlit:    unlit colour channels skip channel_color. */
extern int gx_opt_statever, gx_opt_fogcache, gx_opt_unlit;
void gx_opts_init(void);

/* ---- gx_tex.c ---- */
/* Converted texture for texture map slot `map` (handle 0 if none). */
u32 gx_tex_handle_for(int map, u16* out_w, u16* out_h);
/* Fills the filter fields of a texture unit for the texture gx_tex_handle_for just returned for `map`. */
void gx_tex_unit_sampler(PlatGpuTexUnit* tu, const GxTexObj* t, int map);
void gx_tex_invalidate_all(void);
void gx_tex_frame_end(void);
u32 gx_tex_uploads_this_frame(void);
u32 gx_tex_copy_target(const void* addr, u16 w, u16 h, u8 fmt);
void gx_tex_init(void);
void gx_state_init(u16 efb_w, u16 efb_h);
extern int gx_bbox_active;
extern int gx_opt_vshbbox; /* switch gxvshbbox */
extern int gx_opt_vshlit;  /* switch gxvshlit */
extern int gx_opt_vshtg;   /* switch gxvshtg */
void gx_bbox_clear(void);
void gx_bbox_read(u16* out);
void gx_bbox_counts(u32* reads, u32* mism);
extern void** gx_bbx_hint;     /* set before gx_draw_vsh: the cache entry's box record (switch gxbbox) */
void gx_bbx_free(void* p);
void gx_bbox_stats_log(u32 frames); /* the gxthreadstats bbox line */
/* Decodes a GameCube-format texture to RGBA8 (row-major). */
void gx_tex_decode_rgba8(const u8* src, u32 w, u32 h, u32 fmt, const u16* tlut, u32 tlut_fmt, u8* out);

/* ---- gx_tev.c ---- */
/* Fills the combiner part of `st` from the TEV state. */
void gx_tev_compile(PlatGpuState* st);
/* One more stage after the compiled ones: PREV lerped toward the fog colour (r | g << 8 | b << 16) by the red of
 * the texture on `unit` (the fog ramp). Returns 0, and changes nothing, when six stages are in use already. */
int gx_tev_add_fog(PlatGpuState* st, int unit, u32 rgb);
/* The same through a table of compiled setups (switch gxstatecache): the key is every byte the compile reads. */
void gx_tev_compile_memo(PlatGpuState* st);

/* ---- dl encoding helpers (gx_state.c) ---- */
void gx_dl_replay_port_op(const u8** p, const u8* end);
/* switch gxsdl (gx_state.c): applies the pre-decoded state calls at the start of a list, returns the bytes covered */
extern int gx_opt_sdl;
/* `hint` (or NULL): a u16 that keeps the entry's slot, so the next call of the same list skips the hash and the probe */
u32 gx_sdl_run(const u8* list, u32 nbytes, u16* hint);
/* Decodes the leading NOPs and port opcodes of a list into calls (id | count << 8, then the words; at most cap
 * words, *nw gets the number written); returns the bytes they cover. */
u32 gx_sdl_scan(const u8* list, u32 nbytes, u32* out, u32 cap, u32* nw);
/* gx_dlcache.c: set by the call that found an uncacheable (state-only) list's entry; the slot hint of gx_sdl_run for it.
 * NULL when the last call found no such entry. */
extern u16* gx_sdl_hint;

/* gx_pipe.c: a GXBegin not yet closed by GXEnd (see gx_end_open_primitive). The
 * primitive is the game thread's: a translating thread never closes it. */
extern int gx_prim_open;
void gx_end_open_primitive(const void* caller);
static inline int gx_prim_is_open(void) { return gx_prim_open && !gx_tls_translator; }
#define GX_CLOSE_PRIMITIVE()                                     \
    do {                                                         \
        if (gx_prim_is_open()) {                                 \
            gx_end_open_primitive(__builtin_return_address(0)); \
        }                                                        \
    } while (0)

extern void* gx_begin_caller;
int gx_draw_dumping(void); /* switch gxdump: this frame is logged */

/* ---- gx_perf.c: per-frame counters (switch gxstats=N logs them every N frames) ---- */
typedef struct {
    /* u32 counters (summed word by word up to t_frame) */
    u32 dl_calls, dl_bytes, dl_verts;
    u32 dl_new, dl_repeat, dl_again, dl_changed, dl_repeat_bytes;
    u32 dl_vtx_only, dl_state_only, dl_mixed, dl_raw;
    u32 imm_prims, imm_verts;
    u32 xf_draws, xf_verts, tris_out, tris_culled, lines;
    u32 builds;
    u32 v_pnmtxidx, v_texmtxidx, v_nrm, v_behind;
    u32 v_prim[7]; /* quads, tris, strips, fans, lines, line strips, points */
    u32 v_unlit, v_lit, v_light_evals, v_spot, v_spec, v_distattn, v_second_chan, v_vtx_src;
    u32 v_tg_2x4, v_tg_3x4, v_tg_other, v_tg_pos, v_tg_nrm, v_tg_tex, v_tg_color, v_tg_post, v_tg_normalize;
    u32 max_pnmtx_in_draw;
    u32 builds_same; /* version changes with the state build_state reads unchanged (no rebuild) */
    u32 bbox_draws, bbox_verts; /* shader-path draws while the bounding box is armed (positions on the CPU) */
    /* u64 times in system ticks */
    u64 t_frame, t_dl, t_dl_decode, t_imm, t_imm_decode, t_state, t_xform, t_emit, t_cache;
    /* game thread: waits in GXDrawDone and at the frame's submit, and the gap from the end of GXCopyDisp to the
     * next frame's first GXInvalidateVtxCache (DEMOBeforeRender), which is the draw-sync check and the retrace wait */
    u64 t_sync, t_present, t_idle;
    u64 t_bbox; /* the CPU bounding box of shader-path draws */
} GxPerf;
extern GxPerf gx_perf;
extern int gx_perf_on; /* -1 until the first frame reads the switch */
void gx_perf_dl(const void* list, u32 nbytes);
void gx_perf_frame(void);
u32 gx_perf_frame_no(void);
/* Fast content hash (word at a time). */
u32 gx_hash32(const void* data, u32 n, u32 seed);
#define GX_PERF_T() (gx_perf_on > 0 ? plat_ticks() : 0)
extern u64 gx_perf_drawdone_end; /* tick GXCopyDisp returned (0: counted) */

/* Equality of n bytes a word at a time (ARMv6 loads unaligned words), for the
 * small compares on hot paths: newlib's memcmp goes byte by byte unless both
 * pointers are word-aligned. */
static inline int gx_mem_equal(const void* a, const void* b, u32 n) {
    const u8* p = (const u8*)a;
    const u8* q = (const u8*)b;
    for (; n >= 4; n -= 4, p += 4, q += 4) {
        u32 x, y;
        __builtin_memcpy(&x, p, 4);
        __builtin_memcpy(&y, q, 4);
        if (x != y) {
            return 0;
        }
    }
    while (n-- != 0) {
        if (*p++ != *q++) {
            return 0;
        }
    }
    return 1;
}

/* ---- gx_dlcache.c: decoded display-list cache (switch gxcache) ----
 * State is owned by the thread that translates GX (today the game thread);
 * the decode inputs come in through `st`. */
int gx_dlcache_enabled(void);
/* 1: the list was drawn from the cache. 0: run it (with capture, if set up). */
int gx_dlcache_call(const GxState* st, const void* list, u32 nbytes);
void gx_dlc_cap_finish(const GxState* st, u32 kind);
GxVertex* gx_dlc_cap_reserve(u8 fmt, u32 n);
void gx_dlc_cap_draw(u8 prim, const GxVertex* v, u32 n);
extern int gx_dlc_capturing;
/* the array indices the list's vertices read, in decode order (a list
 * that reads more than the capture holds is not cached) */
#define GX_DLC_IDX_CAP (8192 * 12)
extern u16* gx_dlc_idx; /* GX_DLC_IDX_CAP entries */
extern u32 gx_dlc_nidx;
static inline void gx_dlc_track(int attr, u32 idx) {
    (void)attr;
    if (gx_dlc_nidx < GX_DLC_IDX_CAP) {
        gx_dlc_idx[gx_dlc_nidx] = (u16)idx;
    }
    gx_dlc_nidx++;
}
void gx_dlcache_frame(void);
void gx_dlcache_log(u32 frames);
#include "plat/memstable.h"
u32 gx_dlcache_spans(const void* list, u32 nbytes, PlatMemSpan* sp, u32 max);

/* One draw of a cached list (a captured primitive): vertices [first, first + count). */
typedef struct {
    u8 prim;
    u8 pad;
    u16 count_hi; /* unused: counts fit 16 bits (chunks of at most 4096) */
    u32 first;
    u32 count;
} GxDlcDraw;

/* ---- gx_vsh.c: the vertex-shader path (switch gxvsh, default 1) ----
 * A cached list's vertices as a PICA stream (linear memory, built once), and
 * the pass-group table that decides, per group of draws sharing a matrix,
 * projection and viewport, whether the GPU or the CPU transforms them. */
extern u32 gx_xf_version; /* bumped by matrix, projection and current-matrix changes (gx_state.c) */
extern int gx_vsh_on;
extern int gx_vsh_imm_on; /* switch gxvshimm */
extern int gx_vsh_noted; /* the draws being made were routed already (a cached list replayed on the CPU) */

/* Why a draw cannot take the shader path (a bit each; the census counts them). */
enum {
    GXV_R_IMM = 1u << 0,       /* immediate mode (GXBegin) */
    GXV_R_MISS = 1u << 1,      /* a list decoded, not drawn from the cache */
    GXV_R_PNMTXIDX = 1u << 2,  /* per-vertex matrix index */
    GXV_R_TEXMTXIDX = 1u << 3, /* per-vertex texture matrix index */
    GXV_R_PRIM = 1u << 4,      /* lines or points */
    GXV_R_SLOTS = 1u << 5,     /* more than 3 texture coordinate attributes */
    GXV_R_POOL = 1u << 6,      /* no stream memory */
    GXV_R_BBOX = 1u << 7,      /* bounding box armed */
    GXV_R_LIT = 1u << 8,       /* GX lighting on the colour used */
    GXV_R_TWOCHAN = 1u << 9,   /* a texgen reads a colour (or SRTG) */
    GXV_R_CLR1 = 1u << 10,     /* colour channel 1 from the vertex */
    GXV_R_TGFUNC = 1u << 11,   /* texgen not MTX2x4 */
    GXV_R_TGSRC = 1u << 12,    /* texgen source not a texture coordinate */
    GXV_R_TGPOST = 1u << 13,   /* post-transform texture matrix */
    GXV_R_GROUPFULL = 1u << 14,/* the pass-group table is full */
    GXV_R_COUNT = 15
};

typedef struct {
    u32 seg;           /* pool segment */
    u32 first, nverts; /* vertices in the segment */
    u32 layout;        /* PLAT_VSH_* layout word */
    u8 posk;           /* position = s16 x 2^-posk (0 for floats) */
    u8 attr_slot[8];   /* stream slot of GX_VA_TEX0 + n, 0xFF when absent */
    u8 slot_k[3];
    u32 nidx;
    u32 bytes;         /* memory it takes (vertex bytes and this struct), for the cache budget */
    u16 idx[];         /* triangle list, indices from the segment's base */
} GxVshStream;

void gx_vsh_init(void);
/* The stream of a cache entry, or NULL with *reason set. */
GxVshStream* gx_vsh_stream_build(const GxVertex* v, u32 nv, const GxDlcDraw* d, u32 nd, const u8* vtx_desc,
                                 const GxAttrFmt (*vat)[GX_VA_MAX_ATTR], u8 fmt_mask, u32* reason);
void gx_vsh_stream_free(GxVshStream* s); /* the vertices are reused only after the frames in flight */
const void* gx_vsh_stream_base(const GxVshStream* s);
/* Routes one draw under position matrix `pnmtx`: `reasons` 0 for a shader-eligible
 * cached list (`list`, `nbytes`), else why not. Returns 1 for the shader path. */
int gx_vsh_route(u32 pnmtx, u32 reasons, const void* list, u32 nbytes, u32 nverts, int imm);
extern u32* gx_vsh_rec_hint; /* set before gx_draw_vsh: where the list's pass-group record index is remembered */
/* Immediate mode: a draw's vertices in this frame's ring (gx_vsh.c). */
typedef struct {
    const void* base; /* stream base (a ring chunk) */
    u32 layout;
    u8 attr_slot[8];  /* stream slot of GX_VA_TEX0 + n, 0xFF when absent */
    const u16* idx;   /* triangle list from the chunk base */
    u32 nidx;
} GxVshImm;
u32 gx_vsh_imm_reasons(u8 prim, u32 n);
int gx_vsh_imm_room(u32 n);
const GxVshImm* gx_vsh_imm_build(u8 prim, const GxVertex* v, u32 n);
/* The ring's vertex layout: byte offsets of position, colour, slots 0..2, normal; returns the stride. */
u32 gx_vsh_imm_layout(int32_t off[6]);
void gx_vsh_imm_write(const GxVertex* v, u32 n, const u8* vtx_desc, u8* out, u8 attr_slot[8]);
u8* gx_vsh_imm_dst(void); /* where the next draw's vertices go (gx_vsh_imm_room checked) */
const GxVshImm* gx_vsh_imm_done(u8 prim, u32 n, const u8 attr_slot[8]);
/* gximmdirect while recording: the game thread's chunks (gx_vsh.c). */
u8* gx_vsh_gimm_alloc(u32 n, const void** base, u32* first, u32* chunk); /* game thread; NULL: no room */
void gx_vsh_gimm_translated(u32 chunk);                                   /* translator: one draw of it done */
const GxVshImm* gx_vsh_imm_at(u8 prim, const void* base, u32 first, u32 n, const u8 attr_slot[8]);

/* ---- Switch gximmdirect (PORT_3DS_GX_PERF.md, "Core-2 round 2", stage 4; default 0) ----
 *   1: GXEnd writes an immediate primitive's vertices straight from the pipe writes (indexed attributes read from
 *      their arrays then) into the ring in its PICA layout, with no GxVertex, when the draw takes the shader path.
 *      While recording (gxthread=1..3) the game thread does it at GXEnd into its own chunks: no array snapshot.
 *   2: as 1, and every draw is also decoded the old way and its ring bytes compared (gximmdirect warnings).
 * gx_draw_imm_route (gx_draw.c) routes the draw before it is decoded. */
extern int gx_opt_immdirect;
enum { GXI_OLD, GXI_SHADER, GXI_CPU, GXI_NONE };
int gx_draw_imm_route(u8 prim, u32 n, const void* caller, int ring_given);
void gx_draw_imm_shader(const GxVshImm* im, u32 n);
void gx_draw_vertices_cpu(u8 prim, const GxVertex* v, u32 n); /* routed already: the CPU path */
/* gx_pipe.c: the converter (vertex state g), the eligibility of a recorded primitive, the translator's side */
int gx_imm_convert(const GxState* g, u8 fmt, const PPCWGPipe* sl, u32 n, u8* out, u8 attr_slot[8]);
int gx_imm_rec_ok(const GxState* g, u8 prim, u32 n);
void gx_imm_verify(const GxState* g, u8 fmt, const PPCWGPipe* sl, u32 n, const u8* got, const u8 attr_slot[8]);
void gx_imm_rec_count(int ok);
void gx_draw_prim_ring(u8 primfmt, u16 n, const void* caller, const u8* base, u32 first, const u8 attr_slot[8],
                       int has_nrm, int state_same);
void gx_immdirect_log(u32 frames);
void gx_vsh_count_shader(u32 nverts, u32 nidx);
void gx_vsh_frame(void); /* frame boundary: hysteresis, decal links, counters */
void gx_vsh_log(u32 frames);
/* gx_draw.c: draws a cached entry through the shader when its group takes that path; 0: draw it on the CPU. */
int gx_draw_vsh(const GxVshStream* s, u32 entry_reason, const void* list, u32 nbytes, u32 nverts, const GxVertex* v,
                const GxDlcDraw* d, u32 nd);
int gx_pipe_in_list(void);
void gx_pipe_cur_list(const void** list, u32* nbytes); /* the display list being run */

/* ---- gxbatch (gx_dlcache.c, default 1): prebuilt batches of cached lists (PORT_3DS_GX_PERF.md, "Core-2 round 2") ----
 * A run of consecutive cache hits under one GX state is built once into a batch: its members, the vertex state
 * they were validated under, the GPU draws they merge into and their indices in linear memory. A list call that
 * continues the open batch is a compare; the batch's draws are recorded when it ends, so every other thing that
 * records into the back end calls gx_batch_flush first. */
extern int gx_batch_pending; /* a batch is open or a run is being collected */
void gx_batch_flush_slow(void);
static inline void gx_batch_flush(void) {
    if (gx_batch_pending) {
        gx_batch_flush_slow();
    }
}
int gx_batch_recent(u32 bat);  /* batch `bat` (index + 1) was drawn this frame or in the last two */
/* Member k of batch `bat`: its list and where its pass-group record index is kept; 0 past the end. */
int gx_batch_member(u32 bat, u32 k, const void** list, u32* nbytes, u32** hint);
/* gx_vsh.c */
extern u32 gx_vsh_dec_epoch;
int gx_vsh_rec_decided(u32* hint, const void* list, u32 nbytes, u32 bat);
void* gx_vsh_batch_route(u32 pnmtx, u32 bat, const void* list0, u32 nbytes0, u32* hint0);
void gx_vsh_batch_count(void* group, u32 n, u32 verts, u32 idx);
/* gx_draw.c: what the last gx_draw_vsh recorded (ok 0: it drew nothing through the shader path) */
typedef struct {
    u8 ok;
    u8 unit_attr[3];
    const void* stream;
    u32 layout, scales, cull;
} GxVshLast;
extern GxVshLast gx_vsh_last;
/* The state part of gx_draw_vsh for a whole batch: PICA state, uniforms, the group's route. 1: the batch may
 * draw (its uniform block is recorded, *group and *cull are set); 0: draw the members one by one. */
int gx_draw_batch_begin(const u8 unit_attr[3], u32 bat, const void* list0, u32 nbytes0, u32* hint0, void** group,
                        u32* cull);
void gx_draw_batch_end(void* group, u32 n, u32 verts, u32 idx);

#endif
