/* GX draw emitter: GX vertices -> transformed, lit, textured PICA triangles.
 *
 * The GX transform and lighting unit runs here on the CPU, following the
 * hardware's definitions (as Dolphin implements them): position and normal
 * from the matrix memory (current matrix or per-vertex index), projection,
 * the two color channels with GX lighting, texture coordinate generation
 * with 2x4/3x4 matrices and post-transform matrices. Quads, strips, fans,
 * lines and points become triangle lists. Culling is done here in window
 * coordinates (GX's front face is clockwise on screen), and the bounding box
 * (GXReadBoundingBox) is kept from the drawn vertices.
 *
 * The PICA state (combiners, textures, blending, depth, fog, scissor) is
 * rebuilt when the GX state changed since the last draw.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gx_bigmem.h"
#include "gx_internal.h"
#include "plat/plat.h"

int gx_unit_of_map[GX_MAX_TEXMAP];
static int s_unit_coord[3];  /* GX texcoord each PICA unit samples with */
static int s_unit_map[3];
static int s_units;
static int s_color_chan;     /* 0: COLOR0A0, 1: COLOR1A1 feeds the raster color */
static u32 s_built_version = 0xFFFFFFFFu;
static PlatGpuState s_state;

/* switch gxstatever: the GX state build_state reads (and the transform's
 * precomputed flags), as it was at the last build. Every field from the
 * texgens to the pixel engine (texgen, channels, lights, TEV, indirect,
 * textures, TLUTs, blending, depth, fog, scissor) is one contiguous block
 * of GxState, plus the viewport. A version change that leaves all of it as
 * it was (a setter called again with the same values, a matrix load) needs
 * no rebuild. Cleared every frame (textures are checked again then) and by
 * a texture copy (a texture's handle can change). */
#define SNAP_FROM (offsetof(GxState, num_texgens) & ~(size_t)3)
#define SNAP_TO ((offsetof(GxState, copy_src) + 3) & ~(size_t)3)
#define SNAP_WORDS ((SNAP_TO - SNAP_FROM) / 4)
#define SNAP_LIGHT_START ((offsetof(GxState, light) - SNAP_FROM) / 4)
#define SNAP_LIGHT_END ((offsetof(GxState, num_tev) - SNAP_FROM) / 4)
_Static_assert(offsetof(GxState, light) % 4 == 0 && offsetof(GxState, num_tev) % 4 == 0, "snapshot word layout");
static u32 s_snap[SNAP_WORDS]; /* word-aligned: the compare runs a word at a time (memcmp on an unaligned
                                  buffer goes byte by byte) */
static f32 s_snap_vp[6];
static int s_snap_valid;

void gx_draw_state_lost(void) { s_snap_valid = 0; }

static int state_unchanged(void) {
    if (!s_snap_valid) {
        return 0;
    }
    typedef u32 __attribute__((may_alias)) u32a;
    const u32a* cur = (const u32a*)((const u8*)&gx + SNAP_FROM);
    /* backwards: the textures, blending and TEV (what changes between objects) sit at the end; the
     * lights are skipped (the transform reads them per vertex, build_state does not) */
    for (u32 i = SNAP_WORDS; i-- > SNAP_LIGHT_END;) {
        if (cur[i] != s_snap[i]) {
            return 0;
        }
    }
    for (u32 i = SNAP_LIGHT_START; i-- > 0;) {
        if (cur[i] != s_snap[i]) {
            return 0;
        }
    }
    return gx_mem_equal(s_snap_vp, gx.vp, sizeof(s_snap_vp));
}

static void state_snapshot(void) {
    memcpy(s_snap, (const u8*)&gx + SNAP_FROM, sizeof(s_snap));
    memcpy(s_snap_vp, gx.vp, sizeof(s_snap_vp));
    s_snap_valid = 1;
}
/* switch gxunlit, set by build_state: the draw needs both colour channels
 * (a texgen reads a colour), and per channel pair whether lighting is off for
 * colour and alpha, and which of them take the vertex colour as material */
static int s_need_both, s_need_nrm;
static u8 s_unlit[2], s_mat_vtx_rgb[2], s_mat_vtx_a[2];
u32 gx_stat_draws, gx_stat_vertices, gx_stat_copies;
int gx_bbox_active;
int gx_opt_statever = 0, gx_opt_fogcache = 1, gx_opt_unlit = 1;

static void out_alloc(void); /* s_out, below */

void gx_opts_init(void) {
    out_alloc();
    gx_pipe_alloc();
    gx_opt_statever = plat_switch_int("gxstatever", 0) != 0; /* off by default: fewer rebuilds, but no time saved (PORT_3DS_GX_PERF.md) */
    gx_opt_fogcache = plat_switch_int("gxfogcache", 1) != 0;
    gx_opt_unlit = plat_switch_int("gxunlit", 1) != 0;
    gx_vsh_imm_on = plat_switch_int("gxvshimm", 1) != 0;
    gx_opt_vshbbox = plat_switch_int("gxvshbbox", 0);
    gx_opt_vshlit = plat_switch_int("gxvshlit", 1) != 0;
    gx_opt_vshtg = plat_switch_int("gxvshtg", 1) != 0;
    PLOG_I("gx: statever %d fogcache %d unlit %d", gx_opt_statever, gx_opt_fogcache, gx_opt_unlit);
    gx_vsh_init();
}

/* ---- State ---- */

static u8 pica_func(u8 gxc) {
    static const u8 map[8] = {0 /*NEVER*/, 4 /*LESS*/, 2 /*EQUAL*/, 5 /*LEQUAL*/, 6 /*GREATER*/, 3 /*NOTEQUAL*/,
                              7 /*GEQUAL*/, 1 /*ALWAYS*/};
    return map[gxc & 7];
}

static u8 blend_factor(u8 f, int is_src) {
    switch (f) {
    case GX_BL_ZERO: return 0;
    case GX_BL_ONE: return 1;
    case GX_BL_SRCCLR: return is_src ? 4 /*DST_COLOR*/ : 2 /*SRC_COLOR*/;
    case GX_BL_INVSRCCLR: return is_src ? 5 : 3;
    case GX_BL_SRCALPHA: return 6;
    case GX_BL_INVSRCALPHA: return 7;
    case GX_BL_DSTALPHA: return 8;
    case GX_BL_INVDSTALPHA: return 9;
    default: return 1;
    }
}

static u8 logic_op(u8 lo) {
    /* GX_LO_* -> GPU_LOGICOP */
    static const u8 map[16] = {0 /*CLEAR*/, 1 /*AND*/, 2 /*REVAND=AND_REVERSE*/, 3 /*COPY*/, 13 /*INVAND*/,
                               6 /*NOOP*/,  11 /*XOR*/, 9 /*OR*/, 10 /*NOR*/, 12 /*EQUIV*/, 7 /*INV*/,
                               14 /*REVOR*/, 5 /*INVCOPY*/, 15 /*INVOR*/, 8 /*NAND*/, 4 /*SET*/};
    return map[lo & 15];
}

static void fog_lut(f32* lut);

/* switch gxfogcache: the LUT of the last fog parameters, and its id
 * (PlatGpuState.fog_lut_id: a new id only when the LUT's contents change) */
static struct {
    u8 valid, type;
    f32 s, e, n, f;
    u32 id;
    f32 lut[129];
} s_fog;

static void build_fog(PlatGpuState* st) {
    u8 type = gx.fog_type;
    if (type == GX_FOG_NONE || gx.fog_end == gx.fog_start || gx.fog_far == gx.fog_near) {
        st->fog_enable = 0;
        return;
    }
    st->fog_enable = 1;
    st->fog_color = (u32)gx.fog_color.r | (u32)gx.fog_color.g << 8 | (u32)gx.fog_color.b << 16;
    if (!gx_opt_fogcache) {
        fog_lut(st->fog_lut); /* fog_lut_id stays 0: the back end hashes the LUT */
        return;
    }
    if (!s_fog.valid || s_fog.type != type || s_fog.s != gx.fog_start || s_fog.e != gx.fog_end ||
        s_fog.n != gx.fog_near || s_fog.f != gx.fog_far) {
        f32 lut[129];
        fog_lut(lut);
        if (!s_fog.valid || memcmp(lut, s_fog.lut, sizeof(lut)) != 0) {
            memcpy(s_fog.lut, lut, sizeof(lut));
            if (++s_fog.id == 0) {
                s_fog.id = 1;
            }
        }
        s_fog.valid = 1;
        s_fog.type = type;
        s_fog.s = gx.fog_start;
        s_fog.e = gx.fog_end;
        s_fog.n = gx.fog_near;
        s_fog.f = gx.fog_far;
    }
    memcpy(st->fog_lut, s_fog.lut, sizeof(s_fog.lut));
    st->fog_lut_id = s_fog.id;
}

