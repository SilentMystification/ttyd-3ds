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
int gx_unit_of_stage[GX_MAX_TEV]; /* the texture unit each TEV stage samples, -1 none */
static int s_unit_coord[3];  /* GX texcoord each PICA unit samples with */
static int s_unit_map[3];
static int s_units;
/* Fog by a ramp texture (switch gxfogramp, default 1): the PICA fog LUT is indexed by window depth, which cannot
 * give fog that starts at an eye depth of a few hundred under a far plane thousands of times the near plane. The
 * ramp instead is fed the eye depth, scaled and offset (u = a * z_eye + b: the vertex shader's texture unit with a
 * row of the position matrix, or the CPU path), interpolated across the triangle perspective-correct, and looked
 * up per pixel in a 256-texel table of the fog function with clamped addressing; one more TEV stage lerps toward
 * the fog colour by it. s_fog_unit is the texture unit that takes the ramp (-1: PICA fog, or no fog). */
static int s_fog_unit = -1;
static u32 s_fog_tex;
static f32 s_fog_a, s_fog_b;
int gx_opt_fogramp = 1;
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

void gx_draw_state_lost(void) {
    s_snap_valid = 0;
    gx_dirty |= GXD_TEX; /* gxdirty: the texture units are looked up again */
}

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
static void gx_bbx_init(void); /* switch gxbbox, below */

void gx_opts_init(void) {
    out_alloc();
    gx_pipe_alloc();
    gx_opt_statever = plat_switch_int("gxstatever", 0) != 0; /* off by default: fewer rebuilds, but no time saved (PORT_3DS_GX_PERF.md) */
    gx_opt_fogcache = plat_switch_int("gxfogcache", 1) != 0;
    gx_opt_fogramp = plat_switch_int("gxfogramp", 1) != 0;
    gx_opt_unlit = plat_switch_int("gxunlit", 1) != 0;
    gx_vsh_imm_on = plat_switch_int("gxvshimm", 1) != 0;
    gx_opt_vshbbox = plat_switch_int("gxvshbbox", 1);
    gx_bbx_init();
    gx_opt_vshlit = plat_switch_int("gxvshlit", 1) != 0;
    gx_opt_vshtg = plat_switch_int("gxvshtg", 1) != 0;
    gx_opt_dirty = plat_switch_int("gxdirty", 0);
    if (gx_opt_dirty < 0 || gx_opt_dirty > 2) {
        gx_opt_dirty = 1;
    }
    gx_opt_immdirect = plat_switch_int("gximmdirect", 0);
    if (gx_opt_immdirect < 0 || gx_opt_immdirect > 2) {
        gx_opt_immdirect = 1;
    }
    PLOG_I("gx: statever %d fogcache %d unlit %d dirty %d immdirect %d", gx_opt_statever, gx_opt_fogcache,
           gx_opt_unlit, gx_opt_dirty, gx_opt_immdirect);
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
static int fog_valid(void);

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
    if (!fog_valid() || s_fog_unit >= 0) { /* none, or done by the ramp stage */
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
    /* The PICA looks the table up by window depth, which is nearly 1 - 1/z_eye: with a far plane thousands of
     * times the near plane the whole fog range (z_eye of a few hundred to a few thousand) falls inside the last
     * table segment, which the hardware interpolates linearly in depth. Entry 127 stays exact; entry 128 is the
     * least-squares end of that segment over the eye depths from its start to four times the fog end. */
    {
        f32 z0 = 127.0f / 128.0f, den0 = f - z0 * (f - n);
        f32 ze0 = ortho ? n + z0 * (f - n) : (den0 > 0 ? (f * n) / den0 : f);
        f32 ze1 = ortho ? f : f;
        f32 lim = (e > s ? e : s) * 4.0f;
        if (lim < ze1) {
            ze1 = lim;
        }
        if (ze1 > ze0 * 1.5f) {
            f32 a = lut[127], num = 0, dd = 0;
            for (int k = 0; k < 32; k++) {
                f32 ze = ze0 * powf(ze1 / ze0, (f32)k / 31.0f);
                f32 z = ortho ? (ze - n) / (f - n) : f * (ze - n) / (ze * (f - n));
                f32 t = (z - z0) / (1.0f - z0);
                f32 x = (ze - s) / (e - s);
                x = x < 0 ? 0 : x > 1 ? 1 : x;
                f32 fg;
                switch (fn) {
                case 4: fg = 1.0f - powf(2.0f, -8.0f * x); break;
                case 5: fg = 1.0f - powf(2.0f, -8.0f * x * x); break;
                case 6: fg = powf(2.0f, -8.0f * (1.0f - x)); break;
                case 7: fg = powf(2.0f, -8.0f * (1.0f - x) * (1.0f - x)); break;
                default: fg = x; break;
                }
                num += t * ((1.0f - fg) - a);
                dd += t * t;
            }
            if (dd > 0) {
                f32 b = a + num / dd;
                lut[128] = b < 0 ? 0 : b > 1 ? 1 : b;
            }
        }
    }
}

static int fog_valid(void) {
    return gx.fog_type != GX_FOG_NONE && gx.fog_end != gx.fog_start && gx.fog_far != gx.fog_near;
}

/* The fog amount (0 none .. 1 all fog) of the GX fog function `fn` (fog type & 7) at x = (z_eye - start) / (end - start). */
static f32 fog_amount(u8 fn, f32 x) {
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    switch (fn) {
    case 4: return 1.0f - powf(2.0f, -8.0f * x);
    case 5: return 1.0f - powf(2.0f, -8.0f * x * x);
    case 6: return powf(2.0f, -8.0f * (1.0f - x));
    case 7: return powf(2.0f, -8.0f * (1.0f - x) * (1.0f - x));
    default: return x;
    }
}

static u32 ramp_morton(u32 x, u32 y) {
    u32 r = 0;
    for (u32 i = 0; i < 3; i++) {
        r |= ((x >> i) & 1u) << (2 * i);
        r |= ((y >> i) & 1u) << (2 * i + 1);
    }
    return r;
}

/* The ramp texture of a fog function: L8, 256 x 8 (every row the same), texel i = fog_amount(i / 255). Made once. */
static u32 fog_ramp_tex(u8 fn) {
    static u32 handle[8];
    fn &= 7;
    if (handle[fn] == 0) {
        u8 tiled[256 * 8];
        for (u32 y = 0; y < 8; y++) {
            for (u32 x = 0; x < 256; x++) {
                tiled[(x >> 3) * 64 + ramp_morton(x & 7, y)] = (u8)(fog_amount(fn, (f32)x / 255.0f) * 255.0f + 0.5f);
            }
        }
        handle[fn] = plat_gpu_tex_create(256, 8, PLAT_TEXFMT_L8, tiled, sizeof(tiled));
    }
    return handle[fn];
}

/* The unit and ramp coordinate of the fog in force: with the unit's texture coordinate u = a * z_eye + b
 * (u = 0.5 / 256 at the fog start, 255.5 / 256 at its end: the centres of the first and last texel). */
static void fog_ramp_setup(void) {
    s_fog_unit = -1;
    if (!gx_opt_fogramp || s_units >= 3 || !fog_valid()) {
        return;
    }
    s_fog_tex = fog_ramp_tex(gx.fog_type & 7);
    if (s_fog_tex == 0) {
        return;
    }
    f32 inv = 1.0f / (gx.fog_end - gx.fog_start);
    s_fog_a = inv * (255.0f / 256.0f);
    s_fog_b = -gx.fog_start * inv * (255.0f / 256.0f) + 0.5f / 256.0f;
    s_fog_unit = s_units;
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
        gx_unit_of_stage[s] = -1;
        if (map >= GX_MAX_TEXMAP) {
            continue;
        }
        /* one unit per (map, texcoord): a map sampled at two coordinates takes two units */
        u8 coord = gx.tev[s].coord < GX_MAX_TEXGEN ? gx.tev[s].coord : 0;
        int u = 0;
        while (u < s_units && !(s_unit_map[u] == map && s_unit_coord[u] == coord)) {
            u++;
        }
        if (u < s_units) {
            gx_unit_of_stage[s] = u;
            continue;
        }
        if (s_units == 3) {
            static int warned;
            gx_unit_of_stage[s] = gx_unit_of_map[map]; /* units full: the map's first coordinate */
            if (gx_unit_of_stage[s] < 0 && !warned) {
                warned = 1;
                PLOG_W("GX: a TEV setup samples more than 3 textures (PICA has 3 units)");
            }
            continue;
        }
        if (gx_unit_of_map[map] < 0) {
            gx_unit_of_map[map] = s_units;
        }
        gx_unit_of_stage[s] = s_units;
        s_unit_map[s_units] = map;
        s_unit_coord[s_units] = coord;
        s_units++;
    }
    fog_ramp_setup();
    s_color_chan = chan_votes[1] > chan_votes[0] ? 1 : 0;
    if (chan_votes[0] && chan_votes[1]) {
        static int warned;
        if (!warned) {
            warned = 1;
            PLOG_W("GX: a TEV setup uses both color channels (PICA has one vertex color)");
        }
    }
}

