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
#include <string.h>

#include "gx_internal.h"
#include "plat/plat.h"

int gx_unit_of_map[GX_MAX_TEXMAP];
static int s_unit_coord[3];  /* GX texcoord each PICA unit samples with */
static int s_unit_map[3];
static int s_units;
static int s_color_chan;     /* 0: COLOR0A0, 1: COLOR1A1 feeds the raster color */
static u32 s_built_version = 0xFFFFFFFFu;
static PlatGpuState s_state;
u32 gx_stat_draws, gx_stat_vertices;
int gx_bbox_active;

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

static void build_fog(PlatGpuState* st) {
    u8 type = gx.fog_type;
    if (type == GX_FOG_NONE || gx.fog_end == gx.fog_start || gx.fog_far == gx.fog_near) {
        st->fog_enable = 0;
        return;
    }
    st->fog_enable = 1;
    st->fog_color = (u32)gx.fog_color.r | (u32)gx.fog_color.g << 8 | (u32)gx.fog_color.b << 16;
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
        st->fog_lut[i] = 1.0f - fog; /* PICA: 1 keeps the fragment */
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
        tu->mag_linear = t->mag_filter != GX_NEAR;
        tu->min_linear = t->min_filter == GX_LINEAR || t->min_filter == GX_LIN_MIP_NEAR || t->min_filter == GX_LIN_MIP_LIN;
        tu->lod_bias = (f32)t->lod_bias_q / 32.0f;
    }
    gx_tev_compile(st);

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

static Out s_out[4096 + 8];
static int s_dump; /* this frame's draws are logged (switch gxdump) */

static void transform(const GxVertex* v, Out* o) {
    const f32(*m)[4] = gx.pos_mtx[v->pnmtx % GX_POS_MTX_COUNT];
    f32 p[3];
    for (int r = 0; r < 3; r++) {
        p[r] = m[r][0] * v->pos[0] + m[r][1] * v->pos[1] + m[r][2] * v->pos[2] + m[r][3];
    }
    f32 n[3] = {0, 0, 1};
    if (v->has_nrm) {
        const f32(*nm)[3] = gx.nrm_mtx[v->pnmtx % GX_POS_MTX_COUNT];
        for (int r = 0; r < 3; r++) {
            n[r] = nm[r][0] * v->nrm[0] + nm[r][1] * v->nrm[1] + nm[r][2] * v->nrm[2];
        }
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
    GXColor chan_out[2];
    int need_both = 0;
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

static void bbox_add(const Out* o) {
    if (!gx_bbox_active || !o->front) {
        return;
    }
    s32 x = (s32)o->wx, y = (s32)o->wy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > gx.efb_w - 1) x = gx.efb_w - 1;
    if (y > gx.efb_h - 1) y = gx.efb_h - 1;
    if (x < gx.bbox[0]) gx.bbox[0] = (u16)x;
    if (y < gx.bbox[1]) gx.bbox[1] = (u16)y;
    if (x > gx.bbox[2]) gx.bbox[2] = (u16)x;
    if (y > gx.bbox[3]) gx.bbox[3] = (u16)y;
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

void gx_draw_vertices(u8 prim, const GxVertex* v, u32 n) {
    if (n == 0) {
        return;
    }
    if (gx.version != s_built_version) {
        build_state();
    }
    if (s_state.scissor_x1 <= s_state.scissor_x0 || s_state.scissor_y1 <= s_state.scissor_y0 ||
        gx.vp[2] == 0.0f || gx.vp[3] == 0.0f) {
        return; /* empty scissor or viewport: the PICA200 hangs on them */
    }
    for (u32 i = 0; i < n; i++) {
        transform(&v[i], &s_out[i]);
        bbox_add(&s_out[i]);
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
               "col %02x%02x%02x%02x blend %d zc %d atest %d chans %d map %d from %p",
               prim, (unsigned long)n, s_units, s_state.num_stages, gx.num_tev, t0 ? t0->image : NULL,
               t0 ? t0->format : -1, t0 ? t0->width : 0, t0 ? t0->height : 0, (double)s_out[0].v.pos[0],
               (double)s_out[0].v.pos[1], (double)s_out[0].v.pos[2], (double)s_out[0].v.pos[3], s_out[0].v.color[0],
               s_out[0].v.color[1], s_out[0].v.color[2], s_out[0].v.color[3], s_state.blend_mode,
               s_state.color_write, s_state.alpha_test_enable, gx.num_chans, s_units > 0 ? s_unit_map[0] : -1,
               gx_begin_caller);
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
    if (s_tri_n > 0) {
        plat_gpu_draw(s_tri_out, s_tri_n);
        gx_stat_draws++;
        gx_stat_vertices += s_tri_n;
    }
}

void gx_draw_frame_begin(void) {
    static u32 frame;
    static int dump_at = -1;
    if (dump_at < 0) {
        dump_at = plat_switch_int("gxdump", 0);
    }
    frame++;
    s_dump = dump_at > 0 && frame == (u32)dump_at;
    if (s_dump) {
        PLOG_I("gxdump: frame %lu", (unsigned long)frame);
    }
    gx_stat_draws = gx_stat_vertices = 0;
    s_built_version = 0xFFFFFFFFu; /* every frame starts with a full state */
}
