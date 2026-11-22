/* GX front-end counters (switch gxstats=N).
 *
 * With gxstats=N the front end counts, per frame, the display lists it runs
 * (and how many of them repeat unchanged from an earlier frame: same
 * address, size and content hash), the vertices it decodes from lists and
 * from GXBegin/GXEnd, the draws it transforms, the GX features those draws
 * use (lighting, texgen, matrix indices, primitive types), and where the
 * game thread's time goes (system ticks around each stage). Every N frames
 * it logs the per-frame averages as "gxstats" lines. Without the switch
 * every hook is one branch on gx_perf_on.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gx_internal.h"
#include "plat/gpu.h"
#include "plat/plat.h"

GxPerf gx_perf;
int gx_perf_on = -1;

static GxPerf s_sum;
static u32 s_frames, s_every;
static u64 s_last_tick;
static u32 s_frame_no;
static u32 s_max_pnmtx;

u32 gx_perf_frame_no(void) { return s_frame_no; }

/* ---- Content hash (also the display-list cache's check) ---- */

u32 gx_hash32(const void* data, u32 n, u32 seed) {
    const u8* p = (const u8*)data;
    u32 h = seed ^ (n * 0x9E3779B1u);
    while (((uintptr_t)p & 3) != 0 && n > 0) {
        h = (h ^ *p++) * 0x01000193u;
        n--;
    }
    const u32* w = (const u32*)p;
    while (n >= 16) {
        h = (h ^ w[0]) * 0x9E3779B1u;
        h = (h ^ w[1]) * 0x85EBCA77u;
        h = (h ^ w[2]) * 0xC2B2AE3Du;
        h = (h ^ w[3]) * 0x27D4EB2Fu;
        h ^= h >> 15;
        w += 4;
        n -= 16;
    }
    while (n >= 4) {
        h = (h ^ *w++) * 0x9E3779B1u;
        h ^= h >> 15;
        n -= 4;
    }
    p = (const u8*)w;
    while (n > 0) {
        h = (h ^ *p++) * 0x01000193u;
        n--;
    }
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

/* ---- Repeated display lists ---- */

#define SEEN_SLOTS 16384
typedef struct {
    const void* list;
    u32 bytes;
    u32 hash;
    u32 frame; /* last frame it was called in */
} Seen;
static Seen s_seen[SEEN_SLOTS];

void gx_perf_dl(const void* list, u32 nbytes) {
    u32 h = gx_hash32(list, nbytes, 0);
    u32 i = ((u32)(uintptr_t)list >> 5 ^ nbytes) & (SEEN_SLOTS - 1);
    for (u32 probe = 0; probe < 16; probe++, i = (i + 1) & (SEEN_SLOTS - 1)) {
        Seen* s = &s_seen[i];
        if (s->list == list && s->bytes == nbytes) {
            if (s->hash == h) {
                if (s->frame != s_frame_no) {
                    gx_perf.dl_repeat++;
                    gx_perf.dl_repeat_bytes += nbytes;
                } else {
                    gx_perf.dl_again++; /* a second call in the same frame */
                }
            } else {
                gx_perf.dl_changed++;
            }
            s->hash = h;
            s->frame = s_frame_no;
            return;
        }
        if (s->list == NULL || s_frame_no - s->frame > 600) {
            s->list = list;
            s->bytes = nbytes;
            s->hash = h;
            s->frame = s_frame_no;
            gx_perf.dl_new++;
            return;
        }
    }
    gx_perf.dl_new++; /* table full around here: counted as new */
}

/* ---- Per-frame log ---- */

static u32 us(u64 ticks, u32 frames) { return (u32)(ticks / (PLAT_TICKS_PER_SEC / 1000000u) / (frames ? frames : 1)); }

void gx_perf_frame(void) {
    if (gx_perf_on < 0) {
        s_every = (u32)plat_switch_int("gxstats", 0);
        gx_perf_on = s_every > 0;
    }
    s_frame_no++;
    gx_dlcache_frame();
    if (!gx_perf_on) {
        return;
    }
    u64 now = plat_ticks();
    if (s_last_tick != 0) {
        gx_perf.t_frame = now - s_last_tick;
    }
    s_last_tick = now;
    /* accumulate the frame that just ended */
    {
        u32* d = (u32*)&s_sum;
        const u32* s = (const u32*)&gx_perf;
        size_t nw = offsetof(GxPerf, t_frame) / 4;
        for (size_t i = 0; i < nw; i++) {
            d[i] += s[i];
        }
        u64* dt = &s_sum.t_frame;
        const u64* st = &gx_perf.t_frame;
        size_t nt = (sizeof(GxPerf) - offsetof(GxPerf, t_frame)) / 8;
        for (size_t i = 0; i < nt; i++) {
            dt[i] += st[i];
        }
    }
    if (gx_perf.max_pnmtx_in_draw > s_max_pnmtx) {
        s_max_pnmtx = gx_perf.max_pnmtx_in_draw;
    }
    memset(&gx_perf, 0, sizeof(gx_perf));
    if (++s_frames < s_every) {
        return;
    }
    const GxPerf* a = &s_sum;
    u32 f = s_frames;
    PlatGpuStats gs;
    plat_gpu_stats(&gs);
    PLOG_I("gxstats frame %lu: avg of %lu frames: %lu us/frame | dl calls %lu (new %lu repeat %lu again %lu changed %lu; "
           "verts-only %lu state-only %lu mixed %lu raw %lu) bytes %lu (repeat %lu) verts %lu | imm prims %lu verts %lu",
           (unsigned long)s_frame_no, (unsigned long)f, (unsigned long)us(a->t_frame, f), (unsigned long)(a->dl_calls / f),
           (unsigned long)(a->dl_new / f), (unsigned long)(a->dl_repeat / f), (unsigned long)(a->dl_again / f),
           (unsigned long)(a->dl_changed / f), (unsigned long)(a->dl_vtx_only / f), (unsigned long)(a->dl_state_only / f),
           (unsigned long)(a->dl_mixed / f), (unsigned long)(a->dl_raw / f), (unsigned long)(a->dl_bytes / f),
           (unsigned long)(a->dl_repeat_bytes / f), (unsigned long)(a->dl_verts / f), (unsigned long)(a->imm_prims / f),
           (unsigned long)(a->imm_verts / f));
    PLOG_I("gxstats   xf draws %lu verts %lu tris %lu culled %lu lines %lu | builds %lu (unchanged %lu) | last frame: "
           "gpu draws %lu (of %lu calls) states %lu rec %lu B render %lu us gpu %lu us",
           (unsigned long)(a->xf_draws / f), (unsigned long)(a->xf_verts / f), (unsigned long)(a->tris_out / f),
           (unsigned long)(a->tris_culled / f), (unsigned long)(a->lines / f), (unsigned long)(a->builds / f),
           (unsigned long)(a->builds_same / f),
           (unsigned long)gs.draws, (unsigned long)gs.calls, (unsigned long)gs.state_changes, (unsigned long)gs.record_bytes,
           (unsigned long)gs.render_us, (unsigned long)gs.gpu_us);
    PLOG_I("gxstats   us: dl %lu (decode %lu) imm %lu (decode %lu) state %lu xform %lu emit %lu cache %lu",
           (unsigned long)us(a->t_dl, f), (unsigned long)us(a->t_dl_decode, f), (unsigned long)us(a->t_imm, f),
           (unsigned long)us(a->t_imm_decode, f), (unsigned long)us(a->t_state, f), (unsigned long)us(a->t_xform, f),
           (unsigned long)us(a->t_emit, f), (unsigned long)us(a->t_cache, f));
    PLOG_I("gxstats   verts: pnmtxidx %lu (max %lu mtx/draw) texmtxidx %lu nrm %lu behind-eye %lu | prim q %lu t %lu "
           "ts %lu tf %lu l %lu ls %lu p %lu",
           (unsigned long)(a->v_pnmtxidx / f), (unsigned long)s_max_pnmtx, (unsigned long)(a->v_texmtxidx / f),
           (unsigned long)(a->v_nrm / f), (unsigned long)(a->v_behind / f), (unsigned long)(a->v_prim[0] / f),
           (unsigned long)(a->v_prim[1] / f), (unsigned long)(a->v_prim[2] / f), (unsigned long)(a->v_prim[3] / f),
           (unsigned long)(a->v_prim[4] / f), (unsigned long)(a->v_prim[5] / f), (unsigned long)(a->v_prim[6] / f));
    PLOG_I("gxstats   light: unlit %lu lit %lu vertex-lights %lu (spot %lu spec %lu, dist-attn %lu) 2nd-chan %lu "
           "vtx-src %lu | texgen units: mtx2x4 %lu mtx3x4 %lu other %lu, src pos %lu nrm %lu tex %lu color %lu, "
           "post-mtx %lu normalize %lu",
           (unsigned long)(a->v_unlit / f), (unsigned long)(a->v_lit / f), (unsigned long)(a->v_light_evals / f),
           (unsigned long)(a->v_spot / f), (unsigned long)(a->v_spec / f), (unsigned long)(a->v_distattn / f),
           (unsigned long)(a->v_second_chan / f), (unsigned long)(a->v_vtx_src / f), (unsigned long)(a->v_tg_2x4 / f),
           (unsigned long)(a->v_tg_3x4 / f), (unsigned long)(a->v_tg_other / f), (unsigned long)(a->v_tg_pos / f),
           (unsigned long)(a->v_tg_nrm / f), (unsigned long)(a->v_tg_tex / f), (unsigned long)(a->v_tg_color / f),
           (unsigned long)(a->v_tg_post / f), (unsigned long)(a->v_tg_normalize / f));
    gx_dlcache_log(f);
    gx_vsh_log(f);
    memset(&s_sum, 0, sizeof(s_sum));
    s_frames = 0;
}