/* The parts of build_state, by the GX state they read (switch gxdirty rebuilds only the dirty ones). */
static void build_tex_units(PlatGpuState* st) { /* the units (TEV order), the texture maps and TLUTs */
    for (int u = 0; u < 3; u++) {
        PlatGpuTexUnit* tu = &st->tex[u];
        if (u == s_fog_unit) {
            tu->handle = s_fog_tex;
            tu->wrap_s = tu->wrap_t = 0; /* clamp to edge */
            tu->min_linear = tu->mag_linear = 1;
            continue;
        }
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
}

static void build_tev(PlatGpuState* st) { /* TEV */
    if (gx_opt_statecache) {
        gx_tev_compile_memo(st);
    } else {
        gx_tev_compile(st);
    }
    if (s_fog_unit >= 0) {
        /* the fog stage; with no stage left the PICA fog does it (build_fog) */
        const GXColor fc = gx.fog_color;
        if (!gx_tev_add_fog(st, s_fog_unit, (u32)fc.r | (u32)fc.g << 8 | (u32)fc.b << 16)) {
            s_fog_unit = -1;
        }
    }
}

static void build_chan_flags(void) { /* the units (TEV order), texgens, channels */
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
}

static void build_pix(PlatGpuState* st) { /* the pixel pipeline */
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
}

static void build_sci(PlatGpuState* st) { /* scissor and viewport */
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
}

/* The whole PICA state for the GX state in force, into `st` (and the transform's flags). */
static void build_full(PlatGpuState* st) {
    memset(st, 0, sizeof(*st));
    assign_units();
    build_tev(st);
    build_tex_units(st);
    build_chan_flags();
    build_pix(st);
    build_sci(st);
}

static void build_state(void) {
    build_full(&s_state);
    plat_gpu_set_state(&s_state);
    s_built_version = gx.version;
}

/* ---- switch gxdirty: the dirty classes only, into the kept state ---- */
int gx_opt_dirty;
u32 gx_dirty = GXD_ALL;
#define PIX_FROM offsetof(PlatGpuState, alpha_test_enable)
#define PIX_TO offsetof(PlatGpuState, scissor_x0)
static struct {
    u32 builds, classes[5], checks, bad, vchecks, vbad, logged;
} s_dv;

static void build_dirty(u32 d) {
    PlatGpuState* st = &s_state;
    u32 regions = 0;
    if ((d & GXD_TEV) && gx.fog_type != GX_FOG_NONE) {
        d |= GXD_PIX; /* the fog ramp takes a stage and a unit, or the PICA fog does */
    }
    if (d & GXD_TEV) {
        assign_units();
        memset(st, 0, offsetof(PlatGpuState, tex));
        build_tev(st);
        regions |= PLAT_GPU_R_TEV;
    }
    if (d & (GXD_TEV | GXD_TEX)) {
        memset(st->tex, 0, sizeof(st->tex));
        build_tex_units(st);
        regions |= PLAT_GPU_R_TEX;
    }
    if (d & (GXD_TEV | GXD_CHAN)) {
        build_chan_flags();
    }
    if (d & GXD_PIX) {
        memset((u8*)st + PIX_FROM, 0, PIX_TO - PIX_FROM);
        build_pix(st);
        regions |= PLAT_GPU_R_PIX;
    }
    if (d & GXD_SCI) {
        build_sci(st);
        regions |= PLAT_GPU_R_SCI;
    }
    if (regions != 0) {
        plat_gpu_set_state_dirty(st, regions);
    }
    s_dv.builds++;
    for (int k = 0; k < 5; k++) {
        s_dv.classes[k] += d >> k & 1;
    }
}

/* gxdirty=2: the full build of the same GX state must give the kept state and flags. */
static void verify_dirty(u32 d) {
    int units = s_units, cchan = s_color_chan, need_both = s_need_both, need_nrm = s_need_nrm, fogu = s_fog_unit;
    f32 foga = s_fog_a, fogb = s_fog_b;
    u32 fogt = s_fog_tex;
    int ucoord[3], umap[3], uom[GX_MAX_TEXMAP], uos[GX_MAX_TEV];
    u8 unlit[2] = {s_unlit[0], s_unlit[1]}, mrgb[2] = {s_mat_vtx_rgb[0], s_mat_vtx_rgb[1]};
    u8 ma[2] = {s_mat_vtx_a[0], s_mat_vtx_a[1]};
    memcpy(ucoord, s_unit_coord, sizeof(ucoord));
    memcpy(umap, s_unit_map, sizeof(umap));
    memcpy(uom, gx_unit_of_map, sizeof(uom));
    memcpy(uos, gx_unit_of_stage, sizeof(uos));
    static PlatGpuState full;
    build_full(&full);
    u32 bad = 0;
    static const u16 k_rg[5] = {0, offsetof(PlatGpuState, tex), PIX_FROM, PIX_TO, sizeof(PlatGpuState)};
    for (int r = 0; r < 4; r++) {
        if (memcmp((const u8*)&full + k_rg[r], (const u8*)&s_state + k_rg[r], k_rg[r + 1] - k_rg[r]) != 0) {
            bad |= 1u << r;
        }
    }
    if (units != s_units || fogu != s_fog_unit || cchan != s_color_chan || memcmp(ucoord, s_unit_coord, sizeof(ucoord)) != 0 ||
        memcmp(umap, s_unit_map, sizeof(umap)) != 0 || memcmp(uom, gx_unit_of_map, sizeof(uom)) != 0 ||
        memcmp(uos, gx_unit_of_stage, sizeof(uos)) != 0) {
        bad |= 0x10; /* units */
    }
    if (need_both != s_need_both || need_nrm != s_need_nrm || memcmp(unlit, s_unlit, 2) != 0 ||
        memcmp(mrgb, s_mat_vtx_rgb, 2) != 0 || memcmp(ma, s_mat_vtx_a, 2) != 0) {
        bad |= 0x20; /* transform flags */
    }
    s_dv.checks++;
    if (bad != 0) {
        s_dv.bad++;
        if (s_dv.logged < 20) {
            s_dv.logged++;
            PLOG_W("gxdirty verify: frame %lu state mismatch, parts 0x%02lx (1 TEV, 2 TEX, 4 PIX, 8 SCI, 10 units, "
                   "20 flags), classes rebuilt 0x%02lx",
                   (unsigned long)gx_perf_frame_no(), (unsigned long)bad, (unsigned long)d);
        }
    }
    /* go on with the kept result (what gxdirty=1 draws) */
    s_units = units;
    s_fog_unit = fogu;
    s_fog_a = foga;
    s_fog_b = fogb;
    s_fog_tex = fogt;
    s_color_chan = cchan;
    s_need_both = need_both;
    s_need_nrm = need_nrm;
    memcpy(s_unit_coord, ucoord, sizeof(ucoord));
    memcpy(s_unit_map, umap, sizeof(umap));
    memcpy(gx_unit_of_map, uom, sizeof(uom));
    memcpy(gx_unit_of_stage, uos, sizeof(uos));
    memcpy(s_unlit, unlit, 2);
    memcpy(s_mat_vtx_rgb, mrgb, 2);
    memcpy(s_mat_vtx_a, ma, 2);
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
    if (s_fog_unit >= 0) {
        tcs[s_fog_unit][0] = s_fog_a * -p[2] + s_fog_b; /* the fog ramp's coordinate: p[2] is the eye z */
    }
}

/* The bounding box (GXReadBoundingBox) is made per draw: the window positions of the draw's vertices give a
 * box (BoxAcc, not clamped), and box_commit adds the part of it inside the scissor rectangle to the GX box.
 * On the GameCube only pixels that pass the scissor count, so a draw that lies outside it leaves the box as it
 * was (empty: 1023, 1023, 0, 0). Clamping each vertex to the EFB instead gave a draw right of the screen a
 * box on the last column (rsh_00_c: left = 638, and a 12 MB allocation from it). The box of a draw partly
 * inside is its vertices' box cut to the scissor: at least what the GameCube's pixels give. */
typedef struct {
    s32 x0, y0, x1, y1;
} BoxAcc;
#define BOX_ACC_EMPTY ((BoxAcc){0x7FFFFFFF, 0x7FFFFFFF, -0x7FFFFFFF - 1, -0x7FFFFFFF - 1})

static inline void acc_add(BoxAcc* a, const Out* o) {
    f32 fx = o->wx, fy = o->wy;
    if (!(fx > -1.0e6f)) fx = -1.0e6f; /* also NaN */
    if (fx > 1.0e6f) fx = 1.0e6f;
    if (!(fy > -1.0e6f)) fy = -1.0e6f;
    if (fy > 1.0e6f) fy = 1.0e6f;
    s32 x = (s32)fx, y = (s32)fy;
    if (x < a->x0) a->x0 = x;
    if (y < a->y0) a->y0 = y;
    if (x > a->x1) a->x1 = x;
    if (y > a->y1) a->y1 = y;
}

static void box_commit(u16* box, const BoxAcc* a) {
    if (a->x0 > a->x1) {
        return; /* no vertex in front of the eye */
    }
    /* the scissor rectangle as build_state makes it, inside the EFB; inclusive here */
    s32 sx0 = (s32)gx.scissor[0] - gx.scissor_off[0], sy0 = (s32)gx.scissor[1] - gx.scissor_off[1];
    s32 sx1 = sx0 + (s32)gx.scissor[2] - 1, sy1 = sy0 + (s32)gx.scissor[3] - 1;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > gx.efb_w - 1) sx1 = gx.efb_w - 1;
    if (sy1 > gx.efb_h - 1) sy1 = gx.efb_h - 1;
    if (a->x1 < sx0 || a->x0 > sx1 || a->y1 < sy0 || a->y0 > sy1 || sx1 < sx0 || sy1 < sy0) {
        return; /* the draw is outside the scissor: no pixel of it counts */
    }
    s32 x0 = a->x0 < sx0 ? sx0 : a->x0, y0 = a->y0 < sy0 ? sy0 : a->y0;
    s32 x1 = a->x1 > sx1 ? sx1 : a->x1, y1 = a->y1 > sy1 ? sy1 : a->y1;
    if (x0 < box[0]) box[0] = (u16)x0;
    if (y0 < box[1]) box[1] = (u16)y0;
    if (x1 > box[2]) box[2] = (u16)x1;
    if (y1 > box[3]) box[3] = (u16)y1;
}

