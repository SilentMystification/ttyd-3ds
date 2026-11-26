/* GX call recording and the translator (switch gxthread, PORT_3DS_MULTICORE.md "A").
 *
 * With gxthread=1..3 the game thread does not translate GX: every GX call it
 * makes is appended, by value, to a ring of words, and a translator replays
 * the ring through the same GX functions with gx_tls_translator set, which
 * makes them act on `gx` instead of recording again:
 *   - state calls: the call's id and its words (gx_apply_call);
 *   - GXCallDisplayList: the list's address and size (the bytes and arrays
 *     are read when it is translated, see the fences below);
 *   - GXBegin/GXEnd: the pipe writes, copied;
 *   - raw pipe commands (GXCmd outside a primitive): copied.
 * The game thread's getters read `gx_game`, a shadow updated from the calls
 * it records (gx_shadow_call), so it never reads state a translator is
 * changing. Display lists the game builds (GXBeginDisplayList) are still
 * encoded on the game thread, from the shadow's vertex state.
 *
 * Who translates: gxthread=1 and 3, the game thread itself at each fence;
 * gxthread=2, a thread on core 2 that follows the ring as it fills. With
 * gxthread=2 the game thread also translates the start of each frame itself
 * (gx_rec_direct), while core 2 still replays the frame before; after
 * s_switch_at display-list calls it starts recording. The split moves by
 * hill climbing on the frame's work time (switch gxthreadsplit=0: record
 * everything; gxthreadsplitat=N: a fixed split).
 *
 * Fences (gx_rec_fence): the caller waits until everything recorded so far is
 * translated. They are the points where the GameCube game waits for the GPU
 * or needs its result:
 *   - GXReadDrawSync (the draw-sync token: the frame's end in
 *     __NoHangDoneRender, sysWaitDrawSync before the heap moves or reuses
 *     freed memory), GXDrawDone;
 *   - read-backs: GXReadBoundingBox, GXPeekARGB, GXInit;
 *   - memory changing owner: a plat_memstable_release reported by the game
 *     thread while translation is pending (switch gxthreadrel, default 1), so
 *     a list or array freed and reused is never read after the reuse;
 *   - a full ring.
 * Between fences the translator reads game memory later than the call was
 * made, as the GameCube's GPU does; the GameCube game keeps that memory
 * unchanged until it waits for the GPU (the late-read audit in
 * PORT_3DS_MULTICORE.md).
 *
 * gxthread=3 (verify) translates like 1 and checks: every getter against the
 * translated state, every list's bytes at translation against a hash taken
 * when it was recorded, and the memory each translated call reads against
 * the memory reports the game thread made after recording it.
 *
 * Game-agnostic: no TTYD knowledge. */
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"
#include "plat/memstable.h"
#include "plat/plat.h"
#include "plat/pmu.h"
#include "plat/thread.h"

int gx_rec_mode;
__thread int gx_tls_translator;
int gx_rec_direct;
GxState gx_game;

static __thread int t_game; /* the thread that called GXInit */

enum { GXR_WRAP = 0xF0, GXR_LIST, GXR_PRIM, GXR_RAW, GXR_PRIMD };
#define HDR(type, n) ((u32)(type) << 24 | (n))
#define KICK_WORDS 256u

static u32* s_ring;
static u32 s_ring_words, s_ring_mask;
static u32 s_wr;     /* game thread: next word */
static u32 s_wr_pub; /* published to the translator thread */
static u32 s_rd;     /* translating side: next word */
static u32 s_rd_pub; /* published to the game thread */
static u32 s_cur;    /* translating side: the command being translated */
static PlatEvent* s_ev_work;
static PlatEvent* s_ev_done;
static int s_sleeping, s_want_done;
static int s_fence_release = 1;
static int s_excl;

/* counters: the game thread's (gs), the translator's (ts) */
static struct {
    u32 frames, words, cmds, lists, prims;
    u32 fences[GXF_COUNT];
    u64 t_fence[GXF_COUNT];
    u64 t_frame, t_idle;
} s_gs;
static u64 s_last_frame_tick, s_idle_from;
static u32 s_frame_no; /* game frames (GXCopyDisp calls) */
static int s_after_copydisp;
static u32 s_stats_every;
/* gxthread=2 split (switch gxthreadsplit, default 1): each frame the game thread translates its first
 * s_switch_at display-list calls itself, while core 2 replays the last frame, then records the rest for
 * the translator. The split follows the frame-end fence: a wait moves it later, none moves it earlier. */
static int s_split = 1;
static u32 s_switch_at = 1024, s_lists_direct;
static u32 s_direct_sum; /* lists translated directly, summed for the counters */
static int s_split_fixed; /* switch gxthreadsplitat=N: a fixed split (no adapting), for measuring */
static u32 s_lists_rec;     /* lists recorded this frame */
static u64 s_work_start;    /* the frame's start (DEMOBeforeRender) */
static u64 s_win_sum, s_win_prev;
static u32 s_win_frames, s_step = 256, s_better;
static s32 s_dir = -1;
static u64 s_ts_busy; /* translator thread busy ticks (atomic adds) */
static u64 s_ts_present; /* translating GXCopyDisp (the frame's submit) */
static u32 s_snap_mism; /* snapshots not used: the translator's vertex state or array differed */

/* verify */
#define REP_MAX 4096
static struct {
    uintptr_t a, b;
    u32 pos;
    u8 kind;
} s_rep[REP_MAX];
static u32 s_nrep, s_rep_dropped;
static u32 s_hazard[4][GXV_READ_COUNT]; /* by report kind (fill, write, release) and what was read */
static u32 s_list_changed, s_getter_mism[GXG_COUNT];
static u32 s_logged;

static const char* const k_read_name[GXV_READ_COUNT] = {"list", "array", "tex", "tlut"};
static const char* const k_kind_name[4] = {"?", "fill", "write", "release"};
static const char* const k_getter_name[GXG_COUNT] = {"vtxdesc", "vat", "proj", "viewport", "scissor", "cull", "copysrc"};

