/* GX vertex pipe, primitives and display lists.
 *
 * Vertex data reaches the GPU three ways on the GameCube, all handled here:
 *   1. GXBegin, then GXPosition/GXColor/GXTexCoord calls or direct writes to
 *      GXWGFifo, then GXEnd. On ports each write fills one 8-byte slot of
 *      the pipe (GXVert.h); GXEnd decodes the slots with the vertex format.
 *   2. GXCallDisplayList on display lists from the disc (map meshes): the
 *      GameCube's big-endian command stream (primitive opcodes with vertex
 *      data, NOPs, register loads).
 *   3. GXCallDisplayList on display lists the game recorded between
 *      GXBeginDisplayList and GXEndDisplayList. They are written in the same
 *      GameCube encoding, so they fit the game's buffers and can be copied
 *      like the originals; GX state calls inside them are stored as port
 *      opcodes (0xC0..0xFF, unused by the GameCube) and replayed in order.
 * Raw commands written with GXCmd/GXParam outside a primitive (DEMOInit's
 * performance-counter setup) are parsed from the pipe like a display list.
 *
 * Array data (GXSetArray) is native-endian on the port: the game writes
 * its own arrays natively and the disc loaders swap file arrays at load.
 */
#include <string.h>

#include "gx_bigmem.h"
#include "gx_internal.h"
#include "plat/memstable.h"
#include "plat/plat.h"

#define PIPE_SLOTS (64 * 1024)

static PPCWGPipe* s_pipe; /* PIPE_SLOTS, heap (gx_bigmem.h); set by gx_pipe_alloc */
volatile PPCWGPipe* port_gx_wg_ptr;
volatile PPCWGPipe* port_gx_wg_end;

/* current primitive */
static int s_in_begin;
static u8 s_prim, s_fmt;
static u16 s_nverts;

/* display list recording */
static u8* s_dl_buf;
static u32 s_dl_size, s_dl_used;
static int s_dl_overflow;

#define VERTS_MAX 4096
static GxVertex* s_verts; /* VERTS_MAX, heap */

/* Lists are recorded by the game thread; a translating thread is never inside one. */
int gx_dl_recording(void) { return s_dl_buf != NULL && !gx_tls_translator; }

void gx_dl_put(const void* bytes, u32 n) {
    if (s_dl_used + n > s_dl_size) {
        s_dl_overflow = 1;
        return;
    }
    memcpy(s_dl_buf + s_dl_used, bytes, n);
    s_dl_used += n;
}

/* ---- Readers: pipe slots (native) or display-list bytes (big-endian) ---- */

typedef struct {
    const PPCWGPipe* slot; /* pipe mode */
    const u8* p;           /* byte mode */
    const u8* end;
} Reader;

static inline u32 rd_u8(Reader* r) {
    if (r->slot) {
        return (r->slot++)->u8;
    }
    if (r->p + 1 > r->end) {
        return 0;
    }
    return *r->p++;
}

static inline u32 rd_u16(Reader* r) {
    if (r->slot) {
        return (r->slot++)->u16;
    }
    if (r->p + 2 > r->end) {
        r->p = r->end;
        return 0;
    }
    u32 v = (u32)r->p[0] << 8 | r->p[1];
    r->p += 2;
    return v;
}

static inline u32 rd_u32(Reader* r) {
    if (r->slot) {
        return (r->slot++)->u32;
    }
    if (r->p + 4 > r->end) {
        r->p = r->end;
        return 0;
    }
    u32 v = (u32)r->p[0] << 24 | (u32)r->p[1] << 16 | (u32)r->p[2] << 8 | r->p[3];
    r->p += 4;
    return v;
}

static inline f32 rd_f32(Reader* r) {
    if (r->slot) {
        return (r->slot++)->f32;
    }
    union {
        u32 u;
        f32 f;
    } c;
    c.u = rd_u32(r);
    return c.f;
}

/* One numeric component of `type` (GX_U8..GX_F32), dequantized by frac. */
static inline f32 rd_comp(Reader* r, u8 type, f32 scale) {
    switch (type) {
    case GX_U8: return (f32)(u8)rd_u8(r) * scale;
    case GX_S8: return (f32)(s8)rd_u8(r) * scale;
    case GX_U16: return (f32)(u16)rd_u16(r) * scale;
    case GX_S16: return (f32)(s16)rd_u16(r) * scale;
    default: return rd_f32(r);
    }
}

/* The same from native array memory. */
static inline f32 mem_comp(const u8* p, u8 type, f32 scale, u32 i) {
    switch (type) {
    case GX_U8: return (f32)((const u8*)p)[i] * scale;
    case GX_S8: return (f32)((const s8*)p)[i] * scale;
    case GX_U16: return (f32)((const u16*)p)[i] * scale;
    case GX_S16: return (f32)((const s16*)p)[i] * scale;
    default: return ((const f32*)p)[i];
    }
}

static GXColor color_from_bits(u8 type, u32 v) {
    GXColor c;
    switch (type) {
    case GX_RGB565:
        c.r = (u8)((v >> 11 & 31) * 255 / 31);
        c.g = (u8)((v >> 5 & 63) * 255 / 63);
        c.b = (u8)((v & 31) * 255 / 31);
        c.a = 255;
        break;
    case GX_RGB8:
    case GX_RGBX8:
        c.r = (u8)(v >> 24);
        c.g = (u8)(v >> 16);
        c.b = (u8)(v >> 8);
        c.a = 255;
        break;
    case GX_RGBA4:
        c.r = (u8)((v >> 12 & 15) * 17);
        c.g = (u8)((v >> 8 & 15) * 17);
        c.b = (u8)((v >> 4 & 15) * 17);
        c.a = (u8)((v & 15) * 17);
        break;
    case GX_RGBA6:
        c.r = (u8)((v >> 18 & 63) * 255 / 63);
        c.g = (u8)((v >> 12 & 63) * 255 / 63);
        c.b = (u8)((v >> 6 & 63) * 255 / 63);
        c.a = (u8)((v & 63) * 255 / 63);
        break;
    default: /* RGBA8 */
        c.r = (u8)(v >> 24);
        c.g = (u8)(v >> 16);
        c.b = (u8)(v >> 8);
        c.a = (u8)v;
        break;
    }
    return c;
}

