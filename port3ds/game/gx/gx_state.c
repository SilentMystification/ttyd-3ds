/* GX state calls.
 *
 * Every setter stores the GameCube register state in `gx` and bumps
 * gx.version so the emitter rebuilds the PICA state before the next draw.
 * Inside a display list being recorded, a setter instead appends a port
 * opcode with its arguments (0xC0, call id, word count, LEB128 words),
 * and GXCallDisplayList replays it through the same setter, as the GameCube
 * replays the register writes the SDK put in the list.
 */
#include <stddef.h>
#include <string.h>

#include "gx_bigmem.h"
#include "gx_internal.h"
#include "port_addr.h"
#include "plat/plat.h"

GxState gx;
/* bumped by every write to what the display-list cache's same_state compares (vertex description and formats,
 * arrays, current matrices): an entry that matched at this version still matches */
u32 gx_vtx_version = 1;
u64 gx_vdesc_ver[2] = {1, 1};
#define VDESC_TOUCH(st) (gx_vdesc_ver[(st) == &gx_game]++)

/* ---- Recording ---- */

/* Words are stored as unsigned LEB128 (7 bits a byte, low first): most
 * arguments are small enums and indices, so a recorded call is about as
 * long as the GameCube's register writes for it, and display lists the
 * game builds into fixed heaps (makeDisplayList's map lists) fit as there. */
int gx_dl_has_ops; /* the display list being recorded holds a state call (gx_pipe.c, gxthread's dedup) */

