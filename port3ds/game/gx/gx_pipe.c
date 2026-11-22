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

#include "gx_internal.h"
#include "plat/memstable.h"
#include "plat/plat.h"

#define PIPE_SLOTS (64 * 1024)

static PPCWGPipe s_pipe[PIPE_SLOTS];
volatile PPCWGPipe* port_gx_wg_ptr = s_pipe;
volatile PPCWGPipe* port_gx_wg_end = s_pipe + PIPE_SLOTS;

/* current primitive */
static int s_in_begin;
static u8 s_prim, s_fmt;
static u16 s_nverts;

/* display list recording */
static u8* s_dl_buf;
static u32 s_dl_size, s_dl_used;
static int s_dl_overflow;

static GxVertex s_verts[4096];

int gx_dl_recording(void) { return s_dl_buf != NULL; }

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

/* Decodes one vertex in format `fmt`. */
static void read_vertex(Reader* r, u8 fmt, GxVertex* v) {
    const GxAttrFmt* vat = gx.vat[fmt];
    v->pnmtx = (u8)gx.cur_pnmtx;
    memcpy(v->texmtx, gx.cur_texmtx, sizeof(v->texmtx));
    v->has_nrm = 0;
    v->clr[0] = v->clr[1] = (GXColor){255, 255, 255, 255};

    if (gx.vtx_desc[GX_VA_PNMTXIDX] != GX_NONE) {
        v->pnmtx = (u8)(rd_u8(r) / 3);
    }
    for (int t = 0; t < 8; t++) {
        if (gx.vtx_desc[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
            v->texmtx[t] = (u8)rd_u8(r);
        }
    }
    /* position */
    {
        u8 d = gx.vtx_desc[GX_VA_POS];
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
            const u8* p = gx.array_base[GX_VA_POS] + idx * gx.array_stride[GX_VA_POS];
            for (int i = 0; i < n; i++) {
                v->pos[i] = mem_comp(p, f->type, sc, (u32)i);
            }
        }
    }
    /* normal (NBT: normal, binormal, tangent; only the normal is used) */
    {
        u8 d = gx.vtx_desc[GX_VA_NRM] != GX_NONE ? gx.vtx_desc[GX_VA_NRM] : gx.vtx_desc[GX_VA_NBT];
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
                    const u8* p = gx.array_base[GX_VA_NRM] + idx * gx.array_stride[GX_VA_NRM];
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
        u8 d = gx.vtx_desc[GX_VA_CLR0 + c];
        const GxAttrFmt* f = &vat[GX_VA_CLR0 + c];
        if (d == GX_DIRECT) {
            v->clr[c] = rd_color(r, f->type);
        } else if (d != GX_NONE) {
            u32 idx = rd_index(r, d);
            if (gx_dlc_capturing) {
                gx_dlc_track(GX_VA_CLR0 + c, idx);
            }
            v->clr[c] = mem_color(gx.array_base[GX_VA_CLR0 + c] + idx * gx.array_stride[GX_VA_CLR0 + c], f->type);
        }
        if (f->cnt == GX_CLR_RGB) {
            v->clr[c].a = 255;
        }
    }
    /* texture coordinates */
    for (int t = 0; t < 8; t++) {
        u8 d = gx.vtx_desc[GX_VA_TEX0 + t];
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
            const u8* p = gx.array_base[GX_VA_TEX0 + t] + idx * gx.array_stride[GX_VA_TEX0 + t];
            for (int i = 0; i < n; i++) {
                v->tex[t][i] = mem_comp(p, f->type, sc, (u32)i);
            }
        }
    }
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
    const GxAttrFmt* vat = gx.vat[fmt];
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7MTXIDX; a++) {
        if (gx.vtx_desc[a] != GX_NONE) {
            put_u8(rd_u8(r));
        }
    }
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        u8 d = gx.vtx_desc[a];
        if (a == GX_VA_NRM && d == GX_NONE) {
            d = gx.vtx_desc[GX_VA_NBT];
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

void* gx_begin_caller; /* the code that began the current primitive, for diagnostics */

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    if (s_in_begin) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    run_pending_raw();
    gx_begin_caller = __builtin_return_address(0);
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
    while (done < n) {
        u32 chunk = n - done;
        if (chunk > sizeof(s_verts) / sizeof(s_verts[0])) {
            /* split on a primitive boundary */
            u32 max = sizeof(s_verts) / sizeof(s_verts[0]);
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
    } else if (s_nverts > 0) {
        u64 t0 = GX_PERF_T();
        draw_from_reader(&r, s_prim, s_fmt, s_nverts);
        if (gx_perf_on > 0) {
            gx_perf.imm_prims++;
            gx_perf.t_imm += plat_ticks() - t0;
        }
    }
    port_gx_wg_ptr = s_pipe;
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

static void run_commands(Reader* r, int from_list) {
    for (;;) {
        if (r->slot == NULL && r->p >= r->end) {
            return;
        }
        if (r->slot != NULL && r->slot >= (const PPCWGPipe*)port_gx_wg_ptr) {
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
            static PPCWGPipe slots[PIPE_SLOTS];
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
    if (s_in_begin || port_gx_wg_ptr == s_pipe) {
        return;
    }
    Reader r = {s_pipe, NULL, NULL};
    run_commands(&r, 0);
    port_gx_wg_ptr = s_pipe;
}

void gx_pipe_reset(void) {
    run_pending_raw();
}

void GXCallDisplayList(void* list, u32 nbytes) {
    if (gx_prim_open) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    run_pending_raw();
    if (gx_dl_recording()) {
        plat_fatal("GXCallDisplayList inside a display list being recorded");
    }
    Reader r = {NULL, (const u8*)list, (const u8*)list + nbytes};
    s_list_caller = __builtin_return_address(0);
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
    s_dl_buf = NULL;
    return used;
}
