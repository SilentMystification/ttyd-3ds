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
#include <stdio.h>
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
struct GxRecFast gx_rec_fast;
GxState gx_game;

/* game thread (the only one that changes what it reads, with its own gx_tls_translator) */
void gx_rec_fast_update(void) {
    int quiet = !gx_dl_recording() && !gx_prim_is_open();
    gx_rec_fast.w = gx_rec_mode == 0 && quiet && !gx_tls_translator ? 0 : gx_rec_on() && quiet ? 1 : 2;
}

static __thread int t_game; /* the thread that called GXInit */

enum { GXR_WRAP = 0xF0, GXR_LIST, GXR_PRIM, GXR_RAW, GXR_PRIMD };
enum { GXR_FCHK_LIST = 0xE0, GXR_FCHK_VD }; /* gxrecfuse=2's checks */
/* gxrecfuse counters (game thread; the checks' on the translator) */
static struct {
    u32 vd_calls, vd_flushes, vd_sent, lists, list_calls, list_sent, list_decoded, list_other;
    u32 chk_list, chk_list_bad, chk_vd, chk_vd_bad;
    u32 other_why[4]; /* lists the old way, by FlEntry.why */
} s_fst;
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
/* counters of the hand-off (gxthreadstats): the translator's sleeps, the game thread's wake-up signals, and the
 * display lists hashed to see whether they are a copy of a state list (is_state_list) */
static u32 s_ts_sleeps, s_gs_signals, s_sl_hashes, s_sl_hash_bytes;

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

/* ---- Phase timers (switch gxphase=N, gx_internal.h) ----
 * The counters are the translator's (one writer); the game thread logs them as differences from the last log
 * (32-bit, so no torn reads; tick sums wrap after 16 s, far above a log interval). */
int gx_ph_on;
static u32 gx_ph_sp;
static struct {
    u32 t, seg, n, samp;
} s_ph[GPH_COUNT], s_ph_prev[GPH_COUNT];
static u32 s_ph_outer[2], s_ph_outer_prev[2]; /* outer calls, outer calls timed */
static u8 s_ph_stk[16];
static u32 s_ph_t0, s_ph_cd = 1;
static const char* const k_ph_name[GPH_COUNT] = {"cal", "list", "set", "prim", "primd", "raw", "other",
                                                 "batopen", "batclose", "slow", "form", "state"};

__attribute__((noinline, cold)) int gx_ph_enter_slow(int ph, int outer) {
    s_ph[ph].n++;
    u32 sp = gx_ph_sp;
    if (sp == 0) {
        if (!outer) {
            return 0;
        }
        s_ph_outer[0]++;
        if (--s_ph_cd != 0) {
            return 0;
        }
        s_ph_cd = (u32)gx_ph_on;
        s_ph_outer[1]++;
        /* the outer call's clock starts, then an empty nested phase: its segments are the cost of one segment where
         * the measured ones are taken (taken off every phase at the log) */
        s_ph_stk[0] = (u8)ph;
        gx_ph_sp = 1;
        s_ph[ph].samp++;
        s_ph_t0 = (u32)plat_ticks();
        if (gx_ph_enter_slow(GPH_CAL, 0)) {
            gx_ph_pop();
        }
        return 1;
    }
    if (sp >= 16) {
        return 0;
    }
    u32 t = (u32)plat_ticks();
    u32 o = s_ph_stk[sp - 1];
    s_ph[o].t += t - s_ph_t0;
    s_ph[o].seg++;
    s_ph_stk[sp] = (u8)ph;
    gx_ph_sp = sp + 1;
    s_ph[ph].samp++;
    s_ph_t0 = t;
    return 1;
}

__attribute__((noinline, cold)) void gx_ph_pop(void) {
    u32 t = (u32)plat_ticks();
    u32 sp = --gx_ph_sp;
    u32 ph = s_ph_stk[sp];
    s_ph[ph].t += t - s_ph_t0;
    s_ph[ph].seg++;
    s_ph_t0 = t;
}

/* Game thread, at a log: the phases per frame, less the cost of a segment (the empty nested phase's), scaled by the
 * outer calls per timed outer call. */
static __attribute__((noinline, cold)) void gx_ph_log(u32 frame_no, u32 f) {
    const u32 tpu = PLAT_TICKS_PER_SEC / 1000000u;
    u32 cal_t = s_ph[GPH_CAL].t - s_ph_prev[GPH_CAL].t, cal_seg = s_ph[GPH_CAL].seg - s_ph_prev[GPH_CAL].seg;
    u32 c16 = cal_seg ? (u32)((u64)cal_t * 16u / cal_seg) : 0; /* the cost of one segment, in 1/16 ticks */
    u32 on = s_ph_outer[0] - s_ph_outer_prev[0], os = s_ph_outer[1] - s_ph_outer_prev[1];
    char buf[3][200];
    int len[3] = {0}, k = 0;
    u64 sum_us = 0;
    u32 segs = 0;
    for (int p = GPH_CAL + 1; p < GPH_COUNT; p++) {
        u32 dn = s_ph[p].n - s_ph_prev[p].n, ds = s_ph[p].samp - s_ph_prev[p].samp;
        u32 dt = s_ph[p].t - s_ph_prev[p].t, dg = s_ph[p].seg - s_ph_prev[p].seg;
        u64 net16 = (u64)dt * 16u > (u64)dg * c16 ? (u64)dt * 16u - (u64)dg * c16 : 0;
        u32 us = os ? (u32)(net16 * on / os / 16u / tpu / f) : 0;
        sum_us += us;
        segs += dg;
        if (dn != 0 && k < 3) {
            len[k] += snprintf(buf[k] + len[k], sizeof(buf[k]) - len[k], " %s %lu/%lu/%lu", k_ph_name[p],
                               (unsigned long)(dn / f), (unsigned long)(ds / f), (unsigned long)us);
            if (len[k] > 150) {
                k++;
            }
        }
    }
    PLOG_I("gxphase frame %lu translator: segment %lu.%02lu us, %lu segments, outer %lu timed of %lu per frame, est. "
           "sum %lu us", (unsigned long)frame_no, (unsigned long)(c16 / 16u / tpu),
           (unsigned long)(c16 * 100u / 16u / tpu % 100u), (unsigned long)(segs / f), (unsigned long)(os / f),
           (unsigned long)(on / f), (unsigned long)sum_us);
    for (int i = 0; i <= k && i < 3; i++) {
        if (len[i] != 0) {
            PLOG_I("gxphase   (calls/timed/us per frame):%s", buf[i]);
        }
    }
    s_ph_outer_prev[0] = s_ph_outer[0];
    s_ph_outer_prev[1] = s_ph_outer[1];
    memcpy(s_ph_prev, s_ph, sizeof(s_ph));
}

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