static void rec(u32 id, const u32* words, u32 n) {
    u8 head[3] = {GXDL_OP_PORT, (u8)id, (u8)n};
    gx_dl_has_ops = 1;
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
 * drawn first, with the state it was given (see gx_end_open_primitive).
 * Inside a display list being recorded the call goes into the list; while
 * the game thread records for a translator (gx_rec.c) it goes into that
 * record. RECT: a call display lists do not hold (it applies at once while a
 * list is being recorded, as before), recorded only for the translator. gx_rec_fast.w (gx_internal.h) decides the
 * common cases in one load: nothing records, or the game thread records with nothing open; the translator thread
 * always applies. */
/* gxstatecache: the call is being applied here; when `gx` holds these words already there is nothing to do */
#define DD_APPLY(id, ...)                                                            \
    do {                                                                             \
        if (gx_opt_statededup) {                                                     \
            const u32 dw_[] = {__VA_ARGS__};                                         \
            if (gx_dd_apply_same(REC_##id, dw_, sizeof(dw_) / sizeof(dw_[0]))) {     \
                return;                                                              \
            }                                                                        \
        }                                                                            \
    } while (0)
#define REC(id, ...)                                                   \
    do {                                                               \
        const uintptr_t rw_ = gx_rec_fast.w;                           \
        if (rw_ != 0 && !gx_on_translator()) {                         \
            if (__builtin_expect(rw_ == 1, 1)) {                       \
                const u32 w_[] = {__VA_ARGS__};                        \
                gx_rec_call(REC_##id, w_, sizeof(w_) / sizeof(w_[0])); \
                return;                                                \
            }                                                          \
            GX_CLOSE_PRIMITIVE();                                      \
            if (gx_dl_recording()) {                                   \
                const u32 w_[] = {__VA_ARGS__};                        \
                rec(REC_##id, w_, sizeof(w_) / sizeof(w_[0]));         \
                return;                                                \
            }                                                          \
            if (gx_rec_on()) {                                         \
                const u32 w_[] = {__VA_ARGS__};                        \
                gx_rec_call(REC_##id, w_, sizeof(w_) / sizeof(w_[0])); \
                return;                                                \
            }                                                          \
        }                                                              \
        DD_APPLY(id, __VA_ARGS__);                                     \
    } while (0)
#define REC0(id)                                    \
    do {                                            \
        const uintptr_t rw_ = gx_rec_fast.w;        \
        if (rw_ != 0 && !gx_on_translator()) {      \
            if (__builtin_expect(rw_ == 1, 1)) {    \
                gx_rec_call(REC_##id, NULL, 0);     \
                return;                             \
            }                                       \
            GX_CLOSE_PRIMITIVE();                   \
            if (gx_dl_recording()) {                \
                rec(REC_##id, NULL, 0);             \
                return;                             \
            }                                       \
            if (gx_rec_on()) {                      \
                gx_rec_call(REC_##id, NULL, 0);     \
                return;                             \
            }                                       \
        }                                           \
        if (gx_opt_statededup) {                    \
            gx_dd_apply_same(REC_##id, NULL, 0);    \
        }                                           \
    } while (0)
#define RECT(id, ...)                                                  \
    do {                                                               \
        const uintptr_t rw_ = gx_rec_fast.w;                           \
        if (rw_ != 0 && !gx_on_translator()) {                         \
            if (__builtin_expect(rw_ == 1, 1)) {                       \
                const u32 w_[] = {__VA_ARGS__};                        \
                gx_rec_call(REC_##id, w_, sizeof(w_) / sizeof(w_[0])); \
                return;                                                \
            }                                                          \
            GX_CLOSE_PRIMITIVE();                                      \
            if (gx_rec_on()) {                                         \
                const u32 w_[] = {__VA_ARGS__};                        \
                gx_rec_call(REC_##id, w_, sizeof(w_) / sizeof(w_[0])); \
                return;                                                \
            }                                                          \
        }                                                              \
        DD_APPLY(id, __VA_ARGS__);                                     \
    } while (0)

/* `cls`: the gxdirty class (GXD_*) the setter wrote; set whether the switch is on or not (one store). */
static inline void touch(u32 cls) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx.version++;
    gx_dirty |= cls;
}

/* switch gxdirty: the setter's state holds these values already, so it returns before it changes anything (no
 * version change). The compare is the setter's own, inline: no table. */
#define SAME(cond)                         \
    do {                                   \
        if (gx_opt_dirty && (cond)) {      \
            return;                        \
        }                                  \
    } while (0)

/* A matrix or the projection changed. build_state reads neither (the
 * transform reads them per vertex), so the PICA state is not rebuilt for them
 * (switch gxstatever=0: they bump the state version as every setter does). */
u32 gx_xf_version;

static inline void touch_xf(u32 cls) {
    if (gx_prim_is_open()) {
        gx_end_open_primitive(__builtin_return_address(0));
    }
    gx_xf_version++;
    gx_dirty |= cls;
    if (!gx_opt_statever) {
        gx.version++;
    }
}

static GXColor u2c(u32 v) { return (GXColor){(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; }
static u32 c2u(GXColor c) { return (u32)c.r << 24 | (u32)c.g << 16 | (u32)c.b << 8 | c.a; }

/* ---- gxfifo: FIFO-cheap setters (docs/PORT_3DS_GX_NATIVE.md, revised plan step 2) ----
 * A hot setter is three parts:
 *   GXSetX:  one load of gx_rec_fast.fifo. 0 (gxfifo=1, gxthread=0, nothing recorded, no primitive open, gxdirty on,
 *            no gxstatededup): the fast body. Else X_old (1) or X_ver (2), both out of line in .text.unlikely.
 *   X_fast:  the GameCube's "a few FIFO stores": the arguments packed as the bytes GxState holds them, one or two
 *            word compares with GxState itself (ARMv6 loads unaligned words), return when equal; else the stores,
 *            the version and the class dirty bit. GxState is the packed shadow: no second copy, so the readers
 *            (getters, build_dirty/build_one, the CPU path, the verify modes) read what they read before.
 *   X_old:   the setter as it was (record ring for gxthread, display-list recording, statededup, verify modes,
 *            the open primitive). gxfifo=0 always takes it.
 * The vertex description, formats, arrays and the matrix loads compare too (the old bodies bump their versions on
 * every call): a version changes only when a byte changed.
 * gxfifo=2 (X_ver): runs X_fast, keeps its result, puts the state back, runs X_old, and compares the bytes of the
 * setter's state and, when the fast body found a change, the versions and dirty bits (`gxfifo verify` lines). The
 * state left is X_old's, so the run is the gxfifo=0 run. */
int gx_opt_fifo;
#define FIFO_HOT __attribute__((noinline, noclone))
#define FIFO_COLD __attribute__((noinline, noclone, section(".text.unlikely.gxfifo")))
#define FB(x) ((u32)(x) & 0xFFu)
static inline u32 ld32(const void* p) {
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline void st32(void* p, u32 v) { __builtin_memcpy(p, &v, 4); }
static inline u32 ld16(const void* p) {
    u16 v;
    __builtin_memcpy(&v, p, 2);
    return v;
}
static inline u32 pk4(u32 a, u32 b, u32 c, u32 d) { return FB(a) | FB(b) << 8 | FB(c) << 16 | FB(d) << 24; }
static inline void fifo_touch(u32 cls) {
    gx.version++;
    gx_dirty |= cls;
}
static inline void fifo_touch_xf(u32 cls) {
    gx_xf_version++;
    gx_dirty |= cls;
    if (!gx_opt_statever) {
        gx.version++;
    }
}
/* gxsplit stage 2 (gx_rec.c, docs/PORT_3DS_GX_NATIVE.md "Stage 2 as built"): the same fast bodies run on the game
 * thread's own state gx_game (S == &gx_game, folded at compile time). There they touch none of the translator's
 * counters: a change only bumps gx_gs_chg, and the setter's _gs wrapper marks the bytes it may have written (its
 * gxfifo=2 regions) in the game-side byte bitmap, which gx_rec.c sends as EV_STATE at the next draw boundary. */
u32 gx_gs_chg;
u32 gx_gs_bm[GX_GS_BM_WORDS], gx_gs_sum[GX_GS_SUM_WORDS], gx_gs_any;
#define FIFO_GS __attribute__((noinline, noclone, section(".text.gxsplit")))
#define FV_PRE(...) , ##__VA_ARGS__
#define GS_IS(S) ((S) == &gx_game)
static inline __attribute__((always_inline)) void fifo_touch_s(GxState* const S, u32 cls) {
    if (GS_IS(S)) {
        gx_gs_chg++;
    } else {
        fifo_touch(cls);
    }
}
static inline __attribute__((always_inline)) void fifo_touch_xf_s(GxState* const S, u32 cls) {
    if (GS_IS(S)) {
        gx_gs_chg++;
    } else {
        fifo_touch_xf(cls);
    }
}
#define FS_VTX(S)                \
    do {                         \
        if (GS_IS(S)) {          \
            gx_gs_chg++;         \
        } else {                 \
            gx_vtx_version++;    \
        }                        \
    } while (0)
#define FS_XF(S)                 \
    do {                         \
        if (GS_IS(S)) {          \
            gx_gs_chg++;         \
        } else {                 \
            gx_xf_version++;     \
        }                        \
    } while (0)
#define FS_VDESC(S) (gx_vdesc_ver[GS_IS(S)]++)
/* Marks the bytes [off, off + n) of GxState in the game-side bitmap (and their bitmap words in the summary). */
static void gs_mark(u32 off, u32 n) {
    if (n == 0 || off + n > sizeof(GxState)) {
        return;
    }
    const u32 end = off + n;
    for (u32 w = off >> 5; w <= (end - 1) >> 5; w++) {
        const u32 lo = w << 5 > off ? 0 : off & 31, hi = (w + 1) << 5 < end ? 32 : end - (w << 5);
        gx_gs_bm[w] |= (hi - lo == 32 ? 0xFFFFFFFFu : ((1u << (hi - lo)) - 1)) << lo;
        gx_gs_sum[w >> 5] |= 1u << (w & 31);
    }
    gx_gs_any = 1;
}
/* the regions of a setter (gxfifo=2's: pointers into gx, NULL for none) as offsets */
static FIFO_GS void gx_gs_mark2(const void* p0, u32 n0, const void* p1, u32 n1) {
    if (p0 != NULL) {
        gs_mark((u32)((const u8*)p0 - (const u8*)&gx), n0);
    }
    if (p1 != NULL) {
        gs_mark((u32)((const u8*)p1 - (const u8*)&gx), n1);
    }
}

/* the setter's start: one word decides */
#define FIFO_DISPATCH(name, A)                       \
    do {                                             \
        const uintptr_t ff_ = gx_rec_fast.fifo;      \
        if (__builtin_expect(ff_ != 0, 0)) {         \
            if (ff_ == 3 && !gx_on_translator()) {   \
                name##_gs A;                         \
            } else if (ff_ == 2) {                   \
                name##_ver A;                        \
            } else {                                 \
                name##_old A;                        \
            }                                        \
            return;                                  \
        }                                            \
    } while (0)

/* gxfifo=2 */
#define FV_MAX 128
static struct {
    u8* p[2];
    u32 n[2];
    u8 before[2][FV_MAX], fast[2][FV_MAX];
    u32 c0[6], cf[6];
    u32 checks, changes, bad;
} s_fv;
static void fv_counters(u32* c) {
    c[0] = gx.version;
    c[1] = gx_xf_version;
    c[2] = gx_vtx_version;
    c[3] = (u32)gx_vdesc_ver[0];
    c[4] = (u32)(gx_vdesc_ver[0] >> 32);
    c[5] = gx_dirty;
}
static void fv_set_counters(const u32* c) {
    gx.version = c[0];
    gx_xf_version = c[1];
    gx_vtx_version = c[2];
    gx_vdesc_ver[0] = (u64)c[4] << 32 | c[3];
    gx_dirty = c[5];
}
static FIFO_COLD void fv_begin(void* p0, u32 n0, void* p1, u32 n1) {
    s_fv.p[0] = p0;
    s_fv.n[0] = p0 != NULL ? n0 : 0;
    s_fv.p[1] = p1;
    s_fv.n[1] = p1 != NULL ? n1 : 0;
    for (int i = 0; i < 2; i++) {
        if (s_fv.n[i] > FV_MAX) {
            plat_fatal("gxfifo verify: region of %lu bytes", (unsigned long)s_fv.n[i]);
        }
        memcpy(s_fv.before[i], s_fv.p[i], s_fv.n[i]);
    }
    fv_counters(s_fv.c0);
}
static FIFO_COLD void fv_mid(void) {
    for (int i = 0; i < 2; i++) {
        memcpy(s_fv.fast[i], s_fv.p[i], s_fv.n[i]);
        memcpy(s_fv.p[i], s_fv.before[i], s_fv.n[i]);
    }
    fv_counters(s_fv.cf);
    fv_set_counters(s_fv.c0);
}
static FIFO_COLD void fv_end(const char* name) {
    u32 co[6];
    fv_counters(co);
    int bytes_bad = 0, old_changed = 0;
    for (int i = 0; i < 2; i++) {
        bytes_bad |= memcmp(s_fv.fast[i], s_fv.p[i], s_fv.n[i]) != 0;
        old_changed |= memcmp(s_fv.before[i], s_fv.p[i], s_fv.n[i]) != 0;
    }
    const int fast_changed = memcmp(s_fv.cf, s_fv.c0, sizeof(s_fv.c0)) != 0;
    /* a change: the versions and dirty bits the old body left; no change: the old body changed no byte */
    const int ctr_bad = fast_changed && memcmp(s_fv.cf, co, sizeof(co)) != 0;
    s_fv.checks++;
    s_fv.changes += (u32)fast_changed;
    if (bytes_bad || ctr_bad || fast_changed != old_changed) {
        if (s_fv.bad++ < 32) {
            PLOG_W("gxfifo verify: %s: bytes %s, fast %s / old %s, counters %s (version %lu/%lu xf %lu/%lu vtx %lu/%lu "
                   "vdesc %lu/%lu dirty %lx/%lx)",
                   name, bytes_bad ? "DIFFER" : "same", fast_changed ? "changed" : "unchanged",
                   old_changed ? "changed" : "unchanged", ctr_bad ? "DIFFER" : "ok", (unsigned long)s_fv.cf[0],
                   (unsigned long)co[0], (unsigned long)s_fv.cf[1], (unsigned long)co[1], (unsigned long)s_fv.cf[2],
                   (unsigned long)co[2], (unsigned long)s_fv.cf[3], (unsigned long)co[3], (unsigned long)s_fv.cf[5],
                   (unsigned long)co[5]);
        }
    }
}
/* the regions: p0, n0, p1, n1 */
#define FIFO_VER(name, P, A, ...)                \
    static FIFO_COLD void name##_ver P {         \
        fv_begin(__VA_ARGS__);                   \
        name##_fast(&gx FV_PRE A);               \
        fv_mid();                                \
        name##_old A;                            \
        fv_end(#name);                           \
    }                                            \
    static FIFO_GS void name##_gs P {            \
        const u32 c0_ = gx_gs_chg;               \
        name##_fast(&gx_game FV_PRE A);          \
        if (gx_gs_chg != c0_) {                  \
            gx_gs_mark2(__VA_ARGS__);            \
        }                                        \
    }

void gx_fifo_log(u32 frames) {
    if (gx_opt_fifo != 2 || frames == 0) {
        return;
    }
    PLOG_I("gxfifo frame %lu: verify %lu checks (%lu changes), %lu mismatches (total)", (unsigned long)gx_perf_frame_no(),
           (unsigned long)s_fv.checks, (unsigned long)s_fv.changes, (unsigned long)s_fv.bad);
}

/* ---- Vertex description ---- */

/* The field writes of the calls the game thread's shadow keeps (gx_shadow_call). */
static void vtx_desc_set(GxState* st, u32 attr, u32 type) {
    VDESC_TOUCH(st);
    if (attr == GX_VA_NBT) {
        st->vtx_desc[GX_VA_NBT] = (u8)type;
        st->vtx_desc[GX_VA_NRM] = GX_NONE;
    } else if (attr < GX_VA_MAX_ATTR) {
        st->vtx_desc[attr] = (u8)type;
        if (attr == GX_VA_NRM) {
            st->vtx_desc[GX_VA_NBT] = GX_NONE;
        }
    }
}

static void vtx_desc_clear(GxState* st) {
    VDESC_TOUCH(st);
    memset(st->vtx_desc, GX_NONE, sizeof(st->vtx_desc));
    st->vtx_desc[GX_VA_POS] = GX_DIRECT;
}

static void vat_set(GxState* st, u32 vtxfmt, u32 attr, u32 cnt, u32 type, u8 frac) {
    if (vtxfmt >= GX_MAX_VTXFMT) {
        return;
    }
    if (attr == GX_VA_NBT) {
        attr = GX_VA_NRM;
        cnt = GX_NRM_NBT;
    }
    if (attr >= GX_VA_MAX_ATTR) {
        return;
    }
    st->vat[vtxfmt][attr] = (GxAttrFmt){(u8)cnt, (u8)type, frac};
    VDESC_TOUCH(st);
}

static FIFO_COLD void vtx_desc_old(GXAttr attr, GXAttrType type) {
    GX_CLOSE_PRIMITIVE();
    REC(SetVtxDesc, attr, type);
    vtx_desc_set(&gx, (u32)attr, (u32)type);
    gx_vtx_version++;
}
static inline __attribute__((always_inline)) void vtx_desc_fast(GxState* const S, GXAttr attr, GXAttrType type) {
    const u32 a = (u32)attr, t = FB(type);
    if (a >= GX_VA_MAX_ATTR) {
        return;
    }
    u8* d = S->vtx_desc;
    if (a == GX_VA_NRM || a == GX_VA_NBT) {
        const u32 other = a == GX_VA_NRM ? GX_VA_NBT : GX_VA_NRM;
        if (d[a] == t && d[other] == GX_NONE) {
            return;
        }
        d[other] = GX_NONE;
    } else if (d[a] == t) {
        return;
    }
    d[a] = (u8)t;
    FS_VDESC(S);
    FS_VTX(S);
}
FIFO_VER(vtx_desc, (GXAttr attr, GXAttrType type), (attr, type), gx.vtx_desc, sizeof(gx.vtx_desc), NULL, 0)
FIFO_HOT void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    FIFO_DISPATCH(vtx_desc, (attr, type));
    vtx_desc_fast(&gx, attr, type);
}

void GXSetVtxDescv(const GXVtxDescList* list) {
    GX_CLOSE_PRIMITIVE();
    for (; list->attr != GX_VA_NULL; list++) {
        GXSetVtxDesc(list->attr, list->type);
    }
}

void GXGetVtxDesc(GXAttr attr, GXAttrType* type) {
    gx_rec_getter(GXG_VTXDESC);
    *type = (u32)attr < GX_VA_MAX_ATTR ? (GXAttrType)gx_g()->vtx_desc[attr] : GX_NONE;
}

void GXGetVtxDescv(GXVtxDescList* list) {
    gx_rec_getter(GXG_VTXDESC);
    const GxState* g = gx_g();
    int n = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        list[n].attr = (GXAttr)a;
        list[n].type = (GXAttrType)g->vtx_desc[a];
        n++;
    }
    list[n].attr = GX_VA_NULL;
    list[n].type = GX_NONE;
}

void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* list) {
    gx_rec_getter(GXG_VAT);
    const GxState* g = gx_g();
    int n = 0;
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        list[n].attr = (GXAttr)a;
        list[n].cnt = (GXCompCnt)g->vat[fmt][a].cnt;
        list[n].type = (GXCompType)g->vat[fmt][a].type;
        list[n].frac = g->vat[fmt][a].frac;
        n++;
    }
    list[n].attr = GX_VA_NULL;
}

static FIFO_COLD void clear_vtx_desc_old(void) {
    GX_CLOSE_PRIMITIVE();
    REC0(ClearVtxDesc);
    vtx_desc_clear(&gx);
    gx_vtx_version++;
}
/* the cleared description: GX_NONE (0) everywhere but GX_VA_POS (9) = GX_DIRECT (1) */
_Static_assert(offsetof(GxState, vtx_desc) == 0 && GX_VA_MAX_ATTR == 26 && GX_VA_POS == 9 && GX_NONE == 0 &&
                   GX_DIRECT == 1,
               "gxfifo: cleared vertex description words");
static inline __attribute__((always_inline)) void clear_vtx_desc_fast(GxState* const S) {
    const u32* w = (const u32*)(const void*)S->vtx_desc;
    if ((w[0] | w[1] | (w[2] ^ 0x100u) | w[3] | w[4] | w[5] | ld16(S->vtx_desc + 24)) == 0) {
        return;
    }
    vtx_desc_clear(S);
    FS_VTX(S);
}
FIFO_VER(clear_vtx_desc, (void), (), gx.vtx_desc, sizeof(gx.vtx_desc), NULL, 0)
FIFO_HOT void GXClearVtxDesc(void) {
    FIFO_DISPATCH(clear_vtx_desc, ());
    clear_vtx_desc_fast(&gx);
}

static FIFO_COLD void vtx_attr_fmt_old(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    GX_CLOSE_PRIMITIVE();
    REC(SetVtxAttrFmt, vtxfmt, attr, cnt, type, frac);
    vat_set(&gx, (u32)vtxfmt, (u32)attr, (u32)cnt, (u32)type, frac);
    gx_vtx_version++;
}
static inline __attribute__((always_inline)) void vtx_attr_fmt_fast(GxState* const S, GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    u32 a = (u32)attr, c = FB(cnt);
    if ((u32)vtxfmt >= GX_MAX_VTXFMT) {
        return;
    }
    if (a == GX_VA_NBT) {
        a = GX_VA_NRM;
        c = GX_NRM_NBT;
    }
    if (a >= GX_VA_MAX_ATTR) {
        return;
    }
    u8* p = &S->vat[vtxfmt][a].cnt;
    /* the word reaches one byte past the last format: array_base follows in GxState */
    if (((ld32(p) ^ (c | FB(type) << 8 | (u32)frac << 16)) & 0xFFFFFFu) == 0) {
        return;
    }
    p[0] = (u8)c;
    p[1] = (u8)type;
    p[2] = frac;
    FS_VDESC(S);
    FS_VTX(S);
}
FIFO_VER(vtx_attr_fmt, (GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac),
         (vtxfmt, attr, cnt, type, frac), (u32)vtxfmt < GX_MAX_VTXFMT ? (void*)gx.vat[vtxfmt] : NULL,
         sizeof(gx.vat[0]), NULL, 0)
FIFO_HOT void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    FIFO_DISPATCH(vtx_attr_fmt, (vtxfmt, attr, cnt, type, frac));
    vtx_attr_fmt_fast(&gx, vtxfmt, attr, cnt, type, frac);
}

void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) {
    GX_CLOSE_PRIMITIVE();
    for (; list->attr != GX_VA_NULL; list++) {
        GXSetVtxAttrFmt(vtxfmt, list->attr, list->cnt, list->type, list->frac);
    }
}

static FIFO_COLD void set_array_old(GXAttr attr, void* base_ptr, u8 stride) {
    GX_CLOSE_PRIMITIVE();
    REC(SetArray, attr, (u32)base_ptr, stride);
    if (attr == GX_VA_NBT) {
        attr = GX_VA_NRM;
    }
    if ((u32)attr < GX_VA_MAX_ATTR) {
        gx.array_base[attr] = base_ptr;
        gx_vtx_version++;
        gx.array_stride[attr] = stride;
    }
}
static inline __attribute__((always_inline)) void set_array_fast(GxState* const S, GXAttr attr, void* base_ptr, u8 stride) {
    u32 a = (u32)attr == GX_VA_NBT ? (u32)GX_VA_NRM : (u32)attr;
    if (a >= GX_VA_MAX_ATTR || (S->array_base[a] == base_ptr && S->array_stride[a] == stride)) {
        return;
    }
    S->array_base[a] = base_ptr;
    S->array_stride[a] = stride;
    FS_VTX(S);
}
static inline u32 array_slot(GXAttr attr) { return (u32)attr == GX_VA_NBT ? (u32)GX_VA_NRM : (u32)attr; }
FIFO_VER(set_array, (GXAttr attr, void* base_ptr, u8 stride), (attr, base_ptr, stride),
         array_slot(attr) < GX_VA_MAX_ATTR ? (void*)&gx.array_base[array_slot(attr)] : NULL, sizeof(gx.array_base[0]),
         array_slot(attr) < GX_VA_MAX_ATTR ? (void*)&gx.array_stride[array_slot(attr)] : NULL, 1)
FIFO_HOT void GXSetArray(GXAttr attr, void* base_ptr, u8 stride) {
    FIFO_DISPATCH(set_array, (attr, base_ptr, stride));
    set_array_fast(&gx, attr, base_ptr, stride);
}

void GXInvalidateVtxCache(void) {
    GX_CLOSE_PRIMITIVE();
    gx_rec_frame_start(); /* DEMOBeforeRender: the game frame's start (gxthread counters) */
    if (gx_perf_drawdone_end != 0) {
        gx_perf.t_idle += plat_ticks() - gx_perf_drawdone_end;
        gx_perf_drawdone_end = 0;
    }
}

/* ---- Transform ---- */

static FIFO_COLD void load_pos_mtx_old(const f32 mtx[3][4], u32 id) {
    REC(LoadPosMtx, id, F2U(mtx[0][0]), F2U(mtx[0][1]), F2U(mtx[0][2]), F2U(mtx[0][3]), F2U(mtx[1][0]),
        F2U(mtx[1][1]), F2U(mtx[1][2]), F2U(mtx[1][3]), F2U(mtx[2][0]), F2U(mtx[2][1]), F2U(mtx[2][2]),
        F2U(mtx[2][3]));
    u32 i = id / 3;
    if (i < GX_POS_MTX_COUNT) {
        memcpy(gx.pos_mtx[i], mtx, sizeof(gx.pos_mtx[i]));
        touch_xf(GXD_CLIP);
    }
}
/* the bits of n words (a float compare would call -0 and 0 equal) */
static inline int words_equal(const void* a, const void* b, u32 n) {
    u32 d = 0;
    for (u32 k = 0; k < n; k++) {
        d |= ld32((const u8*)a + 4 * k) ^ ld32((const u8*)b + 4 * k);
    }
    return d == 0;
}
static inline __attribute__((always_inline)) void load_pos_mtx_fast(GxState* const S, const f32 mtx[3][4], u32 id) {
    const u32 i = id / 3;
    if (i >= GX_POS_MTX_COUNT || words_equal(S->pos_mtx[i], mtx, 12)) {
        return;
    }
    memcpy(S->pos_mtx[i], mtx, sizeof(S->pos_mtx[i]));
    fifo_touch_xf_s(S, GXD_CLIP);
}
FIFO_VER(load_pos_mtx, (const f32 mtx[3][4], u32 id), (mtx, id), id / 3 < GX_POS_MTX_COUNT ? gx.pos_mtx[id / 3] : NULL,
         sizeof(gx.pos_mtx[0]), NULL, 0)
FIFO_HOT void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id) {
    FIFO_DISPATCH(load_pos_mtx, (mtx, id));
    load_pos_mtx_fast(&gx, mtx, id);
}

static FIFO_COLD void load_nrm_mtx_old(const f32 mtx[3][4], u32 id) {
    REC(LoadNrmMtx, id, F2U(mtx[0][0]), F2U(mtx[0][1]), F2U(mtx[0][2]), F2U(mtx[1][0]), F2U(mtx[1][1]),
        F2U(mtx[1][2]), F2U(mtx[2][0]), F2U(mtx[2][1]), F2U(mtx[2][2]));
    u32 i = id / 3;
    if (i < GX_POS_MTX_COUNT) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                gx.nrm_mtx[i][r][c] = mtx[r][c];
            }
        }
        touch_xf(GXD_CLIP);
    }
}
static inline __attribute__((always_inline)) void load_nrm_mtx_fast(GxState* const S, const f32 mtx[3][4], u32 id) {
    const u32 i = id / 3;
    if (i >= GX_POS_MTX_COUNT ||
        (words_equal(S->nrm_mtx[i][0], mtx[0], 3) && words_equal(S->nrm_mtx[i][1], mtx[1], 3) &&
         words_equal(S->nrm_mtx[i][2], mtx[2], 3))) {
        return;
    }
    for (int r = 0; r < 3; r++) {
        memcpy(S->nrm_mtx[i][r], mtx[r], sizeof(S->nrm_mtx[i][r]));
    }
    fifo_touch_xf_s(S, GXD_CLIP);
}
FIFO_VER(load_nrm_mtx, (const f32 mtx[3][4], u32 id), (mtx, id), id / 3 < GX_POS_MTX_COUNT ? gx.nrm_mtx[id / 3] : NULL,
         sizeof(gx.nrm_mtx[0]), NULL, 0)
FIFO_HOT void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id) {
    FIFO_DISPATCH(load_nrm_mtx, (mtx, id));
    load_nrm_mtx_fast(&gx, mtx, id);
}

static FIFO_COLD void load_tex_mtx_old(const f32 mtx[][4], u32 id, GXTexMtxType type) {
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
    touch_xf(GXD_TEXROW);
}
/* the matrix id's storage (and its 3x4 flag for a texture matrix), or NULL */
static inline __attribute__((always_inline)) f32 (*tex_mtx_dst(GxState* const S, u32 id, u8** is3x4))[4] {
    *is3x4 = NULL;
    if (id >= GX_PTTEXMTX0) {
        const u32 i = (id - GX_PTTEXMTX0) / 3;
        return i < GX_PTTEX_MTX_COUNT ? S->pttex_mtx[i] : NULL;
    }
    if (id >= GX_TEXMTX0) {
        const u32 i = (id - GX_TEXMTX0) / 3;
        if (i < GX_TEX_MTX_COUNT) {
            *is3x4 = &S->tex_mtx_is_3x4[i];
            return S->tex_mtx[i];
        }
    }
    return NULL;
}
static inline __attribute__((always_inline)) void load_tex_mtx_fast(GxState* const S, const f32 mtx[][4], u32 id, GXTexMtxType type) {
    u8* is3x4;
    f32 (*dst)[4] = tex_mtx_dst(S, id, &is3x4);
    if (dst == NULL) {
        return;
    }
    const u32 three = type == GX_MTX3x4;
    static const u32 row2[4] = {0, 0, 0x3F800000u, 0}; /* 0, 0, 1.0f, 0 */
    if ((is3x4 == NULL || *is3x4 == three) && words_equal(dst, mtx, 8) &&
        words_equal(dst[2], three ? mtx[2] : (const f32*)(const void*)row2, 4)) {
        return;
    }
    if (is3x4 != NULL) {
        *is3x4 = (u8)three;
    }
    memcpy(dst, mtx, sizeof(f32) * 8);
    memcpy(dst[2], three ? mtx[2] : (const f32*)(const void*)row2, sizeof(f32) * 4);
    fifo_touch_xf_s(S, GXD_TEXROW);
}
static inline void* tex_mtx_flag(u32 id) {
    u8* f;
    tex_mtx_dst(&gx, id, &f);
    return f;
}
static inline void* tex_mtx_ptr(u32 id) {
    u8* f;
    return tex_mtx_dst(&gx, id, &f);
}
FIFO_VER(load_tex_mtx, (const f32 mtx[][4], u32 id, GXTexMtxType type), (mtx, id, type), tex_mtx_ptr(id),
         sizeof(gx.tex_mtx[0]), tex_mtx_flag(id), 1)
FIFO_HOT void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, GXTexMtxType type) {
    FIFO_DISPATCH(load_tex_mtx, (mtx, id, type));
    load_tex_mtx_fast(&gx, mtx, id, type);
}