/* Direct color: the component layout depends on the type. */
static GXColor rd_color(Reader* r, u8 type) {
    switch (type) {
    case GX_RGB565:
    case GX_RGBA4: return color_from_bits(type, rd_u16(r));
    case GX_RGB8: {
        /* three byte writes (GXColor3u8) or bytes in a list */
        u32 v = rd_u8(r) << 24;
        v |= rd_u8(r) << 16;
        v |= rd_u8(r) << 8;
        return color_from_bits(type, v);
    }
    case GX_RGBA6: {
        u32 v = rd_u8(r) << 16;
        v |= rd_u8(r) << 8;
        v |= rd_u8(r);
        return color_from_bits(type, v);
    }
    default: {
        /* RGBX8/RGBA8: four bytes (GXColor4u8; the port's GXColor1u32 writes
         * four byte slots too) */
        u32 v = rd_u8(r) << 24;
        v |= rd_u8(r) << 16;
        v |= rd_u8(r) << 8;
        v |= rd_u8(r);
        return color_from_bits(type, v);
    }
    }
}

static GXColor mem_color(const u8* p, u8 type) {
    /* Arrays are native memory written by the game (or swapped at load):
     * byte-ordered formats are plain bytes, 16-bit formats native u16. */
    switch (type) {
    case GX_RGB565:
    case GX_RGBA4: return color_from_bits(type, *(const u16*)p);
    case GX_RGB8:
    case GX_RGBX8:
    case GX_RGBA8: return color_from_bits(type, (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
    case GX_RGBA6: return color_from_bits(type, (u32)p[0] << 16 | (u32)p[1] << 8 | p[2]);
    default: return color_from_bits(type, 0xFFFFFFFFu);
    }
}

static inline u32 rd_index(Reader* r, u8 type) { return type == GX_INDEX8 ? rd_u8(r) : rd_u16(r); }

static inline f32 frac_scale(u8 frac) { return 1.0f / (f32)(1u << (frac & 31)); }

/* Decodes one vertex in format `fmt` under vertex state g. */
static inline __attribute__((always_inline)) void read_vertex_g(Reader* r, u8 fmt, GxVertex* v, const GxState* g) {
    const GxAttrFmt* vat = g->vat[fmt];
    v->pnmtx = (u8)g->cur_pnmtx;
    memcpy(v->texmtx, g->cur_texmtx, sizeof(v->texmtx));
    v->has_nrm = 0;
    v->clr[0] = v->clr[1] = (GXColor){255, 255, 255, 255};

    if (g->vtx_desc[GX_VA_PNMTXIDX] != GX_NONE) {
        v->pnmtx = (u8)(rd_u8(r) / 3);
    }
    for (int t = 0; t < 8; t++) {
        if (g->vtx_desc[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
            v->texmtx[t] = (u8)rd_u8(r);
        }
    }
    /* position */
    {
        u8 d = g->vtx_desc[GX_VA_POS];
        const GxAttrFmt* f = &vat[GX_VA_POS];
        int n = f->cnt == GX_POS_XY ? 2 : 3;
        f32 sc = f->type == GX_F32 ? 1.0f : frac_scale(f->frac);
        v->pos[2] = 0.0f;
        if (d == GX_DIRECT) {
            for (int i = 0; i < n; i++) {
                v->pos[i] = rd_comp(r, f->type, sc);
            }
        } else if (d != GX_NONE) {
            u32 idx = rd_index(r, d);
            if (gx_dlc_capturing) {
                gx_dlc_track(GX_VA_POS, idx);
            }
            const u8* p = g->array_base[GX_VA_POS] + idx * g->array_stride[GX_VA_POS];
            for (int i = 0; i < n; i++) {
                v->pos[i] = mem_comp(p, f->type, sc, (u32)i);
            }
        }
    }
    /* normal (NBT: normal, binormal, tangent; only the normal is used) */
    {
        u8 d = g->vtx_desc[GX_VA_NRM] != GX_NONE ? g->vtx_desc[GX_VA_NRM] : g->vtx_desc[GX_VA_NBT];
        if (d != GX_NONE) {
            const GxAttrFmt* f = &vat[GX_VA_NRM];
            int vecs = f->cnt == GX_NRM_XYZ ? 1 : 3;
            f32 sc = f->type == GX_S8 ? 1.0f / 64.0f : f->type == GX_S16 ? 1.0f / 16384.0f : 1.0f;
            v->has_nrm = 1;
            if (d == GX_DIRECT) {
                for (int k = 0; k < vecs; k++) {
                    f32 c[3];
                    for (int i = 0; i < 3; i++) {
                        c[i] = rd_comp(r, f->type, sc);
                    }
                    if (k == 0) {
                        memcpy(v->nrm, c, sizeof(c));
                    }
                }
            } else {
                int per_vec_index = f->cnt == GX_NRM_NBT3;
                u32 idx = rd_index(r, d);
                if (gx_dlc_capturing) {
                    gx_dlc_track(GX_VA_NRM, idx); /* only the first vector is read from the array */
                }
                for (int k = 0; k < vecs; k++) {
                    if (k > 0 && per_vec_index) {
                        idx = rd_index(r, d);
                    }
                    const u8* p = g->array_base[GX_VA_NRM] + idx * g->array_stride[GX_VA_NRM];
                    if (k == 0) {
                        for (int i = 0; i < 3; i++) {
                            v->nrm[i] = mem_comp(p, f->type, sc, (u32)i);
                        }
                    }
                }
            }
        }
    }
    /* colors */
    for (int c = 0; c < 2; c++) {
        u8 d = g->vtx_desc[GX_VA_CLR0 + c];
        const GxAttrFmt* f = &vat[GX_VA_CLR0 + c];
        if (d == GX_DIRECT) {
            v->clr[c] = rd_color(r, f->type);
        } else if (d != GX_NONE) {
            u32 idx = rd_index(r, d);
            if (gx_dlc_capturing) {
                gx_dlc_track(GX_VA_CLR0 + c, idx);
            }
            v->clr[c] = mem_color(g->array_base[GX_VA_CLR0 + c] + idx * g->array_stride[GX_VA_CLR0 + c], f->type);
        }
        if (f->cnt == GX_CLR_RGB) {
            v->clr[c].a = 255;
        }
    }
    /* texture coordinates */
    for (int t = 0; t < 8; t++) {
        u8 d = g->vtx_desc[GX_VA_TEX0 + t];
        if (d == GX_NONE) {
            v->tex[t][0] = v->tex[t][1] = 0.0f;
            continue;
        }
        const GxAttrFmt* f = &vat[GX_VA_TEX0 + t];
        int n = f->cnt == GX_TEX_S ? 1 : 2;
        f32 sc = f->type == GX_F32 ? 1.0f : frac_scale(f->frac);
        v->tex[t][1] = 0.0f;
        if (d == GX_DIRECT) {
            for (int i = 0; i < n; i++) {
                v->tex[t][i] = rd_comp(r, f->type, sc);
            }
        } else {
            u32 idx = rd_index(r, d);
            if (gx_dlc_capturing) {
                gx_dlc_track(GX_VA_TEX0 + t, idx);
            }
            const u8* p = g->array_base[GX_VA_TEX0 + t] + idx * g->array_stride[GX_VA_TEX0 + t];
            for (int i = 0; i < n; i++) {
                v->tex[t][i] = mem_comp(p, f->type, sc, (u32)i);
            }
        }
    }
}

static void read_vertex(Reader* r, u8 fmt, GxVertex* v) { read_vertex_g(r, fmt, v, &gx); }

/* ---- Switch gximmdirect: pipe writes straight into the ring's PICA layout ----
 * A plan of steps is built once per primitive from the vertex description and
 * format, then run per vertex over the slots. Each step reads what read_vertex
 * reads and converts it with the same expressions, so the bytes equal those
 * gx_vsh_imm_write makes from read_vertex's GxVertex (gximmdirect=2 checks). */
enum { IS_SKIP, IS_COMP, IS_COMP_IDX, IS_CLR, IS_CLR_IDX };
typedef struct {
    u8 op;
    u8 type;  /* GXCompType, or the colour format */
    u8 n;     /* components (IS_COMP*), slots skipped (IS_SKIP) */
    u8 idx16; /* the index is u16 */
    s16 dst;  /* byte offset in the vertex */
    u8 rgb;   /* colour: alpha forced to 255 */
    u8 pad;
    f32 sc;
    const u8* base;
    u32 stride;
} ImmStep;

static u32 s_imm_stride_c;
static int32_t s_imm_off_c[6];

static inline u32 slot_index(const PPCWGPipe** sl, int idx16) {
    const PPCWGPipe* p = (*sl)++;
    return idx16 ? p->u16 : p->u8;
}

static inline f32 slot_comp(const PPCWGPipe* p, u8 type, f32 scale) {
    switch (type) {
    case GX_U8: return (f32)(u8)p->u8 * scale;
    case GX_S8: return (f32)(s8)p->u8 * scale;
    case GX_U16: return (f32)(u16)p->u16 * scale;
    case GX_S16: return (f32)(s16)p->u16 * scale;
    default: return p->f32;
    }
}

static inline void put_color(u8* o, GXColor c, int rgb) {
    o[0] = c.r;
    o[1] = c.g;
    o[2] = c.b;
    o[3] = rgb ? 255 : c.a;
}

static void imm_layout_c(void) {
    if (s_imm_stride_c == 0) {
        s_imm_stride_c = gx_vsh_imm_layout(s_imm_off_c);
    }
}

int gx_imm_convert(const GxState* g, u8 fmt, const PPCWGPipe* sl, u32 n, u8* out, u8 attr_slot[8]) {
    imm_layout_c();
    const u32 stride = s_imm_stride_c;
    const int32_t* off = s_imm_off_c;
    const u8* d = g->vtx_desc;
    const GxAttrFmt* vat = g->vat[fmt];
    ImmStep st[24];
    u32 ns = 0;
    /* the vertex every step writes into: white colour, zero texture slots, the normal (0, 0, 1) when there is none */
    u8 tmpl[64];
    memset(tmpl, 0, sizeof(tmpl));
    tmpl[off[1]] = tmpl[off[1] + 1] = tmpl[off[1] + 2] = tmpl[off[1] + 3] = 255;
    u32 skip = 0;
#define FLUSH_SKIP()                                                                                                   \
    do {                                                                                                               \
        if (skip != 0) {                                                                                               \
            st[ns++] = (ImmStep){IS_SKIP, 0, (u8)skip, 0, 0, 0, 0, 0.0f, NULL, 0};                                     \
            skip = 0;                                                                                                  \
        }                                                                                                              \
    } while (0)
    if (d[GX_VA_PNMTXIDX] != GX_NONE) {
        skip++;
    }
    for (int t = 0; t < 8; t++) {
        skip += d[GX_VA_TEX0MTXIDX + t] != GX_NONE;
    }
    /* position */
    {
        u8 dp = d[GX_VA_POS];
        const GxAttrFmt* f = &vat[GX_VA_POS];
        u8 cnt = f->cnt == GX_POS_XY ? 2 : 3;
        f32 sc = f->type == GX_F32 ? 1.0f : frac_scale(f->frac);
        if (dp == GX_DIRECT) {
            FLUSH_SKIP();
            st[ns++] = (ImmStep){IS_COMP, f->type, cnt, 0, (s16)off[0], 0, 0, sc, NULL, 0};
        } else if (dp != GX_NONE) {
            FLUSH_SKIP();
            st[ns++] = (ImmStep){IS_COMP_IDX, f->type,   cnt, dp != GX_INDEX8, (s16)off[0], 0, 0, sc,
                                 g->array_base[GX_VA_POS], g->array_stride[GX_VA_POS]};
        }
    }
    /* normal (only the first vector of NBT is used) */
    {
        u8 dn = d[GX_VA_NRM] != GX_NONE ? d[GX_VA_NRM] : d[GX_VA_NBT];
        if (dn != GX_NONE) {
            const GxAttrFmt* f = &vat[GX_VA_NRM];
            u32 vecs = f->cnt == GX_NRM_XYZ ? 1 : 3;
            f32 sc = f->type == GX_S8 ? 1.0f / 64.0f : f->type == GX_S16 ? 1.0f / 16384.0f : 1.0f;
            FLUSH_SKIP();
            if (dn == GX_DIRECT) {
                st[ns++] = (ImmStep){IS_COMP, f->type, 3, 0, (s16)off[5], 0, 0, sc, NULL, 0};
                skip += (vecs - 1) * 3;
            } else {
                st[ns++] = (ImmStep){IS_COMP_IDX, f->type,   3, dn != GX_INDEX8, (s16)off[5], 0, 0, sc,
                                     g->array_base[GX_VA_NRM], g->array_stride[GX_VA_NRM]};
                skip += f->cnt == GX_NRM_NBT3 ? 2 : 0;
            }
        } else {
            static const f32 up[3] = {0, 0, 1}; /* as the CPU path lights a vertex without a normal */
            memcpy(tmpl + off[5], up, 12);
        }
    }
    /* colours: 0 into the vertex, 1 read and dropped (the shader path has one colour) */
    for (int c = 0; c < 2; c++) {
        u8 dc = d[GX_VA_CLR0 + c];
        const GxAttrFmt* f = &vat[GX_VA_CLR0 + c];
        if (dc == GX_NONE) {
            continue;
        }
        if (c == 1) {
            u8 ty = f->type;
            skip += dc != GX_DIRECT ? 1u : ty == GX_RGB565 || ty == GX_RGBA4 ? 1u : ty == GX_RGB8 || ty == GX_RGBA6 ? 3u : 4u;
            continue;
        }
        FLUSH_SKIP();
        st[ns++] = (ImmStep){dc == GX_DIRECT ? IS_CLR : IS_CLR_IDX,
                             f->type,
                             0,
                             dc != GX_INDEX8,
                             (s16)off[1],
                             f->cnt == GX_CLR_RGB,
                             0,
                             0.0f,
                             g->array_base[GX_VA_CLR0],
                             g->array_stride[GX_VA_CLR0]};
    }
    /* texture coordinates: the first three present attributes take slots 0..2 */
    memset(attr_slot, 0xFF, 8);
    u32 nslots = 0;
    for (int t = 0; t < 8; t++) {
        u8 dt = d[GX_VA_TEX0 + t];
        if (dt == GX_NONE) {
            continue;
        }
        const GxAttrFmt* f = &vat[GX_VA_TEX0 + t];
        u8 cnt = f->cnt == GX_TEX_S ? 1 : 2;
        if (nslots == 3) {
            skip += dt == GX_DIRECT ? cnt : 1u; /* a fourth: never drawn from the ring (GXV_R_SLOTS) */
            continue;
        }
        attr_slot[t] = (u8)nslots;
        f32 sc = f->type == GX_F32 ? 1.0f : frac_scale(f->frac);
        FLUSH_SKIP();
        if (dt == GX_DIRECT) {
            st[ns++] = (ImmStep){IS_COMP, f->type, cnt, 0, (s16)off[2 + nslots], 0, 0, sc, NULL, 0};
        } else {
            st[ns++] = (ImmStep){IS_COMP_IDX, f->type,   cnt, dt != GX_INDEX8, (s16)off[2 + nslots], 0, 0, sc,
                                 g->array_base[GX_VA_TEX0 + t], g->array_stride[GX_VA_TEX0 + t]};
        }
        nslots++;
    }
    FLUSH_SKIP();
#undef FLUSH_SKIP
    for (u32 v = 0; v < n; v++) {
        u8* o = out + v * stride;
        memcpy(o, tmpl, stride);
        for (u32 k = 0; k < ns; k++) {
            const ImmStep* p = &st[k];
            switch (p->op) {
            case IS_SKIP: sl += p->n; break;
            case IS_COMP: {
                f32 c[3];
                for (u32 i = 0; i < p->n; i++) {
                    c[i] = slot_comp(sl++, p->type, p->sc);
                }
                memcpy(o + p->dst, c, p->n * sizeof(f32));
                break;
            }
            case IS_COMP_IDX: {
                u32 i_ = slot_index(&sl, p->idx16);
                const u8* a = p->base + i_ * p->stride;
                f32 c[3];
                for (u32 i = 0; i < p->n; i++) {
                    c[i] = mem_comp(a, p->type, p->sc, i);
                }
                memcpy(o + p->dst, c, p->n * sizeof(f32));
                break;
            }
            case IS_CLR: {
                Reader r = {sl, NULL, NULL};
                GXColor c = rd_color(&r, p->type);
                sl = r.slot;
                put_color(o + p->dst, c, p->rgb);
                break;
            }
            default: { /* IS_CLR_IDX */
                u32 i_ = slot_index(&sl, p->idx16);
                put_color(o + p->dst, mem_color(p->base + i_ * p->stride, p->type), p->rgb);
                break;
            }
            }
        }
    }
    return 1;
}

/* Whether the game thread can write a recorded primitive to its chunks (gx_rec.c): what the ring holds must
 * rebuild the GxVertex read_vertex would make, for the draws the translator routes to the CPU. */
int gx_imm_rec_ok(const GxState* g, u8 prim, u32 n) {
    const u8* d = g->vtx_desc;
    if (!gx_vsh_on || !gx_vsh_imm_on || n == 0 || n > VERTS_MAX || d[GX_VA_PNMTXIDX] != GX_NONE ||
        d[GX_VA_CLR1] != GX_NONE || d[GX_VA_POS] == GX_NONE) {
        return 0;
    }
    if (prim != GX_QUADS && prim != GX_TRIANGLES && prim != GX_TRIANGLESTRIP && prim != GX_TRIANGLEFAN) {
        return 0;
    }
    u32 slots = 0;
    for (int t = 0; t < 8; t++) {
        if (d[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
            return 0;
        }
        slots += d[GX_VA_TEX0 + t] != GX_NONE;
    }
    return slots <= 3;
}

/* counters (switch gximmdirect; the gxthreadstats line) */
static struct {
    u32 shader, cpu, old, none, rec, rec_fail, verify, mism, state_diff;
} s_id;
/* gximmdirect=2: the reference decode and bytes, one set per thread (the game thread's GXEnd while recording, the
 * translating thread) */
static u8* s_id_ref_t[2];
static GxVertex* s_id_verts_t[2];

/* gximmdirect=2: decodes the primitive the old way (read_vertex under g) and compares the ring bytes. */
void gx_imm_verify(const GxState* g, u8 fmt, const PPCWGPipe* sl, u32 n, const u8* got, const u8 attr_slot[8]) {
    int t = gx_tls_translator != 0;
    if (s_id_ref_t[t] == NULL) {
        s_id_ref_t[t] = gx_big_alloc("gximmdirect verify", (u32)VERTS_MAX * 64u);
        s_id_verts_t[t] = gx_big_alloc("gximmdirect verify vertices", sizeof(GxVertex) * VERTS_MAX);
    }
    u8* s_id_ref = s_id_ref_t[t];
    GxVertex* s_id_verts = s_id_verts_t[t];
    Reader r = {sl, NULL, NULL};
    for (u32 i = 0; i < n; i++) {
        read_vertex_g(&r, fmt, &s_id_verts[i], g);
    }
    u8 slots[8];
    gx_vsh_imm_write(s_id_verts, n, g->vtx_desc, s_id_ref, slots);
    u32 bytes = n * s_imm_stride_c;
    s_id.verify++;
    if (!gx_mem_equal(s_id_ref, got, bytes) || memcmp(slots, attr_slot, 8) != 0) {
        u32 at = 0;
        while (at < bytes && s_id_ref[at] == got[at]) {
            at++;
        }
        if (s_id.mism++ < 8) {
            PLOG_W("gximmdirect verify: primitive of %lu vertices (fmt %u, from %p) differs at vertex %lu byte %lu: "
                   "%02x, decoded %02x",
                   (unsigned long)n, fmt, gx_begin_caller, (unsigned long)(at / s_imm_stride_c),
                   (unsigned long)(at % s_imm_stride_c), at < bytes ? got[at] : 0u, at < bytes ? s_id_ref[at] : 0u);
        }
    }
}

/* The immediate primitive in pipe slots `sl` (gxthread=0, the split's direct part, a recorded GXR_PRIM). 0: the
 * caller takes the GxVertex path. */
static int imm_direct(const PPCWGPipe* sl, u8 prim, u8 fmt, u32 n) {
    int k = gx_draw_imm_route(prim, n, gx_begin_caller, 0);
    switch (k) {
    case GXI_SHADER: {
        u8 attr_slot[8];
        u8* dst = gx_vsh_imm_dst();
        gx_imm_convert(&gx, fmt, sl, n, dst, attr_slot);
        if (gx_opt_immdirect == 2) {
            gx_imm_verify(&gx, fmt, sl, n, dst, attr_slot);
        }
        gx_draw_imm_shader(gx_vsh_imm_done(prim, n, attr_slot), n);
        s_id.shader++;
        return 1;
    }
    case GXI_CPU: {
        Reader r = {sl, NULL, NULL};
        for (u32 i = 0; i < n; i++) {
            read_vertex(&r, fmt, &s_verts[i]);
        }
        gx_draw_vertices_cpu(prim, s_verts, n);
        s_id.cpu++;
        return 1;
    }
    case GXI_NONE: s_id.none++; return 1;
    default: s_id.old++; return 0;
    }
}

/* Translator: a primitive the game thread wrote to its chunks at GXEnd (gx_rec.c, GXR_PRIMD). */
void gx_draw_prim_ring(u8 primfmt, u16 n, const void* caller, const u8* base, u32 first, const u8 attr_slot[8],
                       int has_nrm, int state_same) {
    u8 prim = (u8)(primfmt & 0xF8);
    gx_begin_caller = (void*)caller;
    if (!state_same) {
        s_id.state_diff++; /* the translator's vertex state is not the shadow's (never seen): drawn as written */
    }
    if (gx_perf_on > 0) {
        gx_perf.imm_verts += n;
        gx_perf.imm_prims++;
    }
    int k = gx_draw_imm_route(prim, n, caller, 1);
    if (k == GXI_SHADER) {
        gx_draw_imm_shader(gx_vsh_imm_at(prim, base, first, n, attr_slot), n);
        s_id.shader++;
        return;
    }
    if (k == GXI_NONE) {
        s_id.none++;
        return;
    }
    /* the CPU path: the GxVertex read_vertex makes, rebuilt from the ring (no colour 1, no matrix indices) */
    imm_layout_c();
    const int32_t* off = s_imm_off_c;
    const u8* src = base + first * s_imm_stride_c;
    for (u32 i = 0; i < n; i++, src += s_imm_stride_c) {
        GxVertex* v = &s_verts[i];
        v->pnmtx = (u8)gx.cur_pnmtx;
        memcpy(v->texmtx, gx.cur_texmtx, sizeof(v->texmtx));
        memcpy(v->pos, src + off[0], 12);
        v->has_nrm = (u8)has_nrm;
        if (has_nrm) {
            memcpy(v->nrm, src + off[5], 12);
        }
        v->clr[0] = (GXColor){src[off[1]], src[off[1] + 1], src[off[1] + 2], src[off[1] + 3]};
        v->clr[1] = (GXColor){255, 255, 255, 255};
        for (int t = 0; t < 8; t++) {
            if (attr_slot[t] < 3) {
                memcpy(v->tex[t], src + off[2 + attr_slot[t]], 8);
            } else {
                v->tex[t][0] = v->tex[t][1] = 0.0f;
            }
        }
    }
    if (k == GXI_CPU) {
        gx_draw_vertices_cpu(prim, s_verts, n);
        s_id.cpu++;
    } else {
        gx_draw_vertices(prim, s_verts, n);
        s_id.old++;
    }
}

/* Game thread, recording: a primitive written at GXEnd (1) or recorded the old way (0). */
void gx_imm_rec_count(int ok) {
    if (ok) {
        s_id.rec++;
    } else {
        s_id.rec_fail++;
    }
}

void gx_immdirect_log(u32 frames) {
    if (!gx_opt_immdirect) {
        return;
    }
    u32 f = frames ? frames : 1;
    PLOG_I("gximmdirect frame %lu: per frame %lu to the ring, %lu routed to the cpu, %lu the old way, %lu nothing to "
           "draw | recorded at GXEnd %lu, the old way %lu | verify %lu checks, %lu mismatches, %lu vertex-state "
           "differences (total)",
           (unsigned long)gx_perf_frame_no(), (unsigned long)(s_id.shader / f), (unsigned long)(s_id.cpu / f),
           (unsigned long)(s_id.old / f), (unsigned long)(s_id.none / f), (unsigned long)(s_id.rec / f),
           (unsigned long)(s_id.rec_fail / f), (unsigned long)s_id.verify, (unsigned long)s_id.mism,
           (unsigned long)s_id.state_diff);
    s_id.shader = s_id.cpu = s_id.old = s_id.none = s_id.rec = s_id.rec_fail = 0;
}

/* ---- Encoding a primitive into a recorded display list ---- */

static int s_measure; /* encode_vertex only advances the reader (slot count check) */

static void put_u8(u32 v) {
    if (s_measure) {
        return;
    }
    u8 b = (u8)v;
    gx_dl_put(&b, 1);
}
static void put_u16(u32 v) {
    if (s_measure) {
        return;
    }
    u8 b[2] = {(u8)(v >> 8), (u8)v};
    gx_dl_put(b, 2);
}
static void put_u32(u32 v) {
    if (s_measure) {
        return;
    }
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    gx_dl_put(b, 4);
}

static void copy_comp(Reader* r, u8 type) {
    switch (type) {
    case GX_U8:
    case GX_S8: put_u8(rd_u8(r)); break;
    case GX_U16:
    case GX_S16: put_u16(rd_u16(r)); break;
    default: {
        union {
            f32 f;
            u32 u;
        } c;
        c.f = rd_f32(r);
        put_u32(c.u);
        break;
    }
    }
}

static void copy_index(Reader* r, u8 d) {
    if (d == GX_INDEX8) {
        put_u8(rd_u8(r));
    } else {
        put_u16(rd_u16(r));
    }
}

/* Re-encodes one vertex from pipe slots into GameCube bytes. */
static void encode_vertex(Reader* r, u8 fmt) {
    const GxState* g = gx_g(); /* the game thread records lists: its view of the vertex state */
    const GxAttrFmt* vat = g->vat[fmt];
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7MTXIDX; a++) {
        if (g->vtx_desc[a] != GX_NONE) {
            put_u8(rd_u8(r));
        }
    }
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        u8 d = g->vtx_desc[a];
        if (a == GX_VA_NRM && d == GX_NONE) {
            d = g->vtx_desc[GX_VA_NBT];
        }
        if (d == GX_NONE) {
            continue;
        }
        const GxAttrFmt* f = &vat[a];
        if (d != GX_DIRECT) {
            int reps = a == GX_VA_NRM && f->cnt == GX_NRM_NBT3 ? 3 : 1;
            for (int k = 0; k < reps; k++) {
                copy_index(r, d);
            }
            continue;
        }
        int n;
        switch (a) {
        case GX_VA_POS: n = f->cnt == GX_POS_XY ? 2 : 3; break;
        case GX_VA_NRM: n = f->cnt == GX_NRM_XYZ ? 3 : 9; break;
        case GX_VA_CLR0:
        case GX_VA_CLR1: {
            switch (f->type) {
            case GX_RGB565:
            case GX_RGBA4: put_u16(rd_u16(r)); break;
            case GX_RGB8:
            case GX_RGBA6:
                put_u8(rd_u8(r));
                put_u8(rd_u8(r));
                put_u8(rd_u8(r));
                break;
            default:
                put_u8(rd_u8(r));
                put_u8(rd_u8(r));
                put_u8(rd_u8(r));
                put_u8(rd_u8(r));
                break;
            }
            continue;
        }
        default: n = f->cnt == GX_TEX_S ? 1 : 2; break;
        }
        for (int i = 0; i < n; i++) {
            copy_comp(r, f->type);
        }
    }
}

/* ---- Primitives ---- */

static void run_pending_raw(void);

/* A primitive the game never closed with GXEnd. On the GameCube GXEnd
 * writes nothing (it is only a debug check), so retail code that leaves it
 * out still draws: the GPU takes the vertex count from GXBegin. Here the
 * vertices are decoded at GXEnd, so the next GX call closes the primitive
 * first. The decomp has no GXEnd calls where MWCC inlined the empty
 * function away, so this is the normal case. Logged once per caller
 * (debug level). */
int gx_prim_open;

void gx_end_open_primitive(const void* caller) {
    static const void* s_seen[16];
    int known = 0;
    for (int i = 0; i < 16 && s_seen[i] != NULL; i++) {
        known |= s_seen[i] == caller;
    }
    if (!known) {
        for (int i = 0; i < 16; i++) {
            if (s_seen[i] == NULL) {
                s_seen[i] = caller;
                break;
            }
        }
        PLOG_D("GX: a primitive closed without GXEnd (next GX call from %p)", caller);
    }
    GXEnd();
}

void* gx_begin_caller; /* the code that began the primitive being drawn (the translator's), for diagnostics */
static void* s_begin_ra; /* the game thread's: the code that began the open primitive */

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    if (s_in_begin) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    run_pending_raw();
    s_begin_ra = __builtin_return_address(0);
    s_in_begin = 1;
    gx_prim_open = 1;
    s_prim = (u8)type;
    s_fmt = (u8)vtxfmt;
    s_nverts = nverts;
    port_gx_wg_ptr = s_pipe;
}

static int s_in_list; /* a GXCallDisplayList is running (for the counters) */
int gx_pipe_in_list(void) { return s_in_list; }
static u32 s_list_kind; /* what the running list held: 1 primitives, 2 port opcodes, 4 raw primitives */

static void draw_from_reader(Reader* r, u8 prim, u8 fmt, u32 n) {
    u32 done = 0;
    if (gx_perf_on > 0) {
        if (s_in_list) {
            gx_perf.dl_verts += n;
        } else {
            gx_perf.imm_verts += n;
        }
    }
    if (gx_opt_immdirect && !s_in_list && r->slot != NULL && n <= VERTS_MAX && !gx_dlc_capturing &&
        imm_direct(r->slot, prim, fmt, n)) {
        return; /* immediate mode written straight into the ring (or routed to the CPU first) */
    }
    while (done < n) {
        u32 chunk = n - done;
        if (chunk > VERTS_MAX) {
            /* split on a primitive boundary */
            u32 max = VERTS_MAX;
            switch (prim) {
            case GX_QUADS: chunk = max & ~3u; break;
            case GX_TRIANGLES: chunk = max - max % 3; break;
            case GX_LINES: chunk = max & ~1u; break;
            case GX_POINTS: chunk = max; break;
            default: plat_fatal("GX: strip or fan of %lu vertices", (unsigned long)n);
            }
        }
        /* a display list being cached decodes straight into the capture */
        GxVertex* dst = s_verts;
        if (gx_dlc_capturing) {
            GxVertex* c = gx_dlc_cap_reserve(fmt, chunk);
            if (c != NULL) {
                dst = c;
            }
        }
        u64 t0 = GX_PERF_T();
        for (u32 i = 0; i < chunk; i++) {
            read_vertex(r, fmt, &dst[i]);
        }
        if (gx_perf_on > 0) {
            u64 dt = plat_ticks() - t0;
            if (s_in_list) {
                gx_perf.t_dl_decode += dt;
            } else {
                gx_perf.t_imm_decode += dt;
            }
        }
        gx_draw_vertices(prim, dst, chunk);
        if (gx_dlc_capturing) {
            gx_dlc_cap_draw(prim, dst, chunk);
        }
        done += chunk;
    }
}

void GXEnd(void) {
    if (!s_in_begin) {
        plat_fatal("GXEnd without GXBegin");
    }
    s_in_begin = 0;
    gx_prim_open = 0;
    Reader r = {s_pipe, NULL, NULL};
    if (gx_dl_recording()) {
        /* The GameCube records the written bytes and decodes them with the
         * vertex description in force when the list is called. The port
         * re-encodes the vertices with the description in force now, which
         * gives the same bytes when the writes fill it exactly; otherwise
         * (the game set the description up later) it keeps the raw writes
         * for the call to decode. */
        u32 written = (u32)((const PPCWGPipe*)port_gx_wg_ptr - s_pipe);
        Reader m = {s_pipe, NULL, NULL};
        s_measure = 1;
        for (u32 i = 0; i < s_nverts; i++) {
            encode_vertex(&m, s_fmt);
        }
        s_measure = 0;
        if ((u32)(m.slot - s_pipe) == written) {
            put_u8((u32)s_prim | s_fmt);
            put_u16(s_nverts);
            for (u32 i = 0; i < s_nverts; i++) {
                encode_vertex(&r, s_fmt);
            }
        } else {
            put_u8(GXDL_OP_RAW_PRIM);
            put_u8((u32)s_prim | s_fmt);
            put_u16(s_nverts);
            put_u32(written);
            for (u32 i = 0; i < written; i++) {
                put_u32(s_pipe[i].u32); /* each write's value; u8 and u16 writes use the low bytes */
            }
        }
    } else if (s_nverts > 0 && gx_rec_on()) {
        if (!gx_opt_immdirect || !gx_rec_prim_direct((u8)(s_prim | s_fmt), s_nverts, s_begin_ra, s_pipe)) {
            gx_rec_prim((u8)(s_prim | s_fmt), s_nverts, s_begin_ra, s_pipe,
                        (u32)((const PPCWGPipe*)port_gx_wg_ptr - s_pipe));
        }
    } else if (s_nverts > 0) {
        gx_begin_caller = s_begin_ra;
        u64 t0 = GX_PERF_T();
        draw_from_reader(&r, s_prim, s_fmt, s_nverts);
        if (gx_perf_on > 0) {
            gx_perf.imm_prims++;
            gx_perf.t_imm += plat_ticks() - t0;
        }
    }
    port_gx_wg_ptr = s_pipe;
}

/* Translator side of a recorded GXBegin/GXEnd: the pipe writes as words (each
 * write's low 32 bits, which is what the readers take from a slot). */
static PPCWGPipe* s_tr_slots; /* PIPE_SLOTS, heap */

void gx_draw_prim(u8 primfmt, u16 nverts, const void* caller, const u32* words, u32 nwords) {
    if (nwords > PIPE_SLOTS) {
        nwords = PIPE_SLOTS;
    }
    for (u32 i = 0; i < nwords; i++) {
        s_tr_slots[i].u32 = words[i];
    }
    gx_begin_caller = (void*)caller;
    Reader r = {s_tr_slots, NULL, NULL};
    u64 t0 = GX_PERF_T();
    draw_from_reader(&r, (u8)(primfmt & 0xF8), (u8)(primfmt & 7), nverts);
    if (gx_perf_on > 0) {
        gx_perf.imm_prims++;
        gx_perf.t_imm += plat_ticks() - t0;
    }
}

volatile PPCWGPipe* port_gx_wg_overflow(void) {
    plat_fatal("GX: write-gather pipe overflow (%s)", s_in_begin ? "in a primitive" : "raw commands");
}

void GXResetWriteGatherPipe(void) {}

/* ---- Command streams (display lists, raw pipe commands) ---- */

/* the display list being run, for the unknown-command report */
static void* s_list_caller;
static const u8* s_list_start;
static u32 s_list_bytes;

void gx_pipe_cur_list(const void** list, u32* nbytes) {
    *list = s_list_start;
    *nbytes = s_list_bytes;
}

static const PPCWGPipe* s_slots_end; /* the end of the slots a pipe-mode run_commands reads */

static void run_commands(Reader* r, int from_list) {
    for (;;) {
        if (r->slot == NULL && r->p >= r->end) {
            return;
        }
        if (r->slot != NULL && r->slot >= s_slots_end) {
            return;
        }
        u32 op = rd_u8(r);
        if (op == 0x00 || op == 0x48 || op == 0x44) {
            continue; /* NOP, invalidate vertex cache */
        }
        if (op >= 0x80 && op < 0xC0) {
            u32 n = rd_u16(r);
            if (gx_dl_recording() && !from_list) {
                put_u8(op);
                put_u16(n);
                for (u32 i = 0; i < n; i++) {
                    encode_vertex(r, (u8)(op & 7));
                }
            } else {
                s_list_kind |= 1;
                draw_from_reader(r, (u8)(op & 0xF8), (u8)(op & 7), n);
            }
            continue;
        }
        if (op == GXDL_OP_RAW_PRIM && r->slot == NULL) {
            u32 pf = rd_u8(r);
            u32 n = rd_u16(r);
            u32 count = rd_u32(r);
            if (count > PIPE_SLOTS || (u32)(r->end - r->p) < count * 4) {
                PLOG_W("GX: raw primitive of %lu writes runs past its display list", (unsigned long)count);
                return;
            }
            PPCWGPipe* slots = s_tr_slots; /* never in use here: raw primitives are in lists, not pipe commands */
            for (u32 i = 0; i < count; i++) {
                slots[i].u32 = rd_u32(r);
            }
            Reader sr = {slots, NULL, NULL};
            s_list_kind |= 4;
            draw_from_reader(&sr, (u8)(pf & 0xF8), (u8)(pf & 7), n);
            continue;
        }
        if (op >= GXDL_OP_PORT) {
            if (r->slot != NULL) {
                plat_fatal("GX: port opcode in the pipe");
            }
            r->p--;
            s_list_kind |= 2;
            gx_dl_replay_port_op(&r->p, r->end);
            continue;
        }
        switch (op) {
        case 0x08: /* CP register load: reg u8, value u32 */
            rd_u8(r);
            rd_u32(r);
            break;
        case 0x10: { /* XF load: (count-1)<<16 | address, then count words */
            u32 hdr = rd_u32(r);
            u32 count = (hdr >> 16) + 1;
            for (u32 i = 0; i < count; i++) {
                rd_u32(r);
            }
            break;
        }
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38: /* indexed XF loads */
            rd_u32(r);
            break;
        case 0x61: /* BP register: the performance counters DEMOInit sets */
            rd_u32(r);
            break;
        default:
            if (from_list) {
                plat_fatal("GX: unknown command %02lx at byte %ld of a %lu-byte display list %p called from %p",
                           (unsigned long)op, (long)(r->p - 1 - s_list_start), (unsigned long)s_list_bytes,
                           s_list_start, s_list_caller);
            }
            plat_fatal("GX: unknown command %02lx in the pipe", (unsigned long)op);
        }
    }
}

static void run_pending_raw(void) {
    if (gx_tls_translator && gx_rec_mode) {
        return; /* the pipe is the game thread's; it recorded what was pending */
    }
    if (s_in_begin || port_gx_wg_ptr == s_pipe) {
        return;
    }
    if (gx_rec_on() && !gx_dl_recording()) {
        gx_rec_raw(s_pipe, (u32)((const PPCWGPipe*)port_gx_wg_ptr - s_pipe));
        port_gx_wg_ptr = s_pipe;
        return;
    }
    Reader r = {s_pipe, NULL, NULL};
    s_slots_end = (const PPCWGPipe*)port_gx_wg_ptr;
    run_commands(&r, 0);
    port_gx_wg_ptr = s_pipe;
}

/* Translator side of recorded raw pipe commands. */
void gx_run_raw(const u32* words, u32 nwords) {
    if (nwords > PIPE_SLOTS) {
        nwords = PIPE_SLOTS;
    }
    for (u32 i = 0; i < nwords; i++) {
        s_tr_slots[i].u32 = words[i];
    }
    Reader r = {s_tr_slots, NULL, NULL};
    s_slots_end = s_tr_slots + nwords;
    run_commands(&r, 0);
}

void gx_pipe_alloc(void) {
    if (s_pipe == NULL) {
        s_pipe = gx_big_alloc("gx pipe", sizeof(PPCWGPipe) * PIPE_SLOTS);
        s_tr_slots = gx_big_alloc("gx pipe (translator)", sizeof(PPCWGPipe) * PIPE_SLOTS);
        s_verts = gx_big_alloc("gx pipe vertices", sizeof(GxVertex) * VERTS_MAX);
        port_gx_wg_ptr = s_pipe;
        port_gx_wg_end = s_pipe + PIPE_SLOTS;
    }
}

void gx_pipe_reset(void) {
    run_pending_raw();
}

void GXCallDisplayList(void* list, u32 nbytes) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    run_pending_raw();
    if (gx_dl_recording()) {
        plat_fatal("GXCallDisplayList inside a display list being recorded");
    }
    if (gx_rec_on()) {
        gx_rec_list(list, nbytes, __builtin_return_address(0));
        return;
    }
    gx_call_list(list, nbytes, __builtin_return_address(0));
    if (gx_rec_direct) {
        gx_rec_direct_list();
    }
}