/* The PICA fog LUT for the current GX fog parameters (type not NONE). */
static void fog_lut(f32* lut) {
    u8 type = gx.fog_type;
    int ortho = type >= GX_FOG_ORTHO_LIN;
    u8 fn = type & 7;
    f32 n = gx.fog_near, f = gx.fog_far, s = gx.fog_start, e = gx.fog_end;
    for (int i = 0; i <= 128; i++) {
        f32 z = (f32)i / 128.0f; /* window depth, 0 near .. 1 far */
        f32 ze;
        if (ortho) {
            ze = n + z * (f - n);
        } else {
            f32 denom = f - z * (f - n);
            ze = denom > 0 ? (f * n) / denom : f;
        }
        f32 x = (ze - s) / (e - s);
        if (x < 0) {
            x = 0;
        } else if (x > 1) {
            x = 1;
        }
        f32 fog;
        switch (fn) {
        case 4: fog = 1.0f - powf(2.0f, -8.0f * x); break;           /* EXP */
        case 5: fog = 1.0f - powf(2.0f, -8.0f * x * x); break;       /* EXP2 */
        case 6: fog = powf(2.0f, -8.0f * (1.0f - x)); break;         /* REVEXP */
        case 7: fog = powf(2.0f, -8.0f * (1.0f - x) * (1.0f - x)); break; /* REVEXP2 */
        default: fog = x; break;                                     /* LIN */
        }
        lut[i] = 1.0f - fog; /* PICA: 1 keeps the fragment */
    }
}

/* Texture units: the (texcoord, map) pairs the active TEV stages sample. */
static void assign_units(void) {
    for (int m = 0; m < GX_MAX_TEXMAP; m++) {
        gx_unit_of_map[m] = -1;
    }
    s_units = 0;
    int num = gx.num_tev ? gx.num_tev : 1;
    int chan_votes[2] = {0, 0};
    for (int s = 0; s < num && s < GX_MAX_TEV; s++) {
        u8 map = gx.tev[s].map;
        if (gx.tev[s].color == GX_COLOR0A0 || gx.tev[s].color == GX_COLOR0 || gx.tev[s].color == GX_ALPHA0) {
            chan_votes[0]++;
        } else if (gx.tev[s].color == GX_COLOR1A1 || gx.tev[s].color == GX_COLOR1 || gx.tev[s].color == GX_ALPHA1) {
            chan_votes[1]++;
        }
        if (map >= GX_MAX_TEXMAP || gx_unit_of_map[map] >= 0) {
            continue;
        }
        if (s_units == 3) {
            static int warned;
            if (!warned) {
                warned = 1;
                PLOG_W("GX: a TEV setup samples more than 3 textures (PICA has 3 units)");
            }
            continue;
        }
        gx_unit_of_map[map] = s_units;
        s_unit_map[s_units] = map;
        s_unit_coord[s_units] = gx.tev[s].coord < GX_MAX_TEXGEN ? gx.tev[s].coord : 0;
        s_units++;
    }
    s_color_chan = chan_votes[1] > chan_votes[0] ? 1 : 0;
    if (chan_votes[0] && chan_votes[1]) {
        static int warned;
        if (!warned) {
            warned = 1;
            PLOG_W("GX: a TEV setup uses both color channels (PICA has one vertex color)");
        }
    }
}

static void build_state(void) {
    PlatGpuState* st = &s_state;
    memset(st, 0, sizeof(*st));
    assign_units();
    for (int u = 0; u < 3; u++) {
        PlatGpuTexUnit* tu = &st->tex[u];
        if (u >= s_units) {
            continue;
        }
        u16 w, h;
        int map = s_unit_map[u];
        tu->handle = gx_tex_handle_for(map, &w, &h);
        const GxTexObj* t = &gx.tex_map[map];
        static const u8 wrap[3] = {0 /*CLAMP_TO_EDGE*/, 2 /*REPEAT*/, 3 /*MIRRORED_REPEAT*/};
        tu->wrap_s = wrap[t->wrap_s % 3];
        tu->wrap_t = wrap[t->wrap_t % 3];
        gx_tex_unit_sampler(tu, t, map); /* min/mag filter, mip filter, LOD bias (the texfilter option) */
    }
    gx_tev_compile(st);

    /* what the transform needs per vertex (gxunlit) */
    s_need_both = 0;
    for (int u = 0; u < s_units; u++) {
        u8 src = gx.texgen[s_unit_coord[u]].src;
        if (gx.texgen[s_unit_coord[u]].func == GX_TG_SRTG || src == GX_TG_COLOR0 || src == GX_TG_COLOR1) {
            s_need_both = 1;
        }
    }
    for (int ch = 0; ch < 2; ch++) {
        s_unlit[ch] = !gx.chan[ch].enable && !gx.chan[ch + 2].enable;
        s_mat_vtx_rgb[ch] = gx.chan[ch].mat_src == GX_SRC_VTX;
        s_mat_vtx_a[ch] = gx.chan[ch + 2].mat_src == GX_SRC_VTX;
    }
    s_need_nrm = !s_unlit[s_color_chan] || (s_need_both && !s_unlit[1 - s_color_chan]);

    /* alpha compare */
    u8 c0 = gx.alpha_comp0, c1 = gx.alpha_comp1, op = gx.alpha_op;
    st->alpha_test_enable = 1;
    if ((c0 == GX_ALWAYS && c1 == GX_ALWAYS) || (op == GX_AOP_OR && (c0 == GX_ALWAYS || c1 == GX_ALWAYS))) {
        st->alpha_test_enable = 0;
    } else if (op == GX_AOP_AND && c1 == GX_ALWAYS) {
        st->alpha_test_func = pica_func(c0);
        st->alpha_test_ref = gx.alpha_ref0;
    } else if (op == GX_AOP_AND && c0 == GX_ALWAYS) {
        st->alpha_test_func = pica_func(c1);
        st->alpha_test_ref = gx.alpha_ref1;
    } else if (op == GX_AOP_OR && c1 == GX_NEVER) {
        st->alpha_test_func = pica_func(c0);
        st->alpha_test_ref = gx.alpha_ref0;
    } else if (op == GX_AOP_OR && c0 == GX_NEVER) {
        st->alpha_test_func = pica_func(c1);
        st->alpha_test_ref = gx.alpha_ref1;
    } else {
        static int warned;
        if (!warned) {
            warned = 1;
            PLOG_W("GX: two-sided alpha compare (%u %u op %u %u %u) uses its first test", c0, gx.alpha_ref0, op, c1,
                   gx.alpha_ref1);
        }
        st->alpha_test_func = pica_func(c0);
        st->alpha_test_ref = gx.alpha_ref0;
    }

    /* blending */
    switch (gx.blend_type) {
    case GX_BM_BLEND:
        st->blend_mode = 1;
        st->blend_eq_rgb = st->blend_eq_alpha = 0;
        st->blend_src_rgb = st->blend_src_alpha = blend_factor(gx.blend_src, 1);
        st->blend_dst_rgb = st->blend_dst_alpha = blend_factor(gx.blend_dst, 0);
        break;
    case GX_BM_SUBTRACT:
        st->blend_mode = 1;
        st->blend_eq_rgb = st->blend_eq_alpha = 2; /* dst - src */
        st->blend_src_rgb = st->blend_src_alpha = 1;
        st->blend_dst_rgb = st->blend_dst_alpha = 1;
        break;
    case GX_BM_LOGIC:
        st->blend_mode = 2;
        st->logic_op = logic_op(gx.blend_logic);
        break;
    default:
        st->blend_mode = 0;
        break;
    }

    /* depth and writes */
    st->depth_test = gx.z_enable;
    st->depth_func = gx.z_enable ? pica_func(gx.z_func) : 1;
    st->depth_write = gx.z_update;
    /* RGB8 pixel format has no alpha: destination alpha reads 1, which the
     * port keeps by never writing alpha then (the EFB alpha is cleared to 1). */
    int alpha_writes = gx.alpha_update && gx.pixel_fmt != GX_PF_RGB8_Z24;
    st->color_write = (u8)((gx.color_update ? 0x7 : 0) | (alpha_writes ? 0x8 : 0));
    st->cull = 0; /* culled here */
    build_fog(st);

    /* scissor (inclusive GX box -> exclusive) and viewport */
    s32 x0 = (s32)gx.scissor[0] - gx.scissor_off[0], y0 = (s32)gx.scissor[1] - gx.scissor_off[1];
    s32 x1 = x0 + (s32)gx.scissor[2], y1 = y0 + (s32)gx.scissor[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > gx.efb_w) x1 = gx.efb_w;
    if (y1 > gx.efb_h) y1 = gx.efb_h;
    st->scissor_x0 = (u16)x0;
    st->scissor_y0 = (u16)y0;
    st->scissor_x1 = (u16)(x1 > x0 ? x1 : x0);
    st->scissor_y1 = (u16)(y1 > y0 ? y1 : y0);
    st->viewport_x = gx.vp[0];
    st->viewport_y = gx.vp[1];
    st->viewport_w = gx.vp[2];
    st->viewport_h = gx.vp[3];
    st->depth_near = gx.vp[4];
    st->depth_far = gx.vp[5];

    plat_gpu_set_state(st);
    s_built_version = gx.version;
}