static FIFO_COLD void set_current_mtx_old(u32 id) {
    GX_CLOSE_PRIMITIVE();
    REC(SetCurrentMtx, id);
    SAME(gx.cur_pnmtx == id / 3);
    gx.cur_pnmtx = id / 3;
    gx_vtx_version++;
    gx_xf_version++;
}
static inline __attribute__((always_inline)) void set_current_mtx_fast(GxState* const S, u32 id) {
    const u32 i = id / 3;
    if (S->cur_pnmtx == i) {
        return;
    }
    S->cur_pnmtx = i;
    FS_VTX(S);
    FS_XF(S);
}
FIFO_VER(set_current_mtx, (u32 id), (id), &gx.cur_pnmtx, sizeof(gx.cur_pnmtx), NULL, 0)
FIFO_HOT void GXSetCurrentMtx(u32 id) {
    FIFO_DISPATCH(set_current_mtx, (id));
    set_current_mtx_fast(&gx, id);
}

void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) {
    GX_CLOSE_PRIMITIVE();
    f32 p[7];
    p[0] = (f32)type;
    p[1] = mtx[0][0];
    p[3] = mtx[1][1];
    p[5] = mtx[2][2];
    p[6] = mtx[2][3];
    if (type == GX_PERSPECTIVE) {
        p[2] = mtx[0][2];
        p[4] = mtx[1][2];
    } else {
        p[2] = mtx[0][3];
        p[4] = mtx[1][3];
    }
    REC(SetProjection, F2U(p[0]), F2U(p[1]), F2U(p[2]), F2U(p[3]), F2U(p[4]), F2U(p[5]), F2U(p[6]));
    SAME(gx_mem_equal(gx.proj, p, sizeof(gx.proj)));
    memcpy(gx.proj, p, sizeof(gx.proj));
    touch_xf(GXD_CLIP);
}

void GXSetProjectionv(const f32* ptr) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetProjection, F2U(ptr[0]), F2U(ptr[1]), F2U(ptr[2]), F2U(ptr[3]), F2U(ptr[4]), F2U(ptr[5]), F2U(ptr[6]));
    SAME(gx_mem_equal(gx.proj, ptr, sizeof(gx.proj)));
    memcpy(gx.proj, ptr, sizeof(gx.proj));
    touch_xf(GXD_CLIP);
}

void GXGetProjectionv(f32* ptr) {
    gx_rec_getter(GXG_PROJ);
    memcpy(ptr, gx_g()->proj, sizeof(gx.proj));
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    REC(SetViewport, F2U(left), F2U(top), F2U(wd), F2U(ht), F2U(nearz), F2U(farz));
    if (gx_opt_dirty) {
        const f32 v[6] = {left, top, wd, ht, nearz, farz};
        SAME(gx_mem_equal(gx.vp, v, sizeof(v)));
    }
    gx.vp[0] = left;
    gx.vp[1] = top;
    gx.vp[2] = wd;
    gx.vp[3] = ht;
    gx.vp[4] = nearz;
    gx.vp[5] = farz;
    touch(GXD_SCI);
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    GX_CLOSE_PRIMITIVE();
    /* Field jitter moves the viewport half a line for interlaced output;
     * the 3DS screen is progressive. */
    (void)field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXGetViewportv(f32* vp) {
    gx_rec_getter(GXG_VIEWPORT);
    memcpy(vp, gx_g()->vp, sizeof(gx.vp));
}

void GXSetZScaleOffset(f32 scale, f32 offset) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetZScaleOffset, F2U(scale), F2U(offset));
    SAME(F2U(gx.zscale) == F2U(scale) && F2U(gx.zoffset) == F2U(offset));
    gx.zscale = scale;
    gx.zoffset = offset;
    touch_xf(0);
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    REC(SetScissor, left, top, wd, ht);
    SAME(gx.scissor[0] == left && gx.scissor[1] == top && gx.scissor[2] == wd && gx.scissor[3] == ht);
    gx.scissor[0] = left;
    gx.scissor[1] = top;
    gx.scissor[2] = wd;
    gx.scissor[3] = ht;
    touch(GXD_SCI);
}

void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    gx_rec_getter(GXG_SCISSOR);
    const GxState* g = gx_g();
    *left = g->scissor[0];
    *top = g->scissor[1];
    *wd = g->scissor[2];
    *ht = g->scissor[3];
}

void GXSetScissorBoxOffset(s32 x_off, s32 y_off) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetScissorBoxOffset, (u32)x_off, (u32)y_off);
    SAME(gx.scissor_off[0] == x_off && gx.scissor_off[1] == y_off);
    gx.scissor_off[0] = x_off;
    gx.scissor_off[1] = y_off;
    touch(GXD_SCI);
}

/* ---- Texture coordinate generation ---- */