void gx_call_list(const void* list, u32 nbytes, const void* caller) {
    Reader r = {NULL, (const u8*)list, (const u8*)list + nbytes};
    s_list_caller = (void*)caller;
    s_list_start = (const u8*)list;
    s_list_bytes = nbytes;
    if (gx_perf_on > 0) {
        gx_perf_dl(list, nbytes);
    }
    u64 t0 = GX_PERF_T();
    s_in_list = 1;
    s_list_kind = 0;
    if (gx_dlcache_enabled() && gx_dlcache_call(&gx, list, nbytes)) {
        s_list_kind = 1; /* drawn from the cache: only lists of primitives are cached */
    } else {
        run_commands(&r, 1);
        gx_dlc_cap_finish(&gx, s_list_kind);
    }
    s_in_list = 0;
    s_list_caller = NULL;
    if (gx_perf_on > 0) {
        gx_perf.t_dl += plat_ticks() - t0;
        gx_perf.dl_calls++;
        gx_perf.dl_bytes += nbytes;
        if (s_list_kind & 4) {
            gx_perf.dl_raw++;
        } else if (s_list_kind == 1) {
            gx_perf.dl_vtx_only++;
        } else if (s_list_kind == 2) {
            gx_perf.dl_state_only++;
        } else if (s_list_kind == 3) {
            gx_perf.dl_mixed++;
        }
    }
}

void GXBeginDisplayList(void* list, u32 size) {
    run_pending_raw();
    if (gx_dl_recording()) {
        plat_fatal("GXBeginDisplayList twice");
    }
    plat_memstable_release(list, size); /* the CPU builds a list here: no longer disc content */
    gx_dl_has_ops = 0;
    s_dl_buf = list;
    s_dl_size = size;
    s_dl_used = 0;
    s_dl_overflow = 0;
}

u32 GXEndDisplayList(void) {
    run_pending_raw();
    /* pad to 32 bytes with NOPs, as the SDK's flush does */
    while (s_dl_used & 31) {
        if (s_dl_used >= s_dl_size) {
            s_dl_overflow = 1;
            break;
        }
        s_dl_buf[s_dl_used++] = 0;
    }
    u32 used = s_dl_overflow ? 0 : s_dl_used;
    if (s_dl_overflow) {
        PLOG_W("GX: display list overflowed its %lu-byte buffer", (unsigned long)s_dl_size);
    }
    if (gx_dl_has_ops) {
        gx_rec_state_list(s_dl_buf, used); /* calling it changes the translator's state behind the record */
    }
    s_dl_buf = NULL;
    return used;
}