/* ---- Lighting ---- */

static f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }

static f32 light_channel(int ch, const f32* pos, const f32* nrm, f32 base_mat, f32 base_amb, int comp) {
    /* comp: 0..2 = r,g,b ; 3 = alpha */
    (void)ch;
    const typeof(gx.chan[0])* c = &gx.chan[ch];
    f32 acc = base_amb;
    for (int l = 0; l < GX_MAX_LIGHT; l++) {
        if (!(c->light_mask & (1u << l))) {
            continue;
        }
        const GxLightObj* lt = &gx.light[l];
        f32 ldir[3] = {lt->pos[0] - pos[0], lt->pos[1] - pos[1], lt->pos[2] - pos[2]};
        f32 attn = 1.0f;
        f32 d2 = ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2];
        f32 d = sqrtf(d2);
        if (d > 0) {
            ldir[0] /= d;
            ldir[1] /= d;
            ldir[2] /= d;
        }
        if (c->attn_fn == GX_AF_SPEC) {
            f32 ndl = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            f32 a = ndl >= 0 ? nrm[0] * lt->dir[0] + nrm[1] * lt->dir[1] + nrm[2] * lt->dir[2] : 0;
            if (a < 0) {
                a = 0;
            }
            f32 num = lt->a[0] + lt->a[1] * a + lt->a[2] * a * a;
            f32 den = lt->k[0] + lt->k[1] * a + lt->k[2] * a * a;
            attn = (num > 0 ? num : 0) / (den != 0 ? den : 1e-6f);
        } else if (c->attn_fn == GX_AF_SPOT) {
            f32 cs = ldir[0] * lt->dir[0] + ldir[1] * lt->dir[1] + ldir[2] * lt->dir[2];
            if (cs < 0) {
                cs = 0;
            }
            f32 num = lt->a[0] + lt->a[1] * cs + lt->a[2] * cs * cs;
            f32 den = lt->k[0] + lt->k[1] * d + lt->k[2] * d2;
            attn = (num > 0 ? num : 0) / (den != 0 ? den : 1e-6f);
        }
        f32 diff = 1.0f;
        if (c->diff_fn != GX_DF_NONE) {
            diff = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            if (c->diff_fn == GX_DF_CLAMP && diff < 0) {
                diff = 0;
            }
        }
        const u8* lc = &lt->color.r;
        acc += attn * diff * (f32)lc[comp];
    }
    acc = clampf(acc, 0, 255);
    return base_mat * acc / 255.0f;
}

/* Output color of channel pair `ch` (0 = COLOR0/ALPHA0) for a vertex. */
static GXColor channel_color(int ch, const GxVertex* v, const f32* pos, const f32* nrm) {
    GXColor out;
    for (int part = 0; part < 2; part++) { /* color, alpha */
        int c = ch + part * 2;
        const typeof(gx.chan[0])* cc = &gx.chan[c];
        GXColor mat = cc->mat_src == GX_SRC_VTX ? v->clr[ch] : gx.mat[ch];
        GXColor amb = cc->amb_src == GX_SRC_VTX ? v->clr[ch] : gx.amb[ch];
        if (!cc->enable) {
            if (part == 0) {
                out.r = mat.r;
                out.g = mat.g;
                out.b = mat.b;
            } else {
                out.a = mat.a;
            }
            continue;
        }
        if (part == 0) {
            out.r = (u8)clampf(light_channel(c, pos, nrm, mat.r, amb.r, 0), 0, 255);
            out.g = (u8)clampf(light_channel(c, pos, nrm, mat.g, amb.g, 1), 0, 255);
            out.b = (u8)clampf(light_channel(c, pos, nrm, mat.b, amb.b, 2), 0, 255);
        } else {
            out.a = (u8)clampf(light_channel(c, pos, nrm, mat.a, amb.a, 3), 0, 255);
        }
    }
    return out;
}

/* ---- Texture coordinates ---- */

static const f32* matrix_rows(u32 id) {
    static const f32 ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    if (id < 30) {
        return &gx.pos_mtx[id / 3][0][0];
    }
    if (id < 60) {
        return &gx.tex_mtx[(id - 30) / 3][0][0];
    }
    return &ident[0][0];
}

static void texgen(int coord, const GxVertex* v, const GXColor* chan_out, f32* out) {
    const typeof(gx.texgen[0])* tg = &gx.texgen[coord];
    f32 src[4] = {0, 0, 1, 1};
    switch (tg->src) {
    case GX_TG_POS: src[0] = v->pos[0]; src[1] = v->pos[1]; src[2] = v->pos[2]; break;
    case GX_TG_NRM: src[0] = v->nrm[0]; src[1] = v->nrm[1]; src[2] = v->nrm[2]; break;
    case GX_TG_COLOR0:
    case GX_TG_COLOR1: {
        const GXColor* c = &chan_out[tg->src - GX_TG_COLOR0];
        out[0] = c->r / 255.0f;
        out[1] = c->g / 255.0f;
        return;
    }
    default:
        if (tg->src >= GX_TG_TEX0 && tg->src <= GX_TG_TEX7) {
            int t = tg->src - GX_TG_TEX0;
            src[0] = v->tex[t][0];
            src[1] = v->tex[t][1];
        } else if (tg->src >= GX_TG_TEXCOORD0) {
            /* bump: the source coordinate (embossing offset not applied) */
            int t = tg->src - GX_TG_TEXCOORD0;
            src[0] = v->tex[t][0];
            src[1] = v->tex[t][1];
        }
        break;
    }
    if (tg->func == GX_TG_SRTG) {
        out[0] = chan_out[0].r / 255.0f;
        out[1] = chan_out[0].g / 255.0f;
        return;
    }
    f32 s, t, q = 1.0f;
    if (tg->func == GX_TG_MTX2x4 || tg->func == GX_TG_MTX3x4) {
        u32 id = gx.vtx_desc[GX_VA_TEX0MTXIDX + coord] != GX_NONE ? v->texmtx[coord] : tg->mtx;
        const f32* m = matrix_rows(id);
        s = m[0] * src[0] + m[1] * src[1] + m[2] * src[2] + m[3] * src[3];
        t = m[4] * src[0] + m[5] * src[1] + m[6] * src[2] + m[7] * src[3];
        if (tg->func == GX_TG_MTX3x4) {
            q = m[8] * src[0] + m[9] * src[1] + m[10] * src[2] + m[11] * src[3];
        }
    } else {
        s = src[0];
        t = src[1];
    }
    if (tg->pt_mtx != GX_PTIDENTITY && tg->pt_mtx >= GX_PTTEXMTX0) {
        if (tg->normalize) {
            f32 len = sqrtf(s * s + t * t + q * q);
            if (len > 0) {
                s /= len;
                t /= len;
                q /= len;
            }
        }
        const f32* p = &gx.pttex_mtx[((tg->pt_mtx - GX_PTTEXMTX0) / 3) % GX_PTTEX_MTX_COUNT][0][0];
        f32 s2 = p[0] * s + p[1] * t + p[2] * q + p[3];
        f32 t2 = p[4] * s + p[5] * t + p[6] * q + p[7];
        f32 q2 = p[8] * s + p[9] * t + p[10] * q + p[11];
        s = s2;
        t = t2;
        q = q2;
    }
    if (q != 1.0f && q != 0.0f) {
        s /= q;
        t /= q;
    }
    out[0] = s;
    out[1] = t;
}