static FIFO_COLD void tex_coord_gen_old(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx,
                                        GXBool normalize, u32 pt_texmtx) {
    REC(SetTexCoordGen2, dst_coord, func, src_param, mtx, normalize, pt_texmtx);
    if ((u32)dst_coord >= GX_MAX_TEXGEN) {
        return;
    }
    SAME(gx.texgen[dst_coord].func == (u8)func && gx.texgen[dst_coord].src == (u8)src_param &&
         gx.texgen[dst_coord].mtx == mtx && gx.texgen[dst_coord].normalize == (u8)normalize &&
         gx.texgen[dst_coord].pt_mtx == pt_texmtx && gx.cur_texmtx[dst_coord] == (u8)mtx);
    gx.texgen[dst_coord].func = (u8)func;
    gx.texgen[dst_coord].src = (u8)src_param;
    gx.texgen[dst_coord].mtx = mtx;
    gx.texgen[dst_coord].normalize = normalize;
    gx.texgen[dst_coord].pt_mtx = pt_texmtx;
    gx.cur_texmtx[dst_coord] = (u8)mtx;
    gx_vtx_version++;
    touch(GXD_CHAN);
}
_Static_assert(offsetof(GxState, texgen[0].src) == offsetof(GxState, texgen[0].func) + 1 &&
                   offsetof(GxState, texgen[0].normalize) == offsetof(GxState, texgen[0].func) + 2,
               "gxfifo: texgen bytes");
static inline __attribute__((always_inline)) void tex_coord_gen_fast(GxState* const S, GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx,
                                      GXBool normalize, u32 pt_texmtx) {
    const u32 d = (u32)dst_coord;
    if (d >= GX_MAX_TEXGEN) {
        return;
    }
    typeof(S->texgen[0])* tg = &S->texgen[d];
    if (((ld32(&tg->func) ^ (FB(func) | FB(src_param) << 8 | FB(normalize) << 16)) & 0xFFFFFFu) == 0 &&
        tg->mtx == mtx && tg->pt_mtx == pt_texmtx && S->cur_texmtx[d] == FB(mtx)) {
        return;
    }
    tg->func = (u8)func;
    tg->src = (u8)src_param;
    tg->mtx = mtx;
    tg->normalize = normalize;
    tg->pt_mtx = pt_texmtx;
    S->cur_texmtx[d] = (u8)mtx;
    FS_VTX(S);
    fifo_touch_s(S, GXD_CHAN);
}
FIFO_VER(tex_coord_gen,
         (GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize, u32 pt_texmtx),
         (dst_coord, func, src_param, mtx, normalize, pt_texmtx),
         (u32)dst_coord < GX_MAX_TEXGEN ? (void*)&gx.texgen[dst_coord] : NULL, sizeof(gx.texgen[0]),
         (u32)dst_coord < GX_MAX_TEXGEN ? (void*)&gx.cur_texmtx[dst_coord] : NULL, 1)
FIFO_HOT void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx,
                                GXBool normalize, u32 pt_texmtx) {
    FIFO_DISPATCH(tex_coord_gen, (dst_coord, func, src_param, mtx, normalize, pt_texmtx));
    tex_coord_gen_fast(&gx, dst_coord, func, src_param, mtx, normalize, pt_texmtx);
}

/* the one-byte setters: compare, store, the class bit */
#define FIFO_BYTE_SETTER(pub, name, rec_id, T, field, cls)                    \
    static FIFO_COLD void name##_old(T v) {                                  \
        REC(rec_id, v);                                                      \
        SAME(gx.field == (u8)v);                                             \
        gx.field = (u8)v;                                                    \
        touch(cls);                                                          \
    }                                                                        \
    static inline __attribute__((always_inline)) void name##_fast(GxState* const S, T v) { \
        if (S->field == FB(v)) {                                             \
            return;                                                          \
        }                                                                    \
        S->field = (u8)v;                                                    \
        fifo_touch_s(S, cls);                                                \
    }                                                                        \
    FIFO_VER(name, (T v), (v), &gx.field, sizeof(gx.field), NULL, 0)        \
    FIFO_HOT void pub(T v) {                                                 \
        FIFO_DISPATCH(name, (v));                                            \
        name##_fast(&gx, v);                                                 \
    }

FIFO_BYTE_SETTER(GXSetNumTexGens, num_texgens, SetNumTexGens, u8, num_texgens, GXD_CHAN)

/* ---- Lighting ---- */

FIFO_BYTE_SETTER(GXSetNumChans, num_chans, SetNumChans, u8, num_chans, GXD_CHAN)

static FIFO_COLD void chan_ctrl_old(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                                    u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    REC(SetChanCtrl, chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn);
    int first, last;
    switch (chan) {
    case GX_COLOR0A0: first = 0; last = 2; break; /* COLOR0 and ALPHA0 */
    case GX_COLOR1A1: first = 1; last = 3; break;
    default: first = last = (int)chan; break;
    }
    if (gx_opt_dirty) {
        int same = 1;
        for (int c = first; c <= last && c <= 3; c += 2) {
            same &= gx.chan[c].enable == (u8)enable && gx.chan[c].amb_src == (u8)amb_src &&
                    gx.chan[c].mat_src == (u8)mat_src && gx.chan[c].light_mask == light_mask &&
                    gx.chan[c].diff_fn == (u8)diff_fn && gx.chan[c].attn_fn == (u8)attn_fn;
            if (first == last) {
                break;
            }
        }
        if (same) {
            return;
        }
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
    touch(GXD_CHAN);
}
_Static_assert(offsetof(GxState, chan[0].attn_fn) == offsetof(GxState, chan[0].enable) + 4 &&
                   offsetof(GxState, chan[0].diff_fn) == offsetof(GxState, chan[0].enable) + 3,
               "gxfifo: channel bytes");
static inline __attribute__((always_inline)) int chan_eq(GxState* const S, u32 c, u32 w, u32 attn, u32 mask) {
    return ld32(&S->chan[c].enable) == w && S->chan[c].attn_fn == attn && S->chan[c].light_mask == mask;
}
static inline __attribute__((always_inline)) void chan_put(GxState* const S, u32 c, u32 w, u32 attn, u32 mask) {
    st32(&S->chan[c].enable, w);
    S->chan[c].attn_fn = (u8)attn;
    S->chan[c].light_mask = mask;
}
static inline __attribute__((always_inline)) void chan_ctrl_fast(GxState* const S, GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                                  u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    u32 c = (u32)chan;
    const u32 w = pk4(enable, amb_src, mat_src, diff_fn), a = FB(attn_fn);
    if (c < 4) { /* COLOR0, COLOR1, ALPHA0, ALPHA1 */
        if (chan_eq(S, c, w, a, light_mask)) {
            return;
        }
        chan_put(S, c, w, a, light_mask);
    } else if (c - GX_COLOR0A0 < 2) { /* COLOR0A0: 0 and 2; COLOR1A1: 1 and 3 */
        c -= GX_COLOR0A0;
        if (chan_eq(S, c, w, a, light_mask) && chan_eq(S, c + 2, w, a, light_mask)) {
            return;
        }
        chan_put(S, c, w, a, light_mask);
        chan_put(S, c + 2, w, a, light_mask);
    } else {
        return;
    }
    fifo_touch_s(S, GXD_CHAN);
}
FIFO_VER(chan_ctrl,
         (GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn,
          GXAttnFn attn_fn),
         (chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn), gx.chan, sizeof(gx.chan), NULL, 0)
FIFO_HOT void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
                            GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    FIFO_DISPATCH(chan_ctrl, (chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn));
    chan_ctrl_fast(&gx, chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn);
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

static FIFO_COLD void chan_amb_old(GXChannelID chan, GXColor amb_color) {
    REC(SetChanAmbColor, chan, c2u(amb_color));
    if (gx_opt_dirty) {
        GXColor t[2] = {gx.amb[0], gx.amb[1]};
        set_chan_color(t, chan, amb_color);
        SAME(c2u(t[0]) == c2u(gx.amb[0]) && c2u(t[1]) == c2u(gx.amb[1]));
    }
    set_chan_color(gx.amb, chan, amb_color);
    touch(GXD_CHAN);
}
/* the material and ambient colours: the two colours after the call, compared as two words */
static inline __attribute__((always_inline)) void chan_color_fast(GxState* const S, GXColor* dst, GXChannelID chan, GXColor c) {
    GXColor t[2] = {dst[0], dst[1]};
    set_chan_color(t, chan, c);
    if (ld32(&t[0]) == ld32(&dst[0]) && ld32(&t[1]) == ld32(&dst[1])) {
        return;
    }
    dst[0] = t[0];
    dst[1] = t[1];
    fifo_touch_s(S, GXD_CHAN);
}
static inline __attribute__((always_inline)) void chan_amb_fast(GxState* const S, GXChannelID chan, GXColor c) { chan_color_fast(S, S->amb, chan, c); }
FIFO_VER(chan_amb, (GXChannelID chan, GXColor c), (chan, c), gx.amb, sizeof(gx.amb), NULL, 0)
FIFO_HOT void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    FIFO_DISPATCH(chan_amb, (chan, amb_color));
    chan_amb_fast(&gx, chan, amb_color);
}

static FIFO_COLD void chan_mat_old(GXChannelID chan, GXColor mat_color) {
    REC(SetChanMatColor, chan, c2u(mat_color));
    if (gx_opt_dirty) {
        GXColor t[2] = {gx.mat[0], gx.mat[1]};
        set_chan_color(t, chan, mat_color);
        SAME(c2u(t[0]) == c2u(gx.mat[0]) && c2u(t[1]) == c2u(gx.mat[1]));
    }
    set_chan_color(gx.mat, chan, mat_color);
    touch(GXD_CHAN);
}
static inline __attribute__((always_inline)) void chan_mat_fast(GxState* const S, GXChannelID chan, GXColor c) { chan_color_fast(S, S->mat, chan, c); }
FIFO_VER(chan_mat, (GXChannelID chan, GXColor c), (chan, c), gx.mat, sizeof(gx.mat), NULL, 0)
FIFO_HOT void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    FIFO_DISPATCH(chan_mat, (chan, mat_color));
    chan_mat_fast(&gx, chan, mat_color);
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
    if (gx_dl_recording() || gx_rec_on()) {
        const u32* w = (const u32*)l;
        u32 words[1 + sizeof(GxLightObj) / 4];
        words[0] = (u32)light;
        memcpy(words + 1, w, sizeof(GxLightObj));
        if (gx_dl_recording()) {
            rec(REC_LoadLightObj, words, sizeof(words) / 4);
        } else {
            gx_rec_call(REC_LoadLightObj, words, sizeof(words) / 4);
        }
        return;
    }
    if (idx >= 0 && idx < GX_MAX_LIGHT) {
        SAME(gx_mem_equal(&gx.light[idx], l, sizeof(*l)));
        gx.light[idx] = *l;
        touch(GXD_CHAN);
    }
}

/* ---- TEV ---- */

FIFO_BYTE_SETTER(GXSetNumTevStages, num_tev, SetNumTevStages, u8, num_tev, GXD_TEV)

/* a TEV stage's bytes at `off` (n of 1..8), compared with the packed lo (bytes 0-3) and hi (bytes 4-7) */
#define TEV_OFF(f) offsetof(typeof(gx.tev[0]), f)
static inline __attribute__((always_inline)) int tev_put(GxState* const S, u32 stage, u32 off, u32 n, u32 lo, u32 hi) {
    u8* p = (u8*)&S->tev[stage] + off;
    const u32 m_lo = n >= 4 ? 0xFFFFFFFFu : (1u << (8 * n)) - 1;
    const u32 m_hi = n <= 4 ? 0 : n >= 8 ? 0xFFFFFFFFu : (1u << (8 * (n - 4))) - 1;
    if (((ld32(p) ^ lo) & m_lo) == 0 && (m_hi == 0 || ((ld32(p + 4) ^ hi) & m_hi) == 0)) {
        return 0;
    }
    for (u32 k = 0; k < n; k++) {
        p[k] = (u8)((k < 4 ? lo >> (8 * k) : hi >> (8 * (k - 4))) & 0xFF);
    }
    fifo_touch_s(S, GXD_TEV);
    return 1;
}
#define TEV_FAST(stage, field, n, lo, hi)                     \
    do {                                                      \
        if ((u32)(stage) < GX_MAX_TEV) {                      \
            tev_put(S, (u32)(stage), TEV_OFF(field), n, lo, hi);   \
        }                                                     \
    } while (0)
#define TEV_VER_REGION(stage) (u32)(stage) < GX_MAX_TEV ? (void*)&gx.tev[stage] : NULL, sizeof(gx.tev[0]), NULL, 0

