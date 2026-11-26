/* The vertex-shader path (switch gxvsh): GX draws transformed by the PICA200
 * instead of the CPU. Rules and acceptance tests:
 * docs/PORT_3DS_GPU_OFFLOAD.md; design: docs/PORT_3DS_GX_PERF.md, "Design:
 * the vertex-shader path against the offload rules".
 *
 * Streams. A cache entry (gx_dlcache.c) gets, once, its decoded vertices as a
 * little-endian PICA stream in linear memory: the position and each texture
 * coordinate attribute as s16 with a power-of-two scale where that represents
 * every value exactly (a fixed-point source at its own `frac`, or a float
 * source whose values all fit), otherwise as floats; the colour as u8 x 4;
 * and u16 triangle indices in the CPU path's assembly order (quad split,
 * strip parity, fan order). The scale only moves the exponent, so a vertex
 * shared by two entries reaches the shader's dp4 with the same 24-bit value
 * whatever its stream (seam rule 4). Streams live in pool segments of
 * SEG_VERTS vertices (one stride each), so draws of a pass group that follow
 * each other merge into one GPU draw; a freed range is reused only after the
 * frames in flight.
 *
 * Pass groups (rule 1: the path is chosen per group, never per draw). A group
 * is the draws of a frame that share the bits of the position matrix, the
 * projection and the viewport. A group takes the shader path only when it
 * was fully eligible for K consecutive frames (switch gxvshk, default 30),
 * and drops back to the CPU at once: in the frame an ineligible draw
 * appears, the group's later draws go to the CPU (a split, counted), and the
 * next frame starts on the CPU. Across frames a group is known by its lists
 * (and immediate draws by the code that began them): each one's record keeps
 * the streak of the group it was drawn in, and a group's path for the frame
 * is read from the record of its first eligible draw.
 *
 * Immediate mode and decoded lists. GXBegin/GXEnd draws, and lists decoded
 * rather than drawn from the cache, write their vertices to a per-frame ring
 * of chunks (gx_vsh_imm_*) and take the same group decision, so characters,
 * effects and the map share a path.
 *
 * Decals (coplanar draws under another matrix, which the group key cannot
 * see). Two shader-path draws follow the same arithmetic, so only a
 * decal-like draw (depth EQUAL, or LEQUAL with depth writes off) made on the
 * CPU path links its group with the groups drawn earlier in its viewport
 * that take or could take the shader path. Linked groups count as one at the
 * frame's end: eligible only together, with the largest streak of the set.

 * Game-agnostic: display lists, vertex state and matrices only. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_bigmem.h"
#include "gx_internal.h"
#include "plat/plat.h"

#define SEG_VERTS 16384
#define MAX_SEGS 64
#define SEG_RUNS 128
#define PENDING_MAX 2048
#define GROUPS 2048        /* per frame, power of two */
#define RECS 16384         /* list records, power of two */
#define REC_PROBE 8
#define TOUCH_MAX 16384
#define MAX_VPS 16
#define KEY_WORDS (12 + 7 + 6)

int gx_vsh_on;
int gx_vsh_noted;
static u32 s_k = 30;
static u32 s_check_at;  /* switch gxvsh_check=N: log every group of game frame N */
static int s_s16 = 1;   /* switch gxvshs16 (default 1): s16 attributes where exact; 0 floats only */
static u32 s_seg_budget; /* switch gxvshkb: linear memory for the segments */
static int s_decal_rule = 1; /* switch gxvshdecal (default 1): decal links; 0 only for measuring the path itself */

/* ---- Pool segments ---- */

typedef struct {
    u32 start, len;
} Run;

typedef struct {
    u8* base;
    u32 stride;
    u32 bump;
    u32 nruns;
    Run runs[SEG_RUNS]; /* free runs below bump, sorted by start */
} Seg;

static Seg s_seg[MAX_SEGS];
static u32 s_nseg, s_seg_bytes;
static struct {
    u16 seg;
    u32 start, len, frame;
} s_pending[PENDING_MAX];
static u32 s_npending;

static void seg_release(Seg* sg, u32 start, u32 len) {
    if (start + len == sg->bump) {
        sg->bump = start;
        /* the last free run may now touch the end */
        while (sg->nruns > 0 && sg->runs[sg->nruns - 1].start + sg->runs[sg->nruns - 1].len == sg->bump) {
            sg->bump = sg->runs[--sg->nruns].start;
        }
        return;
    }
    u32 i = 0;
    while (i < sg->nruns && sg->runs[i].start < start) {
        i++;
    }
    if (i > 0 && sg->runs[i - 1].start + sg->runs[i - 1].len == start) {
        sg->runs[i - 1].len += len;
        if (i < sg->nruns && sg->runs[i - 1].start + sg->runs[i - 1].len == sg->runs[i].start) {
            sg->runs[i - 1].len += sg->runs[i].len;
            memmove(&sg->runs[i], &sg->runs[i + 1], (sg->nruns - i - 1) * sizeof(Run));
            sg->nruns--;
        }
        return;
    }
    if (i < sg->nruns && start + len == sg->runs[i].start) {
        sg->runs[i].start = start;
        sg->runs[i].len += len;
        return;
    }
    if (sg->nruns == SEG_RUNS) {
        return; /* too fragmented: the range stays unused until the segment empties at the end */
    }
    memmove(&sg->runs[i + 1], &sg->runs[i], (sg->nruns - i) * sizeof(Run));
    sg->runs[i] = (Run){start, len};
    sg->nruns++;
}

static void pending_reap(void) {
    u32 now = plat_gpu_present_count(), k = 0;
    for (u32 i = 0; i < s_npending; i++) {
        if ((s32)(now - s_pending[i].frame) >= 0) {
            seg_release(&s_seg[s_pending[i].seg], s_pending[i].start, s_pending[i].len);
        } else {
            s_pending[k++] = s_pending[i];
        }
    }
    s_npending = k;
}

/* n vertices of `stride` bytes: segment and first vertex, or -1. */
static int seg_alloc(u32 stride, u32 n, u32* first) {
    pending_reap();
    for (u32 s = 0; s < s_nseg; s++) {
        Seg* sg = &s_seg[s];
        if (sg->stride != stride) {
            continue;
        }
        for (u32 i = 0; i < sg->nruns; i++) {
            if (sg->runs[i].len >= n) {
                *first = sg->runs[i].start;
                sg->runs[i].start += n;
                sg->runs[i].len -= n;
                if (sg->runs[i].len == 0) {
                    memmove(&sg->runs[i], &sg->runs[i + 1], (sg->nruns - i - 1) * sizeof(Run));
                    sg->nruns--;
                }
                return (int)s;
            }
        }
        if (sg->bump + n <= SEG_VERTS) {
            *first = sg->bump;
            sg->bump += n;
            return (int)s;
        }
    }
    u32 bytes = SEG_VERTS * stride;
    if (s_nseg == MAX_SEGS || s_seg_bytes + bytes > s_seg_budget) {
        return -1;
    }
    u8* base = plat_gpu_stream_alloc(bytes);
    if (base == NULL) {
        return -1;
    }
    Seg* sg = &s_seg[s_nseg];
    memset(sg, 0, sizeof(*sg));
    sg->base = base;
    sg->stride = stride;
    sg->bump = n;
    s_seg_bytes += bytes;
    *first = 0;
    return (int)s_nseg++;
}