/* ---- Switch gxbbox: the box of a cached list on the shader path (PORT_3DS_60FPS.md, bounding box) ----
 * exact (default): every vertex through xf_position, as above.
 * cache: the list's box is kept with its cache entry beside the position matrix, projection, viewport and EFB
 *   size it was made under (the entry's own checks cover the list and array contents); the same inputs give
 *   the same box, so a repeat costs a compare.
 * hull: the list's vertices pruned once to a superset of its model-space convex hull vertices (Akl-Toussaint:
 *   a vertex strictly inside a tetrahedron of extreme vertices is not a hull vertex); the screen box of a
 *   convex set lies at its vertices while every vertex is in front of the eye (w > 0), so the box of the kept
 *   vertices is the box of all of them. Any kept vertex behind the eye: exact.
 * corners: the 8 corners of the model-space AABB, a box that can only be larger (for measurement only).
 * gxbbox_verify=1: the exact box is made beside it and compared at each read (the gxthreadstats bbox line). */
enum { BBX_EXACT, BBX_CACHE, BBX_HULL, BBX_CORNERS };
static int s_bbx_mode, s_bbx_verify;
void** gx_bbx_hint; /* set before gx_draw_vsh: the cache entry's box record */
static u16 s_bx_ref[4]; /* gxbbox_verify: the exact box since the clear */
static int s_bx_ref_on;
static struct {
    u32 draws[8], verts[8]; /* lists: exact, cache hit, cache miss, hull, hull fallback, corners, corners fallback;
                             * immediate-mode draws (always exact) */
    u32 list_verts_hull;    /* the lists' vertices on the hull path (the ratio's denominator) */
    u32 reads, mism, shrunk, grow_max, win_reads;
    u32 tot[8];
    u32 cpu_draws, cpu_verts; /* armed draws on the CPU path (their box costs nothing extra) */
    u64 extra_area;
    u32 hulls, hull_in, hull_out;
    u64 t, t_build;
} s_bx;
enum { BX_EXACT, BX_HIT, BX_MISS, BX_HULL, BX_HULL_FB, BX_CORN, BX_CORN_FB, BX_IMM };

typedef struct {
    f32 key[31];  /* position matrix, projection, viewport, EFB size, scissor the box was made under */
    u16 box[4];
    u8 key_ok, one_mtx, hull_done, pad;
    u32 nhull, nall;
    u32* hull;    /* vertex indices */
    f32 lo[3], hi[3];
} GxBbx;

void gx_bbx_free(void* p) {
    GxBbx* b = p;
    if (b != NULL) {
        free(b->hull);
        free(b);
    }
}

static void bbx_key(f32* k, u32 pn) {
    memcpy(k, gx.pos_mtx[pn % GX_POS_MTX_COUNT], 12 * sizeof(f32));
    memcpy(k + 12, gx.proj, 7 * sizeof(f32));
    memcpy(k + 19, gx.vp, 4 * sizeof(f32));
    k[23] = gx.efb_w;
    k[24] = gx.efb_h;
    for (int i = 0; i < 4; i++) {
        k[25 + i] = (f32)gx.scissor[i];
    }
    k[29] = (f32)gx.scissor_off[0];
    k[30] = (f32)gx.scissor_off[1];
}

static void bbx_exact(u16* box, const GxVertex* v, const GxDlcDraw* d, u32 nd, u32* nv) {
    Out o;
    f32 p[3];
    for (u32 i = 0; i < nd; i++) {
        BoxAcc a = BOX_ACC_EMPTY;
        for (u32 j = d[i].first; j < d[i].first + d[i].count; j++) {
            xf_position(&v[j], &o, p);
            if (o.front) {
                acc_add(&a, &o);
            }
        }
        box_commit(box, &a);
        *nv += d[i].count;
    }
}

static f32 orient(const f32* a, const f32* b, const f32* c, const f32* p) {
    f32 ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    f32 vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    f32 wx = p[0] - a[0], wy = p[1] - a[1], wz = p[2] - a[2];
    return ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx);
}

/* p strictly inside tetrahedron t (with a margin of 1/1000 of its volume per face) */
static int in_tet(const f32* const* t, f32 vol, const f32* p) {
    f32 m = vol * 1e-3f;
    for (int f = 0; f < 4; f++) {
        const f32* q[4] = {t[0], t[1], t[2], t[3]};
        q[f] = p;
        f32 s = orient(q[0], q[1], q[2], q[3]);
        if (vol > 0 ? s <= m : s >= m) {
            return 0;
        }
    }
    return 1;
}