static FIFO_COLD void tev_order_old(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    REC(SetTevOrder, stage, coord, map, color);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    const u8 map8 = (u8)(map == GX_TEXMAP_NULL ? 0xFF : (map & 0xFF));
    SAME(gx.tev[stage].coord == (u8)coord && gx.tev[stage].map == map8 && gx.tev[stage].color == (u8)color);
    gx.tev[stage].coord = (u8)coord;
    gx.tev[stage].map = map8;
    gx.tev[stage].color = (u8)color;
    touch(GXD_TEV);
}
/* GX_TEXMAP_NULL is 0xFF: the old map8 is the low byte in both cases */
_Static_assert(GX_TEXMAP_NULL == 0xFF, "gxfifo: TEV order map byte");
static inline __attribute__((always_inline)) void tev_order_fast(GxState* const S, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    TEV_FAST(stage, coord, 3, pk4(coord, map, color, 0), 0);
}
FIFO_VER(tev_order, (GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color),
         (stage, coord, map, color), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    FIFO_DISPATCH(tev_order, (stage, coord, map, color));
    tev_order_fast(&gx, stage, coord, map, color);
}

static FIFO_COLD void tev_color_in_old(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                                       GXTevColorArg d) {
    REC(SetTevColorIn, stage, a, b, c, d);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    SAME(gx.tev[stage].c_in[0] == (u8)a && gx.tev[stage].c_in[1] == (u8)b && gx.tev[stage].c_in[2] == (u8)c &&
         gx.tev[stage].c_in[3] == (u8)d);
    gx.tev[stage].c_in[0] = (u8)a;
    gx.tev[stage].c_in[1] = (u8)b;
    gx.tev[stage].c_in[2] = (u8)c;
    gx.tev[stage].c_in[3] = (u8)d;
    touch(GXD_TEV);
}
static inline __attribute__((always_inline)) void tev_color_in_fast(GxState* const S, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                                     GXTevColorArg d) {
    TEV_FAST(stage, c_in, 4, pk4(a, b, c, d), 0);
}
FIFO_VER(tev_color_in, (GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d),
         (stage, a, b, c, d), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                              GXTevColorArg d) {
    FIFO_DISPATCH(tev_color_in, (stage, a, b, c, d));
    tev_color_in_fast(&gx, stage, a, b, c, d);
}

static FIFO_COLD void tev_alpha_in_old(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                                       GXTevAlphaArg d) {
    REC(SetTevAlphaIn, stage, a, b, c, d);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    SAME(gx.tev[stage].a_in[0] == (u8)a && gx.tev[stage].a_in[1] == (u8)b && gx.tev[stage].a_in[2] == (u8)c &&
         gx.tev[stage].a_in[3] == (u8)d);
    gx.tev[stage].a_in[0] = (u8)a;
    gx.tev[stage].a_in[1] = (u8)b;
    gx.tev[stage].a_in[2] = (u8)c;
    gx.tev[stage].a_in[3] = (u8)d;
    touch(GXD_TEV);
}
static inline __attribute__((always_inline)) void tev_alpha_in_fast(GxState* const S, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                                     GXTevAlphaArg d) {
    TEV_FAST(stage, a_in, 4, pk4(a, b, c, d), 0);
}
FIFO_VER(tev_alpha_in, (GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d),
         (stage, a, b, c, d), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                              GXTevAlphaArg d) {
    FIFO_DISPATCH(tev_alpha_in, (stage, a, b, c, d));
    tev_alpha_in_fast(&gx, stage, a, b, c, d);
}

static FIFO_COLD void tev_color_op_old(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                       GXTevRegID out_reg) {
    REC(SetTevColorOp, stage, op, bias, scale, clamp, out_reg);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    SAME(gx.tev[stage].c_op == (u8)op && gx.tev[stage].c_bias == (u8)bias && gx.tev[stage].c_scale == (u8)scale &&
         gx.tev[stage].c_clamp == (u8)clamp && gx.tev[stage].c_out == (u8)out_reg);
    gx.tev[stage].c_op = (u8)op;
    gx.tev[stage].c_bias = (u8)bias;
    gx.tev[stage].c_scale = (u8)scale;
    gx.tev[stage].c_clamp = clamp;
    gx.tev[stage].c_out = (u8)out_reg;
    touch(GXD_TEV);
}
static inline __attribute__((always_inline)) void tev_color_op_fast(GxState* const S, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                     GXTevRegID out_reg) {
    TEV_FAST(stage, c_op, 5, pk4(op, bias, scale, clamp), FB(out_reg));
}
FIFO_VER(tev_color_op,
         (GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg),
         (stage, op, bias, scale, clamp, out_reg), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                              GXTevRegID out_reg) {
    FIFO_DISPATCH(tev_color_op, (stage, op, bias, scale, clamp, out_reg));
    tev_color_op_fast(&gx, stage, op, bias, scale, clamp, out_reg);
}