/* ---- Counters (per frame, summed for gxstats) ---- */

static struct {
    u32 draws_vsh, verts_vsh, idx_vsh, draws_cpu_elig, draws_inelig, verts_inelig;
    u32 reason_draws[GXV_R_COUNT];
    u32 groups, groups_vsh, groups_elig, splits, switches_up, switches_down, rec_full, group_full;
    u32 decal_draws, decal_triggers, decal_frames, decal_forced;
    u32 decal_imm, decal_list_cpu, decal_vsh, trig_imm, trig_list, decal_frames_imm;
    u32 imm_vsh, imm_verts_vsh, imm_room_fail;
    u32 built, build_fail, built_s16_pos, built_verts;
    u32 build_reason[GXV_R_COUNT];
} s_st, s_sum;
static u32 s_switch_total, s_split_total, s_frames_total, s_decal_frames_total;

static void tables_alloc(void); /* s_rec, s_grp, s_touch: below */

void gx_vsh_init(void) {
    tables_alloc();
    gx_vsh_on = plat_switch_int("gxvsh", 1) != 0; /* default on: PORT_3DS_GX_PERF.md, "Round 2" */
    int k = plat_switch_int("gxvshk", 30);
    s_k = k < 1 ? 1 : (u32)k;
    s_check_at = (u32)plat_switch_int("gxvsh_check", 0);
    s_s16 = plat_switch_int("gxvshs16", 1) != 0;
    s_seg_budget = (u32)plat_switch_int("gxvshkb", 6144) * 1024u;
    s_decal_rule = plat_switch_int("gxvshdecal", 1) != 0;
    PLOG_I("gxvsh: %s, hysteresis %lu frames, s16 %d, decal links %d, segments up to %lu KB%s", gx_vsh_on ? "on" : "off",
           (unsigned long)s_k, s_s16, s_decal_rule, (unsigned long)(s_seg_budget / 1024),
           s_check_at ? ", census logged" : "");
}

/* ---- Stream build ---- */

static int frac_bits(f32 f) {
    union {
        f32 f;
        u32 u;
    } c = {f};
    u32 e = c.u >> 23 & 0xFF;
    u32 m = c.u & 0x7FFFFF;
    if (e == 0xFF) {
        return -1;
    }
    if (e == 0 && m == 0) {
        return 0;
    }
    if (e != 0) {
        m |= 0x800000;
    } else {
        e = 1;
    }
    int k = 23 - ((int)e - 127) - __builtin_ctz(m);
    return k > 0 ? k : 0;
}

static f32 pow2(int k) {
    f32 f = 1.0f;
    for (; k > 0; k--) {
        f *= 2.0f;
    }
    return f;
}

/* Every value times 2^k is an integer that fits s16. */
static int fits_s16(const GxVertex* v, u32 nv, u32 off_f32, u32 ncomp, int k) {
    f32 sc = pow2(k);
    for (u32 i = 0; i < nv; i++) {
        const f32* p = (const f32*)((const u8*)&v[i] + off_f32);
        for (u32 j = 0; j < ncomp; j++) {
            f32 x = p[j] * sc;
            if (!(x >= -32768.0f && x <= 32767.0f) || (f32)(s32)x != x) {
                return 0;
            }
        }
    }
    return 1;
}

/* The exponent k for s16 values (value = s16 x 2^-k), or -1 for floats.
 * `src_frac` >= 0 when every format the list uses has this attribute as a
 * fixed-point type with that frac: the source's own scale, exact by
 * definition (checked anyway). Otherwise the smallest k any value needs. */
static int exact_k(const GxVertex* v, u32 nv, u32 off, u32 ncomp, int src_frac) {
    if (!s_s16) {
        return -1;
    }
    if (src_frac >= 0 && src_frac <= 30 && fits_s16(v, nv, off, ncomp, src_frac)) {
        return src_frac;
    }
    int need = 0;
    for (u32 i = 0; i < nv; i++) {
        const f32* p = (const f32*)((const u8*)&v[i] + off);
        for (u32 j = 0; j < ncomp; j++) {
            int b = frac_bits(p[j]);
            if (b < 0) {
                return -1;
            }
            need = b > need ? b : need;
        }
    }
    if (need > 30 || !fits_s16(v, nv, off, ncomp, need)) {
        return -1;
    }
    return need;
}

static int fixed_type(u8 t) { return t == GX_U8 || t == GX_S8 || t == GX_U16 || t == GX_S16; }

/* The frac every used format gives attribute a, when all are fixed-point; else -1. */
static int source_frac(const GxAttrFmt (*vat)[GX_VA_MAX_ATTR], u8 fmt_mask, int a) {
    int frac = -2;
    u32 k = 0;
    for (u32 f = 0; f < GX_MAX_VTXFMT; f++) {
        if (!(fmt_mask & (1u << f))) {
            continue;
        }
        const GxAttrFmt* af = &vat[k++][a];
        if (!fixed_type(af->type)) {
            return -1;
        }
        if (frac == -2) {
            frac = af->frac;
        } else if (frac != af->frac) {
            return -1;
        }
    }
    return frac < 0 ? -1 : frac;
}

static u32 tri_count(u8 prim, u32 n) {
    switch (prim) {
    case GX_QUADS: return n / 4 * 2;
    case GX_TRIANGLES: return n / 3;
    case GX_TRIANGLESTRIP:
    case GX_TRIANGLEFAN: return n >= 3 ? n - 2 : 0;
    default: return 0;
    }
}

/* Triangle indices of one primitive of n vertices from index b, in
 * gx_draw_vertices' order (quad split, strip parity, fan order). */
static u16* emit_tris(u8 prim, u32 b, u32 n, u16* ix) {
    switch (prim) {
    case GX_QUADS:
        for (u32 q = 0; q + 3 < n; q += 4) {
            *ix++ = (u16)(b + q), *ix++ = (u16)(b + q + 1), *ix++ = (u16)(b + q + 2);
            *ix++ = (u16)(b + q), *ix++ = (u16)(b + q + 2), *ix++ = (u16)(b + q + 3);
        }
        break;
    case GX_TRIANGLES:
        for (u32 q = 0; q + 2 < n; q += 3) {
            *ix++ = (u16)(b + q), *ix++ = (u16)(b + q + 1), *ix++ = (u16)(b + q + 2);
        }
        break;
    case GX_TRIANGLESTRIP:
        for (u32 q = 0; q + 2 < n; q++) {
            if (q & 1) {
                *ix++ = (u16)(b + q + 1), *ix++ = (u16)(b + q), *ix++ = (u16)(b + q + 2);
            } else {
                *ix++ = (u16)(b + q), *ix++ = (u16)(b + q + 1), *ix++ = (u16)(b + q + 2);
            }
        }
        break;
    case GX_TRIANGLEFAN:
        for (u32 q = 1; q + 1 < n; q++) {
            *ix++ = (u16)b, *ix++ = (u16)(b + q), *ix++ = (u16)(b + q + 1);
        }
        break;
    default:
        break;
    }
    return ix;
}

static inline void put_s16(u8* p, f32 v, f32 sc) {
    s16 x = (s16)(s32)(v * sc);
    memcpy(p, &x, 2);
}