/* ---- Vertices ---- */

typedef struct {
    PlatGpuVertex v;
    f32 wx, wy; /* window position (EFB pixels), for culling, lines, bbox */
    int front;  /* w > 0 */
} Out;

static Out* s_out; /* 4096 + 8, heap (gx_bigmem.h) */
static void out_alloc(void) {
    if (s_out == NULL) {
        s_out = gx_big_alloc("gx transformed vertices", sizeof(Out) * (4096 + 8));
    }
}
static int s_dump; /* this frame's draws are logged (switch gxdump) */

int gx_draw_dumping(void) { return s_dump; }

/* The position part of transform(): eye position p, clip position, front test and window position. Shared by
 * the bounding box of shader-path draws (gxvshbbox), so the box comes out of the same arithmetic. */
static inline __attribute__((always_inline)) void xf_position(const GxVertex* v, Out* o, f32* p) {
    const f32(*m)[4] = gx.pos_mtx[v->pnmtx % GX_POS_MTX_COUNT];
    for (int r = 0; r < 3; r++) {
        p[r] = m[r][0] * v->pos[0] + m[r][1] * v->pos[1] + m[r][2] * v->pos[2] + m[r][3];
    }
    f32 cx, cy, cz, cw;
    if (gx.proj[0] == (f32)GX_PERSPECTIVE) {
        cx = gx.proj[1] * p[0] + gx.proj[2] * p[2];
        cy = gx.proj[3] * p[1] + gx.proj[4] * p[2];
        cz = gx.proj[5] * p[2] + gx.proj[6];
        cw = -p[2];
    } else {
        cx = gx.proj[1] * p[0] + gx.proj[2];
        cy = gx.proj[3] * p[1] + gx.proj[4];
        cz = gx.proj[5] * p[2] + gx.proj[6];
        cw = 1.0f;
    }
    o->v.pos[0] = cx;
    o->v.pos[1] = cy;
    o->v.pos[2] = cz;
    o->v.pos[3] = cw;
    o->front = cw > 0.0f;
    if (o->front) {
        o->wx = gx.vp[0] + gx.vp[2] * 0.5f * (1.0f + cx / cw);
        o->wy = gx.vp[1] + gx.vp[3] * 0.5f * (1.0f - cy / cw);
    } else {
        o->wx = o->wy = 0;
    }
}

static void transform(const GxVertex* v, Out* o) {
    f32 p[3];
    xf_position(v, o, p);
    f32 n[3] = {0, 0, 1};
    /* the eye-space normal feeds only the lighting (texgen reads the vertex's own normal) */
    if (v->has_nrm && (!gx_opt_unlit || s_need_nrm)) {
        const f32(*nm)[3] = gx.nrm_mtx[v->pnmtx % GX_POS_MTX_COUNT];
        for (int r = 0; r < 3; r++) {
            n[r] = nm[r][0] * v->nrm[0] + nm[r][1] * v->nrm[1] + nm[r][2] * v->nrm[2];
        }
        /* unit length, as Dolphin lights: a normal matrix with scale in it must not dim the model */
        f32 len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (len2 > 0.0f && len2 != 1.0f) {
            f32 inv = 1.0f / sqrtf(len2);
            n[0] *= inv;
            n[1] *= inv;
            n[2] *= inv;
        }
    }
    GXColor chan_out[2];
    int need_both = 0;
    if (gx_opt_unlit) {
        /* what channel_color gives with lighting off: the material colour,
         * from the vertex or the register, for colour and alpha apart */
        need_both = s_need_both;
        for (int k = 0; k < 1 + need_both; k++) {
            int ch = k == 0 ? s_color_chan : 1 - s_color_chan;
            if (k == 0 && !need_both && gx.num_chans == 0) {
                break; /* the colour is not used */
            }
            if (s_unlit[ch]) {
                GXColor rgb = s_mat_vtx_rgb[ch] ? v->clr[ch] : gx.mat[ch];
                rgb.a = s_mat_vtx_a[ch] ? v->clr[ch].a : gx.mat[ch].a;
                chan_out[ch] = rgb;
            } else {
                chan_out[ch] = channel_color(ch, v, p, n);
            }
        }
    } else {
        for (int u = 0; u < s_units; u++) {
            u8 src = gx.texgen[s_unit_coord[u]].src;
            if (gx.texgen[s_unit_coord[u]].func == GX_TG_SRTG || src == GX_TG_COLOR0 || src == GX_TG_COLOR1) {
                need_both = 1;
            }
        }
        chan_out[s_color_chan] = channel_color(s_color_chan, v, p, n);
        if (need_both) {
            chan_out[1 - s_color_chan] = channel_color(1 - s_color_chan, v, p, n);
        }
    }
    const GXColor* c = &chan_out[s_color_chan];
    if (gx.num_chans == 0) {
        o->v.color[0] = o->v.color[1] = o->v.color[2] = o->v.color[3] = 0;
    } else {
        o->v.color[0] = c->r;
        o->v.color[1] = c->g;
        o->v.color[2] = c->b;
        o->v.color[3] = c->a;
    }
    f32* tcs[3] = {o->v.tc0, o->v.tc1, o->v.tc2};
    for (int u = 0; u < 3; u++) {
        if (u < s_units) {
            texgen(s_unit_coord[u], v, chan_out, tcs[u]);
        } else {
            tcs[u][0] = tcs[u][1] = 0;
        }
    }
}

static void box_add(u16* box, const Out* o) {
    s32 x = (s32)o->wx, y = (s32)o->wy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > gx.efb_w - 1) x = gx.efb_w - 1;
    if (y > gx.efb_h - 1) y = gx.efb_h - 1;
    if (x < box[0]) box[0] = (u16)x;
    if (y < box[1]) box[1] = (u16)y;
    if (x > box[2]) box[2] = (u16)x;
    if (y > box[3]) box[3] = (u16)y;
}

/* Switch gxvshbbox (default 0: eki_00's edge row, PORT_3DS_GX_PERF.md round 3, awaits a decision). 1: a draw made while the bounding box is armed takes the shader path like any
 * other, and the box gets the window positions of its vertices from xf_position on the CPU (the arithmetic
 * transform() uses, so the box is the same); GXReadBoundingBox disarms it (every TTYD read follows its own
 * GXClearBoundingBox). 0: armed draws stay on the CPU path; the read disarms the box there too.
 * 2: as 1, plus the old box (every draw since the clear, through transform()) kept beside it and compared
 * at each read (`gxstats` bbox line, and a warning). */
int gx_opt_vshbbox = 0;
static u16 s_bref[4];
static int s_bref_on;
static u32 s_bbox_reads, s_bbox_mism;

void gx_bbox_clear(void) {
    gx.bbox[0] = 1023;
    gx.bbox[1] = 1023;
    gx.bbox[2] = 0;
    gx.bbox[3] = 0;
    gx_bbox_active = 1;
    memcpy(s_bref, gx.bbox, sizeof(s_bref));
    s_bref_on = gx_opt_vshbbox == 2;
}