static FIFO_COLD void tev_alpha_op_old(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                       GXTevRegID out_reg) {
    REC(SetTevAlphaOp, stage, op, bias, scale, clamp, out_reg);
    if ((u32)stage >= GX_MAX_TEV) {
        return;
    }
    SAME(gx.tev[stage].a_op == (u8)op && gx.tev[stage].a_bias == (u8)bias && gx.tev[stage].a_scale == (u8)scale &&
         gx.tev[stage].a_clamp == (u8)clamp && gx.tev[stage].a_out == (u8)out_reg);
    gx.tev[stage].a_op = (u8)op;
    gx.tev[stage].a_bias = (u8)bias;
    gx.tev[stage].a_scale = (u8)scale;
    gx.tev[stage].a_clamp = clamp;
    gx.tev[stage].a_out = (u8)out_reg;
    touch(GXD_TEV);
}
static inline __attribute__((always_inline)) void tev_alpha_op_fast(GxState* const S, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                     GXTevRegID out_reg) {
    TEV_FAST(stage, a_op, 5, pk4(op, bias, scale, clamp), FB(out_reg));
}
FIFO_VER(tev_alpha_op,
         (GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg),
         (stage, op, bias, scale, clamp, out_reg), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                              GXTevRegID out_reg) {
    FIFO_DISPATCH(tev_alpha_op, (stage, op, bias, scale, clamp, out_reg));
    tev_alpha_op_fast(&gx, stage, op, bias, scale, clamp, out_reg);
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

static FIFO_COLD void tev_color_old(GXTevRegID id, GXColor color) {
    REC(SetTevColor, id, c2u(color));
    if ((u32)id < 4) {
        SAME(gx.tev_reg[id].r == color.r && gx.tev_reg[id].g == color.g && gx.tev_reg[id].b == color.b &&
             gx.tev_reg[id].a == color.a);
        gx.tev_reg[id] = (GXColorS10){color.r, color.g, color.b, color.a};
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_color_fast(GxState* const S, GXTevRegID id, GXColor color) {
    if ((u32)id >= 4) {
        return;
    }
    /* GXColorS10 r, g, b, a as s16: the colour's bytes widened */
    const u32 lo = (u32)color.r | (u32)color.g << 16, hi = (u32)color.b | (u32)color.a << 16;
    u8* p = (u8*)&S->tev_reg[id];
    if (ld32(p) == lo && ld32(p + 4) == hi) {
        return;
    }
    st32(p, lo);
    st32(p + 4, hi);
    fifo_touch_s(S, GXD_TEV);
}
FIFO_VER(tev_color, (GXTevRegID id, GXColor color), (id, color), (u32)id < 4 ? (void*)&gx.tev_reg[id] : NULL,
         sizeof(gx.tev_reg[0]), NULL, 0)
FIFO_HOT void GXSetTevColor(GXTevRegID id, GXColor color) {
    FIFO_DISPATCH(tev_color, (id, color));
    tev_color_fast(&gx, id, color);
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    REC(SetTevColorS10, id, (u32)(u16)color.r << 16 | (u16)color.g, (u32)(u16)color.b << 16 | (u16)color.a);
    if ((u32)id < 4) {
        SAME(gx.tev_reg[id].r == color.r && gx.tev_reg[id].g == color.g && gx.tev_reg[id].b == color.b &&
             gx.tev_reg[id].a == color.a);
        gx.tev_reg[id] = color;
        touch(GXD_TEV);
    }
}

static FIFO_COLD void tev_kcolor_old(GXTevKColorID id, GXColor color) {
    REC(SetTevKColor, id, c2u(color));
    if ((u32)id < 4) {
        SAME(c2u(gx.tev_kcolor[id]) == c2u(color));
        gx.tev_kcolor[id] = color;
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_kcolor_fast(GxState* const S, GXTevKColorID id, GXColor color) {
    if ((u32)id >= 4 || ld32(&S->tev_kcolor[id]) == ld32(&color)) {
        return;
    }
    S->tev_kcolor[id] = color;
    fifo_touch_s(S, GXD_TEV);
}
FIFO_VER(tev_kcolor, (GXTevKColorID id, GXColor color), (id, color), (u32)id < 4 ? (void*)&gx.tev_kcolor[id] : NULL,
         sizeof(gx.tev_kcolor[0]), NULL, 0)
FIFO_HOT void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    FIFO_DISPATCH(tev_kcolor, (id, color));
    tev_kcolor_fast(&gx, id, color);
}

static FIFO_COLD void tev_ksel_old(GXTevStageID stage, GXTevKColorSel sel) {
    REC(SetTevKColorSel, stage, sel);
    if ((u32)stage < GX_MAX_TEV) {
        SAME(gx.tev[stage].kc_sel == (u8)sel);
        gx.tev[stage].kc_sel = (u8)sel;
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_ksel_fast(GxState* const S, GXTevStageID stage, GXTevKColorSel sel) { TEV_FAST(stage, kc_sel, 1, FB(sel), 0); }
FIFO_VER(tev_ksel, (GXTevStageID stage, GXTevKColorSel sel), (stage, sel), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    FIFO_DISPATCH(tev_ksel, (stage, sel));
    tev_ksel_fast(&gx, stage, sel);
}

static FIFO_COLD void tev_kasel_old(GXTevStageID stage, GXTevKAlphaSel sel) {
    REC(SetTevKAlphaSel, stage, sel);
    if ((u32)stage < GX_MAX_TEV) {
        SAME(gx.tev[stage].ka_sel == (u8)sel);
        gx.tev[stage].ka_sel = (u8)sel;
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_kasel_fast(GxState* const S, GXTevStageID stage, GXTevKAlphaSel sel) { TEV_FAST(stage, ka_sel, 1, FB(sel), 0); }
FIFO_VER(tev_kasel, (GXTevStageID stage, GXTevKAlphaSel sel), (stage, sel), TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    FIFO_DISPATCH(tev_kasel, (stage, sel));
    tev_kasel_fast(&gx, stage, sel);
}

static FIFO_COLD void tev_swap_old(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    REC(SetTevSwapMode, stage, ras_sel, tex_sel);
    if ((u32)stage < GX_MAX_TEV) {
        SAME(gx.tev[stage].ras_swap == (u8)ras_sel && gx.tev[stage].tex_swap == (u8)tex_sel);
        gx.tev[stage].ras_swap = (u8)ras_sel;
        gx.tev[stage].tex_swap = (u8)tex_sel;
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_swap_fast(GxState* const S, GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    TEV_FAST(stage, ras_swap, 2, FB(ras_sel) | FB(tex_sel) << 8, 0);
}
FIFO_VER(tev_swap, (GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel), (stage, ras_sel, tex_sel),
         TEV_VER_REGION(stage))
FIFO_HOT void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    FIFO_DISPATCH(tev_swap, (stage, ras_sel, tex_sel));
    tev_swap_fast(&gx, stage, ras_sel, tex_sel);
}

static FIFO_COLD void tev_swap_table_old(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                                         GXTevColorChan blue, GXTevColorChan alpha) {
    REC(SetTevSwapModeTable, table, red, green, blue, alpha);
    if ((u32)table < 4) {
        SAME(gx.swap_table[table][0] == (u8)red && gx.swap_table[table][1] == (u8)green &&
             gx.swap_table[table][2] == (u8)blue && gx.swap_table[table][3] == (u8)alpha);
        gx.swap_table[table][0] = (u8)red;
        gx.swap_table[table][1] = (u8)green;
        gx.swap_table[table][2] = (u8)blue;
        gx.swap_table[table][3] = (u8)alpha;
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_swap_table_fast(GxState* const S, GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                                       GXTevColorChan blue, GXTevColorChan alpha) {
    const u32 v = pk4(red, green, blue, alpha);
    if ((u32)table >= 4 || ld32(S->swap_table[table]) == v) {
        return;
    }
    st32(S->swap_table[table], v);
    fifo_touch_s(S, GXD_TEV);
}
FIFO_VER(tev_swap_table,
         (GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha),
         (table, red, green, blue, alpha), (u32)table < 4 ? (void*)gx.swap_table[table] : NULL,
         sizeof(gx.swap_table[0]), NULL, 0)
FIFO_HOT void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                                    GXTevColorChan alpha) {
    FIFO_DISPATCH(tev_swap_table, (table, red, green, blue, alpha));
    tev_swap_table_fast(&gx, table, red, green, blue, alpha);
}

static FIFO_COLD void tev_direct_old(GXTevStageID tev_stage) {
    REC(SetTevDirect, tev_stage);
    if ((u32)tev_stage < GX_MAX_TEV) {
        SAME(gx.tev[tev_stage].ind_enabled == 0);
        gx.tev[tev_stage].ind_enabled = 0;
        touch(GXD_TEV);
    }
}
static inline __attribute__((always_inline)) void tev_direct_fast(GxState* const S, GXTevStageID tev_stage) { TEV_FAST(tev_stage, ind_enabled, 1, 0, 0); }
FIFO_VER(tev_direct, (GXTevStageID tev_stage), (tev_stage), TEV_VER_REGION(tev_stage))
FIFO_HOT void GXSetTevDirect(GXTevStageID tev_stage) {
    FIFO_DISPATCH(tev_direct, (tev_stage));
    tev_direct_fast(&gx, tev_stage);
}

void GXSetTevIndWarp(GXTevStageID tev_stage, GXIndTexStageID ind_stage, u8 signed_offset, u8 replace_mode,
                     GXIndTexMtxID matrix_sel) {
    REC(SetTevIndirect, tev_stage, ind_stage, signed_offset, replace_mode, matrix_sel);
    if ((u32)tev_stage >= GX_MAX_TEV) {
        return;
    }
    SAME(gx.tev[tev_stage].ind_enabled == 1 && gx.tev[tev_stage].ind_stage == (u8)ind_stage &&
         gx.tev[tev_stage].ind_fmt == GX_ITF_8 &&
         gx.tev[tev_stage].ind_bias == (signed_offset ? GX_ITB_STU : GX_ITB_NONE) &&
         gx.tev[tev_stage].ind_mtx == (u8)matrix_sel && gx.tev[tev_stage].ind_wrap_s == (replace_mode ? 6 : 0) &&
         gx.tev[tev_stage].ind_wrap_t == (replace_mode ? 6 : 0) && gx.tev[tev_stage].ind_add_prev == 0);
    gx.tev[tev_stage].ind_enabled = 1;
    gx.tev[tev_stage].ind_stage = (u8)ind_stage;
    gx.tev[tev_stage].ind_fmt = GX_ITF_8;
    gx.tev[tev_stage].ind_bias = signed_offset ? GX_ITB_STU : GX_ITB_NONE;
    gx.tev[tev_stage].ind_mtx = (u8)matrix_sel;
    gx.tev[tev_stage].ind_wrap_s = replace_mode ? 6 /* GX_ITW_0 */ : 0;
    gx.tev[tev_stage].ind_wrap_t = replace_mode ? 6 : 0;
    gx.tev[tev_stage].ind_add_prev = 0;
    touch(GXD_TEV);
}

FIFO_BYTE_SETTER(GXSetNumIndStages, num_ind, SetNumIndStages, u8, num_ind, GXD_TEV)

void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetIndTexOrder, ind_stage, tex_coord, tex_map);
    if ((u32)ind_stage < 4) {
        SAME(gx.ind[ind_stage].coord == (u8)tex_coord && gx.ind[ind_stage].map == (u8)tex_map);
        gx.ind[ind_stage].coord = (u8)tex_coord;
        gx.ind[ind_stage].map = (u8)tex_map;
        touch(GXD_TEV);
    }
}

void GXSetIndTexCoordScale(GXIndTexStageID ind_state, GXIndTexScale scale_s, GXIndTexScale scale_t) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetIndTexCoordScale, ind_state, scale_s, scale_t);
    if ((u32)ind_state < 4) {
        SAME(gx.ind[ind_state].scale_s == (u8)scale_s && gx.ind[ind_state].scale_t == (u8)scale_t);
        gx.ind[ind_state].scale_s = (u8)scale_s;
        gx.ind[ind_state].scale_t = (u8)scale_t;
        touch(GXD_TEV);
    }
}

void GXSetIndTexMtx(GXIndTexMtxID mtx_id, const f32 offset[2][3], s8 scale_exp) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetIndTexMtx, mtx_id, F2U(offset[0][0]), F2U(offset[0][1]), F2U(offset[0][2]), F2U(offset[1][0]),
         F2U(offset[1][1]), F2U(offset[1][2]), (u32)(s32)scale_exp);
    int i = (int)mtx_id - GX_ITM_0;
    if (i >= 0 && i < 3) {
        SAME(gx_mem_equal(gx.ind_mtx[i], offset, sizeof(gx.ind_mtx[i])) && gx.ind_mtx_scale[i] == scale_exp);
        memcpy(gx.ind_mtx[i], offset, sizeof(gx.ind_mtx[i]));
        gx.ind_mtx_scale[i] = scale_exp;
        touch(GXD_TEV);
    }
}

void __GXSetIndirectMask(u32 mask) { (void)mask; }

/* ---- Pixel engine ---- */

/* `n` bytes of GxState from `first` (n of 1..5), compared with the packed lo (bytes 0-3) and hi (byte 4) */
static inline __attribute__((always_inline)) void pix_put(GxState* const S, u8* p, u32 n, u32 lo, u32 hi, u32 cls) {
    const u32 m_lo = n >= 4 ? 0xFFFFFFFFu : (1u << (8 * n)) - 1;
    if (((ld32(p) ^ lo) & m_lo) == 0 && (n <= 4 || p[4] == hi)) {
        return;
    }
    for (u32 k = 0; k < n && k < 4; k++) {
        p[k] = (u8)(lo >> (8 * k));
    }
    if (n > 4) {
        p[4] = (u8)hi;
    }
    fifo_touch_s(S, cls);
}

static FIFO_COLD void alpha_compare_old(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    REC(SetAlphaCompare, comp0, ref0, op, comp1, ref1);
    SAME(gx.alpha_comp0 == (u8)comp0 && gx.alpha_ref0 == ref0 && gx.alpha_op == (u8)op && gx.alpha_comp1 == (u8)comp1 &&
         gx.alpha_ref1 == ref1);
    gx.alpha_comp0 = (u8)comp0;
    gx.alpha_ref0 = ref0;
    gx.alpha_op = (u8)op;
    gx.alpha_comp1 = (u8)comp1;
    gx.alpha_ref1 = ref1;
    touch(GXD_PIX);
}
_Static_assert(offsetof(GxState, alpha_ref1) == offsetof(GxState, alpha_comp0) + 4 &&
                   offsetof(GxState, blend_logic) == offsetof(GxState, blend_type) + 3 &&
                   offsetof(GxState, z_update) == offsetof(GxState, z_enable) + 2,
               "gxfifo: pixel-engine bytes");
static inline __attribute__((always_inline)) void alpha_compare_fast(GxState* const S, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    pix_put(S, &S->alpha_comp0, 5, pk4(comp0, ref0, op, comp1), ref1, GXD_PIX);
}
FIFO_VER(alpha_compare, (GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1),
         (comp0, ref0, op, comp1, ref1), &gx.alpha_comp0, 5, NULL, 0)
FIFO_HOT void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    FIFO_DISPATCH(alpha_compare, (comp0, ref0, op, comp1, ref1));
    alpha_compare_fast(&gx, comp0, ref0, op, comp1, ref1);
}

static FIFO_COLD void blend_mode_old(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor,
                                     GXLogicOp op) {
    REC(SetBlendMode, type, src_factor, dst_factor, op);
    SAME(gx.blend_type == (u8)type && gx.blend_src == (u8)src_factor && gx.blend_dst == (u8)dst_factor &&
         gx.blend_logic == (u8)op);
    gx.blend_type = (u8)type;
    gx.blend_src = (u8)src_factor;
    gx.blend_dst = (u8)dst_factor;
    gx.blend_logic = (u8)op;
    touch(GXD_PIX);
}
static inline __attribute__((always_inline)) void blend_mode_fast(GxState* const S, GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor,
                                   GXLogicOp op) {
    pix_put(S, &S->blend_type, 4, pk4(type, src_factor, dst_factor, op), 0, GXD_PIX);
}
FIFO_VER(blend_mode, (GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op),
         (type, src_factor, dst_factor, op), &gx.blend_type, 4, NULL, 0)
FIFO_HOT void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    FIFO_DISPATCH(blend_mode, (type, src_factor, dst_factor, op));
    blend_mode_fast(&gx, type, src_factor, dst_factor, op);
}

static FIFO_COLD void z_mode_old(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    REC(SetZMode, compare_enable, func, update_enable);
    SAME(gx.z_enable == (u8)compare_enable && gx.z_func == (u8)func && gx.z_update == (u8)update_enable);
    gx.z_enable = compare_enable;
    gx.z_func = (u8)func;
    gx.z_update = update_enable;
    touch(GXD_PIX);
}
static inline __attribute__((always_inline)) void z_mode_fast(GxState* const S, GXBool compare_enable, GXCompare func, GXBool update_enable) {
    pix_put(S, &S->z_enable, 3, pk4(compare_enable, func, update_enable, 0), 0, GXD_PIX);
}
FIFO_VER(z_mode, (GXBool compare_enable, GXCompare func, GXBool update_enable), (compare_enable, func, update_enable),
         &gx.z_enable, 3, NULL, 0)
FIFO_HOT void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    FIFO_DISPATCH(z_mode, (compare_enable, func, update_enable));
    z_mode_fast(&gx, compare_enable, func, update_enable);
}

FIFO_BYTE_SETTER(GXSetZCompLoc, z_comp_loc, SetZCompLoc, GXBool, z_before_tex, GXD_PIX)
FIFO_BYTE_SETTER(GXSetColorUpdate, color_update, SetColorUpdate, GXBool, color_update, GXD_PIX)
FIFO_BYTE_SETTER(GXSetAlphaUpdate, alpha_update, SetAlphaUpdate, GXBool, alpha_update, GXD_PIX)

void GXSetDstAlpha(GXBool enable, u8 alpha) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetDstAlpha, enable, alpha);
    SAME(gx.dst_alpha_enable == (u8)enable && gx.dst_alpha == alpha);
    gx.dst_alpha_enable = enable;
    gx.dst_alpha = alpha;
    touch(GXD_PIX);
}

FIFO_BYTE_SETTER(GXSetCullMode, cull_mode, SetCullMode, GXCullMode, cull, GXD_PIX)

void GXGetCullMode(GXCullMode* mode) {
    gx_rec_getter(GXG_CULL);
    *mode = (GXCullMode)gx_g()->cull;
}

void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetPixelFmt, pix_fmt, z_fmt);
    SAME(gx.pixel_fmt == (u8)pix_fmt && gx.z_fmt == (u8)z_fmt);
    gx.pixel_fmt = (u8)pix_fmt;
    gx.z_fmt = (u8)z_fmt;
    touch(GXD_PIX);
}

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    REC(SetFog, type, F2U(startz), F2U(endz), F2U(nearz), F2U(farz), c2u(color));
    SAME(gx.fog_type == (u8)type && F2U(gx.fog_start) == F2U(startz) && F2U(gx.fog_end) == F2U(endz) &&
         F2U(gx.fog_near) == F2U(nearz) && F2U(gx.fog_far) == F2U(farz) && c2u(gx.fog_color) == c2u(color));
    gx.fog_type = (u8)type;
    gx.fog_start = startz;
    gx.fog_end = endz;
    gx.fog_near = nearz;
    gx.fog_far = farz;
    gx.fog_color = color;
    touch(GXD_PIX | GXD_TEV); /* the fog takes a TEV stage and a texture unit (gx_draw.c, gxfogramp) */
}

void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    GX_CLOSE_PRIMITIVE();
    RECT(SetZTexture, op, fmt, bias);
    SAME(gx.ztex_op == (u8)op && gx.ztex_fmt == (u8)fmt && gx.ztex_bias == bias);
    gx.ztex_op = (u8)op;
    gx.ztex_fmt = (u8)fmt;
    gx.ztex_bias = bias;
    touch(GXD_PIX);
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets) {
    REC(SetLineWidth, width, texOffsets);
    SAME(gx.line_width == width);
    gx.line_width = width;
    touch(GXD_PIX);
}

/* ---- Textures (objects live in gx_tex.c; loading a map is state) ---- */