/* The AABB and the hull candidates of a list's vertices (once per cache entry). */
static void bbx_build(GxBbx* b, const GxVertex* v, const GxDlcDraw* d, u32 nd) {
    u64 t0 = plat_ticks();
    b->hull_done = 1;
    b->one_mtx = 1;
    u32 n = 0;
    for (int k = 0; k < 3; k++) {
        b->lo[k] = 1e30f;
        b->hi[k] = -1e30f;
    }
    u8 pn = nd ? v[d[0].first].pnmtx : 0;
    for (u32 i = 0; i < nd; i++) {
        for (u32 j = d[i].first; j < d[i].first + d[i].count; j++) {
            if (v[j].pnmtx != pn) {
                b->one_mtx = 0;
            }
            for (int k = 0; k < 3; k++) {
                if (v[j].pos[k] < b->lo[k]) b->lo[k] = v[j].pos[k];
                if (v[j].pos[k] > b->hi[k]) b->hi[k] = v[j].pos[k];
            }
        }
        n += d[i].count;
    }
    b->nall = n;
    if (s_bbx_mode != BBX_HULL || !b->one_mtx || n == 0) {
        s_bx.t_build += plat_ticks() - t0;
        return;
    }
    /* extreme vertices in the 26 directions of a cube's faces, edges and corners */
    static const s8 dir[26][3] = {
        {1, 0, 0},  {-1, 0, 0}, {0, 1, 0},   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}, {1, 1, 0},   {1, -1, 0},  {-1, 1, 0},
        {-1, -1, 0}, {1, 0, 1}, {1, 0, -1},  {-1, 0, 1}, {-1, 0, -1}, {0, 1, 1}, {0, 1, -1},  {0, -1, 1},  {0, -1, -1},
        {1, 1, 1},  {1, 1, -1}, {1, -1, 1},  {1, -1, -1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, 1}, {-1, -1, -1}};
    u32 ext[26];
    f32 best[26];
    for (int k = 0; k < 26; k++) {
        best[k] = -1e30f;
        ext[k] = d[0].first;
    }
    for (u32 i = 0; i < nd; i++) {
        for (u32 j = d[i].first; j < d[i].first + d[i].count; j++) {
            const f32* p = v[j].pos;
            for (int k = 0; k < 26; k++) {
                f32 s = dir[k][0] * p[0] + dir[k][1] * p[1] + dir[k][2] * p[2];
                if (s > best[k]) {
                    best[k] = s;
                    ext[k] = j;
                }
            }
        }
    }
    f32 c[3] = {0, 0, 0};
    for (int k = 0; k < 26; k++) {
        for (int a = 0; a < 3; a++) {
            c[a] += v[ext[k]].pos[a] * (1.0f / 26);
        }
    }
    /* the 48 triangles of the direction sphere (per octant: axis, axis + one more, corner), each a
     * tetrahedron with the centroid; all of them lie inside the hull of the extreme vertices */
    const f32* tet[48][4];
    f32 vol[48];
    int nt = 0;
    for (int o = 0; o < 8; o++) {
        int sg[3] = {o & 1 ? -1 : 1, o & 2 ? -1 : 1, o & 4 ? -1 : 1};
        int corner = -1;
        for (int k = 18; k < 26; k++) {
            if (dir[k][0] == sg[0] && dir[k][1] == sg[1] && dir[k][2] == sg[2]) corner = k;
        }
        for (int a = 0; a < 3; a++) {
            for (int bb = 0; bb < 3; bb++) {
                if (a == bb) continue;
                s8 ax[3] = {0, 0, 0}, ed[3] = {0, 0, 0};
                ax[a] = (s8)sg[a];
                ed[a] = (s8)sg[a];
                ed[bb] = (s8)sg[bb];
                int ka = -1, ke = -1;
                for (int k = 0; k < 18; k++) {
                    if (!memcmp(dir[k], ax, 3)) ka = k;
                    if (!memcmp(dir[k], ed, 3)) ke = k;
                }
                tet[nt][0] = c;
                tet[nt][1] = v[ext[ka]].pos;
                tet[nt][2] = v[ext[ke]].pos;
                tet[nt][3] = v[ext[corner]].pos;
                vol[nt] = orient(tet[nt][0], tet[nt][1], tet[nt][2], tet[nt][3]);
                if (vol[nt] != 0.0f) nt++;
            }
        }
    }
    b->hull = malloc(n * sizeof(u32));
    if (b->hull == NULL) {
        b->hull_done = 2; /* exact */
        s_bx.t_build += plat_ticks() - t0;
        return;
    }
    u32 m = 0;
    for (u32 i = 0; i < nd; i++) {
        for (u32 j = d[i].first; j < d[i].first + d[i].count; j++) {
            int inside = 0;
            for (int t = 0; t < nt && !inside; t++) {
                inside = in_tet(tet[t], vol[t], v[j].pos);
            }
            if (!inside) {
                b->hull[m++] = j;
            }
        }
    }
    b->nhull = m;
    s_bx.hulls++;
    s_bx.hull_in += n;
    s_bx.hull_out += m;
    s_bx.t_build += plat_ticks() - t0;
}

/* The box of a cached list on the shader path, in mode gxbbox; 0: the caller makes it exactly. */
static int bbx_list(void** slot, const GxVertex* v, const GxDlcDraw* d, u32 nd) {
    if (s_bbx_mode == BBX_EXACT || slot == NULL || nd == 0) {
        return 0;
    }
    GxBbx* b = *slot;
    if (b == NULL) {
        b = calloc(1, sizeof(GxBbx));
        if (b == NULL) {
            return 0;
        }
        *slot = b;
    }
    if (!b->hull_done) {
        bbx_build(b, v, d, nd);
    }
    if (!b->one_mtx) {
        return 0;
    }
    u32 pn = v[d[0].first].pnmtx;
    u64 t0 = plat_ticks();
    u32 nv = 0;
    int path;
    if (s_bbx_mode == BBX_CACHE) {
        f32 k[31];
        bbx_key(k, pn);
        if (b->key_ok && memcmp(k, b->key, sizeof(k)) == 0) {
            path = BX_HIT;
        } else {
            b->box[0] = b->box[1] = 1023;
            b->box[2] = b->box[3] = 0;
            bbx_exact(b->box, v, d, nd, &nv);
            memcpy(b->key, k, sizeof(k));
            b->key_ok = 1;
            path = BX_MISS;
        }
        if (b->box[0] < gx.bbox[0]) gx.bbox[0] = b->box[0];
        if (b->box[1] < gx.bbox[1]) gx.bbox[1] = b->box[1];
        if (b->box[2] > gx.bbox[2]) gx.bbox[2] = b->box[2];
        if (b->box[3] > gx.bbox[3]) gx.bbox[3] = b->box[3];
    } else {
        u16 box[4] = {1023, 1023, 0, 0};
        Out o;
        f32 p[3];
        int behind = 0;
        BoxAcc acc = BOX_ACC_EMPTY; /* the list's box: the scissor is applied to the whole list here */
        if (s_bbx_mode == BBX_HULL && b->hull_done == 1) {
            for (u32 i = 0; i < b->nhull && !behind; i++) {
                xf_position(&v[b->hull[i]], &o, p);
                behind = !o.front;
                acc_add(&acc, &o);
            }
            nv = b->nhull;
            path = BX_HULL;
            s_bx.list_verts_hull += b->nall;
        } else if (s_bbx_mode == BBX_CORNERS) {
            GxVertex c = v[d[0].first];
            for (int k = 0; k < 8 && !behind; k++) {
                c.pos[0] = k & 1 ? b->hi[0] : b->lo[0];
                c.pos[1] = k & 2 ? b->hi[1] : b->lo[1];
                c.pos[2] = k & 4 ? b->hi[2] : b->lo[2];
                xf_position(&c, &o, p);
                behind = !o.front;
                acc_add(&acc, &o);
            }
            nv = 8;
            path = BX_CORN;
        } else {
            behind = 1;
            path = BX_HULL;
        }
        if (behind) {
            nv = 0;
            bbx_exact(box, v, d, nd, &nv);
            path = path == BX_HULL ? BX_HULL_FB : BX_CORN_FB;
        } else {
            box_commit(box, &acc);
        }
        if (box[0] < gx.bbox[0]) gx.bbox[0] = box[0];
        if (box[1] < gx.bbox[1]) gx.bbox[1] = box[1];
        if (box[2] > gx.bbox[2]) gx.bbox[2] = box[2];
        if (box[3] > gx.bbox[3]) gx.bbox[3] = box[3];
    }
    s_bx.t += plat_ticks() - t0;
    s_bx.draws[path]++;
    s_bx.verts[path] += nv;
    if (s_bx_ref_on) {
        u32 dummy = 0;
        bbx_exact(s_bx_ref, v, d, nd, &dummy);
    }
    return 1;
}