void gx_bbox_read(u16* out) {
    memcpy(out, gx.bbox, sizeof(gx.bbox));
    s_bbox_reads++;
    if (s_bref_on && memcmp(s_bref, gx.bbox, sizeof(s_bref)) != 0) {
        if (s_bbox_mism++ < 8) {
            PLOG_W("gxvshbbox verify: box %u,%u %u,%u, the CPU path's %u,%u %u,%u", gx.bbox[0], gx.bbox[1], gx.bbox[2],
                   gx.bbox[3], s_bref[0], s_bref[1], s_bref[2], s_bref[3]);
        }
    }
    /* every TTYD read follows its own clear: disarm, in every mode (with gxvshbbox=0 only the draws between a
     * clear and its read stay on the CPU; before, the box stayed armed and every later draw did) */
    gx_bbox_active = 0;
}

void gx_bbox_counts(u32* reads, u32* mism) {
    *reads = s_bbox_reads;
    *mism = s_bbox_mism;
}

static void bbox_add(const Out* o) {
    if (!o->front) {
        return;
    }
    if (gx_bbox_active) {
        box_add(gx.bbox, o);
    }
    if (s_bref_on) {
        box_add(s_bref, o);
    }
}

/* The box of a draw the shader makes: positions only (gxvshbbox=1); with 2 also the old box through transform(). */
static void bbox_shader_draw(const GxVertex* v, u32 n) {
    if (!gx_bbox_active && !s_bref_on) {
        return;
    }
    u64 t0 = GX_PERF_T();
    Out o;
    f32 p[3];
    for (u32 i = 0; i < n; i++) {
        xf_position(&v[i], &o, p);
        if (gx_bbox_active && o.front) {
            box_add(gx.bbox, &o);
        }
        if (s_bref_on) {
            transform(&v[i], &o);
            if (o.front) {
                box_add(s_bref, &o);
            }
        }
    }
    if (gx_perf_on > 0) {
        gx_perf.bbox_draws++;
        gx_perf.bbox_verts += n;
        gx_perf.t_bbox += plat_ticks() - t0;
    }
}

/* GX front faces are clockwise on screen (window y down). */
static int culled(const Out* a, const Out* b, const Out* c) {
    if (gx.cull == GX_CULL_NONE) {
        return 0;
    }
    if (gx.cull == GX_CULL_ALL) {
        return 1;
    }
    if (!a->front || !b->front || !c->front) {
        return 0; /* crosses the eye plane: let the GPU clip it */
    }
    f32 area = (b->wx - a->wx) * (c->wy - a->wy) - (c->wx - a->wx) * (b->wy - a->wy);
    int clockwise = area > 0; /* y down: positive is clockwise */
    return gx.cull == GX_CULL_BACK ? !clockwise : clockwise;
}

static PlatGpuVertex* s_tri_out;
static u32 s_tri_n;

static void tri(const Out* a, const Out* b, const Out* c) {
    if (culled(a, b, c)) {
        return;
    }
    s_tri_out[s_tri_n++] = a->v;
    s_tri_out[s_tri_n++] = b->v;
    s_tri_out[s_tri_n++] = c->v;
}

/* A screen-space quad for a line from a to b, `width` EFB pixels wide. */
static void line(const Out* a, const Out* b, f32 width) {
    if (!a->front || !b->front) {
        return;
    }
    f32 dx = b->wx - a->wx, dy = b->wy - a->wy;
    f32 len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) {
        return;
    }
    f32 nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
    Out q[4] = {*a, *a, *b, *b};
    f32 off[4][2] = {{nx, ny}, {-nx, -ny}, {-nx, -ny}, {nx, ny}};
    for (int i = 0; i < 4; i++) {
        f32 w = q[i].v.pos[3];
        q[i].v.pos[0] += off[i][0] * 2.0f / gx.vp[2] * w;
        q[i].v.pos[1] -= off[i][1] * 2.0f / gx.vp[3] * w;
    }
    s_tri_out[s_tri_n++] = q[0].v;
    s_tri_out[s_tri_n++] = q[1].v;
    s_tri_out[s_tri_n++] = q[2].v;
    s_tri_out[s_tri_n++] = q[0].v;
    s_tri_out[s_tri_n++] = q[2].v;
    s_tri_out[s_tri_n++] = q[3].v;
}