/* never initialised by GXInitTexObj; logged once per caller */
static FIFO_COLD void tex_obj_warn(const GXTexObj* obj, void* ra) {
    const GxTexObj* t = (const GxTexObj*)obj;
    static void* seen[16];
    int i;
    for (i = 0; i < 16 && seen[i] != NULL && seen[i] != ra; i++) {
    }
    if (i < 16 && seen[i] == NULL) {
        seen[i] = ra;
        PLOG_W("GX: GXLoadTexObj(%p) from %p: not a texture object (image %p, format %u)", obj, ra, t->image,
               t->format);
    }
}
static inline int tex_obj_ok(const GxTexObj* t) {
    const int fmt_ok =
        t->format <= GX_TF_CMPR || t->format == GX_TF_Z8 || t->format == GX_TF_Z16 || t->format == GX_TF_Z24X8;
    return PORT_IS_ADDR(t->image) && fmt_ok;
}
static FIFO_COLD void load_tex_obj_old(GXTexObj* obj, GXTexMapID id, void* ra) {
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
        /* checked where the game calls it (a translator replays an object already checked) */
        if (!gx_tls_translator && !tex_obj_ok(t)) {
            tex_obj_warn(obj, ra);
        }
        if (gx_rec_on()) {
            u32 words[1 + sizeof(GxTexObj) / 4];
            words[0] = (u32)id;
            memcpy(words + 1, obj, sizeof(GxTexObj));
            gx_rec_call(REC_LoadTexObj, words, sizeof(words) / 4);
            return;
        }
        if (gx_opt_statededup) {
            u32 words[1 + sizeof(GxTexObj) / 4];
            words[0] = (u32)id;
            memcpy(words + 1, obj, sizeof(GxTexObj));
            if (gx_dd_apply_same(REC_LoadTexObj, words, sizeof(words) / 4)) {
                return; /* this object is the one loaded on the map */
            }
        }
        SAME(gx.tex_map_valid[id] && gx_mem_equal(&gx.tex_map[id], obj, sizeof(GxTexObj)));
        gx.tex_map[id] = *(GxTexObj*)obj;
        gx.tex_map_valid[id] = 1;
        touch(GXD_TEX);
    }
}
_Static_assert(sizeof(GxTexObj) % 4 == 0, "gxfifo: texture object words");
static inline __attribute__((always_inline)) void load_tex_obj_fast(GxState* const S, GXTexObj* obj, GXTexMapID id, void* ra) {
    if ((u32)id >= GX_MAX_TEXMAP) {
        return;
    }
    const GxTexObj* t = (const GxTexObj*)obj;
    if (__builtin_expect(!tex_obj_ok(t), 0)) {
        tex_obj_warn(obj, ra);
    }
    if (S->tex_map_valid[id] && words_equal(&S->tex_map[id], obj, sizeof(GxTexObj) / 4)) {
        return;
    }
    S->tex_map[id] = *t;
    S->tex_map_valid[id] = 1;
    fifo_touch_s(S, GXD_TEX);
}
FIFO_VER(load_tex_obj, (GXTexObj * obj, GXTexMapID id, void* ra), (obj, id, ra),
         (u32)id < GX_MAX_TEXMAP ? (void*)&gx.tex_map[id] : NULL, sizeof(GxTexObj),
         (u32)id < GX_MAX_TEXMAP ? (void*)&gx.tex_map_valid[id] : NULL, 1)
FIFO_HOT void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    void* ra = __builtin_return_address(0);
    FIFO_DISPATCH(load_tex_obj, (obj, id, ra));
    load_tex_obj_fast(&gx, obj, id, ra);
}

void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name) {
    GX_CLOSE_PRIMITIVE();
    if (gx_dl_recording() || gx_rec_on()) {
        u32 words[1 + sizeof(GxTlutObj) / 4];
        words[0] = tlut_name;
        memcpy(words + 1, tlut_obj, sizeof(GxTlutObj));
        if (gx_dl_recording()) {
            rec(REC_LoadTlut, words, sizeof(words) / 4);
        } else {
            gx_rec_call(REC_LoadTlut, words, sizeof(words) / 4);
        }
        return;
    }
    if (tlut_name < 20) {
        if (gx_opt_statededup) {
            u32 words[1 + sizeof(GxTlutObj) / 4];
            words[0] = tlut_name;
            memcpy(words + 1, tlut_obj, sizeof(GxTlutObj));
            if (gx_dd_apply_same(REC_LoadTlut, words, sizeof(words) / 4)) {
                return;
            }
        }
        SAME(gx_mem_equal(&gx.tlut[tlut_name], tlut_obj, sizeof(GxTlutObj)));
        gx.tlut[tlut_name] = *(GxTlutObj*)tlut_obj;
        touch(GXD_TEX);
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
    gx_apply_call(id, w);
}

/* ---- Pre-decoded state calls of display lists (switch gxsdl, default 0) ----
 * A material or state list is the same bytes called again and again. The leading run of NOPs and port opcodes of
 * a list is decoded once into plain call words (id | count << 8, then the words) and kept with a copy of the
 * bytes it came from; each call of the list compares the bytes (the list's own memory may have changed) and then
 * applies the kept calls through gx_apply_call, as gx_dl_replay_port_op would have. What follows the prefix is
 * left to the caller's byte decoder. A list with no such prefix keeps a four-byte signature so the next call
 * does not scan again. A mismatch decodes the list afresh. Same calls, same order, same words: no picture change. */
int gx_opt_sdl;
#define SDL_SLOTS 4096
#define SDL_POOL_WORDS (512 * 1024) /* the most the pool holds before it starts again */
#define SDL_POOL_FIRST (16 * 1024)  /* what it starts with: it grows to what the lists need (gor_01: under 8 K words) */
typedef struct {
    const u8* list;
    u32 nbytes;
    u32 pbytes; /* bytes of the decoded prefix (0: none; sig holds the first four bytes) */
    u32 sig;
    u32 pos;    /* pool word of the copy of the prefix bytes; the calls follow it */
    u32 words;  /* pool words of the calls */
} SdlEntry;
static SdlEntry* s_sdl;
static u32* s_sdl_pool;
static u32 s_sdl_used, s_sdl_cap; /* words in use, words allocated (up to SDL_POOL_WORDS) */
static u32 s_sdl_hit, s_sdl_miss, s_sdl_stale, s_sdl_reset, s_sdl_lookups;

static inline int sdl_is_port(u32 op) { return op >= GXDL_OP_PORT && op != GXDL_OP_RAW_PRIM; }

/* Decodes the prefix into `out` (cap words); returns the prefix bytes, *nw gets the words written. */
u32 gx_sdl_scan(const u8* list, u32 nbytes, u32* out, u32 cap, u32* nw) {
    const u8* p = list;
    const u8* end = list + nbytes;
    u32 k = 0, good = 0;
    while (p < end) {
        u32 op = *p;
        if (op == 0x00 || op == 0x48 || op == 0x44) {
            p++;
            good = (u32)(p - list);
            continue;
        }
        if (!sdl_is_port(op) || end - p < 3) {
            break;
        }
        u32 id = p[1], n = p[2];
        if (n > 32 || k + 1 + n > cap) {
            break;
        }
        const u8* q = p + 3;
        u32 base = k;
        out[k++] = id | n << 8;
        int ok = 1;
        for (u32 i = 0; i < n && ok; i++) {
            u32 v = 0;
            for (u32 shift = 0;; shift += 7) {
                if (q >= end || shift > 28) {
                    ok = 0;
                    break;
                }
                u8 b = *q++;
                v |= (u32)(b & 0x7F) << shift;
                if (!(b & 0x80)) {
                    break;
                }
            }
            out[k++] = v;
        }
        if (!ok) {
            k = base;
            break;
        }
        p = q;
        good = (u32)(p - list);
        /* NOPs after the last call stay in `good` only up to here: trailing ones are counted at the next turn */
    }
    *nw = k;
    return good;
}

static u32 sdl_sig(const u8* list, u32 nbytes) {
    u32 s = 0;
    for (u32 i = 0; i < 4 && i < nbytes; i++) {
        s = s << 8 | list[i];
    }
    return s;
}

/* Applies the state calls at the start of the list; returns the bytes they covered (the caller runs the rest). */
u32 gx_sdl_run(const u8* list, u32 nbytes, u16* hint) {
    if (nbytes < 4) {
        return 0;
    }
    if (s_sdl == NULL) {
        s_sdl = gx_big_alloc("gxsdl entries", sizeof(SdlEntry) * SDL_SLOTS);
        s_sdl_pool = gx_big_alloc("gxsdl calls", sizeof(u32) * SDL_POOL_FIRST);
        s_sdl_cap = SDL_POOL_FIRST;
    }
    if ((++s_sdl_lookups & 0x3FFFF) == 0) {
        PLOG_I("gxsdl: %lu lookups: %lu hit, %lu decoded, %lu stale, %lu resets", (unsigned long)s_sdl_lookups,
               (unsigned long)s_sdl_hit, (unsigned long)s_sdl_miss, (unsigned long)s_sdl_stale,
               (unsigned long)s_sdl_reset);
    }
    u32 h = ((u32)(uintptr_t)list >> 5 ^ nbytes * 0x9E3779B1u) * 2654435761u >> 20;
    SdlEntry* e = NULL;
    SdlEntry* freeslot = NULL;
    if (hint != NULL && *hint != 0) {
        /* the slot this list had last time (the entry holds the same list and size, or it is looked up as before) */
        SdlEntry* c = &s_sdl[*hint - 1];
        if (c->list == list && c->nbytes == nbytes) {
            e = c;
        }
    }
    for (u32 i = 0; e == NULL && i < 4; i++) {
        SdlEntry* c = &s_sdl[(h + i) & (SDL_SLOTS - 1)];
        if (c->list == list && c->nbytes == nbytes) {
            e = c;
            break;
        }
        if (c->list == NULL && freeslot == NULL) {
            freeslot = c;
        }
    }
    if (e != NULL) {
        int same;
        if (e->pbytes == 0) {
            same = e->sig == sdl_sig(list, nbytes);
        } else {
            same = memcmp(list, s_sdl_pool + e->pos, e->pbytes) == 0;
        }
        if (same) {
            s_sdl_hit++;
            if (hint != NULL) {
                *hint = (u16)(e - s_sdl + 1);
            }
            if (e->pbytes == 0) {
                return 0;
            }
            const u32* c = s_sdl_pool + e->pos + ((e->pbytes + 3) >> 2);
            const u32* ce = c + e->words;
            while (c < ce) {
                u32 hd = *c++;
                gx_apply_call(hd & 0xFF, c);
                c += hd >> 8;
            }
            return e->pbytes;
        }
        s_sdl_stale++;
    } else {
        s_sdl_miss++;
        e = freeslot != NULL ? freeslot : &s_sdl[h & (SDL_SLOTS - 1)];
    }
    /* decode (again): the copy of the bytes, then the calls */
    if (s_sdl_used + (nbytes >> 2) + 8 + 1024 > SDL_POOL_WORDS) {
        memset(s_sdl, 0, sizeof(SdlEntry) * SDL_SLOTS);
        s_sdl_used = 0;
        s_sdl_reset++;
        e = &s_sdl[h & (SDL_SLOTS - 1)];
    }
    if (hint != NULL) {
        *hint = (u16)(e - s_sdl + 1);
    }
    /* the calls are scanned into the pool after room for the copy, whose size is not known yet: scan first */
    u32 room = SDL_POOL_WORDS - s_sdl_used;
    u32 cap = room - ((nbytes + 3) >> 2);
    /* the scan writes at most `nbytes` words (a call of 1 + n words takes at least 3 + n bytes): the pool grows to
     * hold them; the words and the reset points are those of a pool of SDL_POOL_WORDS */
    u32 need = s_sdl_used + ((nbytes + 3) >> 2) + (cap < nbytes ? cap : nbytes);
    if (need > s_sdl_cap) {
        u32 ncap = s_sdl_cap * 2;
        while (ncap < need) {
            ncap *= 2;
        }
        ncap = ncap < SDL_POOL_WORDS ? ncap : SDL_POOL_WORDS;
        u32* np = realloc(s_sdl_pool, ncap * sizeof(u32));
        if (np == NULL) {
            e->list = NULL; /* no room: this call runs the list's bytes as before */
            if (hint != NULL) {
                *hint = 0;
            }
            return 0;
        }
        s_sdl_pool = np;
        s_sdl_cap = ncap;
    }
    u32* scratch = s_sdl_pool + s_sdl_used + ((nbytes + 3) >> 2); /* the copy of the whole list would fit before */
    u32 nw = 0;
    u32 pbytes = gx_sdl_scan(list, nbytes, scratch, cap, &nw);
    e->list = list;
    e->nbytes = nbytes;
    e->sig = sdl_sig(list, nbytes);
    if (nw == 0) {
        e->pbytes = 0; /* nothing to apply here (no port opcode in the prefix): run it all as before */
        e->pos = e->words = 0;
        return 0;
    }
    u32 cw = (pbytes + 3) >> 2;
    memmove(s_sdl_pool + s_sdl_used + cw, scratch, nw * sizeof(u32));
    memcpy(s_sdl_pool + s_sdl_used, list, pbytes);
    e->pbytes = pbytes;
    e->pos = s_sdl_used;
    e->words = nw;
    s_sdl_used += cw + nw;
    const u32* c = s_sdl_pool + e->pos + cw;
    const u32* ce = c + nw;
    while (c < ce) {
        u32 hd = *c++;
        gx_apply_call(hd & 0xFF, c);
        c += hd >> 8;
    }
    return pbytes;
}

void gx_apply_call(u32 id, const u32* w) {
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
    case REC_SetZScaleOffset: GXSetZScaleOffset(U2F(w[0]), U2F(w[1])); break;
    case REC_SetScissorBoxOffset: GXSetScissorBoxOffset((s32)w[0], (s32)w[1]); break;
    case REC_SetIndTexOrder: GXSetIndTexOrder((GXIndTexStageID)w[0], (GXTexCoordID)w[1], (GXTexMapID)w[2]); break;
    case REC_SetIndTexCoordScale:
        GXSetIndTexCoordScale((GXIndTexStageID)w[0], (GXIndTexScale)w[1], (GXIndTexScale)w[2]);
        break;
    case REC_SetIndTexMtx: {
        f32 m[2][3];
        for (int i = 0; i < 6; i++) {
            m[i / 3][i % 3] = U2F(w[1 + i]);
        }
        GXSetIndTexMtx((GXIndTexMtxID)w[0], m, (s8)(s32)w[7]);
        break;
    }
    case REC_SetDstAlpha: GXSetDstAlpha((GXBool)w[0], (u8)w[1]); break;
    case REC_SetPixelFmt: GXSetPixelFmt((GXPixelFmt)w[0], (GXZFmt16)w[1]); break;
    case REC_SetZTexture: GXSetZTexture((GXZTexOp)w[0], (GXTexFmt)w[1], w[2]); break;
    case REC_SetCopyClear: GXSetCopyClear(u2c(w[0]), w[1]); break;
    case REC_SetDispCopySrc: GXSetDispCopySrc((u16)w[0], (u16)w[1], (u16)w[2], (u16)w[3]); break;
    case REC_SetDispCopyDst: GXSetDispCopyDst((u16)w[0], (u16)w[1]); break;
    case REC_SetDispCopyYScale: GXSetDispCopyYScale(U2F(w[0])); break;
    case REC_SetTexCopySrc: GXSetTexCopySrc((u16)w[0], (u16)w[1], (u16)w[2], (u16)w[3]); break;
    case REC_SetTexCopyDst: GXSetTexCopyDst((u16)w[0], (u16)w[1], (GXTexFmt)w[2], (GXBool)w[3]); break;
    case REC_CopyTex: GXCopyTex((void*)w[0], (GXBool)w[1]); break;
    case REC_CopyDisp: gx_copy_disp((void*)w[0], (GXBool)w[1], (const void*)w[2]); break;
    case REC_DrawDone: GXDrawDone(); break;
    case REC_SetDrawSync: GXSetDrawSync((u16)w[0]); break;
    case REC_ClearBoundingBox: GXClearBoundingBox(); break;
    default: plat_fatal("GX: unknown port opcode %lu in a display list", (unsigned long)id);
    }
}