static void gx_bbx_init(void) {
    char m[16] = "";
    plat_switch_value("gxbbox", m, sizeof(m));
    s_bbx_mode = !strcmp(m, "cache")     ? BBX_CACHE
                 : !strcmp(m, "hull")    ? BBX_HULL
                 : !strcmp(m, "corners") ? BBX_CORNERS
                                         : BBX_EXACT;
    s_bbx_verify = plat_switch_int("gxbbox_verify", 0) != 0;
    if (s_bbx_mode != BBX_EXACT || s_bbx_verify) {
        PLOG_I("gxbbox %s%s", m, s_bbx_verify ? ", verify" : "");
    }
}

static u32 box_area(const u16* b) {
    return b[2] < b[0] || b[3] < b[1] ? 0 : (u32)(b[2] - b[0] + 1) * (b[3] - b[1] + 1);
}

/* gxbbox_verify: the box against the exact one, at a read */
static void bbx_compare(void) {
    s_bx.reads++;
    if (memcmp(s_bx_ref, gx.bbox, sizeof(s_bx_ref)) == 0) {
        return;
    }
    s_bx.mism++;
    s32 out = 0, in = 0;
    s32 g[4] = {s_bx_ref[0] - gx.bbox[0], s_bx_ref[1] - gx.bbox[1], gx.bbox[2] - s_bx_ref[2], gx.bbox[3] - s_bx_ref[3]};
    for (int k = 0; k < 4; k++) {
        if (g[k] > out) out = g[k];
        if (-g[k] > in) in = -g[k];
    }
    if (in > 0) {
        s_bx.shrunk++; /* smaller than the exact box: wrong */
        if (s_bx.shrunk <= 8) {
            PLOG_W("gxbbox verify: box %u,%u %u,%u smaller than the exact %u,%u %u,%u", gx.bbox[0], gx.bbox[1],
                   gx.bbox[2], gx.bbox[3], s_bx_ref[0], s_bx_ref[1], s_bx_ref[2], s_bx_ref[3]);
        }
    }
    if ((u32)out > s_bx.grow_max) {
        s_bx.grow_max = (u32)out;
    }
    u32 a = box_area(gx.bbox), r = box_area(s_bx_ref);
    s_bx.extra_area += a > r ? a - r : 0;
}

/* The gxthreadstats bbox line (game thread; the counters are the translator's, read without a lock: a stat).
 * Per frame: reads, armed lists and their vertices, the box's time; totals: paths, hull builds, verify. */
void gx_bbox_stats_log(u32 frames) {
    u32 armed = 0, nv = 0;
    for (int k = 0; k < 8; k++) {
        armed += s_bx.draws[k];
        nv += s_bx.verts[k];
    }
    if (frames == 0 || (armed == 0 && s_bx.win_reads == 0 && s_bx.cpu_draws == 0)) {
        return;
    }
    static const char* mode[4] = {"exact", "cache", "hull", "corners"};
    u32 f = frames;
    for (int k = 0; k < 8; k++) {
        s_bx.tot[k] += s_bx.draws[k];
    }
    PLOG_I("gxthread   bbox %s: per frame %lu.%02lu reads; shader path %lu.%02lu armed draws, %lu verts, %lu us; "
           "CPU path %lu.%02lu armed draws, %lu verts",
           mode[s_bbx_mode], (unsigned long)(s_bx.win_reads / f), (unsigned long)(s_bx.win_reads * 100 / f % 100),
           (unsigned long)(armed / f), (unsigned long)(armed * 100 / f % 100), (unsigned long)(nv / f),
           (unsigned long)(s_bx.t * 1000000u / PLAT_TICKS_PER_SEC / f), (unsigned long)(s_bx.cpu_draws / f),
           (unsigned long)(s_bx.cpu_draws * 100 / f % 100), (unsigned long)(s_bx.cpu_verts / f));
    PLOG_I("gxthread   bbox (total): lists exact %lu, cache hit %lu miss %lu, hull %lu fallback %lu, corners %lu "
           "fallback %lu; immediate %lu | hull kept %lu of %lu verts (%lu lists, %lu us)",
           (unsigned long)s_bx.tot[BX_EXACT], (unsigned long)s_bx.tot[BX_HIT], (unsigned long)s_bx.tot[BX_MISS],
           (unsigned long)s_bx.tot[BX_HULL], (unsigned long)s_bx.tot[BX_HULL_FB], (unsigned long)s_bx.tot[BX_CORN],
           (unsigned long)s_bx.tot[BX_CORN_FB], (unsigned long)s_bx.tot[BX_IMM], (unsigned long)s_bx.hull_out,
           (unsigned long)s_bx.hull_in, (unsigned long)s_bx.hulls,
           (unsigned long)(s_bx.t_build * 1000000u / PLAT_TICKS_PER_SEC));
    if (s_bbx_verify) {
        PLOG_I("gxthread   bbox verify (total): reads %lu, mismatches %lu, smaller %lu, grow max %lu px, extra area %lu px",
               (unsigned long)s_bx.reads, (unsigned long)s_bx.mism, (unsigned long)s_bx.shrunk,
               (unsigned long)s_bx.grow_max, (unsigned long)s_bx.extra_area);
    }
    memset(s_bx.draws, 0, sizeof(s_bx.draws));
    memset(s_bx.verts, 0, sizeof(s_bx.verts));
    s_bx.t = 0;
    s_bx.win_reads = 0;
    s_bx.cpu_draws = s_bx.cpu_verts = 0;
}

/* Switch gxvshbbox (default 1 since eki_00's row was found to be texel ties, accepted 2026-11-23). 1: a draw made while the bounding box is armed takes the shader path like any
 * other, and the box gets the window positions of its vertices from xf_position on the CPU (the arithmetic
 * transform() uses, so the box is the same); GXReadBoundingBox disarms it (every TTYD read follows its own
 * GXClearBoundingBox). 0: armed draws stay on the CPU path; the read disarms the box there too.
 * 2: as 1, plus the old box (every draw since the clear, through transform()) kept beside it and compared
 * at each read (`gxstats` bbox line, and a warning). */
int gx_opt_vshbbox = 1;
static u16 s_bref[4];
static int s_bref_on;
static u32 s_bbox_reads, s_bbox_mism;

void gx_bbox_clear(void) {
    gx_batch_flush(); /* draws from here on count for the box */
    gx.bbox[0] = 1023;
    gx.bbox[1] = 1023;
    gx.bbox[2] = 0;
    gx.bbox[3] = 0;
    gx_bbox_active = 1;
    memcpy(s_bref, gx.bbox, sizeof(s_bref));
    s_bref_on = gx_opt_vshbbox == 2;
    memcpy(s_bx_ref, gx.bbox, sizeof(s_bx_ref));
    s_bx_ref_on = s_bbx_verify;
}