/* switch gxstats: which GX features the draw's vertices use */
static void perf_draw(u8 prim, const GxVertex* v, u32 n) {
    GxPerf* p = &gx_perf;
    p->xf_draws++;
    p->xf_verts += n;
    static const u8 prim_slot[8] = {0, 0, 1, 2, 3, 4, 5, 6}; /* (prim - 0x80) >> 3 */
    p->v_prim[prim_slot[((prim - 0x80) >> 3) & 7]] += n;
    if (gx.vtx_desc[GX_VA_PNMTXIDX] != GX_NONE) {
        p->v_pnmtxidx += n;
        u32 seen = 0;
        for (u32 i = 0; i < n; i++) {
            seen |= 1u << (v[i].pnmtx % GX_POS_MTX_COUNT);
        }
        u32 k = (u32)__builtin_popcount(seen);
        if (k > p->max_pnmtx_in_draw) {
            p->max_pnmtx_in_draw = k;
        }
    }
    for (int t = 0; t < 8; t++) {
        if (gx.vtx_desc[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
            p->v_texmtxidx += n;
            break;
        }
    }
    if (v[0].has_nrm) {
        p->v_nrm += n;
    }
    int need_both = 0;
    for (int u = 0; u < s_units; u++) {
        const typeof(gx.texgen[0])* tg = &gx.texgen[s_unit_coord[u]];
        if (tg->func == GX_TG_SRTG || tg->src == GX_TG_COLOR0 || tg->src == GX_TG_COLOR1) {
            need_both = 1;
        }
        if (tg->func == GX_TG_MTX2x4) {
            p->v_tg_2x4 += n;
        } else if (tg->func == GX_TG_MTX3x4) {
            p->v_tg_3x4 += n;
        } else {
            p->v_tg_other += n;
        }
        if (tg->src == GX_TG_POS) {
            p->v_tg_pos += n;
        } else if (tg->src == GX_TG_NRM) {
            p->v_tg_nrm += n;
        } else if (tg->src >= GX_TG_TEX0 && tg->src <= GX_TG_TEX7) {
            p->v_tg_tex += n;
        } else if (tg->src == GX_TG_COLOR0 || tg->src == GX_TG_COLOR1) {
            p->v_tg_color += n;
        }
        if (tg->pt_mtx != GX_PTIDENTITY && tg->pt_mtx >= GX_PTTEXMTX0) {
            p->v_tg_post += n;
            if (tg->normalize) {
                p->v_tg_normalize += n;
            }
        }
    }
    if (need_both) {
        p->v_second_chan += n;
    }
    int lit = 0;
    for (int k = 0; k < (need_both ? 2 : 1); k++) {
        int ch = k == 0 ? s_color_chan : 1 - s_color_chan;
        for (int part = 0; part < 2; part++) {
            const typeof(gx.chan[0])* c = &gx.chan[ch + part * 2];
            if (!c->enable) {
                continue;
            }
            lit = 1;
            u32 lights = (u32)__builtin_popcount(c->light_mask & 0xFF);
            p->v_light_evals += n * lights * (part == 0 ? 3 : 1);
            if (lights && c->attn_fn == GX_AF_SPOT) {
                p->v_spot += n;
            } else if (lights && c->attn_fn == GX_AF_SPEC) {
                p->v_spec += n;
            }
            if (c->mat_src == GX_SRC_VTX || c->amb_src == GX_SRC_VTX) {
                p->v_vtx_src += n;
            }
        }
    }
    if (lit) {
        p->v_lit += n;
    } else {
        p->v_unlit += n;
    }
}

/* The PICA state for the GX state in force (rebuilt when the version changed). */
static void ensure_state(void) {
    if (gx.version == s_built_version) {
        return;
    }
    u64 t0 = GX_PERF_T();
    {
        if (gx_opt_statever && state_unchanged()) {
            s_built_version = gx.version; /* the PICA state would come out the same */
            if (gx_perf_on > 0) {
                gx_perf.builds_same++;
                gx_perf.t_state += plat_ticks() - t0;
            }
        } else {
            build_state();
            if (gx_opt_statever) {
                state_snapshot();
            }
            if (gx_perf_on > 0) {
                gx_perf.builds++;
                gx_perf.t_state += plat_ticks() - t0;
            }
        }
    }
}

static int empty_target(void) {
    /* empty scissor or viewport: the PICA200 hangs on them */
    return s_state.scissor_x1 <= s_state.scissor_x0 || s_state.scissor_y1 <= s_state.scissor_y0 || gx.vp[2] == 0.0f ||
           gx.vp[3] == 0.0f;
}

/* ---- Shader path (switch gxvsh): what the state lets the PICA do, and its uniforms ---- */

static u32 s_vsh_ver = ~0u, s_vsh_xfv = ~0u, s_vsh_pn = ~0u;
static u32 s_vsh_reasons;     /* why the state keeps draws on the CPU (GXV_R_*), 0 if it does not */
static u8 s_vsh_unit_attr[3]; /* GX texture coordinate attribute each PICA unit's texgen reads, 0xFF none */
static PlatGpuVshConst s_vsh_const;
/* The block's id: a new one only when a rebuild changed the block (so the
 * back end compares ids, not 160 bytes, per draw; and equal rebuilds after a
 * setter call keep the draws merging). */
static u32 s_vsh_const_id;
/* switch gxvshlit (default 1): colour channel 0 lit by GX lights in the shader (map materials: diffuse clamp or sign,
 * spot or no attenuation, ambient from the register, alpha unlit); 0 keeps lit draws on the CPU */
int gx_opt_vshlit = 1;
/* switch gxvshtg (default 1): texgen from the position or the normal, and MTX3x4, in the shader */
int gx_opt_vshtg = 1;
static int s_vsh_lit;
static PlatGpuVshLight s_vsh_light;
static u32 s_vsh_light_id;

/* Whether the lit colour channel `ch` can be lit in the shader; fills the light block. */
static int vsh_light_block(int ch, PlatGpuVshLight* L) {
    const typeof(gx.chan[0])* c = &gx.chan[ch];
    if (!gx_opt_vshlit || ch != 0 || !c->enable || gx.chan[ch + 2].enable || c->amb_src != GX_SRC_REG ||
        (c->diff_fn != GX_DF_NONE && c->diff_fn != GX_DF_CLAMP && c->diff_fn != GX_DF_SIGN) ||
        (c->attn_fn != GX_AF_NONE && c->attn_fn != GX_AF_SPOT)) {
        return 0;
    }
    memset(L, 0, sizeof(*L));
    const f32(*m)[4] = gx.pos_mtx[gx.cur_pnmtx % GX_POS_MTX_COUNT];
    memcpy(L->eye, m, sizeof(L->eye));
    int has_nrm = gx.vtx_desc[GX_VA_NRM] != GX_NONE || gx.vtx_desc[GX_VA_NBT] != GX_NONE;
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            /* transform() leaves a vertex without a normal at (0, 0, 1), untransformed */
            L->nrm[r][k] = has_nrm ? gx.nrm_mtx[gx.cur_pnmtx % GX_POS_MTX_COUNT][r][k] : (r == k ? 1.0f : 0.0f);
        }
    }
    L->amb[0] = gx.amb[ch].r;
    L->amb[1] = gx.amb[ch].g;
    L->amb[2] = gx.amb[ch].b;
    L->dmode[0] = c->diff_fn == GX_DF_CLAMP ? 0.0f : -1e15f;
    L->dmode[1] = c->diff_fn == GX_DF_NONE ? 0.0f : 1.0f;
    L->dmode[2] = c->diff_fn == GX_DF_NONE ? 1.0f : 0.0f;
    L->amode[0] = c->attn_fn == GX_AF_SPOT ? 1.0f : 0.0f;
    L->amode[1] = c->attn_fn == GX_AF_SPOT ? 0.0f : 1.0f;
    u32 n = 0;
    for (int l = 0; l < GX_MAX_LIGHT; l++) {
        if (!(c->light_mask & (1u << l))) {
            continue;
        }
        const GxLightObj* lt = &gx.light[l];
        for (int k = 0; k < 3; k++) {
            L->light[n].pos[k] = lt->pos[k];
            L->light[n].dir[k] = lt->dir[k];
            L->light[n].a[k] = lt->a[k];
            L->light[n].k[k] = lt->k[k];
        }
        L->light[n].color[0] = lt->color.r;
        L->light[n].color[1] = lt->color.g;
        L->light[n].color[2] = lt->color.b;
        n++;
    }
    if (n == 0) {
        /* the shader's loop runs once: a light that adds nothing (colour 0, far away, k0 = 1) */
        L->light[0].pos[2] = 1e6f;
        L->light[0].k[0] = 1.0f;
    }
    L->count = n;
    return 1;
}

static void vsh_prepare(void) {
    u32 why = 0;
    s_vsh_lit = 0;
    PlatGpuVshConst cb;
    PlatGpuVshConst* c = &cb;
    memset(c, 0, sizeof(*c));
    if (s_need_both) {
        why |= GXV_R_TWOCHAN;
    }
    /* colour: as transform() gives it with the channel unlit */
    if (gx.num_chans != 0) {
        int ch = s_color_chan;
        if (!s_unlit[ch]) {
            PlatGpuVshLight L;
            if (vsh_light_block(ch, &L)) {
                s_vsh_lit = 1;
                if (s_vsh_light_id == 0 || !gx_mem_equal(&L, &s_vsh_light, sizeof(L))) {
                    s_vsh_light = L;
                    if (++s_vsh_light_id == 0) {
                        s_vsh_light_id = 1;
                    }
                }
            } else {
                why |= GXV_R_LIT;
            }
        }
        if (ch == 1 && (s_mat_vtx_rgb[1] || s_mat_vtx_a[1])) {
            why |= GXV_R_CLR1; /* the stream carries colour 0 */
        }
        const GXColor m = gx.mat[ch];
        for (int i = 0; i < 3; i++) {
            c->clrmix[i] = s_mat_vtx_rgb[ch] ? 1.0f : 0.0f;
            c->matclr[i] = s_mat_vtx_rgb[ch] ? 0.0f : (f32)(&m.r)[i];
        }
        c->clrmix[3] = s_mat_vtx_a[ch] ? 1.0f : 0.0f;
        c->matclr[3] = s_mat_vtx_a[ch] ? 0.0f : (f32)m.a;
    }
    /* texture units: texgen() with an MTX2x4 or (gxvshtg) MTX3x4 matrix on (s, t, 1, 1), or on the position or the
     * normal (x, y, z, 1); MTX3x4 divides by q per vertex, as texgen() does */
    int has_nrm = gx.vtx_desc[GX_VA_NRM] != GX_NONE || gx.vtx_desc[GX_VA_NBT] != GX_NONE;
    for (int u = 0; u < 3; u++) {
        s_vsh_unit_attr[u] = 0xFF;
        c->tex[3 * u + 2][3] = 1.0f; /* q row (0, 0, 0, 1): no divide */
        if (u >= s_units) {
            continue; /* rows 0: the coordinate is (0, 0), as on the CPU */
        }
        const typeof(gx.texgen[0])* tg = &gx.texgen[s_unit_coord[u]];
        int proj = tg->func == GX_TG_MTX3x4;
        if (tg->func != GX_TG_MTX2x4 && !(proj && gx_opt_vshtg)) {
            why |= GXV_R_TGFUNC;
        }
        if (tg->src >= GX_TG_TEX0 && tg->src <= GX_TG_TEX7) {
            s_vsh_unit_attr[u] = (u8)(tg->src - GX_TG_TEX0);
            c->tgsrc[u][2] = 1.0f;
        } else if (gx_opt_vshtg && tg->src == GX_TG_POS) {
            c->tgsrc[u][0] = 1.0f;
        } else if (gx_opt_vshtg && tg->src == GX_TG_NRM && has_nrm) {
            c->tgsrc[u][1] = 1.0f;
        } else {
            why |= GXV_R_TGSRC;
        }
        if (tg->pt_mtx != GX_PTIDENTITY && tg->pt_mtx >= GX_PTTEXMTX0) {
            why |= GXV_R_TGPOST;
        }
        const f32* m = matrix_rows(tg->mtx);
        memcpy(c->tex[3 * u], m, 16);
        memcpy(c->tex[3 * u + 1], m + 4, 16);
        if (proj) {
            memcpy(c->tex[3 * u + 2], m + 8, 16);
            c->tgsrc[u][3] = 1.0f;
        }
    }
    /* clip rows: projection x position matrix, in 32-bit float on the CPU */
    const f32(*m)[4] = gx.pos_mtx[gx.cur_pnmtx % GX_POS_MTX_COUNT];
    const f32* p = gx.proj;
    for (int j = 0; j < 4; j++) {
        if (p[0] == (f32)GX_PERSPECTIVE) {
            c->clip[0][j] = p[1] * m[0][j] + p[2] * m[2][j];
            c->clip[1][j] = p[3] * m[1][j] + p[4] * m[2][j];
            c->clip[2][j] = p[5] * m[2][j] + (j == 3 ? p[6] : 0.0f);
            c->clip[3][j] = -m[2][j];
        } else {
            c->clip[0][j] = p[1] * m[0][j] + (j == 3 ? p[2] : 0.0f);
            c->clip[1][j] = p[3] * m[1][j] + (j == 3 ? p[4] : 0.0f);
            c->clip[2][j] = p[5] * m[2][j] + (j == 3 ? p[6] : 0.0f);
            c->clip[3][j] = j == 3 ? 1.0f : 0.0f;
        }
    }
    c->flags = (p[0] == (f32)GX_PERSPECTIVE ? 0u : PLAT_VSH_ORTHO) | (s_vsh_lit ? PLAT_VSH_LIT : 0u);
    s_vsh_reasons = why;
    if (s_vsh_const_id == 0 || !gx_mem_equal(c, &s_vsh_const, sizeof(*c))) {
        s_vsh_const = *c;
        if (++s_vsh_const_id == 0) {
            s_vsh_const_id = 1;
        }
    }
}