GxVshStream* gx_vsh_stream_build(const GxVertex* v, u32 nv, const GxDlcDraw* d, u32 nd, const u8* vtx_desc,
                                 const GxAttrFmt (*vat)[GX_VA_MAX_ATTR], u8 fmt_mask, u32* reason) {
    u32 why = 0;
    if (vtx_desc[GX_VA_PNMTXIDX] != GX_NONE) {
        why |= GXV_R_PNMTXIDX;
    }
    for (int t = 0; t < 8; t++) {
        if (vtx_desc[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
            why |= GXV_R_TEXMTXIDX;
        }
    }
    u32 ntri = 0;
    for (u32 i = 0; i < nd; i++) {
        if (d[i].prim != GX_QUADS && d[i].prim != GX_TRIANGLES && d[i].prim != GX_TRIANGLESTRIP &&
            d[i].prim != GX_TRIANGLEFAN) {
            why |= GXV_R_PRIM;
        }
        if (d[i].first + d[i].count > nv) {
            why |= GXV_R_PRIM; /* never: the capture made them */
        }
        ntri += tri_count(d[i].prim, d[i].count);
    }
    u8 slot_attr[3];
    u32 nslots = 0;
    u8 attr_slot[8];
    memset(attr_slot, 0xFF, sizeof(attr_slot));
    for (int t = 0; t < 8; t++) {
        if (vtx_desc[GX_VA_TEX0 + t] != GX_NONE) {
            if (nslots == 3) {
                why |= GXV_R_SLOTS;
                break;
            }
            attr_slot[t] = (u8)nslots;
            slot_attr[nslots++] = (u8)t;
        }
    }
    if (ntri == 0 || nv > SEG_VERTS) {
        why |= GXV_R_PRIM;
    }
    if (why != 0) {
        *reason = why;
        s_st.build_fail++;
        for (u32 b = 0; b < GXV_R_COUNT; b++) {
            s_st.build_reason[b] += (why >> b) & 1;
        }
        return NULL;
    }
    /* exactness */
    int posk = exact_k(v, nv, offsetof(GxVertex, pos), 3, source_frac(vat, fmt_mask, GX_VA_POS));
    int sk[3];
    u32 s16mask = 0;
    for (u32 i = 0; i < nslots; i++) {
        sk[i] = exact_k(v, nv, offsetof(GxVertex, tex) + slot_attr[i] * 2 * sizeof(f32), 2,
                        source_frac(vat, fmt_mask, GX_VA_TEX0 + slot_attr[i]));
        if (sk[i] >= 0) {
            s16mask |= 1u << i;
        }
    }
    /* the normal (float, as decoded) when the list has one: GX lighting in the shader reads it */
    u32 nrm = vtx_desc[GX_VA_NRM] != GX_NONE || vtx_desc[GX_VA_NBT] != GX_NONE;
    int32_t off[6];
    u32 stride = plat_vsh_layout_n(posk >= 0, nslots, s16mask, nrm, off);
    u32 layout = plat_vsh_layout_word_n(posk >= 0, nslots, s16mask, nrm);
    u32 nidx = ntri * 3;
    u32 sbytes = sizeof(GxVshStream) + nidx * sizeof(u16);
    GxVshStream* s = malloc(sbytes);
    if (s == NULL) {
        *reason = GXV_R_POOL;
        s_st.build_fail++;
        s_st.build_reason[6]++;
        return NULL;
    }
    u32 first;
    int seg = seg_alloc(stride, nv, &first);
    if (seg < 0) {
        free(s);
        *reason = GXV_R_POOL;
        s_st.build_fail++;
        s_st.build_reason[6]++;
        return NULL;
    }
    s->seg = (u32)seg;
    s->first = first;
    s->nverts = nv;
    s->layout = layout;
    s->posk = (u8)(posk >= 0 ? posk : 0);
    memcpy(s->attr_slot, attr_slot, sizeof(attr_slot));
    for (u32 i = 0; i < 3; i++) {
        s->slot_k[i] = (u8)(i < nslots && sk[i] >= 0 ? sk[i] : 0);
    }
    s->nidx = nidx;
    s->bytes = sbytes + nv * stride;
    /* vertices */
    u8* out = s_seg[seg].base + first * stride;
    f32 psc = pow2(posk >= 0 ? posk : 0);
    for (u32 i = 0; i < nv; i++) {
        u8* p = out + i * stride;
        if (posk >= 0) {
            for (int j = 0; j < 3; j++) {
                put_s16(p + off[0] + 2 * j, v[i].pos[j], psc);
            }
        } else {
            memcpy(p + off[0], v[i].pos, 12);
        }
        for (u32 k = 0; k < nslots; k++) {
            const f32* tc = v[i].tex[slot_attr[k]];
            if (sk[k] >= 0) {
                f32 sc = pow2(sk[k]);
                put_s16(p + off[2 + k], tc[0], sc);
                put_s16(p + off[2 + k] + 2, tc[1], sc);
            } else {
                memcpy(p + off[2 + k], tc, 8);
            }
        }
        if (nrm) {
            memcpy(p + off[5], v[i].nrm, 12);
        }
        const GXColor* c = &v[i].clr[0];
        p[off[1] + 0] = c->r;
        p[off[1] + 1] = c->g;
        p[off[1] + 2] = c->b;
        p[off[1] + 3] = c->a;
        for (u32 b = (u32)off[1] + 4; b < stride; b++) {
            p[b] = 0;
        }
    }
    plat_gpu_stream_commit(out, nv * stride);
    /* indices, in gx_draw_vertices' triangle order */
    u16* ix = s->idx;
    for (u32 i = 0; i < nd; i++) {
        ix = emit_tris(d[i].prim, first + d[i].first, d[i].count, ix);
    }
    s_st.built++;
    s_st.built_verts += nv;
    s_st.built_s16_pos += posk >= 0;
    *reason = 0;
    return s;
}

/* ---- Immediate mode (GXBegin/GXEnd): per-frame streams in a ring ----
 * Immediate vertices change every frame, so they get no cache entry: each
 * eligible draw writes its vertices (floats: the CPU path's own values, no
 * exactness question) to the current chunk of a ring, and a chunk is reused
 * only once every frame that drew from it has left the GPU (3 presented
 * frames). Consecutive draws in a chunk index from its base, so they can
 * merge like cached ones. One layout: float position, three float slots (the
 * first three texture coordinate attributes the draw has), u8 colour. */

#define IMM_CHUNKS 8
#define IMM_CHUNK_VERTS 16384
#define IMM_MAX_VERTS 4096 /* gx_pipe.c draws at most this many vertices at once */

static struct {
    u8* base;
    u32 used;
    u32 last_frame; /* presented-frame count when it was last written */
} s_imm[IMM_CHUNKS];
static int s_imm_cur = -1;
static u32 s_imm_stride, s_imm_layout;
static int32_t s_imm_off[6];
static u16 s_imm_idx[IMM_MAX_VERTS * 3 * 2];
static GxVshImm s_imm_out;

/* Why an immediate draw of `prim`, n vertices, cannot use a stream (0: it can). */
u32 gx_vsh_imm_reasons(u8 prim, u32 n) {
    u32 why = 0;
    if (gx.vtx_desc[GX_VA_PNMTXIDX] != GX_NONE) {
        why |= GXV_R_PNMTXIDX;
    }
    u32 slots = 0;
    for (int t = 0; t < 8; t++) {
        if (gx.vtx_desc[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
            why |= GXV_R_TEXMTXIDX;
        }
        slots += gx.vtx_desc[GX_VA_TEX0 + t] != GX_NONE;
    }
    if (slots > 3) {
        why |= GXV_R_SLOTS;
    }
    if (tri_count(prim, n) == 0 || n > IMM_MAX_VERTS) {
        why |= GXV_R_PRIM; /* lines, points, or nothing to draw */
    }
    return why;
}

static void imm_layout_init(void) {
    if (s_imm_stride == 0) {
        s_imm_stride = plat_vsh_layout_n(0, 3, 0, 1, s_imm_off); /* with the normal, for lit draws */
        s_imm_layout = plat_vsh_layout_word_n(0, 3, 0, 1);
    }
}

u32 gx_vsh_imm_layout(int32_t off[6]) {
    imm_layout_init();
    memcpy(off, s_imm_off, sizeof(s_imm_off));
    return s_imm_stride;
}

/* Room for n vertices in the ring (a fresh chunk when the current one is full). */
int gx_vsh_imm_room(u32 n) {
    if (s_imm_cur >= 0 && s_imm[s_imm_cur].used + n <= IMM_CHUNK_VERTS) {
        return 1;
    }
    imm_layout_init();
    u32 now = plat_gpu_present_count();
    for (int i = 0; i < IMM_CHUNKS; i++) {
        if (i == s_imm_cur) {
            continue;
        }
        if (s_imm[i].base == NULL) {
            s_imm[i].base = plat_gpu_stream_alloc(IMM_CHUNK_VERTS * s_imm_stride);
            if (s_imm[i].base == NULL) {
                break;
            }
            s_imm[i].last_frame = now - 3;
        }
        if ((s32)(now - s_imm[i].last_frame) >= 3) {
            s_imm_cur = i;
            s_imm[i].used = 0;
            return 1;
        }
    }
    s_st.imm_room_fail++;
    return 0;
}

/* The ring layout of n assembled vertices (gx_vsh_imm_build's writer; gximmdirect=2 compares with it). */
void gx_vsh_imm_write(const GxVertex* v, u32 n, const u8* vtx_desc, u8* out, u8 attr_slot[8]) {
    u8 slot_attr[3] = {0, 0, 0};
    u32 nslots = 0;
    memset(attr_slot, 0xFF, 8);
    for (int t = 0; t < 8 && nslots < 3; t++) {
        if (vtx_desc[GX_VA_TEX0 + t] != GX_NONE) {
            attr_slot[t] = (u8)nslots;
            slot_attr[nslots++] = (u8)t;
        }
    }
    for (u32 i = 0; i < n; i++) {
        u8* p = out + i * s_imm_stride;
        memcpy(p + s_imm_off[0], v[i].pos, 12);
        for (u32 k = 0; k < 3; k++) {
            if (k < nslots) {
                memcpy(p + s_imm_off[2 + k], v[i].tex[slot_attr[k]], 8);
            } else {
                memset(p + s_imm_off[2 + k], 0, 8);
            }
        }
        if (v[i].has_nrm) {
            memcpy(p + s_imm_off[5], v[i].nrm, 12);
        } else {
            static const f32 up[3] = {0, 0, 1}; /* as the CPU path lights a vertex without a normal */
            memcpy(p + s_imm_off[5], up, 12);
        }
        const GXColor* cl = &v[i].clr[0];
        p[s_imm_off[1] + 0] = cl->r;
        p[s_imm_off[1] + 1] = cl->g;
        p[s_imm_off[1] + 2] = cl->b;
        p[s_imm_off[1] + 3] = cl->a;
    }
}

u8* gx_vsh_imm_dst(void) { return s_imm[s_imm_cur].base + s_imm[s_imm_cur].used * s_imm_stride; }

/* The n vertices written at gx_vsh_imm_dst are a draw: committed, counted, indexed. */
const GxVshImm* gx_vsh_imm_done(u8 prim, u32 n, const u8 attr_slot[8]) {
    typeof(s_imm[0])* c = &s_imm[s_imm_cur];
    u32 first = c->used;
    plat_gpu_stream_commit(c->base + first * s_imm_stride, n * s_imm_stride);
    c->used += n;
    c->last_frame = plat_gpu_present_count();
    u16* end = emit_tris(prim, first, n, s_imm_idx);
    memcpy(s_imm_out.attr_slot, attr_slot, sizeof(s_imm_out.attr_slot));
    s_imm_out.base = c->base;
    s_imm_out.layout = s_imm_layout;
    s_imm_out.idx = s_imm_idx;
    s_imm_out.nidx = (u32)(end - s_imm_idx);
    s_st.imm_vsh++;
    s_st.imm_verts_vsh += n;
    return &s_imm_out;
}

/* Writes an immediate draw (room checked) and returns its stream. */
const GxVshImm* gx_vsh_imm_build(u8 prim, const GxVertex* v, u32 n) {
    u8 attr_slot[8];
    gx_vsh_imm_write(v, n, gx.vtx_desc, gx_vsh_imm_dst(), attr_slot);
    return gx_vsh_imm_done(prim, n, attr_slot);
}

/* ---- gximmdirect while recording (gxthread=1..3): the game thread's own chunks ----
 * The game thread writes a primitive's vertices at GXEnd into these, the
 * translator draws them from here (or rebuilds GxVertex from them for the CPU
 * path). A chunk is reused once every draw written to it has been translated
 * and 3 frames have been presented since the last of them, the rule of the
 * translator's own ring above. */
static struct {
    u8* base;
    u32 used;         /* game thread */
    u32 pending;      /* draws written, not yet translated (atomic) */
    u32 last_frame;   /* translator: presented-frame count when it last translated one (atomic) */
} s_gimm[IMM_CHUNKS];
static int s_gimm_cur = -1;

u8* gx_vsh_gimm_alloc(u32 n, const void** base, u32* first, u32* chunk) {
    if (n == 0 || n > IMM_MAX_VERTS) {
        return NULL;
    }
    if (s_gimm_cur < 0 || s_gimm[s_gimm_cur].used + n > IMM_CHUNK_VERTS) {
        imm_layout_init();
        u32 now = plat_gpu_present_count();
        int found = -1;
        for (int i = 0; i < IMM_CHUNKS; i++) {
            if (i == s_gimm_cur) {
                continue;
            }
            if (s_gimm[i].base == NULL) {
                s_gimm[i].base = plat_gpu_stream_alloc(IMM_CHUNK_VERTS * s_imm_stride);
                if (s_gimm[i].base == NULL) {
                    break;
                }
                __atomic_store_n(&s_gimm[i].last_frame, now - 3, __ATOMIC_RELAXED);
            }
            if (__atomic_load_n(&s_gimm[i].pending, __ATOMIC_ACQUIRE) == 0 &&
                (s32)(now - __atomic_load_n(&s_gimm[i].last_frame, __ATOMIC_RELAXED)) >= 3) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            return NULL;
        }
        s_gimm_cur = found;
        s_gimm[found].used = 0;
    }
    typeof(s_gimm[0])* c = &s_gimm[s_gimm_cur];
    *base = c->base;
    *first = c->used;
    *chunk = (u32)s_gimm_cur;
    c->used += n;
    __atomic_fetch_add(&c->pending, 1, __ATOMIC_RELAXED);
    return c->base + *first * s_imm_stride;
}

void gx_vsh_gimm_translated(u32 chunk) {
    if (chunk < IMM_CHUNKS) {
        __atomic_store_n(&s_gimm[chunk].last_frame, plat_gpu_present_count(), __ATOMIC_RELAXED);
        __atomic_fetch_sub(&s_gimm[chunk].pending, 1, __ATOMIC_RELEASE);
    }
}

/* Translator: a draw the game thread wrote to its chunks, n vertices from `first` of the chunk at `base`. */
const GxVshImm* gx_vsh_imm_at(u8 prim, const void* base, u32 first, u32 n, const u8 attr_slot[8]) {
    plat_gpu_stream_commit((const u8*)base + first * s_imm_stride, n * s_imm_stride);
    u16* end = emit_tris(prim, first, n, s_imm_idx);
    memcpy(s_imm_out.attr_slot, attr_slot, sizeof(s_imm_out.attr_slot));
    s_imm_out.base = base;
    s_imm_out.layout = s_imm_layout;
    s_imm_out.idx = s_imm_idx;
    s_imm_out.nidx = (u32)(end - s_imm_idx);
    s_st.imm_vsh++;
    s_st.imm_verts_vsh += n;
    return &s_imm_out;
}

void gx_vsh_stream_free(GxVshStream* s) {
    if (s == NULL) {
        return;
    }
    if (s_npending == PENDING_MAX) {
        plat_gpu_sync(); /* nothing in flight reads any of them now */
        u32 now = plat_gpu_present_count();
        for (u32 i = 0; i < s_npending; i++) {
            s_pending[i].frame = now;
        }
        pending_reap();
    }
    s_pending[s_npending].seg = (u16)s->seg;
    s_pending[s_npending].start = s->first;
    s_pending[s_npending].len = s->nverts;
    s_pending[s_npending].frame = plat_gpu_present_count() + 3; /* the record being written and the one rendering */
    s_npending++;
    free(s);
}

const void* gx_vsh_stream_base(const GxVshStream* s) { return s_seg[s->seg].base; }

/* ---- Pass groups and list records ---- */

typedef struct {
    const void* list;
    u32 nbytes;
    u16 streak;    /* consecutive fully eligible frames of the group it was drawn in */
    u8 decision;   /* 1: shader path this frame */
    u8 touched;
    u16 pending;   /* this frame's new streak (minimum over the groups it was in) */
    u16 bat;       /* gxbatch: the batch (index + 1) that last relied on this record; while that batch is in use the
                      record is not evicted (its last_frame is not kept up by batch draws) */
    u32 last_frame;
} Rec;

typedef struct {
    u32 hash;
    u32 key[KEY_WORDS];
    u32 first_rec;   /* record index + 1 of the group's first list, 0 none yet */
    u16 parent;      /* decal links (union-find over this frame's groups) */
    u8 used, path, elig, decal, vp, lists; /* lists: a display list drew in it */
    u32 reasons;
    u32 n_vsh, n_cpu_elig, n_inelig;
    const void* first_list;
} Group;

static Rec* s_rec; /* RECS, heap (gx_bigmem.h) */
static Group* s_grp; /* GROUPS, heap */
static u16 s_order[GROUPS]; /* this frame's groups, in order of their first draw */
static u32 s_ngroups;
static struct {
    u32 rec;
    u16 group;
} * s_touch; /* TOUCH_MAX, heap */
static void tables_alloc(void) {
    if (s_rec == NULL) {
        s_rec = gx_big_alloc("gxvsh records", sizeof(Rec) * RECS);
        s_grp = gx_big_alloc("gxvsh groups", sizeof(Group) * GROUPS);
        s_touch = gx_big_alloc("gxvsh touches", sizeof(*s_touch) * TOUCH_MAX);
    }
}
static u32 s_ntouch;
/* gxbatch: a batch drawn in a group (one touch for all its members). Members' streaks are left alone while the
 * group's set stays eligible (they are at or above K and would only grow); gx_vsh_frame zeroes them when it does
 * not. */
#define BTOUCH_MAX 2048
static struct {
    u16 bat, group;
} s_btouch[BTOUCH_MAX];
static u32 s_nbtouch;
u32 gx_vsh_dec_epoch = 1; /* bumped whenever a record's decision changes or a record is replaced */
static struct {
    f32 vp[6];
    u16 groups[GROUPS]; /* not all used: a frame's groups per viewport */
    u32 n, linked_all, linked_list; /* the groups an immediate / a list decal has linked so far */
    int root_all, root_list;
} s_vps[MAX_VPS];
static u32 s_nvps;
static int s_group_poison; /* a decal had no group slot: every group is ineligible this frame */
static Group* s_last_g;
static u32 s_last_ver = ~0u, s_last_xfv = ~0u, s_last_pn = ~0u;
static int s_decal_this_frame;
static struct {
    const void* caller;
    u32 count;
    u8 zfunc;
    u32 why; /* why the decal was on the CPU (GXV_R_*; 0: eligible, its group still counting) */
} s_trig_callers[8]; /* the code that began the immediate decals that triggered (GXBegin's caller) */

static u32 hash_words(const u32* w, u32 n) {
    u32 h = 0x811C9DC5u;
    for (u32 i = 0; i < n; i++) {
        h = (h ^ w[i]) * 0x01000193u;
        h ^= h >> 13;
    }
    return h;
}

static u32 find(u32 i) {
    while (s_grp[i].parent != i) {
        s_grp[i].parent = s_grp[s_grp[i].parent].parent;
        i = s_grp[i].parent;
    }
    return i;
}

static void unite(u32 a, u32 b) {
    a = find(a);
    b = find(b);
    if (a != b) {
        s_grp[b].parent = (u16)a;
    }
}

static int vp_index(const f32* vp) {
    for (u32 i = 0; i < s_nvps; i++) {
        if (gx_mem_equal(s_vps[i].vp, vp, sizeof(s_vps[i].vp))) {
            return (int)i;
        }
    }
    if (s_nvps == MAX_VPS) {
        return -1;
    }
    memcpy(s_vps[s_nvps].vp, vp, sizeof(s_vps[s_nvps].vp));
    s_vps[s_nvps].n = s_vps[s_nvps].linked_all = s_vps[s_nvps].linked_list = 0;
    s_vps[s_nvps].root_all = s_vps[s_nvps].root_list = -1;
    return (int)s_nvps++;
}

static Group* group_for(u32 pnmtx) {
    if (s_last_g != NULL && s_last_ver == gx.version && s_last_xfv == gx_xf_version && s_last_pn == pnmtx) {
        return s_last_g;
    }
    s_last_ver = gx.version;
    s_last_xfv = gx_xf_version;
    s_last_pn = pnmtx;
    /* the last group's key against the state in place, without building the key first */
    if (s_last_g != NULL && gx_mem_equal(s_last_g->key, gx.pos_mtx[pnmtx % GX_POS_MTX_COUNT], 48) &&
        gx_mem_equal(s_last_g->key + 12, gx.proj, 28) && gx_mem_equal(s_last_g->key + 19, gx.vp, 24)) {
        return s_last_g;
    }
    u32 key[KEY_WORDS];
    memcpy(key, gx.pos_mtx[pnmtx % GX_POS_MTX_COUNT], 48);
    memcpy(key + 12, gx.proj, 28);
    memcpy(key + 19, gx.vp, 24);
    u32 h = hash_words(key, KEY_WORDS);
    for (u32 p = 0; p < GROUPS; p++) {
        Group* g = &s_grp[(h + p) & (GROUPS - 1)];
        if (!g->used) {
            if (s_ngroups >= GROUPS / 2) {
                break; /* keep the table sparse */
            }
            memset(g, 0, sizeof(*g));
            g->used = 1;
            g->hash = h;
            memcpy(g->key, key, sizeof(key));
            g->parent = (u16)(g - s_grp);
            g->elig = 1;
            int vi = vp_index(gx.vp);
            g->vp = (u8)(vi < 0 ? 0xFF : vi);
            if (vi >= 0) {
                s_vps[vi].groups[s_vps[vi].n++] = g->parent;
            }
            s_order[s_ngroups++] = g->parent;
            s_last_g = g;
            return g;
        }
        if (g->hash == h && gx_mem_equal(g->key, key, sizeof(key))) {
            s_last_g = g;
            return g;
        }
    }
    s_st.group_full++;
    s_last_g = NULL;
    return NULL;
}

u32* gx_vsh_rec_hint; /* the caller's remembered record index for the next rec_for (a cache entry's), or NULL */

static u32 rec_for(const void* list, u32 nbytes) {
    u32* hint = gx_vsh_rec_hint;
    gx_vsh_rec_hint = NULL;
    if (hint != NULL && *hint != 0 && s_rec[*hint - 1].list == list && s_rec[*hint - 1].nbytes == nbytes) {
        s_rec[*hint - 1].last_frame = gx_perf_frame_no();
        return *hint;
    }
    u32 got = 0;
    u32 h = ((u32)(uintptr_t)list * 0x9E3779B1u ^ nbytes * 0x85EBCA77u) >> 7;
    u32 now = gx_perf_frame_no();
    u32 victim = ~0u, oldest = 0;
    for (u32 p = 0; p < REC_PROBE; p++) {
        u32 i = (h + p) & (RECS - 1);
        Rec* r = &s_rec[i];
        if (r->list == list && r->nbytes == nbytes) {
            r->last_frame = now;
            got = i + 1;
            if (hint != NULL) {
                *hint = got;
            }
            return got;
        }
        u32 age = r->list == NULL ? ~0u : now - r->last_frame;
        if (age > 2 && age >= oldest && !r->touched && !(r->bat != 0 && gx_batch_recent(r->bat))) {
            oldest = age;
            victim = i;
        }
    }
    if (victim == ~0u) {
        s_st.rec_full++;
        return 0;
    }
    Rec* r = &s_rec[victim];
    if (r->list != NULL) {
        gx_vsh_dec_epoch++; /* a record a batch may have relied on is gone */
    }
    memset(r, 0, sizeof(*r));
    r->list = list;
    r->nbytes = nbytes;
    r->last_frame = now;
    if (hint != NULL) {
        *hint = victim + 1;
    }
    return victim + 1;
}

static void touch(u32 rec, const Group* g) {
    if (rec == 0 || s_ntouch == TOUCH_MAX) {
        return;
    }
    s_touch[s_ntouch].rec = rec;
    s_touch[s_ntouch].group = (u16)(g - s_grp);
    s_ntouch++;
}

static int decal_like(void) {
    return gx.z_enable && (gx.z_func == GX_EQUAL || gx.z_func == GX_LEQUAL) && !gx.z_update;
}

/* A decal-like draw (depth EQUAL, or LEQUAL with no depth write) in group g.
 * Two shader-path draws follow the same arithmetic, whatever their matrices,
 * so only a decal drawn on the CPU path needs protecting: it links its group
 * with every group drawn earlier in its viewport that takes, or could take,
 * the shader path. Linked groups count as one at the frame's end (eligible
 * only together, one streak), so a CPU decal that stays ineligible keeps
 * them on the CPU, and an eligible one moves with them. The link acts from
 * the next frame: in the frame a CPU decal first appears, groups drawn
 * before it may already have taken the shader path. */
static void decal_link(Group* g, u32 reasons, int path, int imm) {
    s_st.decal_draws++;
    if (path) {
        s_st.decal_vsh++;
        return;
    }
    if (imm) {
        s_st.decal_imm++;
    } else {
        s_st.decal_list_cpu++;
    }
    if (g->vp == 0xFF) {
        s_group_poison = s_decal_rule;
        return;
    }
    typeof(s_vps[0])* vp = &s_vps[g->vp];
    u32 gi = (u32)(g - s_grp);
    if (vp->root_all < 0) {
        vp->root_all = (int)gi;
    }
    int trig = 0;
    for (u32 i = vp->linked_all; i < vp->n; i++) {
        u32 o = vp->groups[i];
        if (o == gi || !(s_grp[o].path || s_grp[o].elig)) {
            continue; /* ineligible groups stay on the CPU anyway (eligibility only drops in a frame) */
        }
        trig = 1;
        if (s_decal_rule) {
            unite((u32)vp->root_all, o);
        }
    }
    vp->linked_all = vp->n;
    if (s_decal_rule) {
        unite((u32)vp->root_all, gi);
    }
    if (trig) {
        s_st.decal_triggers++;
        if (imm) {
            s_st.trig_imm++;
        } else {
            s_st.trig_list++;
        }
        const void* c = imm ? gx_begin_caller : NULL;
        for (u32 k = 0; k < 8; k++) {
            if (s_trig_callers[k].caller == c || s_trig_callers[k].count == 0) {
                s_trig_callers[k].caller = c;
                s_trig_callers[k].count++;
                s_trig_callers[k].zfunc = gx.z_func;
                s_trig_callers[k].why = reasons;
                break;
            }
        }
        s_decal_this_frame |= imm ? 1 : 2;
    }
}

int gx_vsh_route(u32 pnmtx, u32 reasons, const void* list, u32 nbytes, u32 nverts, int imm) {
    Group* g = group_for(pnmtx);
    if (g == NULL) {
        s_st.draws_inelig++;
        s_st.reason_draws[14]++;
        if (decal_like()) {
            s_st.decal_draws++;
            s_group_poison = s_decal_rule;
        }
        return 0;
    }
    int path = 0;
    if (!imm) {
        g->lists = 1;
    }
    if (reasons != 0) {
        if (g->path) {
            g->path = 0; /* the rest of this frame's draws of the group go to the CPU */
            s_st.splits++;
        }
        g->elig = 0;
        g->reasons |= reasons;
        g->n_inelig++;
        s_st.draws_inelig++;
        s_st.verts_inelig += nverts;
        for (u32 b = 0; b < GXV_R_COUNT; b++) {
            s_st.reason_draws[b] += (reasons >> b) & 1;
        }
        if (list != NULL) {
            touch(rec_for(list, nbytes), g);
        }
    } else {
        u32 r = rec_for(list, nbytes);
        touch(r, g);
        if (g->first_rec == 0) {
            g->first_rec = r ? r : ~0u;
            g->first_list = list;
            /* the path for the whole frame: the record's, if nothing ineligible came first */
            g->path = (u8)(g->elig && r != 0 && s_rec[r - 1].decision);
        }
        path = g->path;
        if (path) {
            g->n_vsh++;
        } else {
            g->n_cpu_elig++;
            s_st.draws_cpu_elig++;
        }
    }
    if (decal_like()) {
        decal_link(g, reasons, path, imm);
    }
    return path;
}

void gx_vsh_count_shader(u32 nverts, u32 nidx) {
    s_st.draws_vsh++;
    s_st.verts_vsh += nverts;
    s_st.idx_vsh += nidx;
}

/* ---- gxbatch (gx_dlcache.c): one route for a prebuilt batch of cached lists ---- */

/* The record of `list` if it exists (never made or replaced here), 0 if not; *hint as in rec_for. */
static u32 rec_find(u32* hint, const void* list, u32 nbytes) {
    if (hint != NULL && *hint != 0 && s_rec[*hint - 1].list == list && s_rec[*hint - 1].nbytes == nbytes) {
        return *hint;
    }
    u32 h = ((u32)(uintptr_t)list * 0x9E3779B1u ^ nbytes * 0x85EBCA77u) >> 7;
    for (u32 p = 0; p < REC_PROBE; p++) {
        u32 i = (h + p) & (RECS - 1);
        if (s_rec[i].list == list && s_rec[i].nbytes == nbytes) {
            if (hint != NULL) {
                *hint = i + 1;
            }
            return i + 1;
        }
    }
    return 0;
}

/* Does the list's record say "shader"? Then batch `bat` (index + 1) relies on it. */
int gx_vsh_rec_decided(u32* hint, const void* list, u32 nbytes, u32 bat) {
    u32 r = rec_find(hint, list, nbytes);
    if (r == 0 || !s_rec[r - 1].decision) {
        return 0;
    }
    s_rec[r - 1].bat = (u16)bat;
    return 1;
}

/* Routes a whole batch as gx_vsh_route would route each member (all eligible, all records decided): returns
 * the group when its path this frame is the shader, and then the group is committed (its first record, the
 * batch's touch); NULL when the members must be routed one by one (nothing was changed that the per-list route
 * would not change too). */
void* gx_vsh_batch_route(u32 pnmtx, u32 bat, const void* list0, u32 nbytes0, u32* hint0) {
    Group* g = group_for(pnmtx);
    if (g == NULL || s_nbtouch == BTOUCH_MAX) {
        return NULL;
    }
    if (g->first_rec != 0 ? !g->path : !g->elig) {
        return NULL;
    }
    if (g->first_rec == 0) {
        u32 r = rec_find(hint0, list0, nbytes0);
        if (r == 0 || !s_rec[r - 1].decision) {
            return NULL;
        }
        s_rec[r - 1].last_frame = gx_perf_frame_no();
        g->first_rec = r;
        g->first_list = list0;
        g->path = 1;
    }
    g->lists = 1;
    s_btouch[s_nbtouch].bat = (u16)bat;
    s_btouch[s_nbtouch].group = (u16)(g - s_grp);
    s_nbtouch++;
    return g;
}

/* The batch drew n members (verts, idx in total) in group `group`: the per-draw counters of gx_vsh_route. */
void gx_vsh_batch_count(void* group, u32 n, u32 verts, u32 idx) {
    Group* g = group;
    g->n_vsh += n;
    s_st.draws_vsh += n;
    s_st.verts_vsh += verts;
    s_st.idx_vsh += idx;
    if (decal_like()) {
        s_st.decal_draws += n;
        s_st.decal_vsh += n;
    }
}

static const char* reason_letters = "imPTlsoBLcCfrpg";

static void census_log(void) {
    PLOG_I("gxvsh census frame %lu: %lu groups (K %lu)", (unsigned long)gx_perf_frame_no(), (unsigned long)s_ngroups,
           (unsigned long)s_k);
    for (u32 i = 0; i < s_ngroups && i < 400; i++) {
        const Group* g = &s_grp[s_order[i]];
        char rs[GXV_R_COUNT + 1];
        u32 n = 0;
        for (u32 b = 0; b < GXV_R_COUNT; b++) {
            if (g->reasons >> b & 1) {
                rs[n++] = reason_letters[b];
            }
        }
        rs[n] = 0;
        const Rec* r = g->first_rec && g->first_rec != ~0u ? &s_rec[g->first_rec - 1] : NULL;
        const f32* k = (const f32*)g->key;
        PLOG_I("gxvsh   g%lu %s elig %d set %lu streak %u vsh %lu cpu-elig %lu inelig %lu why [%s] decal-link %d "
               "list %p z %.3f..%.3f t %.1f %.1f %.1f %s",
               (unsigned long)i, g->path ? "SHADER" : "cpu", g->elig, (unsigned long)find(s_order[i]),
               r ? r->streak : 0, (unsigned long)g->n_vsh, (unsigned long)g->n_cpu_elig, (unsigned long)g->n_inelig, rs,
               find(s_order[i]) != s_order[i] || g->decal, g->first_list, (double)k[23], (double)k[24], (double)k[3],
               (double)k[7], (double)k[11], k[12] == (f32)GX_PERSPECTIVE ? "persp" : "ortho");
    }
}

void gx_vsh_frame(void) {
    s_frames_total++;
    if (s_check_at != 0 && gx_perf_frame_no() == s_check_at) {
        census_log();
    }
    /* sets of linked groups: eligible only together, one streak: the largest
     * of the set, so a group that joins an established set (a new decal over a
     * floor already on the shader) moves to the floor's path, instead of
     * pulling the floor back to the CPU for K frames */
    static u8 set_elig[GROUPS];
    static u16 set_min[GROUPS]; /* the set's streak (0xFFFF: no record in the set) */
    for (u32 i = 0; i < s_ngroups; i++) {
        u32 gi = s_order[i];
        set_elig[gi] = 1;
        set_min[gi] = 0xFFFF;
    }
    for (u32 i = 0; i < s_ngroups; i++) {
        u32 gi = s_order[i], r = find(gi);
        const Group* g = &s_grp[gi];
        if (!g->elig || s_group_poison) {
            set_elig[r] = 0;
        }
        if (g->first_rec && g->first_rec != ~0u) {
            u16 st = s_rec[g->first_rec - 1].streak;
            set_min[r] = set_min[r] == 0xFFFF || st > set_min[r] ? st : set_min[r];
        }
        s_st.groups_vsh += g->path;
        s_st.groups_elig += g->elig;
    }
    for (u32 i = 0; i < s_ngroups; i++) {
        u32 gi = s_order[i], r = find(gi);
        if (s_grp[gi].elig && !set_elig[r]) {
            s_st.decal_forced++; /* eligible itself, kept on the CPU by a linked group */
        }
    }
    /* every list a group drew: its group set's new streak (a list drawn in several sets takes the smallest) */
    for (u32 i = 0; i < s_ntouch; i++) {
        Rec* rc = &s_rec[s_touch[i].rec - 1];
        u32 r = find(s_touch[i].group);
        u32 ns = 0;
        if (set_elig[r]) {
            u32 m = set_min[r] == 0xFFFF ? rc->streak : set_min[r];
            ns = m + 1 > 0xFFFF ? 0xFFFF : m + 1;
        }
        if (!rc->touched) {
            rc->touched = 1;
            rc->pending = (u16)ns;
        } else if (ns < rc->pending) {
            rc->pending = (u16)ns;
        }
    }
    for (u32 i = 0; i < s_ntouch; i++) {
        Rec* rc = &s_rec[s_touch[i].rec - 1];
        if (!rc->touched) {
            continue;
        }
        rc->touched = 0;
        rc->streak = rc->pending;
        u8 d = rc->streak >= s_k;
        if (d != rc->decision) {
            if (d) {
                s_st.switches_up++;
            } else {
                s_st.switches_down++;
            }
            rc->decision = d;
            gx_vsh_dec_epoch++;
        }
    }
    /* gxbatch: a batch drawn in a set that did not stay eligible: its members' streaks drop to 0, as each
     * member's own touch would have made them (the minimum over the sets a list was drawn in) */
    for (u32 i = 0; i < s_nbtouch; i++) {
        if (set_elig[find(s_btouch[i].group)]) {
            continue;
        }
        const void* list;
        u32 nbytes, *hint;
        for (u32 k = 0; gx_batch_member(s_btouch[i].bat, k, &list, &nbytes, &hint); k++) {
            u32 r = rec_find(hint, list, nbytes);
            if (r == 0) {
                continue;
            }
            Rec* rc = &s_rec[r - 1];
            rc->streak = 0;
            if (rc->decision) {
                rc->decision = 0;
                s_st.switches_down++;
                gx_vsh_dec_epoch++;
            }
        }
    }
    s_nbtouch = 0;
    s_st.groups += s_ngroups;
    if (s_decal_this_frame) {
        s_st.decal_frames++;
        s_decal_frames_total++;
    }
    if (s_decal_this_frame & 1) {
        s_st.decal_frames_imm++;
    }
    s_switch_total += s_st.switches_up + s_st.switches_down;
    s_split_total += s_st.splits;
    /* clear the frame */
    for (u32 i = 0; i < s_ngroups; i++) {
        s_grp[s_order[i]].used = 0;
    }
    s_ngroups = 0;
    s_ntouch = 0;
    s_nvps = 0;
    s_group_poison = 0;
    s_decal_this_frame = 0;
    s_last_g = NULL;
    s_last_ver = ~0u;
    /* sum */
    u32* dd = (u32*)&s_sum;
    const u32* ss = (const u32*)&s_st;
    for (size_t i = 0; i < sizeof(s_st) / 4; i++) {
        dd[i] += ss[i];
    }
    memset(&s_st, 0, sizeof(s_st));
}

void gx_vsh_log(u32 frames) {
    if (!gx_vsh_on) {
        return;
    }
    u32 f = frames ? frames : 1;
    const typeof(s_sum)* a = &s_sum;
    PlatGpuStats gs;
    plat_gpu_stats(&gs);
    PLOG_I("gxstats   vsh: draws shader %lu (verts %lu, idx %lu) cpu-eligible %lu ineligible %lu (verts %lu) | groups %lu "
           "shader %lu eligible %lu | switches up %lu down %lu (total %lu) splits %lu (total %lu) | gpu vsh draws %lu "
           "(of %lu calls) consts %lu",
           (unsigned long)(a->draws_vsh / f), (unsigned long)(a->verts_vsh / f), (unsigned long)(a->idx_vsh / f),
           (unsigned long)(a->draws_cpu_elig / f), (unsigned long)(a->draws_inelig / f),
           (unsigned long)(a->verts_inelig / f), (unsigned long)(a->groups / f), (unsigned long)(a->groups_vsh / f),
           (unsigned long)(a->groups_elig / f), (unsigned long)a->switches_up, (unsigned long)a->switches_down,
           (unsigned long)s_switch_total, (unsigned long)a->splits, (unsigned long)s_split_total,
           (unsigned long)gs.vsh_draws, (unsigned long)gs.vsh_calls, (unsigned long)gs.vsh_consts);
    PLOG_I("gxstats   vsh decals%s: %lu decal-like draws/frame (on the cpu: immediate %lu, lists %lu; on the shader %lu), "
           "%lu triggers/frame (immediate %lu, lists %lu), %lu of %lu frames with a trigger (%lu by an immediate "
           "decal; total %lu of %lu), %lu groups/frame kept on the CPU by a link | immediate on the shader %lu draws "
           "%lu verts/frame, ring full %lu",
           s_decal_rule ? "" : " (links off: counted only)", (unsigned long)(a->decal_draws / f),
           (unsigned long)(a->decal_imm / f), (unsigned long)(a->decal_list_cpu / f),
           (unsigned long)(a->decal_vsh / f), (unsigned long)(a->decal_triggers / f),
           (unsigned long)(a->trig_imm / f), (unsigned long)(a->trig_list / f), (unsigned long)a->decal_frames,
           (unsigned long)f, (unsigned long)a->decal_frames_imm, (unsigned long)s_decal_frames_total, (unsigned long)s_frames_total,
           (unsigned long)(a->decal_forced / f), (unsigned long)(a->imm_vsh / f), (unsigned long)(a->imm_verts_vsh / f),
           (unsigned long)a->imm_room_fail);
    for (u32 k = 0; k < 8 && s_trig_callers[k].count; k++) {
        PLOG_I("gxstats   vsh decal trigger: CPU decal %s %p (z func %u, on the CPU for %#lx), %lu times so far",
               s_trig_callers[k].caller ? "begun from" : "from a display list", s_trig_callers[k].caller,
               s_trig_callers[k].zfunc, (unsigned long)s_trig_callers[k].why, (unsigned long)s_trig_callers[k].count);
    }
    char why[256];
    u32 n = 0;
    for (u32 b = 0; b < GXV_R_COUNT && n + 24 < sizeof(why); b++) {
        if (a->reason_draws[b] || a->build_reason[b]) {
            n += (u32)snprintf(why + n, sizeof(why) - n, " %c %lu/%lu", reason_letters[b],
                               (unsigned long)(a->reason_draws[b] / f), (unsigned long)a->build_reason[b]);
        }
    }
    why[n] = 0;
    PLOG_I("gxstats   vsh streams: built %lu (%lu verts, %lu with s16 positions) failed %lu | segments %lu (%lu KB), "
           "streams %lu KB, frees pending %lu | records full %lu groups full %lu | why (draws/frame / builds):%s",
           (unsigned long)a->built, (unsigned long)a->built_verts, (unsigned long)a->built_s16_pos,
           (unsigned long)a->build_fail, (unsigned long)s_nseg, (unsigned long)(s_seg_bytes / 1024),
           (unsigned long)gs.stream_kb, (unsigned long)s_npending, (unsigned long)a->rec_full,
           (unsigned long)a->group_full, why);
    /* since boot: CPU-path draws by reason (a census over a sweep: which features the game uses at all) */
    static u64 total[GXV_R_COUNT];
    n = 0;
    for (u32 b = 0; b < GXV_R_COUNT && n + 24 < sizeof(why); b++) {
        total[b] += a->reason_draws[b];
        if (total[b]) {
            n += (u32)snprintf(why + n, sizeof(why) - n, " %c %llu", reason_letters[b], (unsigned long long)total[b]);
        }
    }
    why[n] = 0;
    PLOG_I("gxstats   vsh census since boot, CPU-path draws by reason:%s", why);
    memset(&s_sum, 0, sizeof(s_sum));
}