void gx_bbox_read(u16* out) {
    memcpy(out, gx.bbox, sizeof(gx.bbox));
    s_bbox_reads++;
    s_bx.win_reads++;
    if (s_bx_ref_on) {
        bbx_compare();
    }
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

/* A CPU-path draw's box: its vertices as transform() gave them, committed once per draw. */
static void bbox_draw_out(const Out* o, u32 n) {
    if (!gx_bbox_active && !s_bref_on) {
        return;
    }
    BoxAcc a = BOX_ACC_EMPTY;
    for (u32 i = 0; i < n; i++) {
        if (o[i].front) {
            acc_add(&a, &o[i]);
        }
    }
    if (gx_bbox_active) {
        box_commit(gx.bbox, &a);
        if (s_bx_ref_on) {
            box_commit(s_bx_ref, &a);
        }
    }
    if (s_bref_on) {
        box_commit(s_bref, &a);
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
    BoxAcc a = BOX_ACC_EMPTY, ar = BOX_ACC_EMPTY;
    for (u32 i = 0; i < n; i++) {
        xf_position(&v[i], &o, p);
        if (gx_bbox_active && o.front) {
            acc_add(&a, &o);
        }
        if (s_bref_on) {
            transform(&v[i], &o);
            if (o.front) {
                acc_add(&ar, &o);
            }
        }
    }
    if (gx_bbox_active) {
        box_commit(gx.bbox, &a);
        if (s_bx_ref_on) {
            box_commit(s_bx_ref, &a);
        }
    }
    if (s_bref_on) {
        box_commit(s_bref, &ar);
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
static u32 s_vsh_dirty = GXD_ALL; /* gxdirty: the classes the uniform block has not seen yet */

static void ensure_state(void) {
    if (gx_opt_dirty) {
        u32 d = gx_dirty & GXD_STATE;
        if (gx_opt_dirty == 2 && (gx.version != s_built_version || d != 0)) {
            /* where the switch-off path rebuilds: the kept state must be the full build's */
            if (d != 0) {
                build_dirty(d);
            }
            gx_dirty &= ~GXD_STATE;
            s_vsh_dirty |= d;
            s_built_version = gx.version;
            verify_dirty(d);
            return;
        }
        if (d == 0) {
            return;
        }
        u64 t0 = GX_PERF_T();
        build_dirty(d);
        gx_dirty &= ~GXD_STATE;
        s_vsh_dirty |= d;
        if (gx_perf_on > 0) {
            gx_perf.builds++;
            gx_perf.t_state += plat_ticks() - t0;
        }
        return;
    }
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

/* The fog ramp's unit on the shader path: s = a * z_eye + b is a row of the position matrix applied to the position
 * (tgsrc x = 1: the unit's source is the object position), t = 0, no divide. */
static void vsh_fog_unit(PlatGpuVshConst* c) {
    const f32(*m)[4] = gx.pos_mtx[gx.cur_pnmtx % GX_POS_MTX_COUNT];
    int u = s_fog_unit;
    for (int j = 0; j < 4; j++) {
        c->tex[3 * u][j] = -s_fog_a * m[2][j] + (j == 3 ? s_fog_b : 0.0f);
    }
    c->tgsrc[u][0] = 1.0f;
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
    if (s_fog_unit >= 0) {
        c->tex[3 * s_fog_unit + 2][3] = 1.0f;
        vsh_fog_unit(c);
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

/* ---- switch gxdirty: the uniform block by parts ----
 * colour part (clrmix, matclr, the light block, why LIT/TWOCHAN/CLR1): TEV and channel classes; when lit, also the
 *   position and normal matrix (GXD_CLIP) and whether the vertex has a normal;
 * texture part (tex, tgsrc, the unit attributes, why TGFUNC/TGSRC/TGPOST): TEV, channel (texgens) and texture
 *   matrix classes, whether the vertex has a normal, and GXD_CLIP when a texgen reads a position matrix;
 * clip part (clip, the ORTHO flag): GXD_CLIP (matrices, projection, current matrix). */
static PlatGpuVshConst s_vw;        /* the block being kept */
static u32 s_vw_why_c, s_vw_why_t;
static int s_vw_has_nrm = -1, s_vw_tex_pos;

static void vsh_part_color(PlatGpuVshConst* c, u32* why) {
    *why = 0;
    s_vsh_lit = 0;
    memset(c->clrmix, 0, sizeof(c->clrmix));
    memset(c->matclr, 0, sizeof(c->matclr));
    if (s_need_both) {
        *why |= GXV_R_TWOCHAN;
    }
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
                *why |= GXV_R_LIT;
            }
        }
        if (ch == 1 && (s_mat_vtx_rgb[1] || s_mat_vtx_a[1])) {
            *why |= GXV_R_CLR1;
        }
        const GXColor m = gx.mat[ch];
        for (int i = 0; i < 3; i++) {
            c->clrmix[i] = s_mat_vtx_rgb[ch] ? 1.0f : 0.0f;
            c->matclr[i] = s_mat_vtx_rgb[ch] ? 0.0f : (f32)(&m.r)[i];
        }
        c->clrmix[3] = s_mat_vtx_a[ch] ? 1.0f : 0.0f;
        c->matclr[3] = s_mat_vtx_a[ch] ? 0.0f : (f32)m.a;
    }
}

static void vsh_part_tex(PlatGpuVshConst* c, u32* why, int has_nrm) {
    *why = 0;
    s_vw_tex_pos = 0;
    memset(c->tex, 0, sizeof(c->tex));
    memset(c->tgsrc, 0, sizeof(c->tgsrc));
    for (int u = 0; u < 3; u++) {
        s_vsh_unit_attr[u] = 0xFF;
        c->tex[3 * u + 2][3] = 1.0f;
        if (u >= s_units) {
            continue;
        }
        const typeof(gx.texgen[0])* tg = &gx.texgen[s_unit_coord[u]];
        int proj = tg->func == GX_TG_MTX3x4;
        if (tg->func != GX_TG_MTX2x4 && !(proj && gx_opt_vshtg)) {
            *why |= GXV_R_TGFUNC;
        }
        if (tg->src >= GX_TG_TEX0 && tg->src <= GX_TG_TEX7) {
            s_vsh_unit_attr[u] = (u8)(tg->src - GX_TG_TEX0);
            c->tgsrc[u][2] = 1.0f;
        } else if (gx_opt_vshtg && tg->src == GX_TG_POS) {
            c->tgsrc[u][0] = 1.0f;
        } else if (gx_opt_vshtg && tg->src == GX_TG_NRM && has_nrm) {
            c->tgsrc[u][1] = 1.0f;
        } else {
            *why |= GXV_R_TGSRC;
        }
        if (tg->pt_mtx != GX_PTIDENTITY && tg->pt_mtx >= GX_PTTEXMTX0) {
            *why |= GXV_R_TGPOST;
        }
        s_vw_tex_pos |= tg->mtx < 30;
        const f32* m = matrix_rows(tg->mtx);
        memcpy(c->tex[3 * u], m, 16);
        memcpy(c->tex[3 * u + 1], m + 4, 16);
        if (proj) {
            memcpy(c->tex[3 * u + 2], m + 8, 16);
            c->tgsrc[u][3] = 1.0f;
        }
    }
    if (s_fog_unit >= 0) {
        c->tex[3 * s_fog_unit + 2][3] = 1.0f;
        vsh_fog_unit(c);
        s_vw_tex_pos = 1; /* a row of the position matrix */
    }
}

static void vsh_part_clip(PlatGpuVshConst* c) {
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
}

static void vsh_verify(void);

static void vsh_state_dirty(void) {
    u32 d = s_vsh_dirty | (gx_dirty & (GXD_CLIP | GXD_TEXROW));
    if (s_vsh_pn != gx.cur_pnmtx) {
        d |= GXD_CLIP;
    }
    int has_nrm = gx.vtx_desc[GX_VA_NRM] != GX_NONE || gx.vtx_desc[GX_VA_NBT] != GX_NONE;
    int nrm_changed = has_nrm != s_vw_has_nrm;
    if (d == 0 && !nrm_changed) {
        return;
    }
    s_vsh_dirty = 0;
    gx_dirty &= ~(GXD_CLIP | GXD_TEXROW);
    s_vsh_pn = gx.cur_pnmtx;
    s_vw_has_nrm = has_nrm;
    PlatGpuVshConst* c = &s_vw;
    int part_c = (d & (GXD_TEV | GXD_CHAN)) || (s_vsh_lit && ((d & GXD_CLIP) || nrm_changed));
    int part_t = (d & (GXD_TEV | GXD_CHAN | GXD_TEXROW)) || nrm_changed || (s_vw_tex_pos && (d & GXD_CLIP));
    int part_p = (d & GXD_CLIP) != 0;
    if (part_c) {
        vsh_part_color(c, &s_vw_why_c);
    }
    if (part_t) {
        vsh_part_tex(c, &s_vw_why_t, has_nrm);
    }
    if (part_p) {
        vsh_part_clip(c);
    }
    if (part_c || part_p) {
        c->flags = (gx.proj[0] == (f32)GX_PERSPECTIVE ? 0u : PLAT_VSH_ORTHO) | (s_vsh_lit ? PLAT_VSH_LIT : 0u);
    }
    s_vsh_reasons = s_vw_why_c | s_vw_why_t;
    if (part_c || part_t || part_p) {
        if (s_vsh_const_id == 0 || !gx_mem_equal(c, &s_vsh_const, sizeof(*c))) {
            s_vsh_const = *c;
            if (++s_vsh_const_id == 0) {
                s_vsh_const_id = 1;
            }
        }
    }
}

static void vsh_state(void) {
    if (gx_opt_dirty) {
        vsh_state_dirty();
        if (gx_opt_dirty == 2 && (s_vsh_ver != gx.version || s_vsh_xfv != gx_xf_version)) {
            s_vsh_ver = gx.version;
            s_vsh_xfv = gx_xf_version;
            vsh_verify();
        }
        return;
    }
    if (s_vsh_ver != gx.version || s_vsh_xfv != gx_xf_version || s_vsh_pn != gx.cur_pnmtx) {
        vsh_prepare();
        s_vsh_ver = gx.version;
        s_vsh_xfv = gx_xf_version;
        s_vsh_pn = gx.cur_pnmtx;
    }
}

/* gxdirty=2: vsh_prepare's block, reasons, unit attributes and light flag must be the kept ones. */
static void vsh_verify(void) {
    u32 reasons = s_vsh_reasons, cid = s_vsh_const_id, lid = s_vsh_light_id;
    int lit = s_vsh_lit;
    u8 ua[3] = {s_vsh_unit_attr[0], s_vsh_unit_attr[1], s_vsh_unit_attr[2]};
    PlatGpuVshConst kept = s_vsh_const;
    PlatGpuVshLight kl = s_vsh_light;
    vsh_prepare();
    u32 bad = 0;
    if (!gx_mem_equal(&kept, &s_vsh_const, sizeof(kept))) {
        bad |= 1;
    }
    if (reasons != s_vsh_reasons) {
        bad |= 2;
    }
    if (memcmp(ua, s_vsh_unit_attr, 3) != 0) {
        bad |= 4;
    }
    if (lit != s_vsh_lit || (lit && !gx_mem_equal(&kl, &s_vsh_light, sizeof(kl)))) {
        bad |= 8;
    }
    s_dv.vchecks++;
    if (bad != 0) {
        s_dv.vbad++;
        if (s_dv.logged < 20) {
            s_dv.logged++;
            PLOG_W("gxdirty verify: frame %lu uniform block mismatch, parts 0x%lx (1 block, 2 reasons, 4 units, "
                   "8 light)", (unsigned long)gx_perf_frame_no(), (unsigned long)bad);
        }
    }
    /* go on with the kept result */
    s_vsh_reasons = reasons;
    s_vsh_lit = lit;
    memcpy(s_vsh_unit_attr, ua, 3);
    s_vsh_const = kept;
    s_vsh_const_id = cid;
    s_vsh_light = kl;
    s_vsh_light_id = lid;
}

void gx_dirty_log(u32 frames) {
    if (!gx_opt_dirty || frames == 0) {
        return;
    }
    PLOG_I("gxdirty frame %lu: per frame %lu builds (TEV %lu TEX %lu PIX %lu SCI %lu CHAN %lu); verify %lu+%lu "
           "checks, %lu+%lu mismatches (total)",
           (unsigned long)gx_perf_frame_no(), (unsigned long)(s_dv.builds / frames),
           (unsigned long)(s_dv.classes[0] / frames), (unsigned long)(s_dv.classes[1] / frames),
           (unsigned long)(s_dv.classes[2] / frames), (unsigned long)(s_dv.classes[3] / frames),
           (unsigned long)(s_dv.classes[4] / frames), (unsigned long)s_dv.checks, (unsigned long)s_dv.vchecks,
           (unsigned long)s_dv.bad, (unsigned long)s_dv.vbad);
    s_dv.builds = 0;
    memset(s_dv.classes, 0, sizeof(s_dv.classes));
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
    if (gx_bbox_active) {
        u64 t0 = plat_ticks();
        bbox_shader_draw(v, n);
        s_bx.t += plat_ticks() - t0;
        s_bx.draws[BX_IMM]++;
        s_bx.verts[BX_IMM] += n;
    } else {
        bbox_shader_draw(v, n);
    }
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

GxVshLast gx_vsh_last;

int gx_opt_immdirect;

/* gximmdirect: an immediate draw routed before its vertices are decoded, as gx_draw_vertices and draw_vsh_imm route
 * it. GXI_OLD: not handled here (the caller decodes and calls gx_draw_vertices); GXI_SHADER: the caller writes the
 * ring and calls gx_draw_imm_shader; GXI_CPU: routed to the CPU (decode, gx_draw_vertices_cpu); GXI_NONE: nothing
 * to draw. ring_given: the vertices are in the game thread's chunks already (no room check in this ring). */
int gx_draw_imm_route(u8 prim, u32 n, const void* caller, int ring_given) {
    if (n == 0) {
        return GXI_NONE;
    }
    if (!gx_vsh_on || gx_vsh_noted || gx.vtx_desc[GX_VA_PNMTXIDX] != GX_NONE || gx_bbox_active || s_bref_on ||
        s_dump) {
        return GXI_OLD; /* the bounding box and the dump need the assembled vertices */
    }
    gx_batch_flush(); /* an open batch's draws come before this one */
    ensure_state();
    if (empty_target()) {
        return GXI_NONE;
    }
    vsh_state();
    u32 why = gx_vsh_imm_on ? s_vsh_reasons | gx_vsh_imm_reasons(prim, n) : GXV_R_IMM;
    if (why == 0 && !ring_given && !gx_vsh_imm_room(n)) {
        why = GXV_R_POOL;
    }
    if (!gx_vsh_route(gx.cur_pnmtx, why, caller, 0xFFFFFFFFu, n, 1)) {
        return GXI_CPU;
    }
    if (gx.cull == GX_CULL_ALL) {
        return GXI_NONE;
    }
    return GXI_SHADER;
}

void gx_draw_imm_shader(const GxVshImm* im, u32 n) {
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
}


int gx_draw_batch_begin(const u8 unit_attr[3], u32 bat, const void* list0, u32 nbytes0, u32* hint0, void** group,
                        u32* cull) {
    ensure_state();
    if (empty_target()) {
        return 0;
    }
    vsh_state();
    if (s_vsh_reasons != 0 || gx_bbox_active || s_bref_on || gx.cull == GX_CULL_ALL || s_dump ||
        unit_attr[0] != s_vsh_unit_attr[0] || unit_attr[1] != s_vsh_unit_attr[1] || unit_attr[2] != s_vsh_unit_attr[2]) {
        return 0;
    }
    void* g = gx_vsh_batch_route(gx.cur_pnmtx, bat, list0, nbytes0, hint0);
    if (g == NULL) {
        return 0;
    }
    if (s_vsh_lit) {
        plat_gpu_vsh_light(&s_vsh_light, s_vsh_light_id);
    }
    plat_gpu_vsh_const(&s_vsh_const, s_vsh_const_id);
    *group = g;
    *cull = vsh_cull();
    return 1;
}

void gx_draw_batch_end(void* group, u32 n, u32 verts, u32 idx) {
    gx_vsh_batch_count(group, n, verts, idx);
    gx_stat_draws += n;
}

int gx_draw_vsh(const GxVshStream* s, u32 entry_reason, const void* list, u32 nbytes, u32 nverts, const GxVertex* v,
                const GxDlcDraw* d, u32 nd) {
    gx_vsh_last.ok = 0;
    ensure_state();
    if (empty_target()) {
        gx_bbx_hint = NULL;
        return 1; /* the CPU path would draw nothing either */
    }
    vsh_state();
    u32 why = s == NULL ? (entry_reason ? entry_reason : GXV_R_POOL) : s_vsh_reasons;
    if (gx_bbox_active && gx_opt_vshbbox == 0) {
        why |= GXV_R_BBOX; /* GXReadBoundingBox needs the window positions on the CPU */
    }
    if (!gx_vsh_route(gx.cur_pnmtx, why, list, nbytes, nverts, 0)) {
        gx_bbx_hint = NULL;
        return 0;
    }
    void** bbx = gx_bbx_hint;
    gx_bbx_hint = NULL;
    if (gx_bbox_active || s_bref_on) {
        if (s_bref_on || !gx_bbox_active || !bbx_list(bbx, v, d, nd)) {
            u64 t0 = plat_ticks();
            for (u32 i = 0; i < nd; i++) { /* the vertices replay() would transform, in its order */
                bbox_shader_draw(v + d[i].first, d[i].count);
                s_bx.verts[BX_EXACT] += d[i].count;
            }
            s_bx.draws[BX_EXACT]++;
            s_bx.t += plat_ticks() - t0;
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
    gx_vsh_last = (GxVshLast){(u8)(!gx_bbox_active && !s_bref_on && !s_dump),
                              {s_vsh_unit_attr[0], s_vsh_unit_attr[1], s_vsh_unit_attr[2]},
                              gx_vsh_stream_base(s), s->layout, scales, vsh_cull()};
    gx_vsh_count_shader(nverts, s->nidx);
    gx_stat_draws++;
    return 1;
}

void gx_draw_vertices(u8 prim, const GxVertex* v, u32 n) {
    if (n == 0) {
        return;
    }
    gx_batch_flush(); /* an open batch's draws come before this one */
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
            gx_draw_vertices_cpu(prim, v, n);
            return;
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
    gx_draw_vertices_cpu(prim, v, n);
}

/* The CPU path of a draw routed already (gx_draw_vertices, gx_draw_imm_route). */
void gx_draw_vertices_cpu(u8 prim, const GxVertex* v, u32 n) {
    if (empty_target()) {
        return;
    }
    u64 t1 = GX_PERF_T();
    for (u32 i = 0; i < n; i++) {
        transform(&v[i], &s_out[i]);
    }
    bbox_draw_out(s_out, n);
    if (gx_bbox_active) {
        s_bx.cpu_draws++; /* the box came with the CPU transform */
        s_bx.cpu_verts += n;
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
    gx_dirty = GXD_ALL;
    s_vsh_dirty = GXD_ALL;
}

/* ---- Compiled TEV setups, kept (switch gxstatecache) ----
 * gx_tev_compile is a function of the stages in use (gx.tev[0 .. num_tev)), the four TEV registers, the four
 * konst colours, the swap table and the texture unit each stage samples (gx_unit_of_stage); the game's override
 * (gx_tev_game_override) reads a subset of the same. Its result is the combiner part of the PICA state: the six
 * stages, their count and the buffer inputs. The key holds all of those bytes and is compared in full on a hit,
 * so a reused result is the compile's result for the same input. Direct-mapped, two probes; a setup of more
 * stages than the key holds is compiled every time. gxstatecache=2 compiles every hit again and compares. */
#define TEVM_SLOTS 512
#define TEVM_KEY 320
#define TEVM_OUT offsetof(PlatGpuState, buffer_color)
typedef struct {
    u32 hash;
    u16 klen;
    u8 used, pad;
    u8 key[TEVM_KEY];
    u8 out[TEVM_OUT];
} TevMemo;
static TevMemo* s_tevm;
static u32 s_tevm_hits, s_tevm_misses, s_tevm_big, s_tevm_bad;

void gx_tev_compile_memo(PlatGpuState* st) {
    if (s_tevm == NULL) {
        s_tevm = calloc(TEVM_SLOTS, sizeof(TevMemo));
        if (s_tevm == NULL) {
            gx_tev_compile(st);
            return;
        }
    }
    u8 key[TEVM_KEY];
    u32 n = gx.num_tev ? gx.num_tev : 1;
    if (n > 16) {
        n = 16;
    }
    u32 klen = 4 + n * sizeof(gx.tev[0]) + sizeof(gx.tev_reg) + sizeof(gx.tev_kcolor) + sizeof(gx.swap_table) + n;
    if (klen > TEVM_KEY) {
        s_tevm_big++;
        gx_tev_compile(st);
        return;
    }
    u8* k = key;
    k[0] = gx.num_tev;
    k[1] = k[2] = k[3] = 0;
    k += 4;
    memcpy(k, gx.tev, n * sizeof(gx.tev[0]));
    k += n * sizeof(gx.tev[0]);
    memcpy(k, gx.tev_reg, sizeof(gx.tev_reg));
    k += sizeof(gx.tev_reg);
    memcpy(k, gx.tev_kcolor, sizeof(gx.tev_kcolor));
    k += sizeof(gx.tev_kcolor);
    memcpy(k, gx.swap_table, sizeof(gx.swap_table));
    k += sizeof(gx.swap_table);
    for (u32 i = 0; i < n; i++) {
        *k++ = (u8)gx_unit_of_stage[i];
    }
    u32 h = gx_hash32(key, klen, 0x7E5Du);
    TevMemo* m = NULL;
    for (u32 p = 0; p < 2; p++) {
        TevMemo* c = &s_tevm[(h + p * 0x9E37u) & (TEVM_SLOTS - 1)];
        if (c->used && c->hash == h && c->klen == klen && gx_mem_equal(c->key, key, klen)) {
            m = c;
            break;
        }
    }
    if (m != NULL) {
        s_tevm_hits++;
        if (gx_opt_statecache == 2) {
            PlatGpuState chk;
            memset(&chk, 0, sizeof(chk));
            gx_tev_compile(&chk);
            if (!gx_mem_equal(&chk, m->out, TEVM_OUT)) {
                if (s_tevm_bad++ == 0) {
                    PLOG_W("gxstatecache verify: a kept TEV result differs from a fresh compile (%u GX stages)", gx.num_tev);
                }
                memcpy(m->out, &chk, TEVM_OUT);
            }
        }
        memcpy(st, m->out, TEVM_OUT);
        return;
    }
    s_tevm_misses++;
    /* compiled into a clean state, so the kept bytes do not depend on what the caller's state held */
    PlatGpuState fresh;
    memset(&fresh, 0, sizeof(fresh));
    gx_tev_compile(&fresh);
    memcpy(st, &fresh, TEVM_OUT);
    m = &s_tevm[h & (TEVM_SLOTS - 1)];
    if (m->used) {
        TevMemo* alt = &s_tevm[(h + 0x9E37u) & (TEVM_SLOTS - 1)];
        if (!alt->used) {
            m = alt;
        }
    }
    m->used = 1;
    m->hash = h;
    m->klen = (u16)klen;
    memcpy(m->key, key, klen);
    memcpy(m->out, &fresh, TEVM_OUT);
}

void gx_tev_memo_log(u32 frames) {
    u32 f = frames ? frames : 1;
    PLOG_I("gxthread   statecache: TEV setups per frame: %lu reused, %lu compiled, %lu too long for the key; verify "
           "mismatches %lu (total)",
           (unsigned long)(s_tevm_hits / f), (unsigned long)(s_tevm_misses / f), (unsigned long)(s_tevm_big / f),
           (unsigned long)s_tevm_bad);
    s_tevm_hits = s_tevm_misses = s_tevm_big = 0;
}