static void vsh_state(void) {
    if (s_vsh_ver != gx.version || s_vsh_xfv != gx_xf_version || s_vsh_pn != gx.cur_pnmtx) {
        vsh_prepare();
        s_vsh_ver = gx.version;
        s_vsh_xfv = gx_xf_version;
        s_vsh_pn = gx.cur_pnmtx;
    }
}

static u32 vsh_cull(void) { return gx.cull == GX_CULL_FRONT ? 1u : gx.cull == GX_CULL_BACK ? 2u : 0u; }

int gx_vsh_imm_on = 1; /* switch gxvshimm (default 1): immediate-mode draws may take the shader path */

/* An immediate-mode draw (GXBegin/GXEnd) on the shader path, when its group
 * takes it: the vertices go to this frame's ring. 0: draw it on the CPU (it
 * was routed already). */
static int draw_vsh_imm(u8 prim, const GxVertex* v, u32 n, const void* ident, u32 ident_n, int imm) {
    vsh_state();
    u32 why = gx_vsh_imm_on ? s_vsh_reasons | gx_vsh_imm_reasons(prim, n) : GXV_R_IMM;
    if (gx_bbox_active && gx_opt_vshbbox == 0) {
        why |= GXV_R_BBOX;
    }
    if (why == 0 && !gx_vsh_imm_room(n)) {
        why = GXV_R_POOL;
    }
    if (!gx_vsh_route(v[0].pnmtx, why, ident, ident_n, n, imm)) {
        return 0;
    }
    bbox_shader_draw(v, n);
    if (gx.cull == GX_CULL_ALL) {
        return 1;
    }
    const GxVshImm* im = gx_vsh_imm_build(prim, v, n);
    u32 scales = 0;
    for (int u = 0; u < 3; u++) {
        u8 a = s_vsh_unit_attr[u];
        u32 slot = a < 8 ? im->attr_slot[a] : 0xFF;
        scales |= (slot < 3 ? PLAT_VSH_UNIT(slot, 0) : PLAT_VSH_UNIT(3, 0)) << (8 + 8 * u);
    }
    if (s_vsh_lit) {
        plat_gpu_vsh_light(&s_vsh_light, s_vsh_light_id);
    }
    plat_gpu_vsh_const(&s_vsh_const, s_vsh_const_id);
    plat_gpu_vsh_draw(im->base, im->layout, scales, vsh_cull(), im->idx, im->nidx);
    gx_vsh_count_shader(n, im->nidx);
    gx_stat_draws++;
    return 1;
}

int gx_draw_vsh(const GxVshStream* s, u32 entry_reason, const void* list, u32 nbytes, u32 nverts, const GxVertex* v,
                const GxDlcDraw* d, u32 nd) {
    ensure_state();
    if (empty_target()) {
        return 1; /* the CPU path would draw nothing either */
    }
    vsh_state();
    u32 why = s == NULL ? (entry_reason ? entry_reason : GXV_R_POOL) : s_vsh_reasons;
    if (gx_bbox_active && gx_opt_vshbbox == 0) {
        why |= GXV_R_BBOX; /* GXReadBoundingBox needs the window positions on the CPU */
    }
    if (!gx_vsh_route(gx.cur_pnmtx, why, list, nbytes, nverts, 0)) {
        return 0;
    }
    if (gx_bbox_active || s_bref_on) {
        for (u32 i = 0; i < nd; i++) { /* the vertices replay() would transform, in its order */
            bbox_shader_draw(v + d[i].first, d[i].count);
        }
    }
    if (gx.cull == GX_CULL_ALL) {
        return 1; /* every triangle culled */
    }
    u32 scales = s->posk;
    for (int u = 0; u < 3; u++) {
        u8 a = s_vsh_unit_attr[u];
        u32 slot = a < 8 ? s->attr_slot[a] : 0xFF;
        scales |= (slot < 3 ? PLAT_VSH_UNIT(slot, s->slot_k[slot]) : PLAT_VSH_UNIT(3, 0)) << (8 + 8 * u);
    }
    if (s_vsh_lit) {
        plat_gpu_vsh_light(&s_vsh_light, s_vsh_light_id);
    }
    plat_gpu_vsh_const(&s_vsh_const, s_vsh_const_id);
    plat_gpu_vsh_draw(gx_vsh_stream_base(s), s->layout, scales, vsh_cull(), s->idx, s->nidx);
    gx_vsh_count_shader(nverts, s->nidx);
    gx_stat_draws++;
    return 1;
}