/* The game thread's shadow: what its getters and its display-list recording
 * read back (vertex description and formats, projection, viewport, scissor,
 * cull mode, the copy source and the draw-sync token), and the arrays, for
 * the snapshots of GXBegin primitives (gx_rec.c). */
void gx_shadow_call(u32 id, const u32* w) {
    GxState* g = &gx_game;
    switch (id) {
    case REC_SetVtxDesc: vtx_desc_set(g, w[0], w[1]); break;
    case REC_ClearVtxDesc: vtx_desc_clear(g); break;
    case REC_SetVtxAttrFmt: vat_set(g, w[0], w[1], w[2], w[3], (u8)w[4]); break;
    case REC_SetArray: {
        u32 a = w[0] == GX_VA_NBT ? GX_VA_NRM : w[0];
        if (a < GX_VA_MAX_ATTR) {
            g->array_base[a] = (const u8*)w[1];
            g->array_stride[a] = (u8)w[2];
        }
        break;
    }
    case REC_SetProjection:
        for (int i = 0; i < 7; i++) {
            g->proj[i] = U2F(w[i]);
        }
        break;
    case REC_SetViewport:
        for (int i = 0; i < 6; i++) {
            g->vp[i] = U2F(w[i]);
        }
        break;
    case REC_SetScissor: memcpy(g->scissor, w, sizeof(g->scissor)); break;
    case REC_SetCullMode: g->cull = (u8)w[0]; break;
    case REC_SetDispCopySrc:
    case REC_SetTexCopySrc:
        for (int i = 0; i < 4; i++) {
            g->copy_src[i] = (u16)w[i];
        }
        break;
    case REC_SetDrawSync: g->draw_sync_token = (u16)w[0]; break;
    /* gxsplit stage 2: the cold calls that write bytes the hot setters' regions hold (a TEV stage, a TEV register),
     * as GXSetTevIndWarp and GXSetTevColorS10 write them */
    case REC_SetTevIndirect: {
        const u32 st = w[0];
        if (st < GX_MAX_TEV) {
            g->tev[st].ind_enabled = 1;
            g->tev[st].ind_stage = (u8)w[1];
            g->tev[st].ind_fmt = GX_ITF_8;
            g->tev[st].ind_bias = (u8)w[2] ? GX_ITB_STU : GX_ITB_NONE;
            g->tev[st].ind_mtx = (u8)w[4];
            g->tev[st].ind_wrap_s = (u8)w[3] ? 6 : 0;
            g->tev[st].ind_wrap_t = (u8)w[3] ? 6 : 0;
            g->tev[st].ind_add_prev = 0;
        }
        break;
    }
    case REC_SetTevColorS10:
        if (w[0] < 4) {
            g->tev_reg[w[0]] = (GXColorS10){(s16)(w[1] >> 16), (s16)w[1], (s16)(w[2] >> 16), (s16)w[2]};
        }
        break;
    default: break;
    }
}

/* gxsplit stage 2: a hot setter's call by its words (as gx_apply_call decodes them) on the game thread's state, with
 * its bytes marked for the next EV_STATE. 0: not one of them (the caller records it). */
int gx_gs_call(u32 id, const u32* w) {
    switch (id) {
    case REC_SetVtxDesc: vtx_desc_gs((GXAttr)w[0], (GXAttrType)w[1]); return 1;
    case REC_ClearVtxDesc: clear_vtx_desc_gs(); return 1;
    case REC_SetVtxAttrFmt:
        vtx_attr_fmt_gs((GXVtxFmt)w[0], (GXAttr)w[1], (GXCompCnt)w[2], (GXCompType)w[3], (u8)w[4]);
        return 1;
    case REC_SetArray: set_array_gs((GXAttr)w[0], (void*)w[1], (u8)w[2]); return 1;
    case REC_LoadPosMtx: {
        f32 m[3][4];
        memcpy(m, w + 1, sizeof(m));
        load_pos_mtx_gs(m, w[0]);
        return 1;
    }
    case REC_LoadNrmMtx: {
        f32 m[3][4] = {{0}};
        for (int i = 0; i < 9; i++) {
            m[i / 3][i % 3] = U2F(w[1 + i]);
        }
        load_nrm_mtx_gs(m, w[0]);
        return 1;
    }
    case REC_LoadTexMtx: {
        f32 m[3][4];
        memcpy(m, w + 2, sizeof(m));
        load_tex_mtx_gs(m, w[0], (GXTexMtxType)w[1]);
        return 1;
    }
    case REC_SetCurrentMtx: set_current_mtx_gs(w[0]); return 1;
    case REC_SetTexCoordGen2:
        tex_coord_gen_gs((GXTexCoordID)w[0], (GXTexGenType)w[1], (GXTexGenSrc)w[2], w[3], (GXBool)w[4], w[5]);
        return 1;
    case REC_SetNumTexGens: num_texgens_gs((u8)w[0]); return 1;
    case REC_SetNumChans: num_chans_gs((u8)w[0]); return 1;
    case REC_SetChanCtrl:
        chan_ctrl_gs((GXChannelID)w[0], (GXBool)w[1], (GXColorSrc)w[2], (GXColorSrc)w[3], w[4], (GXDiffuseFn)w[5],
                     (GXAttnFn)w[6]);
        return 1;
    case REC_SetChanAmbColor: chan_amb_gs((GXChannelID)w[0], u2c(w[1])); return 1;
    case REC_SetChanMatColor: chan_mat_gs((GXChannelID)w[0], u2c(w[1])); return 1;
    case REC_SetNumTevStages: num_tev_gs((u8)w[0]); return 1;
    case REC_SetTevOrder:
        tev_order_gs((GXTevStageID)w[0], (GXTexCoordID)w[1], (GXTexMapID)w[2], (GXChannelID)w[3]);
        return 1;
    case REC_SetTevColorIn:
        tev_color_in_gs((GXTevStageID)w[0], (GXTevColorArg)w[1], (GXTevColorArg)w[2], (GXTevColorArg)w[3],
                        (GXTevColorArg)w[4]);
        return 1;
    case REC_SetTevAlphaIn:
        tev_alpha_in_gs((GXTevStageID)w[0], (GXTevAlphaArg)w[1], (GXTevAlphaArg)w[2], (GXTevAlphaArg)w[3],
                        (GXTevAlphaArg)w[4]);
        return 1;
    case REC_SetTevColorOp:
        tev_color_op_gs((GXTevStageID)w[0], (GXTevOp)w[1], (GXTevBias)w[2], (GXTevScale)w[3], (GXBool)w[4],
                        (GXTevRegID)w[5]);
        return 1;
    case REC_SetTevAlphaOp:
        tev_alpha_op_gs((GXTevStageID)w[0], (GXTevOp)w[1], (GXTevBias)w[2], (GXTevScale)w[3], (GXBool)w[4],
                        (GXTevRegID)w[5]);
        return 1;
    case REC_SetTevColor: tev_color_gs((GXTevRegID)w[0], u2c(w[1])); return 1;
    case REC_SetTevKColor: tev_kcolor_gs((GXTevKColorID)w[0], u2c(w[1])); return 1;
    case REC_SetTevKColorSel: tev_ksel_gs((GXTevStageID)w[0], (GXTevKColorSel)w[1]); return 1;
    case REC_SetTevKAlphaSel: tev_kasel_gs((GXTevStageID)w[0], (GXTevKAlphaSel)w[1]); return 1;
    case REC_SetTevSwapMode: tev_swap_gs((GXTevStageID)w[0], (GXTevSwapSel)w[1], (GXTevSwapSel)w[2]); return 1;
    case REC_SetTevSwapModeTable:
        tev_swap_table_gs((GXTevSwapSel)w[0], (GXTevColorChan)w[1], (GXTevColorChan)w[2], (GXTevColorChan)w[3],
                          (GXTevColorChan)w[4]);
        return 1;
    case REC_SetTevDirect: tev_direct_gs((GXTevStageID)w[0]); return 1;
    case REC_SetNumIndStages: num_ind_gs((u8)w[0]); return 1;
    case REC_LoadTexObj: {
        GXTexObj obj;
        memcpy(&obj, w + 1, sizeof(GxTexObj));
        load_tex_obj_gs(&obj, (GXTexMapID)w[0], NULL);
        return 1;
    }
    case REC_SetAlphaCompare:
        alpha_compare_gs((GXCompare)w[0], (u8)w[1], (GXAlphaOp)w[2], (GXCompare)w[3], (u8)w[4]);
        return 1;
    case REC_SetBlendMode:
        blend_mode_gs((GXBlendMode)w[0], (GXBlendFactor)w[1], (GXBlendFactor)w[2], (GXLogicOp)w[3]);
        return 1;
    case REC_SetZMode: z_mode_gs((GXBool)w[0], (GXCompare)w[1], (GXBool)w[2]); return 1;
    case REC_SetZCompLoc: z_comp_loc_gs((GXBool)w[0]); return 1;
    case REC_SetColorUpdate: color_update_gs((GXBool)w[0]); return 1;
    case REC_SetAlphaUpdate: alpha_update_gs((GXBool)w[0]); return 1;
    case REC_SetCullMode: cull_mode_gs((GXCullMode)w[0]); return 1;
    default: return 0;
    }
}

/* ---- Defaults (the SDK's GXInit state) ---- */

void gx_state_init(u16 efb_w, u16 efb_h) {
    gx_opts_init();
    memset(&gx, 0, sizeof(gx));
    gx_vdesc_ver[0]++;
    gx_dd_apply_forget(); /* the setters' table described the state before this */
    gx_dirty = GXD_ALL;    /* gxdirty: everything is built again */
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