static inline u32 load_sc(const u32* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void store_sc(u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline int load_sci(const int* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void store_sci(int* p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

/* ---- Translating ---- */

static u32 vtx_state_hash(const GxState* g, u32 fmt);

static void check_textures(void) {
    for (int s = 0; s < gx.num_tev && s < GX_MAX_TEV; s++) {
        u32 m = gx.tev[s].map;
        if (m >= GX_MAX_TEXMAP || !gx.tex_map_valid[m]) {
            continue;
        }
        const GxTexObj* t = &gx.tex_map[m];
        if (t->copy_handle != 0) {
            continue;
        }
        gx_rec_check_read(t->image, GXGetTexBufferSize(t->width, t->height, t->format, t->mipmap, t->max_lod_q / 16 + 1),
                          GXV_READ_TEX);
        if (t->is_ci && t->tlut_name < 20 && gx.tlut[t->tlut_name].data != NULL) {
            gx_rec_check_read(gx.tlut[t->tlut_name].data, (u32)gx.tlut[t->tlut_name].n_entries * 2, GXV_READ_TLUT);
        }
    }
}

static void run_one(u32 type, const u32* w, u32 n) {
    switch (type) {
    case GXR_LIST: {
        const void* list = (const void*)w[0];
        if (gx_rec_mode == 3) {
            if (gx_hash32(list, w[1], 0x5EC0u) != w[3]) {
                if (s_list_changed++ < 8) {
                    PLOG_W("gxthread verify: list %p (%lu bytes, called from %p) changed between its call and its "
                           "translation", list, (unsigned long)w[1], (const void*)w[2]);
                }
            }
            gx_rec_check_read(list, w[1], GXV_READ_LIST);
        }
        gx_call_list(list, w[1], (const void*)w[2]);
        if (gx_rec_mode == 3) {
            PlatMemSpan sp[16];
            u32 ns = gx_dlcache_spans(list, w[1], sp, 16);
            for (u32 i = 0; i < ns; i++) {
                gx_rec_check_read(sp[i].p, sp[i].n, GXV_READ_ARRAY);
            }
            check_textures();
        }
        break;
    }
    case GXR_PRIM: {
        u32 nslots = w[3], nsnap = w[4];
        const u8* saved[GX_VA_MAX_ATTR];
        u32 used = 0; /* attributes drawn from a snapshot (bit each) */
        if (nsnap != 0) {
            if (vtx_state_hash(&gx, w[0] & 7) != w[5]) {
                s_snap_mism++; /* the translator's vertex state is not the shadow's: read live */
            } else {
                const u32* q = w + 6 + nslots;
                for (u32 i = 0; i < nsnap; i++) {
                    u32 a = q[0];
                    if (a < GX_VA_MAX_ATTR && (u32)gx.array_base[a] == q[1] && gx.array_stride[a] == q[2]) {
                        saved[a] = gx.array_base[a];
                        gx.array_base[a] = (const u8*)(q + 5) - q[3] * q[2];
                        used |= 1u << a;
                    } else {
                        s_snap_mism++;
                    }
                    q += 5 + (q[4] + 3) / 4;
                }
            }
        }
        gx_draw_prim((u8)w[0], (u16)w[1], (const void*)w[2], w + 6, nslots);
        for (u32 a = 0; used != 0; a++, used >>= 1) {
            if (used & 1) {
                gx.array_base[a] = saved[a];
            }
        }
        if (gx_rec_mode == 3) {
            check_textures();
        }
        break;
    }
    case GXR_RAW: gx_run_raw(w, n); break;
    case GXR_PRIMD: {
        /* gximmdirect: the vertices are in the game thread's chunks since GXEnd */
        u8 slots[8];
        memcpy(slots, w + 6, 8);
        gx_draw_prim_ring((u8)w[0], (u16)w[1], (const void*)w[2], (const u8*)w[3], w[4], slots, (int)(w[8] & 1),
                          vtx_state_hash(&gx, w[0] & 7) == w[9]);
        gx_vsh_gimm_translated(w[5]);
        if (gx_rec_mode == 3) {
            check_textures();
        }
        break;
    }
    case REC_CopyDisp: {
        /* the frame's end: its submit waits for the replay of the last frame (counted apart) */
        u64 t0 = plat_ticks();
        plat_pmu_begin(PLAT_PMU_PRESENT);
        gx_apply_call(type, w);
        plat_pmu_end(PLAT_PMU_PRESENT);
        __atomic_fetch_add(&s_ts_present, plat_ticks() - t0, __ATOMIC_RELAXED);
        break;
    }
    default: gx_apply_call(type, w); break;
    }
}

/* Translates the ring up to word `upto` (the caller has gx_tls_translator set). */
static int s_null; /* switch gxnull=1 (measuring only): the record is dropped, nothing is translated or drawn */

static void consume(u32 upto) {
    if (s_null) {
        s_rd = upto;
        store_sc(&s_rd_pub, s_rd);
        return;
    }
    u32 pub = s_rd;
    while (s_rd != upto) {
        const u32* c = s_ring + (s_rd & s_ring_mask);
        u32 type = c[0] >> 24, n = c[0] & 0xFFFFFFu;
        if (type != GXR_WRAP) {
            s_cur = s_rd;
            run_one(type, c + 1, n);
        }
        s_rd += n + 1;
        if (gx_rec_mode == 2 && s_rd - pub >= 4096) {
            store_sc(&s_rd_pub, s_rd); /* room for the game thread */
            pub = s_rd;
        }
    }
    store_sc(&s_rd_pub, s_rd);
}

static void translator_main(void* arg) {
    (void)arg;
    gx_tls_translator = 1;
    for (;;) {
        u32 w = load_sc(&s_wr_pub);
        if (w != s_rd) {
            u64 t0 = plat_ticks();
            plat_pmu_begin(PLAT_PMU_TRANSLATOR);
            consume(w);
            plat_pmu_end(PLAT_PMU_TRANSLATOR);
            __atomic_fetch_add(&s_ts_busy, plat_ticks() - t0, __ATOMIC_RELAXED);
            continue;
        }
        if (load_sci(&s_want_done)) {
            plat_event_signal(s_ev_done);
        }
        store_sci(&s_sleeping, 1);
        if (load_sc(&s_wr_pub) != s_rd) {
            store_sci(&s_sleeping, 0);
            continue;
        }
        plat_event_wait(s_ev_work);
        store_sci(&s_sleeping, 0);
    }
}

/* ---- Recording (game thread) ---- */

static void publish(void) {
    if (gx_rec_mode != 2) {
        return;
    }
    store_sc(&s_wr_pub, s_wr);
    if (load_sci(&s_sleeping)) {
        plat_event_signal(s_ev_work);
    }
}

static u32 rd_seen(void) { return gx_rec_mode == 2 ? load_sc(&s_rd_pub) : s_rd; }

/* Room for a command of n words; returns its payload. s_room_end: the producer may write below it (the
 * translator's position, read again only when the command would not fit before it). */
static u32 s_room_end;

static u32* put_slow(u32 type, u32 n) {
    u32 need = n + 1;
    u32 off = s_wr & s_ring_mask;
    u32 pad = off + need > s_ring_words ? s_ring_words - off : 0;
    if (need + pad > s_ring_words / 2) {
        plat_fatal("gxthread: a command of %lu words does not fit the ring", (unsigned long)n);
    }
    for (;;) {
        s_room_end = rd_seen() + s_ring_words;
        if ((s32)(s_room_end - (s_wr + need + pad)) >= 0) {
            break;
        }
        gx_rec_fence(GXF_FULL);
    }
    if (pad != 0) {
        s_ring[off] = HDR(GXR_WRAP, pad - 1);
        s_wr += pad;
        off = 0;
    }
    u32* c = s_ring + off;
    c[0] = HDR(type, n);
    return c + 1;
}

static inline u32* put(u32 type, u32 n) {
    u32 off = s_wr & s_ring_mask;
    if (__builtin_expect(off + n + 1 <= s_ring_words && (s32)(s_room_end - (s_wr + n + 1)) >= 0, 1)) {
        u32* c = s_ring + off;
        c[0] = HDR(type, n);
        return c + 1;
    }
    return put_slow(type, n);
}

static inline void commit(u32 n, int kick) {
    s_wr += n + 1;
    s_gs.cmds++;
    s_gs.words += n + 1;
    if (gx_rec_mode == 2 && (kick || s_wr - s_wr_pub >= KICK_WORDS)) {
        publish();
    }
}

/* ---- Only state that changed (switch gxthreaddedup, default 1) ----
 * The game sets most state again before each draw with the value in force
 * (TEV, blend, vertex format, texture objects). A call whose words equal the
 * last ones recorded for the same piece of state (its slot) is not recorded:
 * the translator's state already holds them. Slots are chosen so that no two
 * slots' calls write the same field (calls that can overlap share a slot and
 * the stored words include the call id; the colour-channel calls, whose
 * channel selectors overlap, keep only the last one valid). Everything is
 * forgotten when the translator's state can change behind the record: a call
 * to a display list the game built with state calls in it, an exclusive
 * section or the split's direct part. A call skipped this way would only have
 * bumped the state version again: build_state is a function of the state. */
#define DD_WORDS 14
#define DD_SLOTS 768
typedef struct {
    u32 gen;
    u8 id, n;
    u32 w[DD_WORDS];
} DdSlot;
static DdSlot* s_dd; /* DD_SLOTS, allocated with the ring (the image must stay small) */
static u32 s_dd_gen = 1;
static int s_dedup = 1;
static u32 s_dd_skipped, s_dd_calls;

static inline u32 nbt_nrm(u32 a) { return a == GX_VA_NBT ? GX_VA_NRM : a; }

/* The slot of call `id`, or -1 (always recorded). */
static int dd_slot(u32 id, const u32* w) {
    enum { B_VTX = 0, B_VAT = 32, B_ARR = 32 + 8 * 32, B_POSMTX = B_ARR + 32, B_NRMMTX = B_POSMTX + 10,
           B_TEXMTX = B_NRMMTX + 10, B_TG = B_TEXMTX + 64, B_CHAN = B_TG + 8, B_LIGHT = B_CHAN + 18,
           B_TEV = B_LIGHT + 8, B_KCOLOR = B_TEV + 16 * 9, B_SWAPT = B_KCOLOR + 4, B_TEVREG = B_SWAPT + 4,
           B_TEXOBJ = B_TEVREG + 4, B_TLUT = B_TEXOBJ + 8, B_IND = B_TLUT + 20, B_GLOBAL = B_IND + 12 };
    _Static_assert(B_GLOBAL + 40 <= DD_SLOTS, "dedup slots");
    switch (id) {
    case REC_SetVtxDesc: return w[0] < 32 ? B_VTX + (int)nbt_nrm(w[0]) : -1;
    case REC_SetVtxAttrFmt: return w[0] < 8 && w[1] < 32 ? B_VAT + (int)(w[0] * 32 + nbt_nrm(w[1])) : -1;
    case REC_SetArray: return w[0] < 32 ? B_ARR + (int)nbt_nrm(w[0]) : -1;
    case REC_LoadPosMtx: return w[0] / 3 < 10 ? B_POSMTX + (int)(w[0] / 3) : -1;
    case REC_LoadNrmMtx: return w[0] / 3 < 10 ? B_NRMMTX + (int)(w[0] / 3) : -1;
    case REC_LoadTexMtx: return w[0] / 3 < 64 ? B_TEXMTX + (int)(w[0] / 3) : -1;
    case REC_SetTexCoordGen2: return w[0] < 8 ? B_TG + (int)w[0] : -1;
    case REC_SetChanCtrl: return w[0] < 6 ? B_CHAN + (int)w[0] : -1;
    case REC_SetChanAmbColor: return w[0] < 6 ? B_CHAN + 6 + (int)w[0] : -1;
    case REC_SetChanMatColor: return w[0] < 6 ? B_CHAN + 12 + (int)w[0] : -1;
    case REC_LoadLightObj: {
        u32 l = w[0] & 0xFF;
        return l != 0 && (l & (l - 1)) == 0 ? B_LIGHT + (31 - __builtin_clz(l)) : -1;
    }
    case REC_SetTevOrder: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 0 : -1;
    case REC_SetTevColorIn: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 1 : -1;
    case REC_SetTevAlphaIn: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 2 : -1;
    case REC_SetTevColorOp: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 3 : -1;
    case REC_SetTevAlphaOp: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 4 : -1;
    case REC_SetTevKColorSel: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 5 : -1;
    case REC_SetTevKAlphaSel: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 6 : -1;
    case REC_SetTevSwapMode: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 7 : -1;
    case REC_SetTevDirect:
    case REC_SetTevIndirect: return w[0] < 16 ? B_TEV + (int)w[0] * 9 + 8 : -1; /* both write ind_enabled */
    case REC_SetTevKColor: return w[0] < 4 ? B_KCOLOR + (int)w[0] : -1;
    case REC_SetTevSwapModeTable: return w[0] < 4 ? B_SWAPT + (int)w[0] : -1;
    case REC_SetTevColor:
    case REC_SetTevColorS10: return w[0] < 4 ? B_TEVREG + (int)w[0] : -1; /* one register */
    case REC_LoadTexObj: return w[0] < 8 ? B_TEXOBJ + (int)w[0] : -1;
    case REC_LoadTlut: return w[0] < 20 ? B_TLUT + (int)w[0] : -1;
    case REC_SetIndTexOrder: return w[0] < 4 ? B_IND + (int)w[0] : -1;
    case REC_SetIndTexCoordScale: return w[0] < 4 ? B_IND + 4 + (int)w[0] : -1;
    case REC_SetIndTexMtx: return w[0] >= GX_ITM_0 && w[0] < GX_ITM_0 + 3 ? B_IND + 8 + (int)(w[0] - GX_ITM_0) : -1;
    case REC_SetDispCopySrc:
    case REC_SetTexCopySrc: return B_GLOBAL + 0; /* one copy source */
    case REC_SetCurrentMtx: return B_GLOBAL + 1;
    case REC_SetNumTexGens: return B_GLOBAL + 2;
    case REC_SetNumChans: return B_GLOBAL + 3;
    case REC_SetNumTevStages: return B_GLOBAL + 4;
    case REC_SetAlphaCompare: return B_GLOBAL + 5;
    case REC_SetBlendMode: return B_GLOBAL + 6;
    case REC_SetZMode: return B_GLOBAL + 7;
    case REC_SetZCompLoc: return B_GLOBAL + 8;
    case REC_SetColorUpdate: return B_GLOBAL + 9;
    case REC_SetAlphaUpdate: return B_GLOBAL + 10;
    case REC_SetCullMode: return B_GLOBAL + 11;
    case REC_SetFog: return B_GLOBAL + 12;
    case REC_SetScissor: return B_GLOBAL + 13;
    case REC_SetViewport: return B_GLOBAL + 14;
    case REC_SetProjection: return B_GLOBAL + 15;
    case REC_SetLineWidth: return B_GLOBAL + 16;
    case REC_SetNumIndStages: return B_GLOBAL + 17;
    case REC_SetZScaleOffset: return B_GLOBAL + 18;
    case REC_SetScissorBoxOffset: return B_GLOBAL + 19;
    case REC_SetDstAlpha: return B_GLOBAL + 20;
    case REC_SetPixelFmt: return B_GLOBAL + 21;
    case REC_SetZTexture: return B_GLOBAL + 22;
    case REC_SetCopyClear: return B_GLOBAL + 23;
    case REC_SetDispCopyDst: return B_GLOBAL + 24;
    case REC_SetDispCopyYScale: return B_GLOBAL + 25;
    case REC_SetTexCopyDst: return B_GLOBAL + 26;
    default: return -1; /* ClearVtxDesc (see below), copies, syncs, the bounding box */
    }
}

static void dd_forget(void) {
    if (++s_dd_gen == 0) {
        memset(s_dd, 0, DD_SLOTS * sizeof(DdSlot));
        s_dd_gen = 1;
    }
}

/* Lists the game built with state calls in them (their start addresses). */
#define SL_SLOTS 2048
static const void** s_state_lists; /* SL_SLOTS, allocated with the ring */

static u32 sl_hash(const void* p) { return ((u32)p >> 5) * 2654435761u >> 21; }

void gx_rec_state_list(const void* list) {
    if (!gx_rec_mode || s_state_lists == NULL) {
        return;
    }
    u32 h = sl_hash(list);
    for (u32 i = 0; i < SL_SLOTS; i++, h = (h + 1) & (SL_SLOTS - 1)) {
        if (s_state_lists[h] == list) {
            return;
        }
        if (s_state_lists[h] == NULL) {
            s_state_lists[h] = list;
            return;
        }
    }
    s_dedup = 0; /* table full: stop skipping calls (never seen) */
    PLOG_W("gxthread: more than %d display lists with state calls: dedup off", SL_SLOTS);
}

static int is_state_list(const void* list) {
    u32 h = sl_hash(list);
    for (u32 i = 0; i < SL_SLOTS; i++, h = (h + 1) & (SL_SLOTS - 1)) {
        if (s_state_lists[h] == list) {
            return 1;
        }
        if (s_state_lists[h] == NULL) {
            return 0;
        }
    }
    return 0;
}

/* 1: table `tbl` (at generation `gen`) holds call `id` with these words; else it notes them. */
static int dd_same(DdSlot* tbl, u32 gen, u32 id, const u32* w, u32 n) {
    int sl = n != 0 && n <= DD_WORDS ? dd_slot(id, w) : -1;
    if (sl >= 0) {
        DdSlot* d = &tbl[sl];
        if (d->gen == gen && d->id == id && d->n == n && gx_mem_equal(d->w, w, n * 4)) {
            return 1;
        }
        d->gen = gen;
        d->id = (u8)id;
        d->n = (u8)n;
        memcpy(d->w, w, n * 4);
        if (id == REC_SetChanCtrl || id == REC_SetChanAmbColor || id == REC_SetChanMatColor) {
            /* channel selectors overlap (COLOR0A0 covers COLOR0 and ALPHA0): only this one stays valid */
            int base = sl - (int)w[0];
            for (int c = 0; c < 6; c++) {
                if (c != (int)w[0]) {
                    tbl[base + c].gen = 0;
                }
            }
        }
    } else if (id == REC_ClearVtxDesc) {
        for (int a = 0; a < 32; a++) {
            tbl[a].gen = 0; /* the vertex description slots */
        }
    }
    return 0;
}

/* ---- The same table where calls are applied (switch gxstatecache) ----
 * `gx` holds what the setters last wrote, so a setter called again with the words in force has nothing to do:
 * it returns before the state version changes, and the next draw does not rebuild the PICA state for it. The
 * table is kept by the setters themselves (gx_state.c's REC macros ask before they write), on whichever thread
 * applies calls; with a translator the record's own table has dropped most repeats already. */
int gx_opt_statecache;
int gx_opt_statededup;
static DdSlot* s_dda;
static u32 s_dda_gen = 1;
static u32 s_dda_skipped, s_dda_calls;

int gx_dd_apply_same(u32 id, const u32* w, u32 n) {
    s_dda_calls++;
    if (dd_same(s_dda, s_dda_gen, id, w, n)) {
        s_dda_skipped++;
        return 1;
    }
    return 0;
}

void gx_dd_apply_forget(void) {
    if (s_dda != NULL && ++s_dda_gen == 0) {
        memset(s_dda, 0, DD_SLOTS * sizeof(DdSlot));
        s_dda_gen = 1;
    }
}

void gx_rec_call(u32 id, const u32* w, u32 n) {
    if (s_dedup) {
        s_dd_calls++;
        if (dd_same(s_dd, s_dd_gen, id, w, n)) {
            s_dd_skipped++;
            return; /* the translator's state holds it already */
        }
    }
    u32* c = put(id, n);
    for (u32 i = 0; i < n; i++) {
        c[i] = w[i];
    }
    commit(n, 0);
    /* only the calls the shadow keeps (gx_shadow_call) */
    if (id <= REC_SetViewport ? (id <= REC_SetArray || id == REC_SetCullMode || id >= REC_SetScissor)
                              : (id == REC_SetProjection || id == REC_SetDispCopySrc || id == REC_SetTexCopySrc ||
                                 id == REC_SetDrawSync)) {
        gx_shadow_call(id, w);
    }
}

void gx_rec_list(const void* list, u32 nbytes, const void* caller) {
    if (s_dedup && is_state_list(list)) {
        dd_forget(); /* its state calls change the translator's state */
    }
    u32 n = gx_rec_mode == 3 ? 4 : 3;
    u32* c = put(GXR_LIST, n);
    c[0] = (u32)list;
    c[1] = nbytes;
    c[2] = (u32)caller;
    if (n == 4) {
        c[3] = gx_hash32(list, nbytes, 0x5EC0u);
    }
    s_gs.lists++;
    s_lists_rec++;
    commit(n, 1);
}

/* ---- Snapshots of the arrays a GXBegin primitive indexes ----
 * The game writes CPU-built arrays (skinned characters: animdrv's shared
 * scratch) and draws from them at once with GXBegin, then reuses the memory
 * for the next model. A translator reading them later sees the next model.
 * So the game thread finds the index range of every indexed attribute (a
 * walk over the pipe writes with the shadow's vertex description) and copies
 * that part of the array into the record; the translator draws from the
 * copy when its own vertex state and array match the shadow's. */
static u32 comp_bytes(u8 type) { return type == GX_U8 || type == GX_S8 ? 1 : type == GX_U16 || type == GX_S16 ? 2 : 4; }

static u32 vtx_state_hash(const GxState* g, u32 fmt) {
    return gx_hash32(g->vat[fmt], sizeof(g->vat[fmt]), gx_hash32(g->vtx_desc, sizeof(g->vtx_desc), 0x51A9u));
}

typedef struct {
    u32 lo, hi, elem; /* index range read, bytes of one element */
} SnapRange;

/* Index ranges of a primitive's indexed attributes (GX_VA_POS..GX_VA_TEX7); 0 when the writes do not parse. */
static int snap_ranges(const GxState* g, u32 fmt, u32 nverts, const PPCWGPipe* sl, u32 nslots, SnapRange* r) {
    const u8* d = g->vtx_desc;
    const GxAttrFmt* f = g->vat[fmt];
    for (int a = 0; a < GX_VA_MAX_ATTR; a++) {
        r[a] = (SnapRange){0xFFFFFFFFu, 0, 0};
    }
    u32 k = 0;
#define TAKE(n)                  \
    do {                         \
        k += (n);                \
        if (k > nslots) {        \
            return 0;            \
        }                        \
    } while (0)
#define INDEX(a, dsc)                                                         \
    do {                                                                      \
        if (k >= nslots) {                                                    \
            return 0;                                                         \
        }                                                                     \
        u32 i_ = (dsc) == GX_INDEX8 ? sl[k].u8 : sl[k].u16;                   \
        k++;                                                                  \
        r[a].lo = i_ < r[a].lo ? i_ : r[a].lo;                                \
        r[a].hi = i_ > r[a].hi ? i_ : r[a].hi;                                \
    } while (0)
    for (u32 v = 0; v < nverts; v++) {
        if (d[GX_VA_PNMTXIDX] != GX_NONE) {
            TAKE(1);
        }
        for (int t = 0; t < 8; t++) {
            if (d[GX_VA_TEX0MTXIDX + t] != GX_NONE) {
                TAKE(1);
            }
        }
        u32 n = f[GX_VA_POS].cnt == GX_POS_XY ? 2 : 3;
        if (d[GX_VA_POS] == GX_DIRECT) {
            TAKE(n);
        } else if (d[GX_VA_POS] != GX_NONE) {
            INDEX(GX_VA_POS, d[GX_VA_POS]);
            r[GX_VA_POS].elem = n * comp_bytes(f[GX_VA_POS].type);
        }
        u8 dn = d[GX_VA_NRM] != GX_NONE ? d[GX_VA_NRM] : d[GX_VA_NBT];
        if (dn != GX_NONE) {
            u32 vecs = f[GX_VA_NRM].cnt == GX_NRM_XYZ ? 1 : 3;
            if (dn == GX_DIRECT) {
                TAKE(vecs * 3);
            } else {
                INDEX(GX_VA_NRM, dn); /* only the first vector is read from the array */
                if (f[GX_VA_NRM].cnt == GX_NRM_NBT3) {
                    TAKE(2);
                }
                r[GX_VA_NRM].elem = 3 * comp_bytes(f[GX_VA_NRM].type);
            }
        }
        for (int c = 0; c < 2; c++) {
            u8 dc = d[GX_VA_CLR0 + c];
            u8 ty = f[GX_VA_CLR0 + c].type;
            if (dc == GX_DIRECT) {
                TAKE(ty == GX_RGB565 || ty == GX_RGBA4 ? 1 : ty == GX_RGB8 || ty == GX_RGBA6 ? 3 : 4);
            } else if (dc != GX_NONE) {
                INDEX(GX_VA_CLR0 + c, dc);
                r[GX_VA_CLR0 + c].elem = ty == GX_RGB565 || ty == GX_RGBA4 ? 2 : ty == GX_RGBA6 ? 3 : 4;
            }
        }
        for (int t = 0; t < 8; t++) {
            u8 dt = d[GX_VA_TEX0 + t];
            u32 nt = f[GX_VA_TEX0 + t].cnt == GX_TEX_S ? 1 : 2;
            if (dt == GX_DIRECT) {
                TAKE(nt);
            } else if (dt != GX_NONE) {
                INDEX(GX_VA_TEX0 + t, dt);
                r[GX_VA_TEX0 + t].elem = nt * comp_bytes(f[GX_VA_TEX0 + t].type);
            }
        }
    }
#undef TAKE
#undef INDEX
    return 1;
}

#define SNAP_MAX_BYTES (64u << 10) /* per attribute; larger spans are read live (counted) */
static u32 s_snap_bytes, s_snap_live;

void gx_rec_prim(u8 primfmt, u16 nverts, const void* caller, const PPCWGPipe* slots, u32 nslots) {
    SnapRange r[GX_VA_MAX_ATTR];
    u32 fmt = primfmt & 7;
    int ok = snap_ranges(&gx_game, fmt, nverts, slots, nslots, r);
    u32 nsnap = 0, snap_words = 0;
    if (ok) {
        for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
            if (r[a].elem == 0 || gx_game.array_base[a] == NULL) {
                continue;
            }
            u32 bytes = (r[a].hi - r[a].lo) * gx_game.array_stride[a] + r[a].elem;
            if (bytes > SNAP_MAX_BYTES) {
                s_snap_live++;
                r[a].elem = 0;
                continue;
            }
            nsnap++;
            snap_words += 5 + (bytes + 3) / 4;
        }
    }
    u32 n = 6 + nslots + snap_words;
    u32* c = put(GXR_PRIM, n);
    c[0] = primfmt;
    c[1] = nverts;
    c[2] = (u32)caller;
    c[3] = nslots;
    c[4] = nsnap;
    c[5] = nsnap ? vtx_state_hash(&gx_game, fmt) : 0;
    for (u32 i = 0; i < nslots; i++) {
        c[6 + i] = slots[i].u32; /* the readers take u8/u16/u32/f32 writes from the low bytes */
    }
    u32* q = c + 6 + nslots;
    for (int a = GX_VA_POS; nsnap && a <= GX_VA_TEX7; a++) {
        if (r[a].elem == 0 || gx_game.array_base[a] == NULL) {
            continue;
        }
        u32 stride = gx_game.array_stride[a];
        u32 bytes = (r[a].hi - r[a].lo) * stride + r[a].elem;
        q[0] = (u32)a;
        q[1] = (u32)gx_game.array_base[a];
        q[2] = stride;
        q[3] = r[a].lo;
        q[4] = bytes;
        memcpy(q + 5, gx_game.array_base[a] + r[a].lo * stride, bytes);
        q += 5 + (bytes + 3) / 4;
        s_snap_bytes += bytes;
    }
    s_gs.prims++;
    commit(n, 1);
}

/* gximmdirect: the game thread writes the primitive's vertices into its chunks now, in the ring's PICA layout,
 * reading indexed attributes from their arrays at once, so no snapshot is taken. 0: record it the old way. */
int gx_rec_prim_direct(u8 primfmt, u16 nverts, const void* caller, const PPCWGPipe* slots) {
    u32 fmt = primfmt & 7;
    if (s_null || !gx_imm_rec_ok(&gx_game, (u8)(primfmt & 0xF8), nverts)) {
        gx_imm_rec_count(0);
        return 0;
    }
    const void* base;
    u32 first, chunk;
    u8* dst = gx_vsh_gimm_alloc(nverts, &base, &first, &chunk);
    if (dst == NULL) {
        gx_imm_rec_count(0);
        return 0;
    }
    u8 attr_slot[8];
    gx_imm_convert(&gx_game, (u8)fmt, slots, nverts, dst, attr_slot);
    if (gx_opt_immdirect == 2) {
        gx_imm_verify(&gx_game, (u8)fmt, slots, nverts, dst, attr_slot);
    }
    const u8* d = gx_game.vtx_desc;
    u32* c = put(GXR_PRIMD, 10);
    c[0] = primfmt;
    c[1] = nverts;
    c[2] = (u32)caller;
    c[3] = (u32)base;
    c[4] = first;
    c[5] = chunk;
    memcpy(c + 6, attr_slot, 8);
    c[8] = d[GX_VA_NRM] != GX_NONE || d[GX_VA_NBT] != GX_NONE;
    c[9] = vtx_state_hash(&gx_game, fmt);
    s_gs.prims++;
    gx_imm_rec_count(1);
    commit(10, 1);
    return 1;
}

void gx_rec_raw(const PPCWGPipe* slots, u32 nslots) {
    if (nslots == 0) {
        return;
    }
    u32* c = put(GXR_RAW, nslots);
    for (u32 i = 0; i < nslots; i++) {
        c[i] = slots[i].u32;
    }
    commit(nslots, 1);
}

/* ---- Fences ---- */

static void frame_sync(u64 waited, u64 now);

/* Waits until everything recorded before ring position `upto` is translated. */
static void wait_translated(u32 upto) {
    if (gx_rec_mode == 2) {
        publish();
        if ((s32)(load_sc(&s_rd_pub) - upto) < 0) {
            store_sci(&s_want_done, 1);
            while ((s32)(load_sc(&s_rd_pub) - upto) < 0) {
                plat_event_wait(s_ev_done);
            }
            store_sci(&s_want_done, 0);
        }
    } else if ((s32)(s_rd - upto) < 0) {
        gx_tls_translator = 1;
        consume(upto);
        gx_tls_translator = 0;
    }
    /* verify: drop the reports no pending call can be read after */
    u32 k = 0;
    for (u32 i = 0; i < s_nrep; i++) {
        if ((s32)(s_rep[i].pos - upto) > 0) {
            s_rep[k++] = s_rep[i];
        }
    }
    s_nrep = k;
}

/* gxpipeline=1: the frame's own draw sync (the first GXReadDrawSync after GXCopyDisp) does not wait for this
 * frame's translation, only for the frame before's, so core 2 translates and replays frame N while the game
 * runs frame N+1, as the GameCube's GPU overlaps the CPU's next frame. Every other fence still waits for all. */
static int s_pipeline;
static u32 s_frame_mark; /* ring position at the last frame's draw sync */
static u32 s_pipe_waits; /* frame-end fences that had to wait for the frame before */

void gx_rec_fence(int why) {
    if (!gx_rec_on()) {
        return;
    }
    u64 t0 = plat_ticks();
    if (why == GXF_SYNC && s_pipeline && s_after_copydisp) {
        u32 prev = s_frame_mark;
        s_frame_mark = s_wr;
        if ((s32)(rd_seen() - prev) < 0) {
            s_pipe_waits++;
        }
        wait_translated(prev);
    } else {
        wait_translated(s_wr);
    }
    u64 t1 = plat_ticks();
    s_gs.fences[why]++;
    s_gs.t_fence[why] += t1 - t0;
    if (why == GXF_SYNC) {
        frame_sync(t1 - t0, t1);
    }
}

/* The frame's draw sync (the first after GXCopyDisp): from here to the next frame's start is the retrace
 * wait; with the split, the next frame starts translated directly (the translator has caught up). */
static void frame_sync(u64 waited, u64 now) {
    if (!s_after_copydisp) {
        return;
    }
    s_after_copydisp = 0;
    if (s_stats_every != 0) {
        s_idle_from = now != 0 ? now : plat_ticks();
    }
    if (gx_rec_mode != 2 || !s_split) {
        return;
    }
    (void)waited;
    if (now == 0) {
        now = plat_ticks();
    }
    u32 lists = s_lists_direct + s_lists_rec;
    if (!s_split_fixed && s_work_start != 0) {
        /* Hill climbing on the frame's work (its start to this fence): every 8 frames the split moves one step;
         * a window slower than the last turns it round with half the step, two better ones in a row double it. */
        s_win_sum += now - s_work_start;
        if (++s_win_frames == 8) {
            u64 avg = s_win_sum / 8;
            if (s_win_prev != 0 && avg > s_win_prev) {
                s_dir = -s_dir;
                s_better = 0;
                if (s_step > 32) {
                    s_step /= 2;
                }
            } else if (++s_better >= 2 && s_step < 1024) {
                s_step *= 2;
                s_better = 0;
            }
            s_win_prev = avg;
            s_win_sum = 0;
            s_win_frames = 0;
            s32 next = (s32)s_switch_at + s_dir * (s32)s_step;
            if (next <= 0) {
                next = 0;
                s_dir = 1;
            } else if ((u32)next >= lists) {
                next = (s32)lists;
                s_dir = -1;
            }
            s_switch_at = (u32)next;
        }
    }
    s_work_start = 0;
    s_direct_sum += s_lists_direct;
    s_lists_direct = 0;
    s_lists_rec = 0;
    gx_rec_direct = 1;
    if (s_switch_at == 0) {
        gx_rec_switch();
    }
}

/* Direct translation ends for this frame: the game thread records from here. The translator is idle (the
 * ring is empty since the last fence), so the shadow can start from the translated state. */
void gx_rec_switch(void) {
    memcpy(&gx_game, &gx, sizeof(gx));
    gx_rec_direct = 0;
    dd_forget();
}

void gx_rec_direct_list(void) {
    if (++s_lists_direct >= s_switch_at) {
        gx_rec_switch();
    }
}

void gx_rec_excl_begin(int why) {
    if (!gx_rec_on()) {
        return;
    }
    gx_rec_fence(why);
    gx_tls_translator = 1;
    s_excl = 1;
}

void gx_rec_excl_end(void) {
    if (s_excl) {
        s_excl = 0;
        gx_tls_translator = 0;
        dd_forget();
    }
}

/* Memory reports (include/plat/memstable.h), on the reporting thread before they apply. */
static void mem_hook(int kind, const void* p, uint32_t n) {
    if (!t_game || gx_tls_translator) {
        return;
    }
    if (gx_rec_mode == 3) {
        if (s_rd != s_wr) {
            if (s_nrep < REP_MAX) {
                s_rep[s_nrep].a = (uintptr_t)p;
                s_rep[s_nrep].b = (uintptr_t)p + n;
                s_rep[s_nrep].pos = s_wr;
                s_rep[s_nrep].kind = (u8)kind;
                s_nrep++;
            } else {
                s_rep_dropped++;
            }
        }
    }
    if (kind == PLAT_MEMSTABLE_RELEASE && s_fence_release && rd_seen() != s_wr) {
        gx_rec_fence(GXF_RELEASE);
    }
}

/* ---- Verify ---- */

void gx_rec_check_read(const void* p, u32 n, int what) {
    if (gx_rec_mode != 3 || p == NULL || n == 0) {
        return;
    }
    uintptr_t a = (uintptr_t)p, b = a + n;
    for (u32 i = 0; i < s_nrep; i++) {
        /* a report made after this call was recorded, on memory the call reads */
        if ((s32)(s_rep[i].pos - s_cur) > 0 && s_rep[i].a < b && a < s_rep[i].b) {
            s_hazard[s_rep[i].kind & 3][what]++;
            if (s_logged++ < 16) {
                PLOG_W("gxthread verify: %s %p+%lu read %lu words after a %s report on %p+%lu", k_read_name[what], p,
                       (unsigned long)n, (unsigned long)(s_rep[i].pos - s_cur), k_kind_name[s_rep[i].kind & 3],
                       (const void*)s_rep[i].a, (unsigned long)(s_rep[i].b - s_rep[i].a));
            }
            return;
        }
    }
}

void gx_rec_getter_check(int which) {
    gx_rec_fence(GXF_VERIFY);
    const GxState* g = &gx_game;
    int same = 1;
    switch (which) {
    case GXG_VTXDESC: same = gx_mem_equal(g->vtx_desc, gx.vtx_desc, sizeof(gx.vtx_desc)); break;
    case GXG_VAT: same = gx_mem_equal(g->vat, gx.vat, sizeof(gx.vat)); break;
    case GXG_PROJ: same = gx_mem_equal(g->proj, gx.proj, sizeof(gx.proj)); break;
    case GXG_VIEWPORT: same = gx_mem_equal(g->vp, gx.vp, sizeof(gx.vp)); break;
    case GXG_SCISSOR: same = gx_mem_equal(g->scissor, gx.scissor, sizeof(gx.scissor)); break;
    case GXG_CULL: same = g->cull == gx.cull; break;
    case GXG_COPYSRC: same = gx_mem_equal(g->copy_src, gx.copy_src, sizeof(gx.copy_src)); break;
    default: break;
    }
    if (!same && s_getter_mism[which]++ < 4) {
        PLOG_W("gxthread verify: %s read back from the shadow differs from the translated state (from %p)",
               k_getter_name[which], __builtin_return_address(0));
    }
}

/* ---- Frames and counters ---- */

/* Mode 0: the frame's draw sync (GXReadDrawSync after GXCopyDisp) starts the retrace wait too. */
void gx_rec_sync_mark(void) {
    frame_sync(0, 0);
}

void gx_rec_frame_start(void) {
    if (gx_rec_mode == 2 && s_split && s_work_start == 0) {
        s_work_start = plat_ticks();
    }
    if (s_idle_from != 0) {
        s_gs.t_idle += plat_ticks() - s_idle_from;
        s_idle_from = 0;
    }
}

static u32 us_of(u64 ticks, u32 frames) {
    return (u32)(ticks / (PLAT_TICKS_PER_SEC / 1000000u) / (frames ? frames : 1));
}

void gx_rec_frame(void) {
    plat_pmu_frame(); /* switch pmu=N: ARM11 counters per frame, next to these */
    s_after_copydisp = 1;
    if (s_stats_every == 0) {
        return;
    }
    u64 now = plat_ticks();
    s_frame_no++;
    if (s_last_frame_tick != 0) {
        s_gs.t_frame += now - s_last_frame_tick;
        s_gs.frames++;
    }
    s_last_frame_tick = now;
    if (s_gs.frames < s_stats_every) {
        return;
    }
    u32 f = s_gs.frames;
    u64 fence_all = 0;
    for (int i = 0; i < GXF_COUNT; i++) {
        fence_all += s_gs.t_fence[i];
    }
    u32 frame = us_of(s_gs.t_frame, f), idle = us_of(s_gs.t_idle, f), fence = us_of(fence_all, f);
    u32 busy = frame > idle + fence ? frame - idle - fence : 0;
    u64 tb = __atomic_exchange_n(&s_ts_busy, 0, __ATOMIC_RELAXED);
    PlatGpuStats gs;
    plat_gpu_stats(&gs);
    u32 tr = gx_rec_mode == 2 ? us_of(tb, f) : 0;
    u32 tp = us_of(__atomic_exchange_n(&s_ts_present, 0, __ATOMIC_RELAXED), f);
    PLOG_I("gxthread %d frame %lu: game us: frame %lu busy %lu fences %lu idle %lu | translator %lu (copydisp %lu) render %lu (core 2 %lu%%) | "
           "rec %lu words %lu cmds %lu lists %lu prims per frame, %lu lists direct (split at %lu)",
           gx_rec_mode, (unsigned long)s_frame_no, (unsigned long)frame, (unsigned long)busy, (unsigned long)fence, (unsigned long)idle,
           (unsigned long)tr, (unsigned long)tp, (unsigned long)gs.render_us,
           (unsigned long)(frame ? (u64)(tr + gs.render_us) * 100u / frame : 0), (unsigned long)(s_gs.words / f),
           (unsigned long)(s_gs.cmds / f), (unsigned long)(s_gs.lists / f), (unsigned long)(s_gs.prims / f),
           (unsigned long)(s_direct_sum / f), (unsigned long)s_switch_at);
    s_direct_sum = 0;
    PLOG_I("gxthread   render (last frame): %lu us, %lu of them in C3D_FrameBegin's vblank wait; %lu records "
           "(segments %lu); syncs since boot %lu",
           (unsigned long)gs.render_us, (unsigned long)gs.begin_wait_us, (unsigned long)gs.records,
           (unsigned long)gs.segments, (unsigned long)gs.syncs);
    PLOG_I("gxthread   fences per frame (count, us): sync %lu.%02lu %lu, drawdone %lu.%02lu %lu, readback %lu.%02lu %lu, "
           "release %lu.%02lu %lu, full %lu.%02lu %lu, verify %lu.%02lu %lu",
           (unsigned long)(s_gs.fences[0] / f), (unsigned long)(s_gs.fences[0] * 100 / f % 100),
           (unsigned long)us_of(s_gs.t_fence[0], f), (unsigned long)(s_gs.fences[1] / f),
           (unsigned long)(s_gs.fences[1] * 100 / f % 100), (unsigned long)us_of(s_gs.t_fence[1], f),
           (unsigned long)(s_gs.fences[2] / f), (unsigned long)(s_gs.fences[2] * 100 / f % 100),
           (unsigned long)us_of(s_gs.t_fence[2], f), (unsigned long)(s_gs.fences[3] / f),
           (unsigned long)(s_gs.fences[3] * 100 / f % 100), (unsigned long)us_of(s_gs.t_fence[3], f),
           (unsigned long)(s_gs.fences[4] / f), (unsigned long)(s_gs.fences[4] * 100 / f % 100),
           (unsigned long)us_of(s_gs.t_fence[4], f), (unsigned long)(s_gs.fences[5] / f),
           (unsigned long)(s_gs.fences[5] * 100 / f % 100), (unsigned long)us_of(s_gs.t_fence[5], f));
    PLOG_I("gxthread   array snapshots: %lu bytes per frame, %lu spans too large (read live), %lu not used (total)%s",
           (unsigned long)(s_snap_bytes / f), (unsigned long)s_snap_live, (unsigned long)s_snap_mism,
           s_pipeline ? " | pipelined" : "");
    if (s_dedup) {
        PLOG_I("gxthread   dedup: %lu of %lu state calls per frame not recorded (unchanged)",
               (unsigned long)(s_dd_skipped / f), (unsigned long)(s_dd_calls / f));
        s_dd_skipped = s_dd_calls = 0;
    }
    if (s_pipeline) {
        PLOG_I("gxthread   pipeline: %lu of %lu frame ends waited for the frame before", (unsigned long)s_pipe_waits,
               (unsigned long)f);
        s_pipe_waits = 0;
    }
    s_snap_bytes = 0;
    if (gx_opt_statecache) {
        PLOG_I("gxthread   statecache: %lu of %lu applied state calls per frame returned early (unchanged)",
               (unsigned long)(s_dda_skipped / f), (unsigned long)(s_dda_calls / f));
        s_dda_skipped = s_dda_calls = 0;
        gx_tev_memo_log(f);
    }
    gx_bbox_stats_log(f);
    gx_dirty_log(f);
    gx_immdirect_log(f);
    if (gx_rec_mode == 3) {
        u32 gm = 0;
        for (int i = 0; i < GXG_COUNT; i++) {
            gm += s_getter_mism[i];
        }
        PLOG_I("gxthread   verify (total): lists changed %lu, getter mismatches %lu, reads after a report: "
               "fill %lu/%lu/%lu/%lu write %lu/%lu/%lu/%lu release %lu/%lu/%lu/%lu (list/array/tex/tlut), "
               "reports dropped %lu",
               (unsigned long)s_list_changed, (unsigned long)gm, (unsigned long)s_hazard[1][0],
               (unsigned long)s_hazard[1][1], (unsigned long)s_hazard[1][2], (unsigned long)s_hazard[1][3],
               (unsigned long)s_hazard[2][0], (unsigned long)s_hazard[2][1], (unsigned long)s_hazard[2][2],
               (unsigned long)s_hazard[2][3], (unsigned long)s_hazard[3][0], (unsigned long)s_hazard[3][1],
               (unsigned long)s_hazard[3][2], (unsigned long)s_hazard[3][3], (unsigned long)s_rep_dropped);
    }
    memset(&s_gs, 0, sizeof(s_gs));
}

/* ---- Set-up (GXInit, game thread) ---- */

void gx_rec_init(void) {
    t_game = 1;
    if (s_dda == NULL) {
        int sc = plat_switch_int("gxstatecache", 1);
        s_dda = sc >= 1 && sc <= 2 ? calloc(DD_SLOTS, sizeof(DdSlot)) : NULL;
        gx_opt_statecache = s_dda != NULL ? sc : 0;
        gx_opt_statededup = gx_opt_statecache && plat_switch_int("gxstatededup", 0) != 0;
        if (gx_opt_statecache) {
            PLOG_I("gxstatecache=%d: unchanged state calls return early, compiled TEV setups are reused%s",
                   gx_opt_statecache, gx_opt_statecache == 2 ? " (verify)" : "");
        }
    }
    int mode = plat_switch_int("gxthread", 0);
    if (mode < 0 || mode > 3) {
        mode = 0;
    }
    /* gxthreadstats=N: only these counters (a tick read per fence and frame), without gxstats' per-call timers */
    s_stats_every = (u32)plat_switch_int("gxthreadstats", plat_switch_int("gxstats", 0));
    s_fence_release = plat_switch_int("gxthreadrel", 1) != 0;
    s_split = plat_switch_int("gxthreadsplit", 1) != 0;
    s_pipeline = plat_switch_int("gxpipeline", 0) != 0;
    s_dedup = plat_switch_int("gxthreaddedup", 1) != 0;
    if (s_pipeline) {
        s_split = 0; /* the split needs the translator idle at a frame's start */
    }
    s_split_fixed = plat_switch_int("gxthreadsplitat", -1) >= 0;
    if (s_split_fixed) {
        s_switch_at = (u32)plat_switch_int("gxthreadsplitat", 0);
    }
    /* gxnull=1: record as gxthread=1 and drop the record at each fence (the fences still answer), so the game
     * thread's time is the game's own work plus recording; nothing is shown */
    s_null = plat_switch_int("gxnull", 0) != 0;
    if (s_null) {
        mode = 1;
    }
    if (mode == 0) {
        return;
    }
    u32 kb = (u32)plat_switch_int("gxthreadkb", 4096);
    u32 words = 1u << 16;
    while (words < kb * 256u && words < (1u << 22)) {
        words <<= 1;
    }
    /* a placement, so that place=gx_ring:qtm can move it (heap by default; QTM holds it only with a small
     * gxthreadkb): the game thread writes it, the translator on core 2 reads it */
    int ring_place = plat_place_register("gx ring", words * 4u, 64, PLAT_PREFS(PLAT_TIER_HEAP), PLAT_PF_CPU_SHARED);
    s_ring = plat_place_alloc(ring_place, words * 4u, 64);
    if (s_ring == NULL) {
        PLOG_W("gxthread: no memory for a %lu KB ring: GX translates on the game thread", (unsigned long)(words / 256u));
        return;
    }
    s_dd = calloc(DD_SLOTS, sizeof(DdSlot));
    s_state_lists = calloc(SL_SLOTS, sizeof(*s_state_lists));
    if (s_dd == NULL || s_state_lists == NULL) {
        s_dedup = 0;
    }
    s_ring_words = words;
    s_ring_mask = words - 1;
    if (mode == 2) {
        s_ev_work = plat_event_create();
        s_ev_done = plat_event_create();
        PlatThread* t = NULL;
        if (s_ev_work != NULL && s_ev_done != NULL && plat_is_new3ds()) {
            /* below the render thread (0x2E), above the job worker (0x30) */
            t = plat_thread_create(translator_main, NULL, 256 * 1024, 0x2F, 2, "gxtranslate");
        }
        if (t == NULL) {
            PLOG_W("gxthread=2: no translator thread on core 2: recording and translating on the game thread (1)");
            mode = 1;
        }
    }
    gx_rec_mode = mode;
    plat_memstable_set_hook(mem_hook);
    PLOG_I("gxthread=%d: GX calls recorded into a %lu KB ring, translated %s%s", mode,
           (unsigned long)(words / 256u), mode == 2 ? "on core 2" : "on the game thread at each fence",
           mode == 3 ? " (verify)" : "");
}