void gx_draw_vertices(u8 prim, const GxVertex* v, u32 n) {
    if (n == 0) {
        return;
    }
    ensure_state();
    if (gx_vsh_on && !gx_vsh_noted) {
        int in_list = gx_pipe_in_list();
        if (gx.vtx_desc[GX_VA_PNMTXIDX] == GX_NONE) {
            /* immediate mode, or a display list being decoded (first call, changed, uncached): the shader
             * path from this frame's ring when the draw and its group can take it. Known across frames by
             * the code that began it, or by the list (as its cached hits are). */
            if (empty_target()) {
                return;
            }
            const void* ident = gx_begin_caller;
            u32 ident_n = 0xFFFFFFFFu;
            if (in_list) {
                gx_pipe_cur_list(&ident, &ident_n);
            }
            if (draw_vsh_imm(prim, v, n, ident, ident_n, !in_list)) {
                return;
            }
            goto routed;
        }
        /* a draw only the CPU path makes (a decoded list, per-vertex matrices): its group goes to the CPU */
        u32 why = in_list ? GXV_R_MISS : GXV_R_IMM;
        if (gx.vtx_desc[GX_VA_PNMTXIDX] != GX_NONE) {
            u32 seen = 0;
            for (u32 i = 0; i < n; i++) {
                seen |= 1u << (v[i].pnmtx % GX_POS_MTX_COUNT);
            }
            for (u32 m = 0; m < GX_POS_MTX_COUNT; m++) {
                if (seen >> m & 1) {
                    gx_vsh_route(m, why | GXV_R_PNMTXIDX, NULL, 0, n, !in_list);
                }
            }
        } else {
            gx_vsh_route(v[0].pnmtx, why, NULL, 0, n, !in_list);
        }
    }
routed:
    if (empty_target()) {
        return;
    }
    u64 t1 = GX_PERF_T();
    for (u32 i = 0; i < n; i++) {
        transform(&v[i], &s_out[i]);
        bbox_add(&s_out[i]);
    }
    if (gx_perf_on > 0) {
        u64 t2 = plat_ticks();
        gx_perf.t_xform += t2 - t1;
        perf_draw(prim, v, n);
        for (u32 i = 0; i < n; i++) {
            gx_perf.v_behind += !s_out[i].front;
        }
        t1 = t2;
    }
    u32 max_tris;
    switch (prim) {
    case GX_QUADS: max_tris = n / 4 * 2; break;
    case GX_TRIANGLES: max_tris = n / 3; break;
    case GX_TRIANGLESTRIP:
    case GX_TRIANGLEFAN: max_tris = n >= 3 ? n - 2 : 0; break;
    case GX_LINES: max_tris = n / 2 * 2; break;
    case GX_LINESTRIP: max_tris = n >= 2 ? (n - 1) * 2 : 0; break;
    default: max_tris = n * 2; break; /* points */
    }
    if (max_tris == 0) {
        return;
    }
    s_tri_out = plat_gpu_alloc_vertices(max_tris * 3);
    if (s_tri_out == NULL) {
        return; /* larger than the vertex arena (logged) */
    }
    if (s_dump) {
        /* switch gxdump=N: the draws of game frame N; a run of draws with the same matrix and texture is one line */
        const GxTexObj* t0 = s_units > 0 && gx.tex_map_valid[s_unit_map[0]] ? &gx.tex_map[s_unit_map[0]] : NULL;
        static const void* last_img;
        static f32 last_m[3][4];
        static u32 run;
        const void* img = t0 ? t0->image : NULL;
        if (img == last_img && memcmp(last_m, gx.pos_mtx[v[0].pnmtx % GX_POS_MTX_COUNT], sizeof(last_m)) == 0) {
            run++;
            goto dumped;
        }
        if (run > 0) {
            PLOG_I("gxdump   (+%lu draws alike)", (unsigned long)run);
        }
        run = 0;
        last_img = img;
        memcpy(last_m, gx.pos_mtx[v[0].pnmtx % GX_POS_MTX_COUNT], sizeof(last_m));
        PLOG_I("gxdump prim %02x n %lu units %d stages %d tev %d tex0 %p fmt %d %dx%d clip %.2f %.2f %.2f %.2f "
               "col %02x%02x%02x%02x blend %d zc %d atest %d chans %d map %d from %p draw %lu",
               prim, (unsigned long)n, s_units, s_state.num_stages, gx.num_tev, t0 ? t0->image : NULL,
               t0 ? t0->format : -1, t0 ? t0->width : 0, t0 ? t0->height : 0, (double)s_out[0].v.pos[0],
               (double)s_out[0].v.pos[1], (double)s_out[0].v.pos[2], (double)s_out[0].v.pos[3], s_out[0].v.color[0],
               s_out[0].v.color[1], s_out[0].v.color[2], s_out[0].v.color[3], s_state.blend_mode,
               s_state.color_write, s_state.alpha_test_enable, gx.num_chans, s_units > 0 ? s_unit_map[0] : -1,
               gx_begin_caller, (unsigned long)gx_stat_draws);
        const f32(*m)[4] = gx.pos_mtx[v[0].pnmtx % GX_POS_MTX_COUNT];
        PLOG_I("gxdump   v0 %.2f %.2f %.2f mtx[%d] %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f",
               (double)v[0].pos[0], (double)v[0].pos[1], (double)v[0].pos[2], v[0].pnmtx, (double)m[0][0],
               (double)m[0][1], (double)m[0][2], (double)m[0][3], (double)m[1][0], (double)m[1][1], (double)m[1][2],
               (double)m[1][3], (double)m[2][0], (double)m[2][1], (double)m[2][2], (double)m[2][3]);
    }
dumped:
    s_tri_n = 0;
    f32 lw = gx.line_width / 6.0f;
    switch (prim) {
    case GX_QUADS:
        for (u32 i = 0; i + 3 < n; i += 4) {
            tri(&s_out[i], &s_out[i + 1], &s_out[i + 2]);
            tri(&s_out[i], &s_out[i + 2], &s_out[i + 3]);
        }
        break;
    case GX_TRIANGLES:
        for (u32 i = 0; i + 2 < n; i += 3) {
            tri(&s_out[i], &s_out[i + 1], &s_out[i + 2]);
        }
        break;
    case GX_TRIANGLESTRIP:
        for (u32 i = 0; i + 2 < n; i++) {
            if (i & 1) {
                tri(&s_out[i + 1], &s_out[i], &s_out[i + 2]);
            } else {
                tri(&s_out[i], &s_out[i + 1], &s_out[i + 2]);
            }
        }
        break;
    case GX_TRIANGLEFAN:
        for (u32 i = 1; i + 1 < n; i++) {
            tri(&s_out[0], &s_out[i], &s_out[i + 1]);
        }
        break;
    case GX_LINES:
        for (u32 i = 0; i + 1 < n; i += 2) {
            line(&s_out[i], &s_out[i + 1], lw);
        }
        break;
    case GX_LINESTRIP:
        for (u32 i = 0; i + 1 < n; i++) {
            line(&s_out[i], &s_out[i + 1], lw);
        }
        break;
    default: /* points: small screen-space squares */
        for (u32 i = 0; i < n; i++) {
            Out b = s_out[i];
            b.wx += lw;
            line(&s_out[i], &b, lw);
        }
        break;
    }
    plat_gpu_draw(s_tri_out, s_tri_n); /* with 0 (all culled) it only gives the vertices back */
    if (s_tri_n > 0) {
        gx_stat_draws++;
        gx_stat_vertices += s_tri_n;
    }
    if (gx_perf_on > 0) {
        gx_perf.t_emit += plat_ticks() - t1;
        if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
            gx_perf.lines += s_tri_n / 6;
        } else {
            gx_perf.tris_out += s_tri_n / 3;
            gx_perf.tris_culled += max_tris - s_tri_n / 3;
        }
    }
}

void gx_draw_frame_begin(void) {
    static u32 frame;
    static int dump_at = -1;
    static int copy_from, copy_left, copy_stride = 1, copy_seen; /* switch gxdumpcopy=N,K[,S] */
    if (dump_at < 0) {
        char buf[32];
        dump_at = plat_switch_int("gxdump", 0);
        if (plat_switch_value("gxdumpcopy", buf, sizeof(buf))) {
            sscanf(buf, "%d,%d,%d", &copy_from, &copy_left, &copy_stride);
            if (copy_stride < 1) {
                copy_stride = 1;
            }
        }
    }
    frame++;
    s_dump = dump_at > 0 && frame == (u32)dump_at;
    if (s_dump) {
        PLOG_I("gxdump: frame %lu", (unsigned long)frame);
    }
    /* gxdumpcopy=N,K: from frame N, the next K frames that follow a frame with an EFB copy (GXCopyTex) are dumped
     * and their picture saved as shots/dumpFFFFF.ppm; no frame number needed for a copy-dependent effect */
    if (copy_left > 0 && copy_from > 0 && frame >= (u32)copy_from && gx_stat_copies > 0 && copy_seen++ % copy_stride == 0) {
        s_dump = 1;
        copy_left--;
        char path[32];
        snprintf(path, sizeof(path), "shots/dump%05lu.ppm", (unsigned long)frame);
        PLOG_I("gxdump: frame %lu present %lu (copy-triggered, %d left)", (unsigned long)frame,
               (unsigned long)plat_gpu_present_count(), copy_left);
        plat_gpu_shot(path);
    }
    if (gx_vsh_on) {
        gx_vsh_frame();
    }
    gx_perf_frame();
    gx_stat_draws = gx_stat_vertices = gx_stat_copies = 0;
    s_built_version = 0xFFFFFFFFu; /* every frame starts with a full state */
    s_snap_valid = 0;
}