static inline __attribute__((always_inline)) void run_one(u32 type, const u32* w, u32 n) {
    switch (type) {
    case GXR_LIST: {
        const void* list = (const void*)w[0];
        if (n > 4) {
            /* a group (gxreclgroup): 3 words a call */
            for (u32 i = 0; i < n; i += 3) {
                gx_call_list((const void*)w[i], w[i + 1], (const void*)w[i + 2]);
            }
            break;
        }
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
    case GXR_FCHK_LIST: {
        /* gxrecfuse=2: the list's calls went through the dedup table; applying the list now must change nothing */
        static GxState s_before;
        memcpy(&s_before, &gx, sizeof(gx));
        u32 done = gx_sdl_run((const u8*)w[0], w[1], NULL);
        s_before.version = gx.version;
        s_fst.chk_list++;
        if (done != w[1] || memcmp(&s_before, &gx, sizeof(gx)) != 0) {
            if (s_fst.chk_list_bad++ < 8) {
                PLOG_W("gxrecfuse verify: state list %p (%lu bytes) changes the translated state after its fused "
                       "calls", (const void*)w[0], (unsigned long)w[1]);
            }
        }
        break;
    }
    case GXR_FCHK_VD:
        s_fst.chk_vd++;
        if (memcmp(w, gx.vtx_desc, GX_VA_MAX_ATTR) != 0) {
            if (s_fst.chk_vd_bad++ < 8) {
                PLOG_W("gxrecfuse verify: the translated vertex description differs from the game thread's shadow");
            }
        }
        break;
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

/* consume with the phase timers (gxphase): a list call of a group counts as one outer call */
static __attribute__((noinline, cold)) void consume_ph(u32 upto) {
    u32 pub = s_rd;
    while (s_rd != upto) {
        const u32* c = s_ring + (s_rd & s_ring_mask);
        u32 type = c[0] >> 24, n = c[0] & 0xFFFFFFu;
        if (type == GXR_LIST && n > 4) {
            s_cur = s_rd;
            for (u32 i = 0; i < n; i += 3) {
                int p = gx_ph_enter(GPH_T_LIST);
                gx_call_list((const void*)c[1 + i], c[2 + i], (const void*)c[3 + i]);
                gx_ph_leave(p);
            }
        } else if (type != GXR_WRAP) {
            s_cur = s_rd;
            int ph = type == GXR_LIST ? GPH_T_LIST : type == GXR_PRIM ? GPH_T_PRIM : type == GXR_PRIMD ? GPH_T_PRIMD
                     : type == GXR_RAW ? GPH_T_RAW : type < REC_COUNT && type != REC_CopyDisp ? GPH_T_SET : GPH_T_OTHER;
            int p = gx_ph_enter(ph);
            run_one(type, c + 1, n);
            gx_ph_leave(p);
        }
        s_rd += n + 1;
        if (gx_rec_mode == 2 && s_rd - pub >= 4096) {
            store_sc(&s_rd_pub, s_rd);
            pub = s_rd;
        }
    }
    store_sc(&s_rd_pub, s_rd);
}

static void consume(u32 upto) {
    if (s_null) {
        s_rd = upto;
        store_sc(&s_rd_pub, s_rd);
        return;
    }
    if (__builtin_expect(gx_ph_on, 0)) {
        consume_ph(upto);
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

#define TR_STACK (256 * 1024)

static void translator_main(void* arg) {
    (void)arg;
    gx_tls_translator = 1;
    /* this thread's stack, for gx_on_translator: below the entry frame, plus a margin above it */
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    gx_rec_fast.tr_lo = sp - TR_STACK;
    gx_rec_fast.tr_len = TR_STACK + 4096;
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
        __atomic_fetch_add(&s_ts_sleeps, 1, __ATOMIC_RELAXED);
        plat_event_wait(s_ev_work);
        store_sci(&s_sleeping, 0);
    }
}

/* ---- Recording (game thread) ---- */

static u32 s_lg_end; /* gxreclgroup: s_wr just after the open group, 0 when none is open */

static void publish(void) {
    s_lg_end = 0; /* gxreclgroup: a published group is not extended */
    if (gx_rec_mode != 2) {
        return;
    }
    store_sc(&s_wr_pub, s_wr);
    /* One signal per sleep: the flag is taken (set back to 0) by the thread that signals, so the publishes that
     * follow, until the translator runs and sleeps again, make no system call. Before, every publish signalled
     * while the flag stood (up to 2500 signals a frame for 1 to 3 sleeps). The translator sets the flag, then
     * looks at s_wr_pub again before it waits, and the event stays set until waited for: no wake-up is lost. */
    if (load_sci(&s_sleeping) && __atomic_exchange_n(&s_sleeping, 0, __ATOMIC_SEQ_CST)) {
        s_gs_signals++;
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

static u32 s_vd_n; /* gxrecfuse: vertex-description calls waiting in the log (fuse_vd_flush) */
static void fuse_vd_flush(void);

static inline u32* put(u32 type, u32 n) {
    /* state setters read no vertex description: the log waits for a draw, list, copy or sync */
    if (__builtin_expect(s_vd_n != 0, 0) && type >= REC_CopyTex) {
        fuse_vd_flush();
    }
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

/* The slot of call `id`, or -1 (always recorded): a rule per call id, read from a table (one indexed load and
 * a short switch on the rule's kind instead of a jump over every call id). */
enum { B_VTX = 0, B_VAT = 32, B_ARR = 32 + 8 * 32, B_POSMTX = B_ARR + 32, B_NRMMTX = B_POSMTX + 10,
       B_TEXMTX = B_NRMMTX + 10, B_TG = B_TEXMTX + 64, B_CHAN = B_TG + 8, B_LIGHT = B_CHAN + 18,
       B_TEV = B_LIGHT + 8, B_KCOLOR = B_TEV + 16 * 9, B_SWAPT = B_KCOLOR + 4, B_TEVREG = B_SWAPT + 4,
       B_TEXOBJ = B_TEVREG + 4, B_TLUT = B_TEXOBJ + 8, B_IND = B_TLUT + 20, B_GLOBAL = B_IND + 12 };
_Static_assert(B_GLOBAL + 40 <= DD_SLOTS, "dedup slots");
/* DK_IDX: base + w[0] * mul when w[0] < limit; DK_NBT: an attribute (NBT shares NRM's slot); DK_VAT: format and
 * attribute; DK_DIV3: a matrix id (base + w[0] / 3); DK_LIGHT: one light bit; DK_ITM: an indirect matrix id;
 * DK_FIXED: one slot. DK_NONE (0, every id not listed): ClearVtxDesc (see dd_same), copies, syncs, the bounding box. */
enum { DK_NONE, DK_IDX, DK_NBT, DK_VAT, DK_DIV3, DK_LIGHT, DK_ITM, DK_FIXED };
typedef struct {
    u16 base;
    u8 kind, limit, mul;
} DdRule;
static const DdRule k_dd_rule[REC_COUNT] = {
    [REC_SetVtxDesc] = {B_VTX, DK_NBT, 32, 1},
    [REC_SetVtxAttrFmt] = {B_VAT, DK_VAT, 8, 32},
    [REC_SetArray] = {B_ARR, DK_NBT, 32, 1},
    [REC_LoadPosMtx] = {B_POSMTX, DK_DIV3, 10, 1},
    [REC_LoadNrmMtx] = {B_NRMMTX, DK_DIV3, 10, 1},
    [REC_LoadTexMtx] = {B_TEXMTX, DK_DIV3, 64, 1},
    [REC_SetTexCoordGen2] = {B_TG, DK_IDX, 8, 1},
    [REC_SetChanCtrl] = {B_CHAN, DK_IDX, 6, 1},
    [REC_SetChanAmbColor] = {B_CHAN + 6, DK_IDX, 6, 1},
    [REC_SetChanMatColor] = {B_CHAN + 12, DK_IDX, 6, 1},
    [REC_LoadLightObj] = {B_LIGHT, DK_LIGHT, 0, 1},
    [REC_SetTevOrder] = {B_TEV + 0, DK_IDX, 16, 9},
    [REC_SetTevColorIn] = {B_TEV + 1, DK_IDX, 16, 9},
    [REC_SetTevAlphaIn] = {B_TEV + 2, DK_IDX, 16, 9},
    [REC_SetTevColorOp] = {B_TEV + 3, DK_IDX, 16, 9},
    [REC_SetTevAlphaOp] = {B_TEV + 4, DK_IDX, 16, 9},
    [REC_SetTevKColorSel] = {B_TEV + 5, DK_IDX, 16, 9},
    [REC_SetTevKAlphaSel] = {B_TEV + 6, DK_IDX, 16, 9},
    [REC_SetTevSwapMode] = {B_TEV + 7, DK_IDX, 16, 9},
    [REC_SetTevDirect] = {B_TEV + 8, DK_IDX, 16, 9}, /* both write ind_enabled */
    [REC_SetTevIndirect] = {B_TEV + 8, DK_IDX, 16, 9},
    [REC_SetTevKColor] = {B_KCOLOR, DK_IDX, 4, 1},
    [REC_SetTevSwapModeTable] = {B_SWAPT, DK_IDX, 4, 1},
    [REC_SetTevColor] = {B_TEVREG, DK_IDX, 4, 1}, /* one register */
    [REC_SetTevColorS10] = {B_TEVREG, DK_IDX, 4, 1},
    [REC_LoadTexObj] = {B_TEXOBJ, DK_IDX, 8, 1},
    [REC_LoadTlut] = {B_TLUT, DK_IDX, 20, 1},
    [REC_SetIndTexOrder] = {B_IND, DK_IDX, 4, 1},
    [REC_SetIndTexCoordScale] = {B_IND + 4, DK_IDX, 4, 1},
    [REC_SetIndTexMtx] = {B_IND + 8, DK_ITM, 3, 1},
    [REC_SetDispCopySrc] = {B_GLOBAL + 0, DK_FIXED, 0, 0}, /* one copy source */
    [REC_SetTexCopySrc] = {B_GLOBAL + 0, DK_FIXED, 0, 0},
    [REC_SetCurrentMtx] = {B_GLOBAL + 1, DK_FIXED, 0, 0},
    [REC_SetNumTexGens] = {B_GLOBAL + 2, DK_FIXED, 0, 0},
    [REC_SetNumChans] = {B_GLOBAL + 3, DK_FIXED, 0, 0},
    [REC_SetNumTevStages] = {B_GLOBAL + 4, DK_FIXED, 0, 0},
    [REC_SetAlphaCompare] = {B_GLOBAL + 5, DK_FIXED, 0, 0},
    [REC_SetBlendMode] = {B_GLOBAL + 6, DK_FIXED, 0, 0},
    [REC_SetZMode] = {B_GLOBAL + 7, DK_FIXED, 0, 0},
    [REC_SetZCompLoc] = {B_GLOBAL + 8, DK_FIXED, 0, 0},
    [REC_SetColorUpdate] = {B_GLOBAL + 9, DK_FIXED, 0, 0},
    [REC_SetAlphaUpdate] = {B_GLOBAL + 10, DK_FIXED, 0, 0},
    [REC_SetCullMode] = {B_GLOBAL + 11, DK_FIXED, 0, 0},
    [REC_SetFog] = {B_GLOBAL + 12, DK_FIXED, 0, 0},
    [REC_SetScissor] = {B_GLOBAL + 13, DK_FIXED, 0, 0},
    [REC_SetViewport] = {B_GLOBAL + 14, DK_FIXED, 0, 0},
    [REC_SetProjection] = {B_GLOBAL + 15, DK_FIXED, 0, 0},
    [REC_SetLineWidth] = {B_GLOBAL + 16, DK_FIXED, 0, 0},
    [REC_SetNumIndStages] = {B_GLOBAL + 17, DK_FIXED, 0, 0},
    [REC_SetZScaleOffset] = {B_GLOBAL + 18, DK_FIXED, 0, 0},
    [REC_SetScissorBoxOffset] = {B_GLOBAL + 19, DK_FIXED, 0, 0},
    [REC_SetDstAlpha] = {B_GLOBAL + 20, DK_FIXED, 0, 0},
    [REC_SetPixelFmt] = {B_GLOBAL + 21, DK_FIXED, 0, 0},
    [REC_SetZTexture] = {B_GLOBAL + 22, DK_FIXED, 0, 0},
    [REC_SetCopyClear] = {B_GLOBAL + 23, DK_FIXED, 0, 0},
    [REC_SetDispCopyDst] = {B_GLOBAL + 24, DK_FIXED, 0, 0},
    [REC_SetDispCopyYScale] = {B_GLOBAL + 25, DK_FIXED, 0, 0},
    [REC_SetTexCopyDst] = {B_GLOBAL + 26, DK_FIXED, 0, 0},
};

static inline int dd_slot(u32 id, const u32* w) {
    if (id >= REC_COUNT) {
        return -1;
    }
    const DdRule* r = &k_dd_rule[id];
    switch (r->kind) {
    case DK_FIXED: return r->base;
    case DK_IDX: return w[0] < r->limit ? r->base + (int)(w[0] * r->mul) : -1;
    case DK_NBT: return w[0] < 32 ? r->base + (int)nbt_nrm(w[0]) : -1;
    case DK_VAT: return w[0] < 8 && w[1] < 32 ? r->base + (int)(w[0] * 32 + nbt_nrm(w[1])) : -1;
    case DK_DIV3: return w[0] / 3 < r->limit ? r->base + (int)(w[0] / 3) : -1;
    case DK_LIGHT: {
        u32 l = w[0] & 0xFF;
        return l != 0 && (l & (l - 1)) == 0 ? r->base + (31 - __builtin_clz(l)) : -1;
    }
    case DK_ITM: return w[0] >= GX_ITM_0 && w[0] < GX_ITM_0 + 3 ? r->base + (int)(w[0] - GX_ITM_0) : -1;
    default: return -1;
    }
}

static int s_vd_valid; /* gxrecfuse: s_vd_em is the translator's vertex description */
static int s_fuse; /* switch gxrecfuse (see "Fused state" below) */
static int s_fuse_parts = 7; /* switch gxrecfuseparts (measuring): 1 state lists, 2 vertex description, 4 channels */
/* gxrecfuse: the dedup table's material and ambient colours (dd_col_same) */
static struct {
    u32 gen[2][2]; /* [colour][0: rgb, 1: alpha]: valid at the table's generation */
    u32 val[2];
} s_dd_col[2]; /* [0] ambient, [1] material */
static struct {
    u32 gen, a, mask, attn; /* a: enable, ambient and material source, diffuse function (a byte each) */
} s_dd_chan[4];          /* gxrecfuse: GXSetChanCtrl by channel (dd_chan_same) */

static void dd_forget(void) {
    s_vd_valid = 0;
    if (++s_dd_gen == 0) {
        memset(s_dd, 0, DD_SLOTS * sizeof(DdSlot));
        memset(s_dd_col, 0, sizeof(s_dd_col));
        memset(s_dd_chan, 0, sizeof(s_dd_chan));
        s_dd_gen = 1;
    }
}

/* Lists the game built with state calls in them (their start addresses). */
#define SL_SLOTS 2048
static const void** s_state_lists; /* SL_SLOTS, allocated with the ring */

static u32 sl_hash(const void* p) { return ((u32)p >> 5) * 2654435761u >> 21; }

/* A state list is known by address (the buffer it was built in) and by content: the game copies built lists
 * (mapdrv's per-material lists go from a scratch buffer into the map's block), and the copy is what it calls.
 * Content entries: size and hash of the bytes at GXEndDisplayList; `s_sl_sizes` has a bit per 32 bytes of size
 * so that only a list of a known size is hashed when it is called. */
#define SC_SLOTS 2048
typedef struct {
    u32 size, hash;
} StateContent;
static StateContent* s_state_content; /* SC_SLOTS, allocated with the ring */
static u32 s_sl_sizes[(0x10000 >> 10) + 1];
static u32 s_sc_count;

static void sl_add_addr(const void* list) {
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

void gx_rec_state_list(const void* list, u32 nbytes) {
    if (!gx_rec_mode || s_state_lists == NULL) {
        return;
    }
    sl_add_addr(list);
    if (nbytes == 0 || nbytes > 0x10000 || s_state_content == NULL) {
        return;
    }
    u32 hash = gx_hash32(list, nbytes, 0x5EC1u);
    u32 h = hash * 2654435761u >> 21;
    for (u32 i = 0; i < SC_SLOTS; i++, h = (h + 1) & (SC_SLOTS - 1)) {
        StateContent* e = &s_state_content[h];
        if (e->size == nbytes && e->hash == hash) {
            return;
        }
        if (e->size == 0) {
            e->size = nbytes;
            e->hash = hash;
            s_sl_sizes[(nbytes >> 5) >> 5] |= 1u << ((nbytes >> 5) & 31);
            if (++s_sc_count > SC_SLOTS / 2) {
                s_dedup = 0; /* table (half) full: stop skipping calls */
                PLOG_W("gxthread: more than %d state list contents: dedup off", SC_SLOTS / 2);
            }
            return;
        }
    }
}

static int is_state_list(const void* list, u32 nbytes) {
    u32 h = sl_hash(list);
    for (u32 i = 0; i < SL_SLOTS; i++, h = (h + 1) & (SL_SLOTS - 1)) {
        if (s_state_lists[h] == list) {
            return 1;
        }
        if (s_state_lists[h] == NULL) {
            break;
        }
    }
    /* a copy of a state list: same size and bytes as one the game built */
    if (nbytes == 0 || nbytes > 0x10000 || s_state_content == NULL || !(s_sl_sizes[(nbytes >> 5) >> 5] >> ((nbytes >> 5) & 31) & 1)) {
        return 0;
    }
    s_sl_hashes++;
    s_sl_hash_bytes += nbytes;
    u32 hash = gx_hash32(list, nbytes, 0x5EC1u);
    h = hash * 2654435761u >> 21;
    for (u32 i = 0; i < SC_SLOTS; i++, h = (h + 1) & (SC_SLOTS - 1)) {
        const StateContent* e = &s_state_content[h];
        if (e->size == nbytes && e->hash == hash) {
            sl_add_addr(list); /* known from now on by address (a stale entry only costs a dedup reset) */
            return 1;
        }
        if (e->size == 0) {
            return 0;
        }
    }
    return 0;
}

/* gxrecfuse: the material and ambient colours by component, as GXSetChanMatColor and GXSetChanAmbColor write them
 * (a COLOR0 call writes red, green and blue of colour 0, an ALPHA0 call its alpha, COLOR0A0 both), so that calls
 * on overlapping channels compare with what the translator holds instead of forgetting each other. */
static int dd_col_same(u32 gen, u32 id, const u32* w) {
    u32 ch = w[0], c = w[1];
    u32 idx, parts; /* bit 0: rgb, bit 1: alpha */
    switch (ch) {
    case GX_COLOR0: idx = 0, parts = 1; break;
    case GX_COLOR1: idx = 1, parts = 1; break;
    case GX_ALPHA0: idx = 0, parts = 2; break;
    case GX_ALPHA1: idx = 1, parts = 2; break;
    case GX_COLOR0A0: idx = 0, parts = 3; break;
    case GX_COLOR1A1: idx = 1, parts = 3; break;
    default: return 0; /* recorded */
    }
    __typeof__(s_dd_col[0])* t = &s_dd_col[id == REC_SetChanMatColor];
    int same = 1;
    if (parts & 1) {
        same &= t->gen[idx][0] == gen && (t->val[idx] & 0xFFFFFF00u) == (c & 0xFFFFFF00u);
        t->val[idx] = (t->val[idx] & 0xFFu) | (c & 0xFFFFFF00u);
        t->gen[idx][0] = gen;
    }
    if (parts & 2) {
        same &= t->gen[idx][1] == gen && (t->val[idx] & 0xFFu) == (c & 0xFFu);
        t->val[idx] = (t->val[idx] & 0xFFFFFF00u) | (c & 0xFFu);
        t->gen[idx][1] = gen;
    }
    return same;
}

/* gxrecfuse: GXSetChanCtrl by channel (COLOR0A0 writes COLOR0 and ALPHA0, COLOR1A1 COLOR1 and ALPHA1), the fields
 * in the form the setter stores them */
static int dd_chan_same(u32 gen, const u32* w) {
    u32 first, last;
    switch (w[0]) {
    case GX_COLOR0A0: first = 0, last = 2; break;
    case GX_COLOR1A1: first = 1, last = 3; break;
    default:
        if (w[0] > 3) {
            return 0; /* recorded */
        }
        first = last = w[0];
        break;
    }
    u32 a = (w[1] & 0xFF) | (w[2] & 0xFF) << 8 | (w[3] & 0xFF) << 16 | (w[5] & 0xFF) << 24, b = w[4], c6 = w[6] & 0xFF;
    int same = 1;
    for (u32 c = first; c <= last; c += 2) {
        same &= s_dd_chan[c].gen == gen && s_dd_chan[c].a == a && s_dd_chan[c].mask == b && s_dd_chan[c].attn == c6;
        s_dd_chan[c].gen = gen;
        s_dd_chan[c].a = a;
        s_dd_chan[c].mask = b;
        s_dd_chan[c].attn = c6;
    }
    return same;
}

/* 1: table `tbl` (at generation `gen`) holds call `id` with these words; else it notes them. */
static int dd_same(DdSlot* tbl, u32 gen, u32 id, const u32* w, u32 n) {
    if (s_fuse && (s_fuse_parts & 4) && tbl == s_dd) {
        if (n == 2 && (id == REC_SetChanAmbColor || id == REC_SetChanMatColor)) {
            return dd_col_same(gen, id, w);
        }
        if (n == 7 && id == REC_SetChanCtrl) {
            return dd_chan_same(gen, w);
        }
    }
    int sl = n != 0 && n <= DD_WORDS ? dd_slot(id, w) : -1;
    if (sl >= 0) {
        DdSlot* d = &tbl[sl];
        /* one pass: the words are noted while they are compared (a store of an equal word changes nothing), with
         * no libc call; two words a turn so the compiler keeps the loop */
        u32 diff = (d->gen ^ gen) | ((u32)d->id ^ id) | ((u32)d->n ^ n);
        u32 i = 0;
        for (; i + 2 <= n; i += 2) {
            u32 a0 = w[i], a1 = w[i + 1];
            diff |= (d->w[i] ^ a0) | (d->w[i + 1] ^ a1);
            d->w[i] = a0;
            d->w[i + 1] = a1;
        }
        if (i < n) {
            diff |= d->w[i] ^ w[i];
            d->w[i] = w[i];
        }
        if (diff == 0) {
            return 1;
        }
        d->gen = gen;
        d->id = (u8)id;
        d->n = (u8)n;
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

/* ---- Fused state (switch gxrecfuse, default 0) ----
 * Two kinds of state calls reach the translator although they change nothing there:
 *   - the calls after a call to a state list (a display list the game built with state calls in it: mapdrv's
 *     material lists). The dedup table forgot everything at such a call, so every call after it was recorded
 *     again; and the list's own calls (about 24 a list) ran on the translator, most of them with the values in
 *     force (5519 of 5855 a frame in gor_01).
 *   - the vertex description: the game calls GXClearVtxDesc and then GXSetVtxDesc for each attribute before
 *     each object, and the sequence mostly gives the description in force.
 * gxrecfuse=1:
 *   - A state list made only of NOPs and port opcodes, whose bytes equal a copy kept on the game thread (each call
 *     compares them in full; a changed list is decoded again), is not recorded as a list call. Its calls go
 *     through the dedup table in their order, as calls the game made itself, and only those that change the
 *     translator's state are recorded. The game thread's shadow does not take them (it did not before either:
 *     on the GameCube a display list does not change the SDK's copy of the registers). Any other list takes the
 *     old way (the table forgets, a list call is recorded).
 *   - GXClearVtxDesc and GXSetVtxDesc calls go into a log on the game thread (and into its shadow, as before).
 *     Before the next recorded command that is not a state call (a list, primitive, raw command, copy, sync) or
 *     fence, the log is applied to a copy of the translator's vertex description, and only the attributes that
 *     differ are recorded, as GXSetVtxDesc calls (no setter reads the description, so the state calls recorded
 *     in between may go first). When that copy is not known (start, after the table forgets), the log is
 *     recorded from its last GXClearVtxDesc on, or all of it when it has none.
 *   - GXSetChanCtrl, GXSetChanAmbColor and GXSetChanMatColor are compared in the table by channel and colour
 *     component (dd_chan_same, dd_col_same), so calls on overlapping channels (COLOR0A0, COLOR0, ALPHA0) no longer
 *     make each other count as changed.
 * The translator's state after each command is the same as before, and so are the draws; fewer state version
 * changes let more lists continue a batch (the merged draws carry the same vertices in the same order).
 * gxrecfuse=2: as 1, plus checks on the translator: after the calls of a fused list the list itself is applied
 * too and must change nothing in `gx`; after each vertex-description flush, `gx`'s description must equal the
 * game thread's shadow (the `gxrecfuse frame N:` line with gxthreadstats counts checks and mismatches). */
static u8 s_vd_em[GX_VA_MAX_ATTR];
/* Pending calls (s_vd_n is not 0 while any is pending): when s_vd_em was known at the first one, they are applied at
 * once to s_vd_cur (s_vd_incur); else they are kept in the log. */
static u8 s_vd_cur[GX_VA_MAX_ATTR];
static int s_vd_incur;
#define VD_LOG 64
static u8 s_vd_log[VD_LOG][2]; /* attribute (0xFF: GXClearVtxDesc), type */

static inline void rec_words(u32 id, const u32* w, u32 n) {
    u32* c = put(id, n);
    for (u32 i = 0; i < n; i++) {
        c[i] = w[i];
    }
    commit(n, 0);
}

/* GXClearVtxDesc (attr 0xFF) and GXSetVtxDesc on a description, as vtx_desc_clear and vtx_desc_set do */
static inline void vd_apply(u8* d, u32 attr, u32 type) {
    if (attr == 0xFF) {
        memset(d, GX_NONE, GX_VA_MAX_ATTR);
        d[GX_VA_POS] = GX_DIRECT;
    } else if (attr == GX_VA_NBT) {
        d[GX_VA_NBT] = (u8)type;
        d[GX_VA_NRM] = GX_NONE;
    } else if (attr < GX_VA_MAX_ATTR) {
        d[attr] = (u8)type;
        if (attr == GX_VA_NRM) {
            d[GX_VA_NBT] = GX_NONE;
        }
    }
}

/* Records the GXSetVtxDesc calls that take the translator's description (s_vd_em) to t; s_vd_em becomes t. */
static void vd_send(const u8* t) {
    for (u32 a = 0; a < GX_VA_MAX_ATTR; a++) {
        if (a == GX_VA_NRM || a == GX_VA_NBT || t[a] == s_vd_em[a]) {
            continue;
        }
        const u32 w[2] = {a, t[a]};
        rec_words(REC_SetVtxDesc, w, 2);
        s_fst.vd_sent++;
    }
    if (t[GX_VA_NRM] != s_vd_em[GX_VA_NRM] || t[GX_VA_NBT] != s_vd_em[GX_VA_NBT]) {
        /* the setter clears the other one of the pair; at most one of them is set */
        const u32 w[2] = {t[GX_VA_NBT] != GX_NONE ? (u32)GX_VA_NBT : (u32)GX_VA_NRM,
                          t[GX_VA_NBT] != GX_NONE ? t[GX_VA_NBT] : t[GX_VA_NRM]};
        rec_words(REC_SetVtxDesc, w, 2);
        s_fst.vd_sent++;
    }
    memcpy(s_vd_em, t, GX_VA_MAX_ATTR);
}

static void fuse_vd_flush(void) {
    u32 n = s_vd_n;
    s_vd_n = 0; /* before any put: put flushes while it is not 0 */
    s_fst.vd_flushes++;
    if (s_vd_incur) {
        /* s_vd_em was the translator's description at the first pending call, and nothing recorded since changed
         * it (a list that takes the old way forgets before it is recorded, and its record flushes first) */
        s_vd_incur = 0;
        if (memcmp(s_vd_cur, s_vd_em, sizeof(s_vd_em)) != 0) {
            vd_send(s_vd_cur);
        }
    } else {
        u32 from = 0;
        if (!s_vd_valid) {
            int k = -1;
            for (u32 i = 0; i < n; i++) {
                if (s_vd_log[i][0] == 0xFF) {
                    k = (int)i;
                }
            }
            if (k < 0) {
                /* the translator's description is not known and nothing in the log resets it: the calls as made */
                for (u32 i = 0; i < n; i++) {
                    const u32 w[2] = {s_vd_log[i][0], s_vd_log[i][1]};
                    rec_words(REC_SetVtxDesc, w, 2);
                    s_fst.vd_sent++;
                }
                goto check;
            }
            rec_words(REC_ClearVtxDesc, NULL, 0);
            s_fst.vd_sent++;
            vd_apply(s_vd_em, 0xFF, 0);
            s_vd_valid = 1;
            from = (u32)k + 1;
        }
        u8 t[GX_VA_MAX_ATTR];
        memcpy(t, s_vd_em, sizeof(t));
        for (u32 i = from; i < n; i++) {
            vd_apply(t, s_vd_log[i][0], s_vd_log[i][1]);
        }
        vd_send(t);
    }
check:
    if (s_fuse == 2) {
        u32 nw = (GX_VA_MAX_ATTR + 3) / 4;
        u32* c = put(GXR_FCHK_VD, nw);
        memset(c, 0, nw * 4);
        memcpy(c, gx_game.vtx_desc, GX_VA_MAX_ATTR);
        commit(nw, 0);
    }
}

void gx_rec_call(u32 id, const u32* w, u32 n) {
    if (s_fuse && (s_fuse_parts & 2) && (id == REC_SetVtxDesc || id == REC_ClearVtxDesc)) {
        u32 attr = id == REC_ClearVtxDesc ? 0xFF : (w[0] < 0xFF ? w[0] : 0xFE);
        u32 type = id == REC_ClearVtxDesc ? 0 : w[1];
        s_fst.vd_calls++;
        gx_shadow_call(id, w);
        if (s_vd_n == 0 && s_vd_valid) {
            s_vd_incur = 1;
            memcpy(s_vd_cur, s_vd_em, sizeof(s_vd_cur));
        }
        if (s_vd_incur) {
            vd_apply(s_vd_cur, attr, type);
            s_vd_n = 1;
            return;
        }
        if (s_vd_n == VD_LOG) {
            fuse_vd_flush();
        }
        s_vd_log[s_vd_n][0] = (u8)attr;
        s_vd_log[s_vd_n][1] = (u8)type;
        s_vd_n++;
        return;
    }
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

/* gxrecfuse: decoded state lists, keyed by address and size, with a copy of their bytes (game thread) */
#define FL_SLOTS 2048
#define FL_POOL_WORDS (256 * 1024)
typedef struct {
    const void* list;
    u32 nbytes;
    u32 pos;   /* pool word of the copy of the bytes; the calls (id | count << 8, then the words) follow */
    u32 words; /* of the calls; 0: not a list of state calls only (the old way) */
    u32 why;   /* words 0: 1 a vertex-description call, 2 bytes after the calls (primitives, raw), 3 another call */
} FlEntry;
static FlEntry* s_fl;
static u32* s_fl_pool;
static u32 s_fl_used;

/* The entry for this list with its bytes as they are now, or NULL. */
static FlEntry* fl_get(const void* list, u32 nbytes) {
    u32 h = ((u32)(uintptr_t)list >> 5 ^ nbytes * 0x9E3779B1u) * 2654435761u >> 21;
    FlEntry* e = NULL;
    FlEntry* freeslot = NULL;
    for (u32 i = 0; i < 4; i++) {
        FlEntry* c = &s_fl[(h + i) & (FL_SLOTS - 1)];
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
        if (((uintptr_t)list & 3) == 0) {
            /* word compare (lists are 32-byte aligned on the GameCube; libc's memcmp goes by bytes) */
            const u32* a = (const u32*)list;
            const u32* b = s_fl_pool + e->pos;
            u32 nw = nbytes >> 2, diff = 0;
            for (u32 i = 0; i < nw; i++) {
                diff |= a[i] ^ b[i];
            }
            same = diff == 0 && memcmp((const u8*)list + nw * 4, b + nw, nbytes & 3) == 0;
        } else {
            same = memcmp(list, s_fl_pool + e->pos, nbytes) == 0;
        }
        if (same) {
            return e;
        }
    }
    if (e == NULL) {
        e = freeslot != NULL ? freeslot : &s_fl[h & (FL_SLOTS - 1)];
    }
    s_fst.list_decoded++;
    /* the copy, then the calls: a port opcode takes at least 4 bytes (3 and a word) for its 1 + n words, so the
     * calls need at most as many words as the list has bytes */
    u32 cw = (nbytes + 3) >> 2;
    if (s_fl_used + cw + nbytes + 64 > FL_POOL_WORDS) {
        memset(s_fl, 0, sizeof(FlEntry) * FL_SLOTS);
        s_fl_used = 0;
        e = &s_fl[h & (FL_SLOTS - 1)];
    }
    u32* calls = s_fl_pool + s_fl_used + cw;
    u32 nw = 0;
    u32 good = gx_sdl_scan((const u8*)list, nbytes, calls, nbytes + 64, &nw);
    memcpy(s_fl_pool + s_fl_used, list, nbytes);
    e->list = list;
    e->nbytes = nbytes;
    e->pos = s_fl_used;
    int pure = good == nbytes && nw != 0;
    e->why = pure ? 0 : 2;
    for (u32 k = 0; pure && k < nw; k += 1 + (calls[k] >> 8)) {
        u32 id = calls[k] & 0xFF;
        if (id == REC_SetVtxDesc || id == REC_ClearVtxDesc || id == 0 || id > REC_SetTevIndirect) {
            pure = 0; /* the vertex description keeps its own copy (fuse_vd_flush): such a list takes the old way */
            e->why = id == REC_SetVtxDesc || id == REC_ClearVtxDesc ? 1 : 3;
        }
    }
    /* the dedup slot of each call, resolved once: id | count << 8 | (slot + 1) << 16, bit 31 for the calls the
     * table compares its own way (dd_same) */
    for (u32 k = 0; pure && k < nw; k += 1 + (calls[k] >> 8 & 0xFF)) {
        u32 id = calls[k] & 0xFF, n = calls[k] >> 8 & 0xFF;
        if (id == REC_SetChanCtrl || id == REC_SetChanAmbColor || id == REC_SetChanMatColor) {
            calls[k] |= 1u << 31;
        } else {
            int sl = n != 0 && n <= DD_WORDS ? dd_slot(id, calls + k + 1) : -1;
            calls[k] |= (u32)(sl + 1) << 16;
        }
    }
    e->words = pure ? nw : 0;
    s_fl_used += cw + (pure ? nw : 0);
    return e;
}

/* fuse_list: a call that compares its own way (bit 31), has no slot, or whose slot differs. Notes it in the table as
 * dd_same does and records it; 1 when recorded. */
static __attribute__((noinline)) u32 fuse_call_slow(u32 hd, const u32* c, u32 gen) {
    u32 id = hd & 0xFF, n = hd >> 8 & 0xFF, sl = hd >> 16 & 0xFFF;
    if (hd >> 31) {
        if (dd_same(s_dd, gen, id, c, n)) {
            return 0;
        }
    } else if (sl != 0) {
        /* the loop found the slot different */
        DdSlot* d = &s_dd[sl - 1];
        d->gen = gen;
        d->id = (u8)id;
        d->n = (u8)n;
        for (u32 i = 0; i < n; i++) {
            d->w[i] = c[i];
        }
    }
    rec_words(id, c, n); /* not into the shadow (see above) */
    return 1;
}

/* A state list's calls through the dedup table: 1 when done, 0 when the list takes the old way. */
static int fuse_list(const void* list, u32 nbytes) {
    if (nbytes == 0 || nbytes > 0x4000) {
        return 0;
    }
    if (s_fl == NULL) {
        s_fl = calloc(FL_SLOTS, sizeof(FlEntry));
        s_fl_pool = malloc(sizeof(u32) * FL_POOL_WORDS);
        if (s_fl == NULL || s_fl_pool == NULL) {
            free(s_fl);
            free(s_fl_pool);
            s_fl = NULL;
            s_fl_pool = NULL;
            s_fuse = 0;
            return 0;
        }
    }
    const FlEntry* e = fl_get(list, nbytes);
    if (e->words == 0) {
        s_fst.list_other++;
        s_fst.other_why[e->why & 3]++;
        return 0;
    }
    s_fst.lists++;
    const u32* c = s_fl_pool + e->pos + ((e->nbytes + 3) >> 2);
    const u32* ce = c + e->words;
    const u32 gen = s_dd_gen;
    const DdSlot* const dd = s_dd;
    u32 calls = 0, sent = 0;
    /* dd_same's compare with the slot known; a call that compares its own way, has no slot or changed goes to
     * fuse_call_slow (5519 of 5856 calls a frame in gor_01 are unchanged), so the loop keeps its values in registers */
    while (c < ce) {
        u32 hd = *c++;
        u32 n = hd >> 8 & 0xFF, sl = hd >> 16 & 0xFFF;
        calls++;
        if (__builtin_expect((s32)hd < 0 || sl == 0, 0)) {
            sent += fuse_call_slow(hd, c, gen);
            c += n;
            continue;
        }
        const DdSlot* d = &dd[sl - 1];
        u32 diff = (d->gen ^ gen) | ((u32)d->id ^ (hd & 0xFF)) | ((u32)d->n ^ n);
        const u32* dw = d->w;
        switch (n) { /* n <= DD_WORDS for a call with a slot */
        default:
            for (u32 i = 8; i < n; i++) {
                diff |= dw[i] ^ c[i];
            }
            /* fallthrough */
        case 8: diff |= dw[7] ^ c[7]; /* fallthrough */
        case 7: diff |= dw[6] ^ c[6]; /* fallthrough */
        case 6: diff |= dw[5] ^ c[5]; /* fallthrough */
        case 5: diff |= dw[4] ^ c[4]; /* fallthrough */
        case 4: diff |= dw[3] ^ c[3]; /* fallthrough */
        case 3: diff |= dw[2] ^ c[2]; /* fallthrough */
        case 2: diff |= dw[1] ^ c[1]; /* fallthrough */
        case 1: diff |= dw[0] ^ c[0]; /* fallthrough */
        case 0: break;
        }
        if (__builtin_expect(diff != 0, 0)) {
            sent += fuse_call_slow(hd, c, gen);
        }
        c += n;
    }
    s_fst.list_calls += calls;
    s_fst.list_sent += sent;
    s_dd_calls += calls;
    s_dd_skipped += calls - sent;
    if (s_fuse == 2) {
        u32* w = put(GXR_FCHK_LIST, 2);
        w[0] = (u32)list;
        w[1] = nbytes;
        commit(2, 0);
    }
    return 1;
}

/* switch gxreclgroup (default 0): display-list calls that follow each other in the record go into one GXR_LIST
 * command (3 words a call), extended while the translator has not been given it. The translator calls them in the
 * same order as before; only the per-command header, dispatch and the publish after each list are saved. A group is
 * published once KICK_WORDS words wait (as the state calls are) or by the next command that kicks. */
static int s_lgroup;
#define LG_MAX_CALLS 85u  /* 255 words */
static u32* s_lg_hdrp;    /* the open group's header */
static u32 s_lg_n, s_lg_lim; /* its words; the ring position it may grow to */
static u32 s_lg_groups, s_lg_calls; /* counters (gxthreadstats) */

static void rec_list_grouped(const void* list, u32 nbytes, const void* caller) {
    if (s_vd_n != 0) {
        fuse_vd_flush(); /* the vertex description goes before the list */
    }
    if (s_wr == s_lg_end && (s32)(s_lg_lim - (s_wr + 3)) >= 0) {
        /* the open group is the last command and the translator has not been given it (a publish closes it) */
        u32* c = s_ring + (s_wr & s_ring_mask);
        c[0] = (u32)list;
        c[1] = nbytes;
        c[2] = (u32)caller;
        s_lg_n += 3;
        *s_lg_hdrp = HDR(GXR_LIST, s_lg_n);
        s_wr += 3;
        s_lg_end = s_wr;
        s_gs.words += 3;
        s_gs.lists++;
        s_lists_rec++;
        s_lg_calls++;
        if (gx_rec_mode == 2 && s_wr - s_wr_pub >= KICK_WORDS) {
            publish();
        }
        return;
    }
    u32* c = put(GXR_LIST, 3);
    c[0] = (u32)list;
    c[1] = nbytes;
    c[2] = (u32)caller;
    s_gs.lists++;
    s_lists_rec++;
    s_lg_groups++;
    s_lg_calls++;
    commit(3, 0);
    /* the group may grow up to LG_MAX_CALLS, the ring's end (no wrap inside a command) and the room seen now */
    u32 off = (s_wr - 4) & s_ring_mask, room = s_ring_words - off - 1;
    u32 lim = s_wr - 3 + (room < LG_MAX_CALLS * 3 ? room : LG_MAX_CALLS * 3);
    s_lg_lim = (s32)(s_room_end - lim) < 0 ? s_room_end : lim;
    s_lg_hdrp = c - 1;
    s_lg_n = 3;
    s_lg_end = gx_rec_mode == 2 && s_wr_pub == s_wr ? 0 : s_wr;
}

void gx_rec_list(const void* list, u32 nbytes, const void* caller) {
    if (s_dedup && is_state_list(list, nbytes)) {
        if (s_fuse && (s_fuse_parts & 1) && fuse_list(list, nbytes)) {
            return;
        }
        dd_forget(); /* its state calls change the translator's state */
    }
    if (s_lgroup && gx_rec_mode != 3) {
        rec_list_grouped(list, nbytes, caller);
        return;
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

/* The hash of a state's vertex description and format `fmt` (g is &gx or &gx_game). Kept per state and format
 * while that state's gx_vdesc_ver stands still (every write to either bumps it), so a run of primitives under one
 * vertex state hashes once. */
static u32 vtx_state_hash(const GxState* g, u32 fmt) {
    static struct {
        u64 ver; /* 0: nothing kept (the counters start at 1) */
        u32 hash;
    } s_kept[2][GX_MAX_VTXFMT];
    u32 w = g == &gx_game;
    if ((g == &gx || w) && fmt < GX_MAX_VTXFMT) {
        typeof(s_kept[0][0])* k = &s_kept[w][fmt];
        if (k->ver != gx_vdesc_ver[w]) {
            k->hash = gx_hash32(g->vat[fmt], sizeof(g->vat[fmt]), gx_hash32(g->vtx_desc, sizeof(g->vtx_desc), 0x51A9u));
            k->ver = gx_vdesc_ver[w];
        }
        return k->hash;
    }
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
        s_lg_end = 0; /* the group is translated now */
        gx_tls_translator = 1;
        gx_rec_fast_update();
        consume(upto);
        gx_tls_translator = 0;
        gx_rec_fast_update();
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
    if (s_vd_n != 0 && why != GXF_FULL) {
        fuse_vd_flush(); /* the translation the fence waits for includes the logged vertex description */
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
    gx_rec_fast_update();
    if (s_switch_at == 0) {
        gx_rec_switch();
    }
}

/* Direct translation ends for this frame: the game thread records from here. The translator is idle (the
 * ring is empty since the last fence), so the shadow can start from the translated state. */
void gx_rec_switch(void) {
    memcpy(&gx_game, &gx, sizeof(gx));
    gx_vdesc_ver[1]++;
    gx_rec_direct = 0;
    gx_rec_fast_update();
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
    gx_rec_fast_update();
    s_excl = 1;
}

void gx_rec_excl_end(void) {
    if (s_excl) {
        s_excl = 0;
        gx_tls_translator = 0;
        gx_rec_fast_update();
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
        PLOG_I("gxthread   dedup: %lu of %lu state calls per frame not recorded (unchanged); lists hashed as "
               "possible state-list copies: %lu, %lu bytes per frame",
               (unsigned long)(s_dd_skipped / f), (unsigned long)(s_dd_calls / f), (unsigned long)(s_sl_hashes / f),
               (unsigned long)(s_sl_hash_bytes / f));
        s_dd_skipped = s_dd_calls = 0;
    }
    s_sl_hashes = s_sl_hash_bytes = 0;
    if (gx_rec_mode == 2) {
        PLOG_I("gxthread   hand-off: the translator slept %lu times, the game thread signalled it %lu times per frame",
               (unsigned long)(__atomic_exchange_n(&s_ts_sleeps, 0, __ATOMIC_RELAXED) / f),
               (unsigned long)(s_gs_signals / f));
        s_gs_signals = 0;
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
    if (s_fuse) {
        PLOG_I("gxrecfuse frame %lu: per frame vtxdesc %lu calls, %lu flushes, %lu recorded | state lists %lu fused "
               "(%lu calls, %lu recorded), %lu the old way (vtxdesc %lu, more bytes %lu, other calls %lu), %lu decoded "
               "| verify %lu+%lu checks, %lu+%lu mismatches (total)",
               (unsigned long)s_frame_no, (unsigned long)(s_fst.vd_calls / f), (unsigned long)(s_fst.vd_flushes / f),
               (unsigned long)(s_fst.vd_sent / f), (unsigned long)(s_fst.lists / f),
               (unsigned long)(s_fst.list_calls / f), (unsigned long)(s_fst.list_sent / f),
               (unsigned long)(s_fst.list_other / f), (unsigned long)(s_fst.other_why[1] / f),
               (unsigned long)(s_fst.other_why[2] / f), (unsigned long)(s_fst.other_why[3] / f),
               (unsigned long)(s_fst.list_decoded / f),
               (unsigned long)s_fst.chk_list, (unsigned long)s_fst.chk_vd, (unsigned long)s_fst.chk_list_bad,
               (unsigned long)s_fst.chk_vd_bad);
        s_fst.vd_calls = s_fst.vd_flushes = s_fst.vd_sent = s_fst.lists = s_fst.list_calls = s_fst.list_sent = 0;
        s_fst.list_other = s_fst.list_decoded = 0;
        memset(s_fst.other_why, 0, sizeof(s_fst.other_why));
    }
    if (s_lgroup) {
        PLOG_I("gxreclgroup frame %lu: %lu list calls in %lu commands per frame", (unsigned long)s_frame_no,
               (unsigned long)(s_lg_calls / f), (unsigned long)(s_lg_groups / f));
        s_lg_calls = s_lg_groups = 0;
    }
    if (gx_ph_on) {
        gx_ph_log(s_frame_no, f);
    }
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
    gx_opt_sdl = plat_switch_int("gxsdl", 0) != 0;
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
    } else if (mode != 0 && plat_switch_int("gxonehop", 0) != 0) {
        /* gxonehop (plat/gpu.c): the game thread writes the PICA words itself, no translator */
        PLOG_W("gxthread=%d: off with gxonehop (no translator thread in that mode)", mode);
        mode = 0;
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
    s_state_content = calloc(SC_SLOTS, sizeof(*s_state_content));
    if (s_dd == NULL || s_state_lists == NULL || s_state_content == NULL) {
        s_dedup = 0;
    }
    s_fuse = s_dedup ? plat_switch_int("gxrecfuse", 0) : 0;
    if (s_fuse < 0 || s_fuse > 2) {
        s_fuse = 0;
    }
    s_fuse_parts = plat_switch_int("gxrecfuseparts", 7) & 7;
    s_lgroup = plat_switch_int("gxreclgroup", 0) != 0;
    gx_ph_on = plat_switch_int("gxphase", 0);
    if (gx_ph_on < 0) {
        gx_ph_on = 0;
    }
    s_ph_cd = 1;
    if (s_fuse) {
        PLOG_I("gxrecfuse=%d: state lists through the record's dedup, vertex description recorded as a difference%s",
               s_fuse, s_fuse == 2 ? " (verify)" : "");
    }
    s_ring_words = words;
    s_ring_mask = words - 1;
    if (mode == 2) {
        s_ev_work = plat_event_create();
        s_ev_done = plat_event_create();
        PlatThread* t = NULL;
        if (s_ev_work != NULL && s_ev_done != NULL && plat_is_new3ds()) {
            /* below the render thread (0x2E), above the job worker (0x30) */
            t = plat_thread_create(translator_main, NULL, TR_STACK, 0x2F, 2, "gxtranslate");
        }
        if (t == NULL) {
            PLOG_W("gxthread=2: no translator thread on core 2: recording and translating on the game thread (1)");
            mode = 1;
        }
    }
    gx_rec_mode = mode;
    gx_rec_fast_update();
    plat_memstable_set_hook(mem_hook);
    PLOG_I("gxthread=%d: GX calls recorded into a %lu KB ring, translated %s%s", mode,
           (unsigned long)(words / 256u), mode == 2 ? "on core 2" : "on the game thread at each fence",
           mode == 3 ? " (verify)" : "");
}
