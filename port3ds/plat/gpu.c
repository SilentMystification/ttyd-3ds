/* PICA200 back end for the GX front end (include/plat/gpu.h), on citro3d.
 *
 * Threads: the game thread records a frame (commands and vertices); the
 * render thread (core 2 on the New 3DS, core 0 with switch gxsync) replays
 * it into citro3d while the game thread records the next one. Every
 * citro3d call is made on the render thread.
 *
 * The EFB: a 512x1024 render target in the screens' rotated layout (power of
 * two, so EFB copies can sample it). Its 480x800 corner is twice the top
 * screen; the game's EFB (608x480 for TTYD) sits in it with its left edge
 * 96 pixels in. Presenting is a display transfer of that region scaled by
 * half into a linear 256x400 buffer, then a texture copy that drops the
 * 16 unused columns into the 240x400 framebuffer: the image is centred on
 * the 400x240 top screen with black borders.
 *
 * EFB coordinates (x right, y down, the game's) map to the rotated target
 * by efb_to_target(); vertex clip positions are rotated in the shader with
 * the same orientation (s_rot). The orientation was set from the gputest
 * self-test screenshots (switch gputest).
 */
#include <citro3d.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ctrdk/gpureset.h"
#include "gx_shbin.h"
#include "plat/gpu.h"
#include "plat/hud.h"
#include "plat/pmu.h"
#include "plat_internal.h"
#include "stereo.h"

/* The EFB target is TARGET_H rows (switch efb608: 608, else 1024), sampled as a texture through a VIEW_H = 1024 row view
 * at the colour buffer's address (the PICA samples only power-of-two sizes). Viewport pixel y of the target is memory
 * row TARGET_H - 1 - y, so texel v = (y + VIEW_V0) / VIEW_H with VIEW_V0 = VIEW_H - TARGET_H. */
#define TARGET_W 512
static int s_target_h = 1024;
static int s_region_h = 800;  /* the part shown, rotated: 800 (the picture centred) or 608 (it fills the target) */
static int s_efb_xoff = 96;   /* (REGION_H - 608) / 2: TTYD's 608-wide EFB centred */
static int s_efb608;          /* switch efb608 */
#define TARGET_H s_target_h
#define VIEW_H 1024
#define VIEW_V0 (VIEW_H - s_target_h)
#define REGION_W 480  /* the part shown: twice the top screen, rotated */
#define REGION_H s_region_h
#define EFB_X_OFFSET s_efb_xoff
#define MAX_TEX 4096
#define ARENA_VERTICES (96 * 1024)
#define RECORD_BYTES (512u << 10) /* two records fill the 1 MiB gx record budget in QTM */

#define INDEX_ARENA (192 * 1024) /* u16 indices of shader-path draws per record */
#define STREAM_PENDING 4096       /* stream frees waiting for the frames in flight */

enum { CMD_STATE_DELTA = 0x20, CMD_STATE = 1, CMD_FOG, CMD_DRAW, CMD_CLEAR, CMD_COPY, CMD_FRAME_END, CMD_SEGMENT_END, CMD_VSH_CONST, CMD_VSH_DRAW,
       CMD_VSH_LIGHT, CMD_VSH_DRAWP,
       /* gxonehop (the one-hop section below): PICA words the recording thread wrote, and the few steps only the
        * render thread can take (citro3d calls) */
       CMD_OH_WORDS = 0x30, CMD_OH_FLUSH, CMD_OH_UNIF, CMD_OH_CAPTURE, CMD_OH_MARK };
#define CMD_DRAW_DROP 0x80000000u /* gxonehop: CMD_DRAW count bit, the gpudrawcut/gpudrawskip filter dropped the draw */

/* CMD_VSH_DRAW body */
typedef struct {
    const void* stream;
    u32 layout, scales, cull;
    u32 first, count; /* indices in the record's index arena */
} VshDraw;

/* CMD_VSH_DRAWP body: a draw whose indices stay in linear memory (a prebuilt batch, front end switch gxbatch) */
typedef struct {
    const void* stream;
    u32 layout, scales, cull;
    const u16* idx;
    u32 count;
} VshDrawP;

typedef struct {
    u16 type;
    u16 size;
} CmdHead;

typedef struct {
    u8* data;
    u32 used;
    PlatGpuVertex* verts;
    u32 nverts;
    u16* idx; /* index arena (linear) of the shader-path draws */
    u32 nidx;
    int present;
    /* stream ranges the CPU wrote for this record: flushed by the render thread before it replays (a flush is a
     * blocking GSP request; on the recording thread it cost 5-8% of its time) */
    struct {
        uintptr_t lo, hi;
    } dirty[32];
    u32 ndirty;
    u32 seq; /* submit number (gpufence) */
    u32 frame; /* game frame (s_frame_counter) at submit, for the gpudump files */
    /* gxonehop */
    u32* ow;                   /* the record's PICA words (CMD_OH_WORDS and CMD_OH_MARK point into it) */
    u32 now;                   /* words used */
    u8* side;                  /* gxonehop=2: the one-hop commands, compared with the gxdirect replay, not replayed */
    u32 side_used;
    s32 oh_split;              /* byte offset in data of the last stereo mark (gxonehop=1), -1: none */
    u32 oh_tail_draws, oh_tail_z; /* draws after that mark, and those of them writing depth (stereostats) */
    void* caps[256];           /* the CMD_OH_CAPTURE bodies the render thread fills (imported at reuse) */
    u32 ncaps;
} Record;

/* GPU fence for the records (switch gpufence, default 1). The game thread refills a record's vertex and index
 * arenas (linear memory the GPU reads) right after handing over the next one; the render thread's
 * C3D_FrameEnd only queues the GPU work. C3D_FrameBegin waits until the queued work has finished, so once the
 * render thread is past it for record S, record S - 1 is done on the GPU: s_gpu_done_seq says so, and submit
 * waits for it before reusing that record's memory. Before, only the vblank wait of C3D_FRAME_SYNCDRAW on the
 * render thread kept the GPU ahead of the CPU, and only when there was one record per frame.
 * Switch gpusyncall=1: every record takes the vblank wait (the behaviour before Step 1). */
static int s_gpu_fence = 1, s_sync_all;
static u32 s_submit_seq, s_gpu_done_seq;
static LightEvent s_fence_ev;

static void gpu_done_publish(u32 seq) {
    __atomic_store_n(&s_gpu_done_seq, seq, __ATOMIC_RELEASE);
    LightEvent_Signal(&s_fence_ev);
}

/* Hang diagnosis: the render thread's phase and the record it works on, and the last plat_gpu_* call the game
 * thread entered. The watchdog report prints both (plat_gpu_phase_line); gpu_fence_wait logs them on a timeout. */
static u32 s_frame_counter;
static volatile int s_busy;
static volatile u32 s_rphase, s_rseq;
static const char* volatile s_gmark = "none";
/* render phases: 0 idle, 1 C3D_FrameBegin, 2 replay, 3 replay after a copy or a gxdirect step, 4 reap_targets,
 * 5 C3D_RenderTargetDelete, 6 copy, 7 C3D_FrameEnd, 8 gxdirect capture (citro3d setters into the scratch buffer),
 * 9 gxdirect shader draw */
#define RPHASE(n) (s_rphase = (n))
#define GMARK(name) (s_gmark = (name))

const char* plat_gpu_phase_line(void) {
    static char buf[160];
    snprintf(buf, sizeof(buf), "gpu: render phase %lu rec %lu busy %d done %lu game frame %lu last gpu call %s",
             (unsigned long)s_rphase, (unsigned long)s_rseq, (int)s_busy, (unsigned long)s_gpu_done_seq,
             (unsigned long)s_frame_counter, s_gmark);
    return buf;
}

/* plat/gpudump.c */
void plat_gpudump_init(void);
void plat_gpudump_record(u32 seq, u32 frame, const void* verts, u32 vert_bytes, const void* idx, u32 idx_bytes);
void plat_gpudump_hang(u32 seq, u32 done, u32 game_frame, const char* phase_line);
int plat_gpudump_test_hang(u32 frame);
int plat_gpudump_draw_filter(u32 frame, u32 seq, u32 idx); /* switches gpudrawcut / gpudrawskip: 0 drops the draw */
int plat_gpudump_test_hang_from(u32 frame);
static void gpudump_note(void);

static volatile int s_fence_overdue; /* a fence wait passed its first 2 s timeout and is still waiting */
static int gpu_reset_try(const char* why);
/* GPU hang reset (gpu_reset_try on the game thread, gpu_reset_apply on the render thread) */
static GpuResetLimiter s_reset_lim;
static volatile u32 s_reset_req, s_reset_seen; /* requested, applied by the render thread */
static u32 s_reset_count;                       /* resets so far (the counter in the log) */
static int s_hang_recover, s_hang_times = 1, s_hang_fired; /* switches gpuhangrecover, gpuhangtimes (test aid) */

static void gpu_fence_wait(u32 seq) {
    while ((s32)(__atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE) - seq) < 0) {
        LightEvent_Clear(&s_fence_ev);
        if ((s32)(__atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE) - seq) >= 0) {
            break;
        }
        u64 tw = svcGetSystemTick();
        if (LightEvent_WaitTimeout(&s_fence_ev, 2000000000LL) != 0 && svcGetSystemTick() - tw >= SYSCLOCK_ARM11 * 3 / 2) {
            PLOG_W("gpu: fence wait for seq %lu: done seq %lu, busy %d, game frame %lu, render phase %lu (rec %lu)",
                   (unsigned long)seq, (unsigned long)__atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE),
                   (int)s_busy, (unsigned long)s_frame_counter, (unsigned long)s_rphase, (unsigned long)s_rseq);
            __atomic_store_n(&s_fence_overdue, 1, __ATOMIC_RELEASE);
            /* First timeout: the command list of the record the GPU is stuck on, to sdmc:/3ds/ttyd/gpuhang/ */
            plat_gpudump_hang(seq, __atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE), s_frame_counter,
                              plat_gpu_phase_line());
            /* Reset the GPU and let the render thread rebuild citro3d: the wait then ends (done seq moves on). */
            gpu_reset_try("fence wait");
        }
    }
    __atomic_store_n(&s_fence_overdue, 0, __ATOMIC_RELEASE);
}

typedef struct {
    C3D_Tex tex;
    u8 live;
    u8 is_target;
    u8 pending_free;
    u8 via_scratch; /* a copy target in linear memory, filled through the VRAM scratch target */
    u32 free_after_frame;
    C3D_RenderTarget* target;
} TexSlot;

static Record s_rec[2];
static int s_cur;
static LightEvent s_submit, s_done;
static volatile int s_busy;
static int s_render_idx; /* record the render thread replays; set before s_busy */
static Thread s_thread;
static int s_threaded;
static C3D_RenderTarget* s_efb;
static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uloc_rot0, s_uloc_rot1;
/* shader path (vsh) uniforms */
static int s_uloc_vsh, s_uloc_clip, s_uloc_texrow, s_uloc_texsel, s_uloc_vscale, s_uloc_clrmix, s_uloc_matclr;
/* GX lighting in the shader (PlatGpuVshLight) */
static int s_uloc_lit, s_uloc_eyerow, s_uloc_nrmrow, s_uloc_amb, s_uloc_dmode, s_uloc_amode, s_uloc_lpos, s_uloc_ldir,
    s_uloc_lcol, s_uloc_lka, s_uloc_lkk, s_uloc_lcount;
static u32 s_last_vlight_id; /* the light block last recorded in the current record (0: none) */
static int s_uloc_tgsrc;
static PlatGpuVshLight s_last_vlight;
static C3D_AttrInfo s_attr;
static TexSlot s_tex[MAX_TEX];
/* Bound to texture units the draw does not use: citro3d's C3D_TexBind
 * cannot take NULL (it reads the texture's type). 8x8 opaque white. */
static C3D_Tex s_blank_tex;
static LightLock s_tex_lock;
static u32 s_frame_counter;
static PlatGpuState s_last_state;
static int s_last_state_valid; /* s_last_state was recorded in the current record */
static u32 s_last_fog_hash;
static u32 s_last_fog_id; /* PlatGpuState.fog_lut_id of the last LUT sent (0: s_last_fog_hash is) */
static PlatGpuStats s_stats, s_stats_frame;
static int s_rot;              /* orientation mode, see efb_to_target */
static u32 s_shots_every, s_shots_left, s_shots_from;
/* switch gxmerge (default 1): a draw that continues the previous one in the
 * vertex arena, with no command between them, extends its CMD_DRAW */
static int s_merge = 1;
static u32* s_last_draw;              /* body of the last command when it is a CMD_DRAW */
static PlatGpuVertex* s_last_alloc;   /* the last plat_gpu_alloc_vertices block, until its draw */
static VshDraw* s_last_vdraw;         /* body of the last command when it is a CMD_VSH_DRAW */
static PlatGpuVshConst s_last_vconst;
static int s_last_vconst_valid;       /* s_last_vconst was recorded in the current record */
static u32 s_last_vconst_id;
static struct {
    void* p;
    u32 frame;
} s_stream_pending[STREAM_PENDING];
static u32 s_stream_npending, s_stream_bytes;
static int s_depth_shots; /* switch gxdepthshot: every shot also saves the depth buffer */

/* ---- Orientation ----
 * EFB pixel (x, y) -> target pixel. Mode 0 is the layout citro3d uses for
 * the top screen (target x runs down the screen from the bottom edge... );
 * modes 1-3 rotate further. Default from the gputest screenshots. */
static void efb_to_target_rect(float x, float y, float w, float h, float* tx, float* ty, float* tw, float* th) {
    float ex = x + EFB_X_OFFSET;
    switch (s_rot) {
    case 1:
        *tx = y;
        *ty = ex;
        break;
    case 2:
        *tx = REGION_W - (y + h);
        *ty = REGION_H - (ex + w);
        break;
    case 3:
        *tx = y;
        *ty = REGION_H - (ex + w);
        break;
    default:
        *tx = REGION_W - (y + h);
        *ty = ex;
        break;
    }
    *tw = h;
    *th = w;
}

static void rot_uniforms(void) {
    /* clip (x, y) -> target clip; mirrors efb_to_target_rect */
    C3D_FVec r0, r1;
    switch (s_rot) {
    case 1: r0 = FVec4_New(0, -1, 0, 0); r1 = FVec4_New(1, 0, 0, 0); break;
    case 2: r0 = FVec4_New(0, 1, 0, 0); r1 = FVec4_New(-1, 0, 0, 0); break;
    case 3: r0 = FVec4_New(0, -1, 0, 0); r1 = FVec4_New(-1, 0, 0, 0); break;
    default: r0 = FVec4_New(0, 1, 0, 0); r1 = FVec4_New(1, 0, 0, 0); break;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot0, r0.x, r0.y, r0.z, r0.w);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot1, r1.x, r1.y, r1.z, r1.w);
}

/* ---- Recording (game thread) ---- */

static void flush_segment(void);

/* Switch gxonehop (stage 5, PORT_3DS_GX_PERF.md; the one-hop section below): 0 off; 1 the recording thread writes
 * the shader path's PICA words itself (the render thread copies them); 2 as gxdirect=1, and the words of mode 1 are
 * built too and compared with the replay's. */
static int s_onehop;
static void oh_close(void);       /* the open run of words becomes a CMD_OH_WORDS */
static void oh_event(void);       /* something the record would hold came: no draw merges across it */
/* a CPU-path draw the replay skips (no state in the record, under 3 vertices), one it draws through citro3d, a
 * clear, a copy */
enum { OHC_DRAW0, OHC_DRAW, OHC_CLEAR, OHC_COPY };
static void oh_c3d_cmd(int what); /* before the command: stereo mark, gxonehop=1 the state as a CMD_STATE */
static void oh_after_c3d(int what);
static int oh_cpu_draw_keep(void); /* gxonehop=1: the gpudrawcut/gpudrawskip filter for a CPU-path draw */
static void oh_need_room(u32 bytes, u32 words);
static void oh_fog(const float* lut129);
static void oh_const(const PlatGpuVshConst* c);
static void oh_light(const PlatGpuVshLight* l);
static void oh_draw(const void* stream, u32 layout, u32 scales, u32 cull, const u16* idx, u32 count, int drawp,
                    int merged);
static void oh_record_start(void);
static void oh_record_end(int present);
static void oh_import(Record* r);
static void oh_log(void);
static int oh_init(void);
static int s_oh_state_pending; /* the state changed since the words last sent it (mirrors the replay's) */
static u32 s_oh_idx_end;        /* index arena end of the last mergeable shader draw (gxonehop=1) */

static void* rec_put(u16 type, u32 size) {
    Record* r = &s_rec[s_cur];
    u32 need = (u32)sizeof(CmdHead) + ((size + 3u) & ~3u);
    if (s_onehop == 1 && type != CMD_OH_WORDS) {
        oh_close(); /* the words so far go before this command */
        r = &s_rec[s_cur];
    }
    /* keep room for the end marker; a full record is drawn as a segment */
    if (type != CMD_SEGMENT_END && type != CMD_FRAME_END && type != CMD_OH_WORDS && r->used + need + 64 > RECORD_BYTES) {
        flush_segment();
        r = &s_rec[s_cur];
    }
    if (r->used + need > RECORD_BYTES) {
        static int warned;
        if (!warned) {
            warned = 1;
            PLOG_E("gpu: frame record full (%u bytes): later commands this frame are dropped", RECORD_BYTES);
        }
        return NULL;
    }
    CmdHead* h = (CmdHead*)(r->data + r->used);
    h->type = type;
    h->size = (u16)(need - sizeof(CmdHead));
    r->used += need;
    s_last_draw = NULL; /* plat_gpu_draw sets it again for a draw */
    s_last_vdraw = NULL;
    return h + 1;
}

static volatile u32 s_apt_hold;
static volatile int s_apt_parked, s_apt_discard, s_apt_off;

static void wait_idle(void) {
    u64 t0 = svcGetSystemTick();
    int logged = 0;
    u64 next_reset = t0 + 5ull * SYSCLOCK_ARM11 / 2;
    while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) {
        /* Waits as long as the render thread works (a frame cannot be abandoned), but a render thread that does
         * not finish in 2.5 s is waiting for a hung GPU (C3D_FrameBegin or a transfer): reset the GPU, and again
         * every 2 s until the limit in gpu_reset_try. Not while the app is suspended or asleep. */
        if (LightEvent_WaitTimeout(&s_done, 1000000000LL) != 0) {
            u64 now = svcGetSystemTick();
            if (!logged && now - t0 > 2ull * SYSCLOCK_ARM11) {
                logged = 1;
                PLOG_W("gpu: the game thread has waited 2 s for the render thread (hold %u, parked %d, vblank %lu): %s",
                       (unsigned)s_apt_hold, s_apt_parked, (unsigned long)plat_vblank_count(), plat_gpu_phase_line());
            }
            if ((s64)(now - next_reset) >= 0 && !__atomic_load_n(&s_apt_hold, __ATOMIC_ACQUIRE) &&
                !__atomic_load_n(&s_apt_parked, __ATOMIC_ACQUIRE)) {
                next_reset = now + 2ull * SYSCLOCK_ARM11;
                __atomic_store_n(&s_fence_overdue, 1, __ATOMIC_RELEASE);
                plat_gpudump_hang(s_rseq, __atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE), s_frame_counter,
                                  plat_gpu_phase_line());
                gpu_reset_try("render thread stuck");
            }
        }
    }
    __atomic_store_n(&s_fence_overdue, 0, __ATOMIC_RELEASE);
}

static void render_record(Record* r);
static void gpu_reset_apply(void);
static int gpu_reset_pending(void);

static void stream_flush_dirty(void);
static void stream_take_dirty(Record* r);

static void submit(int present) {
    Record* r = &s_rec[s_cur];
    r->present = present;
    r->seq = ++s_submit_seq;
    r->frame = s_frame_counter;
    stream_take_dirty(r); /* the streams this record draws: the render thread flushes them */
    wait_idle(); /* the render thread finished the previous record */
    if (s_threaded) {
        /* hand over the record explicitly: s_cur flips below, and the render
         * thread reading it too early replayed the record being refilled */
        s_render_idx = s_cur;
        __atomic_store_n(&s_busy, 1, __ATOMIC_RELEASE);
        LightEvent_Signal(&s_submit);
    } else {
        render_record(r);
    }
    s_cur ^= 1;
    if (s_gpu_fence && s_rec[s_cur].seq != 0) {
        gpu_fence_wait(s_rec[s_cur].seq); /* the record refilled next is finished on the GPU */
    }
    if (s_onehop) {
        oh_import(&s_rec[s_cur]); /* the render thread is done with it: its captured words go into the tables */
    }
    s_rec[s_cur].used = 0;
    s_rec[s_cur].nverts = 0;
    s_rec[s_cur].nidx = 0;
    s_last_draw = NULL;
    s_last_vdraw = NULL;
    s_last_vconst_valid = 0;
    s_last_vlight_id = 0;
    s_last_alloc = NULL;
    memset(&s_last_state, 0xFF, sizeof(s_last_state));
    s_last_state_valid = 0;
    s_last_fog_hash = 0;
    s_last_fog_id = 0;
    if (s_onehop) {
        oh_record_start();
    }
}

/* The record is full mid-frame: hand it to the render thread as a segment
 * (drawn, not presented) and go on in the other one, with the current GX
 * state put back at its start. */
static void end_segment(void) {
    /* The replay skips draws until a record's first state command, so the
     * state in force goes at the start of the next record too. */
    PlatGpuState keep = s_last_state;
    int have_state = s_last_state_valid;
    PlatGpuVshConst keep_v = s_last_vconst;
    u32 keep_vid = s_last_vconst_id;
    int have_v = s_last_vconst_valid;
    u32 keep_lid = s_last_vlight_id;
    if (s_onehop) {
        oh_record_end(0);
    }
    rec_put(CMD_SEGMENT_END, 0);
    submit(0);
    if (keep_lid != 0) {
        plat_gpu_vsh_light(&s_last_vlight, keep_lid);
    }
    if (have_state) {
        plat_gpu_set_state(&keep);
    }
    if (have_v) {
        plat_gpu_vsh_const(&keep_v, keep_vid);
    }
}

static void flush_segment(void) {
    end_segment();
    s_stats_frame.segments++;
}

PlatGpuVertex* plat_gpu_alloc_vertices(uint32_t count) {
    Record* r = &s_rec[s_cur];
    if (count > ARENA_VERTICES) {
        PLOG_E("gpu: a draw of %lu vertices is larger than the vertex arena", (unsigned long)count);
        return NULL;
    }
    /* the draw command these vertices get must go into the same record:
     * room for it is made here, so plat_gpu_draw never starts a segment */
    if (r->nverts + count > ARENA_VERTICES || r->used + sizeof(CmdHead) + 8 + 64 > RECORD_BYTES) {
        flush_segment();
        r = &s_rec[s_cur];
    }
    if (s_onehop) {
        oh_need_room(0, 0); /* and for what gxonehop puts before the draw (a state, a stereo mark) */
        r = &s_rec[s_cur];
        if (r->nverts + count > ARENA_VERTICES) {
            return NULL; /* cannot happen: a segment flush leaves an empty arena */
        }
    }
    PlatGpuVertex* v = r->verts + r->nverts;
    r->nverts += count;
    s_last_alloc = v;
    return v;
}

static u32 fog_hash(const PlatGpuState* st) {
    u32 h = 2166136261u ^ st->fog_enable ^ st->fog_color;
    const u8* p = (const u8*)st->fog_lut;
    for (size_t i = 0; i < sizeof(st->fog_lut); i += 4) {
        h = (h ^ (p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (u32)p[i + 3] << 24)) * 16777619u;
    }
    return h | 1u;
}

static int s_state_delta = 1; /* switch gxstatedelta (default 1): CMD_STATE_DELTA records */
static PlatGpuState s_rstate; /* render thread: the state the record's last CMD_STATE and deltas give */

void plat_gpu_set_state(const PlatGpuState* state) {
    if (state->fog_enable) {
        /* the front end's LUT id when it gives one (no hashing), else a hash of the LUT */
        u32 id = state->fog_lut_id;
        u32 h = id != 0 ? 0 : fog_hash(state);
        if (id != 0 ? id != s_last_fog_id : h != s_last_fog_hash) {
            if (s_onehop == 1) {
                oh_fog(state->fog_lut);
                s_last_fog_hash = h;
                s_last_fog_id = id;
            } else {
                float* lut = rec_put(CMD_FOG, sizeof(state->fog_lut) + 8);
                if (lut != NULL) {
                    memcpy(lut, state->fog_lut, sizeof(state->fog_lut));
                    s_last_fog_hash = h;
                    s_last_fog_id = id;
                    if (s_onehop) {
                        oh_fog(state->fog_lut);
                    }
                }
            }
        }
    }
    /* compare everything except the LUT and its id */
    size_t cmp = offsetof(PlatGpuState, fog_lut_id);
    if (memcmp(&s_last_state, state, cmp) == 0 &&
        memcmp((const u8*)&s_last_state + offsetof(PlatGpuState, scissor_x0),
               (const u8*)state + offsetof(PlatGpuState, scissor_x0),
               sizeof(PlatGpuState) - offsetof(PlatGpuState, scissor_x0)) == 0) {
        return;
    }
    if (s_onehop == 1) {
        /* the words send it at the next shader draw; a clear, copy or CPU draw gets it as a CMD_STATE */
        oh_event();
        s_last_state = *state;
        s_last_state_valid = 1;
        s_oh_state_pending = 1;
        s_stats_frame.state_changes++;
        return;
    }
    if (s_onehop) {
        s_oh_state_pending = 1; /* gxonehop=2: the replay's CMD_STATE or delta below */
    }
    if (s_last_state_valid && s_state_delta) {
        /* the words that changed since the last state of this record (gxstatedelta): most states differ from
         * the one before in a few words of their ~700 bytes */
        const u32* a = (const u32*)&s_last_state;
        const u32* b = (const u32*)state;
        u32 n = 0;
        const u32 nw = (u32)(sizeof(PlatGpuState) / 4);
        for (u32 i = 0; i < nw && n * 2 + 1 < nw / 2; i++) {
            n += a[i] != b[i];
        }
        if (n * 2 + 1 < nw / 2) {
            u32* d = rec_put(CMD_STATE_DELTA, 4 + n * 8);
            if (d != NULL) {
                d[0] = n;
                u32 k = 1;
                for (u32 i = 0; i < nw; i++) {
                    if (a[i] != b[i]) {
                        d[k++] = i;
                        d[k++] = b[i];
                    }
                }
                s_last_state = *state;
                s_stats_frame.state_changes++;
            }
            return;
        }
    }
    PlatGpuState* dst = rec_put(CMD_STATE, sizeof(PlatGpuState));
    if (dst != NULL) {
        *dst = *state;
        s_last_state = *state;
        s_last_state_valid = 1;
        s_stats_frame.state_changes++;
    }
}

/* The front end's switch gxdirty: the words outside `regions` are the ones of the last state it gave, so only the
 * regions' words are compared. The record gets what plat_gpu_set_state would put there (the same delta, in the same
 * word order, or the same full state). */
void plat_gpu_set_state_dirty(const PlatGpuState* state, uint32_t regions) {
    if (!s_last_state_valid || !s_state_delta) {
        plat_gpu_set_state(state);
        return;
    }
    enum { NW = sizeof(PlatGpuState) / 4 };
    static const u16 k_range[4][2] = {
        {0, offsetof(PlatGpuState, tex) / 4},
        {offsetof(PlatGpuState, tex) / 4, offsetof(PlatGpuState, alpha_test_enable) / 4},
        {offsetof(PlatGpuState, alpha_test_enable) / 4, offsetof(PlatGpuState, fog_lut_id) / 4},
        {offsetof(PlatGpuState, scissor_x0) / 4, NW},
    };
    _Static_assert(offsetof(PlatGpuState, tex) % 4 == 0 && offsetof(PlatGpuState, alpha_test_enable) % 4 == 0 &&
                       offsetof(PlatGpuState, fog_lut_id) % 4 == 0 && offsetof(PlatGpuState, scissor_x0) % 4 == 0,
                   "state regions are word ranges");
    if ((regions & PLAT_GPU_R_PIX) && state->fog_enable) {
        u32 id = state->fog_lut_id;
        u32 h = id != 0 ? 0 : fog_hash(state);
        if (id != 0 ? id != s_last_fog_id : h != s_last_fog_hash) {
            if (s_onehop == 1) {
                oh_fog(state->fog_lut);
                s_last_fog_hash = h;
                s_last_fog_id = id;
            } else {
                float* lut = rec_put(CMD_FOG, sizeof(state->fog_lut) + 8);
                if (lut != NULL) {
                    memcpy(lut, state->fog_lut, sizeof(state->fog_lut));
                    s_last_fog_hash = h;
                    s_last_fog_id = id;
                    if (s_onehop) {
                        oh_fog(state->fog_lut);
                    }
                }
            }
        }
    }
    u32* a = (u32*)&s_last_state;
    const u32* b = (const u32*)state;
    u16 idx[NW];
    u32 n = 0;
    for (u32 r = 0; r < 4; r++) {
        if (regions & (1u << r)) {
            for (u32 i = k_range[r][0]; i < k_range[r][1]; i++) {
                if (a[i] != b[i]) {
                    idx[n++] = (u16)i;
                }
            }
        }
    }
    if ((regions & PLAT_GPU_R_PIX) && state->fog_enable &&
        (state->fog_lut_id == 0 || state->fog_lut_id != s_last_state.fog_lut_id)) {
        /* the LUT words are not compared, but s_last_state carries them (end_segment gives it again). They are read
         * only with fog_enable (end_segment's set_state), and a nonzero id names the LUT's contents, so the copy is
         * made only for an enabled state whose LUT is not the kept one. */
        memcpy(&s_last_state.fog_lut_id, &state->fog_lut_id,
               offsetof(PlatGpuState, scissor_x0) - offsetof(PlatGpuState, fog_lut_id));
    }
    if (n == 0) {
        return;
    }
    if (s_onehop) {
        s_oh_state_pending = 1;
    }
    if (s_onehop == 1) {
        oh_event();
        for (u32 k = 0; k < n; k++) {
            a[idx[k]] = b[idx[k]];
        }
        s_stats_frame.state_changes++;
        return;
    }
    if (n * 2 + 1 < NW / 2) {
        u32* d = rec_put(CMD_STATE_DELTA, 4 + n * 8);
        if (d == NULL) {
            memset(&s_last_state, 0xFF, sizeof(s_last_state)); /* the record is full: the next state is whole */
            s_last_state_valid = 0;
            return;
        }
        d[0] = n;
        for (u32 k = 0; k < n; k++) {
            u32 i = idx[k];
            d[1 + 2 * k] = i;
            d[2 + 2 * k] = b[i];
            a[i] = b[i];
        }
        s_stats_frame.state_changes++;
        return;
    }
    PlatGpuState* dst = rec_put(CMD_STATE, sizeof(PlatGpuState));
    if (dst != NULL) {
        *dst = *state;
        s_last_state = *state;
        s_stats_frame.state_changes++;
    } else {
        memset(&s_last_state, 0xFF, sizeof(s_last_state)); /* the record is full: the next state is whole */
        s_last_state_valid = 0;
    }
}

void plat_gpu_draw(const PlatGpuVertex* vertices, uint32_t count) {
    Record* r = &s_rec[s_cur];
    if (s_merge && vertices != NULL && vertices == s_last_alloc &&
        (u32)(vertices - r->verts) + count <= r->nverts) {
        /* the last block allocated: give back what the draw did not use
         * (culled triangles), so the next block follows this draw directly */
        r->nverts = (u32)(vertices - r->verts) + count;
    }
    s_last_alloc = NULL;
    if (vertices == NULL || count == 0) {
        return;
    }
    u32 first = (u32)(vertices - r->verts);
    s_stats_frame.calls++;
    if (s_merge && s_last_draw != NULL && s_last_draw[0] + (s_last_draw[1] & ~CMD_DRAW_DROP) == first &&
        (s_last_draw[1] & ~CMD_DRAW_DROP) % 3 == 0) {
        /* same state, no command since, contiguous triangles: one draw */
        s_last_draw[1] += count;
        s_stats_frame.vertices += count;
        return;
    }
    /* the replay takes the citro3d path for it when the record has a state */
    int what = s_last_state_valid && count >= 3 ? OHC_DRAW : OHC_DRAW0;
    if (s_onehop) {
        oh_c3d_cmd(what); /* the stereo class of this draw; gxonehop=1: the state as a CMD_STATE */
    }
    u32* d = rec_put(CMD_DRAW, 8);
    if (d != NULL) {
        d[0] = first;
        d[1] = count;
        s_last_draw = d;
        s_stats_frame.draws++;
        s_stats_frame.vertices += count;
        if (s_onehop) {
            oh_after_c3d(what);
            if (s_onehop == 1 && what == OHC_DRAW && !oh_cpu_draw_keep()) {
                d[1] |= CMD_DRAW_DROP;
            }
            s_last_draw = d; /* oh_after_c3d forgets it with the shader draw */
        }
    }
}

/* ---- Shader path: streams and draws (game thread) ---- */

static void stream_reap(int all) {
    u32 k = 0;
    for (u32 i = 0; i < s_stream_npending; i++) {
        if (all || s_frame_counter >= s_stream_pending[i].frame) {
            s_stream_bytes -= linearGetSize(s_stream_pending[i].p);
            linearFree(s_stream_pending[i].p);
        } else {
            s_stream_pending[k++] = s_stream_pending[i];
        }
    }
    s_stream_npending = k;
}

void* plat_gpu_stream_alloc(uint32_t bytes) {
    stream_reap(0);
    void* p = linearMemAlign(bytes, 16);
    if (p != NULL) {
        s_stream_bytes += linearGetSize(p);
    }
    return p;
}

/* Written streams are flushed from the CPU cache once, before the record
 * that may draw them goes to the render thread (a flush per draw cost more
 * than the draw). Adjacent ranges merge. */
#define DIRTY_MAX 32
static struct {
    uintptr_t lo, hi;
} s_dirty[DIRTY_MAX];
static u32 s_ndirty;

static void stream_flush_dirty(void) {
    for (u32 i = 0; i < s_ndirty; i++) {
        GSPGPU_FlushDataCache((const void*)s_dirty[i].lo, (u32)(s_dirty[i].hi - s_dirty[i].lo));
    }
    s_ndirty = 0;
}

/* Switch gxflushrender (default 1): the record's stream flushes are made by the render thread before it replays
 * the record; 0 flushes on the recording thread at submit, as before. */
static int s_flush_on_render = 1;

static void stream_take_dirty(Record* r) {
    r->ndirty = 0;
    if (!s_flush_on_render || !s_threaded) {
        stream_flush_dirty();
        return;
    }
    memcpy(r->dirty, s_dirty, s_ndirty * sizeof(s_dirty[0]));
    r->ndirty = s_ndirty;
    s_ndirty = 0;
}

void plat_gpu_stream_commit(const void* p, uint32_t bytes) {
    uintptr_t lo = (uintptr_t)p, hi = lo + bytes;
    for (u32 i = 0; i < s_ndirty; i++) {
        if (lo <= s_dirty[i].hi && hi >= s_dirty[i].lo) {
            s_dirty[i].lo = lo < s_dirty[i].lo ? lo : s_dirty[i].lo;
            s_dirty[i].hi = hi > s_dirty[i].hi ? hi : s_dirty[i].hi;
            return;
        }
    }
    if (s_ndirty == DIRTY_MAX) {
        stream_flush_dirty();
    }
    s_dirty[s_ndirty].lo = lo;
    s_dirty[s_ndirty].hi = hi;
    s_ndirty++;
}

void plat_gpu_stream_free(void* p) {
    if (p == NULL) {
        return;
    }
    if (s_stream_npending == STREAM_PENDING) {
        /* the queue is full: wait until the render thread is idle, then nothing in flight reads them */
        plat_gpu_sync();
        stream_reap(1);
    }
    /* the record being written and the one being rendered may both read it */
    s_stream_pending[s_stream_npending].p = p;
    s_stream_pending[s_stream_npending].frame = s_frame_counter + 3;
    s_stream_npending++;
}

void plat_gpu_vsh_const(const PlatGpuVshConst* c, uint32_t id) {
    if (s_last_vconst_valid && (id != 0 ? id == s_last_vconst_id : memcmp(&s_last_vconst, c, sizeof(*c)) == 0)) {
        return;
    }
    if (s_onehop == 1) {
        s_last_vconst = *c;
        s_last_vconst_id = id;
        s_last_vconst_valid = 1;
        s_stats_frame.vsh_consts++;
        oh_const(c);
        return;
    }
    if (s_onehop) {
        oh_const(c);
    }
    PlatGpuVshConst* d = rec_put(CMD_VSH_CONST, sizeof(*c));
    if (d != NULL) {
        *d = *c;
        s_last_vconst = *c;
        s_last_vconst_id = id;
        s_last_vconst_valid = 1;
        s_stats_frame.vsh_consts++;
    }
}

/* The light block for the following lit shader draws; recorded when its id changed. */
void plat_gpu_vsh_light(const PlatGpuVshLight* l, uint32_t id) {
    if (id != 0 && id == s_last_vlight_id) {
        return;
    }
    if (s_onehop == 1) {
        s_last_vlight = *l;
        s_last_vlight_id = id;
        oh_light(l);
        return;
    }
    if (s_onehop) {
        oh_light(l);
    }
    PlatGpuVshLight* d = rec_put(CMD_VSH_LIGHT, sizeof(*l));
    if (d != NULL) {
        *d = *l;
        s_last_vlight = *l;
        s_last_vlight_id = id;
    }
}

void plat_gpu_vsh_draw(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, const uint16_t* idx,
                       uint32_t nidx) {
    if (stream == NULL || nidx < 3 || nidx > INDEX_ARENA || PLAT_VSH_STRIDE(layout) == 0) {
        return;
    }
    nidx -= nidx % 3;
    Record* r = &s_rec[s_cur];
    if (r->nidx + nidx > INDEX_ARENA || r->used + sizeof(CmdHead) + sizeof(VshDraw) + 64 > RECORD_BYTES) {
        flush_segment();
        r = &s_rec[s_cur];
    }
    if (s_onehop) {
        oh_need_room(0, 0);
        r = &s_rec[s_cur];
    }
    if (!s_last_vconst_valid) {
        return; /* no uniform block in this record (cannot happen: end_segment puts it back) */
    }
    u32 first = r->nidx;
    memcpy(r->idx + first, idx, nidx * sizeof(u16));
    r->nidx += nidx;
    s_stats_frame.vsh_calls++;
    s_stats_frame.vsh_indices += nidx;
    if (s_onehop == 1) {
        /* the merge test of the record below, on what the words hold */
        VshDraw* l = s_last_vdraw;
        int merged = s_merge && l != NULL && l->stream == stream && l->layout == layout && l->scales == scales &&
                     l->cull == cull && s_oh_idx_end == first;
        if (!merged) {
            s_stats_frame.vsh_draws++;
        }
        oh_draw(stream, layout, scales, cull, r->idx + first, nidx, 0, merged);
        return;
    }
    VshDraw* l = s_last_vdraw;
    if (s_merge && l != NULL && l->stream == stream && l->layout == layout && l->scales == scales && l->cull == cull &&
        l->first + l->count == first) {
        l->count += nidx; /* same uniforms, stream and scales, nothing recorded since: one draw */
        if (s_onehop) {
            oh_draw(stream, layout, scales, cull, r->idx + first, nidx, 0, 1);
        }
        return;
    }
    if (s_onehop) {
        oh_draw(stream, layout, scales, cull, r->idx + first, nidx, 0, 0);
    }
    VshDraw* d = rec_put(CMD_VSH_DRAW, sizeof(VshDraw));
    if (d != NULL) {
        *d = (VshDraw){stream, layout, scales, cull, first, nidx};
        s_last_vdraw = d;
        s_stats_frame.vsh_draws++;
    }
}

static const u8 *s_ixpool_lo, *s_ixpool_hi; /* gxbatch's index pool: every DRAWP index range lies inside it */
void plat_gpu_set_index_pool(const void* base, uint32_t bytes) {
    s_ixpool_lo = base;
    s_ixpool_hi = base ? (const u8*)base + bytes : NULL;
}

void plat_gpu_vsh_drawp(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, const uint16_t* idx,
                        uint32_t nidx, uint32_t calls) {
    nidx -= nidx % 3;
    if (stream == NULL || idx == NULL || nidx < 3 || PLAT_VSH_STRIDE(layout) == 0) {
        return;
    }
    Record* r = &s_rec[s_cur];
    if (r->used + sizeof(CmdHead) + sizeof(VshDrawP) + 64 > RECORD_BYTES) {
        flush_segment();
    }
    if (s_onehop) {
        oh_need_room(0, 0);
    }
    if (!s_last_vconst_valid) {
        return; /* no uniform block in this record (cannot happen: end_segment puts it back) */
    }
    s_stats_frame.vsh_calls += calls;
    s_stats_frame.vsh_indices += nidx;
    if (s_onehop) {
        oh_draw(stream, layout, scales, cull, idx, nidx, 1, 0);
        if (s_onehop == 1) {
            s_stats_frame.vsh_draws++;
            return;
        }
    }
    VshDrawP* d = rec_put(CMD_VSH_DRAWP, sizeof(VshDrawP));
    if (d != NULL) {
        *d = (VshDrawP){stream, layout, scales, cull, idx, nidx};
        s_stats_frame.vsh_draws++;
    }
}

void plat_gpu_clear(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t rgba, uint32_t depth24, int clear_color,
                    int clear_depth) {
    if (s_onehop) {
        oh_need_room(16, 0);
        oh_c3d_cmd(OHC_CLEAR);
    }
    u32* d = rec_put(CMD_CLEAR, 16);
    if (d != NULL) {
        d[0] = (u32)x | (u32)y << 16;
        d[1] = (u32)w | (u32)h << 16;
        d[2] = rgba;
        d[3] = depth24 | (u32)(clear_color ? 1u : 0u) << 24 | (u32)(clear_depth ? 1u : 0u) << 25;
    }
    if (s_onehop) {
        oh_after_c3d(OHC_CLEAR);
    }
}

void plat_gpu_copy_tex(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int half, uint32_t handle, uint32_t dst_w,
                       uint32_t dst_h) {
    GMARK("copy_tex");
    if (s_onehop) {
        oh_need_room(20, 0);
        oh_c3d_cmd(OHC_COPY);
    }
    u32* d = rec_put(CMD_COPY, 20);
    if (d != NULL) {
        d[0] = (u32)x | (u32)y << 16;
        d[1] = (u32)w | (u32)h << 16;
        d[2] = (u32)half;
        d[3] = handle;
        d[4] = dst_w | dst_h << 16;
    }
    if (s_onehop) {
        oh_after_c3d(OHC_COPY);
    }
}

void plat_gpu_frame_begin(void) {}

static uint32_t s_presents;
uint32_t plat_gpu_present_count(void) { return s_presents; }

void plat_gpu_frame_end(uint16_t efb_w, uint16_t efb_h) {
    GMARK("frame_end");
    s_presents++;
    (void)efb_w;
    (void)efb_h;
    if (s_onehop) {
        oh_need_room(0, 0);
        oh_record_end(1);
    }
    rec_put(CMD_FRAME_END, 0);
    s_stats_frame.record_bytes = s_rec[s_cur].used;
    submit(1);
    s_stats.draws = s_stats_frame.draws;
    s_stats.vertices = s_stats_frame.vertices;
    s_stats.state_changes = s_stats_frame.state_changes;
    s_stats.record_bytes = s_stats_frame.record_bytes;
    s_stats.segments = s_stats_frame.segments;
    s_stats.calls = s_stats_frame.calls;
    s_stats.vsh_draws = s_stats_frame.vsh_draws;
    s_stats.vsh_calls = s_stats_frame.vsh_calls;
    s_stats.vsh_indices = s_stats_frame.vsh_indices;
    s_stats.vsh_consts = s_stats_frame.vsh_consts;
    s_stats.stream_kb = s_stream_bytes >> 10;
    s_stats.stream_pending = s_stream_npending;
    memset(&s_stats_frame, 0, sizeof(s_stats_frame));
    s_frame_counter++;
    stream_reap(0);
    if (s_onehop) {
        oh_log();
    }
}

void plat_gpu_sync(void) {
    GMARK("sync");
    s_stats.syncs++;
    end_segment();
    if (s_gpu_fence) {
        /* the callers read GPU results (GXPeekARGB) or free memory the GPU read (streams): wait until the GPU,
         * not only the render thread, is done. An empty segment's C3D_FrameBegin is that fence. */
        u32 seq = s_submit_seq;
        end_segment();
        gpu_fence_wait(seq);
    }
    wait_idle();
}

void plat_gpu_stats(PlatGpuStats* out) { *out = s_stats; }

/* ---- Textures (any thread; citro3d's TexInit only allocates) ---- */

static u32 tex_alloc_slot(void) {
    LightLock_Lock(&s_tex_lock);
    /* Release every texture whose last use has left the GPU first, so its
     * memory (VRAM for copy targets) is back before the new one is made. */
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        /* a slot with a render target is released by the render thread
         * (reap_targets): citro3d deletes targets only outside a frame */
        if (t->pending_free && t->target == NULL && s_frame_counter >= t->free_after_frame) {
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof(*t));
        }
    }
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        if (!t->live && !t->pending_free) {
            t->live = 1;
            LightLock_Unlock(&s_tex_lock);
            return i;
        }
    }
    LightLock_Unlock(&s_tex_lock);
    return 0;
}

/* Render thread, between frames: frees the copy targets whose textures were
 * released (C3D_RenderTargetDelete panics inside a frame). */
static int s_copy_full = 1; /* switch copyfull: a copy fills its whole power-of-two texture (e31fb632) */
static int s_gx_entries; /* GX queue entries the strip copies of this record use (the queue holds 32) */

static void reap_targets(void) {
    s_gx_entries = 0;
    RPHASE(4);
    LightLock_Lock(&s_tex_lock);
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        if (t->pending_free && t->target != NULL && s_frame_counter >= t->free_after_frame) {
            PLOG_I("gpu: reap copy target %lu at frame %lu", (unsigned long)i, (unsigned long)s_frame_counter);
            RPHASE(5);
            C3D_RenderTargetDelete(t->target);
            RPHASE(4);
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof(*t));
        }
    }
    LightLock_Unlock(&s_tex_lock);
}

uint32_t plat_gpu_tex_create(uint32_t width, uint32_t height, uint32_t fmt, const void* pixels, uint32_t bytes) {
    GMARK("tex_create");
    if (width < 8 || height < 8 || width > 1024 || height > 1024) {
        return 0;
    }
    u32 h = tex_alloc_slot();
    if (h == 0) {
        return 0;
    }
    TexSlot* t = &s_tex[h];
    if (!C3D_TexInit(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt)) {
        t->live = 0;
        return 0;
    }
    memcpy(t->tex.data, pixels, bytes < t->tex.size ? bytes : t->tex.size);
    C3D_TexFlush(&t->tex);
    return h;
}

/* plat_gpu_tex_create in two steps, for a caller that writes level 0 itself: the texture's memory (*size bytes;
 * its contents undefined, as after plat_gpu_tex_create past `bytes`), then plat_gpu_tex_create_end once written. */
uint32_t plat_gpu_tex_create_begin(uint32_t width, uint32_t height, uint32_t fmt, void** data, uint32_t* size) {
    GMARK("tex_create");
    if (width < 8 || height < 8 || width > 1024 || height > 1024) {
        return 0;
    }
    u32 h = tex_alloc_slot();
    if (h == 0) {
        return 0;
    }
    TexSlot* t = &s_tex[h];
    if (!C3D_TexInit(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt)) {
        t->live = 0;
        return 0;
    }
    *data = t->tex.data;
    *size = t->tex.size;
    return h;
}

void plat_gpu_tex_create_end(uint32_t handle) { C3D_TexFlush(&s_tex[handle].tex); }

uint32_t plat_gpu_tex_max_levels(uint32_t width, uint32_t height) {
    if (width < 8 || height < 8 || width > 1024 || height > 1024) {
        return 0;
    }
    return (uint32_t)C3D_TexCalcMaxLevel(width, height);
}

uint32_t plat_gpu_tex_create_mip(uint32_t width, uint32_t height, uint32_t fmt, const void* pixels, uint32_t bytes,
                                 const void* mip_pixels, uint32_t mip_bytes, uint32_t levels) {
    if (levels == 0 || levels > plat_gpu_tex_max_levels(width, height)) {
        return plat_gpu_tex_create(width, height, fmt, pixels, bytes);
    }
    u32 h = tex_alloc_slot();
    if (h == 0) {
        return 0;
    }
    TexSlot* t = &s_tex[h];
    if (!C3D_TexInitWithParams(&t->tex, NULL,
                               (C3D_TexInitParams){(u16)width, (u16)height, (u8)levels, (GPU_TEXCOLOR)fmt, GPU_TEX_2D, false})) {
        t->live = 0;
        return 0;
    }
    /* level 0 first, then the levels in order: citro3d's layout (C3D_TexGetImagePtr) */
    u32 total = C3D_TexCalcTotalSize(t->tex.size, levels);
    memcpy(t->tex.data, pixels, bytes < t->tex.size ? bytes : t->tex.size);
    if (mip_bytes > total - t->tex.size) {
        mip_bytes = total - t->tex.size;
    }
    memcpy((u8*)t->tex.data + t->tex.size, mip_pixels, mip_bytes);
    GSPGPU_FlushDataCache(t->tex.data, total);
    return h;
}

/* VRAM budget helpers (copy targets): the largest block vramAlloc can still give. */
static u32 vram_largest_kb(void) {
    for (u32 kb = 3072; kb >= 64; kb -= 64) {
        void* p = vramAlloc(kb * 1024);
        if (p != NULL) {
            vramFree(p);
            return kb;
        }
    }
    return 0;
}

uint32_t plat_gpu_vram_largest_kb(void) { return vram_largest_kb(); }

static void* s_scratch_mem; /* VRAM for the strip copies (scratch_get); NULL: none */
#define SCRATCH_BYTES (256 * 1024)

uint32_t plat_gpu_tex_create_target(uint32_t width, uint32_t height, uint32_t fmt) {
    GMARK("tex_create_target");
    u32 h = tex_alloc_slot();
    if (h == 0) {
        return 0;
    }
    TexSlot* t = &s_tex[h];
    /* VRAM when it has room (the EFB and its depth buffer take 4 of its
     * 6 MB). citro3d renders only to VRAM textures, so when it has no room
     * the texture is in linear memory and copy_to_texture fills it through
     * the VRAM scratch target in strips. vramfail=1 forces that path. */
    static int s_vramfail = -1;
    if (s_vramfail < 0) {
        s_vramfail = plat_switch_int("vramfail", 0);
    }
    if (s_vramfail != 1 && C3D_TexInitVRAM(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt)) {
        t->is_target = 1;
        return h;
    }
    PLOG_W("gpu: no VRAM for a %lux%lu copy target, format %lu (%lu KB): free %lu KB, largest block %lu KB%s",
           (unsigned long)width, (unsigned long)height, (unsigned long)fmt,
           (unsigned long)(width * height * (fmt == GPU_RGBA8 ? 4 : 2) / 1024),
           (unsigned long)(vramSpaceFree() / 1024), (unsigned long)vram_largest_kb(),
           s_scratch_mem ? "; filled through the scratch target" : "; no scratch target");
    if (!C3D_TexInit(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt)) {
        t->live = 0;
        return 0;
    }
    /* the GPU writes this memory by transfer only: start it defined (clear) and out of the CPU cache */
    memset(t->tex.data, 0, t->tex.size);
    GSPGPU_FlushDataCache(t->tex.data, t->tex.size);
    t->is_target = 1;
    t->via_scratch = s_scratch_mem != NULL;
    return h;
}

void plat_gpu_tex_free(uint32_t handle) {
    GMARK("tex_free");
    if (handle == 0 || handle >= MAX_TEX) {
        return;
    }
    LightLock_Lock(&s_tex_lock);
    TexSlot* t = &s_tex[handle];
    t->live = 0;
    t->pending_free = 1;
    t->free_after_frame = s_frame_counter + 3; /* frames in flight */
    LightLock_Unlock(&s_tex_lock);
}

void plat_gpu_tex_reclaim(void) {
    GMARK("tex_reclaim");
    plat_gpu_sync();
    LightLock_Lock(&s_tex_lock);
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        /* copy targets are released by the render thread (reap_targets) */
        if (t->pending_free && t->target == NULL) {
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof(*t));
        }
    }
    LightLock_Unlock(&s_tex_lock);
}

uint32_t plat_gpu_tex_read(uint32_t handle, void* rgba8, uint32_t width, uint32_t height) {
    plat_gpu_sync();
    if (handle == 0 || handle >= MAX_TEX || !s_tex[handle].live) {
        return 0;
    }
    C3D_Tex* tex = &s_tex[handle].tex;
    u32 tw = tex->width, th = tex->height;
    if (width > tw) width = tw;
    if (height > th) height = th;
    if (tex->fmt != GPU_RGBA8) {
        PLOG_W("gpu: read of a texture in format %d (only RGBA8 copy targets are read)", (int)tex->fmt);
        return 0;
    }
    /* The GPU wrote this texture: a display transfer copies it to linear
     * memory and untiles it, and the CPU reads that after invalidating its
     * cache (a direct read could see stale cache lines on the console, and
     * VRAM is not kept current in Azahar). */
    u32* src = (u32*)linearAlloc(tw * th * 4);
    if (src == NULL) {
        PLOG_W("gpu: no linear memory to read a %lux%lu texture", (unsigned long)tw, (unsigned long)th);
        return 0;
    }
    GSPGPU_InvalidateDataCache(src, tw * th * 4);
    C3D_SyncDisplayTransfer((u32*)tex->data, GX_BUFFER_DIM(tw, th), src, GX_BUFFER_DIM(tw, th),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(src, tw * th * 4);
    u8* out = rgba8;
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 my = th - 1 - y;
            u32 v = src[my * tw + x];
            u8* p = out + (y * width + x) * 4;
            p[0] = (u8)(v >> 24);
            p[1] = (u8)(v >> 16);
            p[2] = (u8)(v >> 8);
            p[3] = (u8)v;
        }
    }
    linearFree(src);
    return width * height * 4;
}

/* ---- Replay (render thread) ---- */

static int s_scissor_empty;   /* the last apply_state set an empty scissor: draws are skipped */
static int s_scissor_gx[4];   /* that scissor, x0 y0 x1 y1 */
static int scissor_rect(const PlatGpuState* st, int out[4]);
static void scissor_skip_log(void) {
    static u32 s_logged;
    if (s_logged++ == 0) {
        PLOG_W("gpu: empty scissor (x0 %d y0 %d x1 %d y1 %d): draws skipped", s_scissor_gx[0], s_scissor_gx[1],
               s_scissor_gx[2], s_scissor_gx[3]);
    }
}

/* Switch gxdirect (stage 3, PORT_3DS_GX_PERF.md): 0 off; 1 the replay writes the shader path's PICA words itself
 * (gxdirect section below); 2 as 1, and every word is also built through citro3d and compared. With it on, the
 * setters below make words that depend only on the state (`det`): a register the state leaves unused (the fog colour
 * with fog off, the blend factors under a logic op, the logic op under blending) gets a fixed value instead of the
 * value set last. */
static int s_direct;

/* apply_state in three classes. The order of citro3d's setters does not change what C3Di_UpdateContext emits: it
 * writes the dirty groups in its own fixed order. */
static void apply_tev(const PlatGpuState* st, int det) {
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv* env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i < st->num_stages) {
            const PlatGpuTevStage* s = &st->stages[i];
            env->srcRgb = s->src_rgb;
            env->srcAlpha = s->src_alpha;
            env->opRgb = s->op_rgb;
            env->opAlpha = s->op_alpha;
            env->funcRgb = s->func_rgb;
            env->funcAlpha = s->func_alpha;
            env->scaleRgb = s->scale_rgb;
            env->scaleAlpha = s->scale_alpha;
            env->color = s->constant;
        }
    }
    C3D_TexEnvBufUpdate(C3D_RGB, st->buffer_input_rgb & 0xF);
    C3D_TexEnvBufUpdate(C3D_Alpha, st->buffer_input_alpha & 0xF);
    C3D_TexEnvBufColor(st->buffer_color);
    /* the fog mode and colour are in the texture-environment buffer group (GPUREG_TEXENV_UPDATE_BUFFER) */
    C3D_FogGasMode(st->fog_enable ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (st->fog_enable || det) {
        C3D_FogColor(st->fog_enable ? st->fog_color : 0);
    }
}

/* The texture objects' sampler fields (inline setters, no citro3d state) and the object each unit binds. */
static void tex_prepare(const PlatGpuState* st, C3D_Tex* t_out[3]) {
    for (int u = 0; u < 3; u++) {
        const PlatGpuTexUnit* tu = &st->tex[u];
        if (tu->handle == 0 || tu->handle >= MAX_TEX || !s_tex[tu->handle].tex.data) {
            t_out[u] = &s_blank_tex;
            continue;
        }
        C3D_Tex* t = &s_tex[tu->handle].tex;
        C3D_TexSetFilter(t, tu->mag_linear ? GPU_LINEAR : GPU_NEAREST, tu->min_linear ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetFilterMipmap(t, tu->mip_linear && t->maxLevel != 0 ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetLodBias(t, tu->lod_bias);
        C3D_TexSetWrap(t, (GPU_TEXTURE_WRAP_PARAM)tu->wrap_s, (GPU_TEXTURE_WRAP_PARAM)tu->wrap_t);
        t->param = (t->param & ~GPU_TEXTURE_MODE(7)) |
                   GPU_TEXTURE_MODE(u == 0 && tu->proj ? GPU_TEX_PROJECTION : GPU_TEX_2D);
        t_out[u] = t;
    }
}

static void tex_bind3(C3D_Tex* const t[3]) {
    for (int u = 0; u < 3; u++) {
        C3D_TexBind(u, t[u]);
    }
}

static void apply_pix(const PlatGpuState* st, int det) {
    C3D_AlphaTest(st->alpha_test_enable, (GPU_TESTFUNC)st->alpha_test_func, st->alpha_test_ref);
    if (det) {
        /* both the blend and the logic-op fields get a value; the second call below picks the mode */
        if (st->blend_mode == 2) {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        } else {
            C3D_ColorLogicOp(GPU_LOGICOP_COPY);
        }
    }
    if (st->blend_mode == 1) {
        C3D_AlphaBlend((GPU_BLENDEQUATION)st->blend_eq_rgb, (GPU_BLENDEQUATION)st->blend_eq_alpha,
                       (GPU_BLENDFACTOR)st->blend_src_rgb, (GPU_BLENDFACTOR)st->blend_dst_rgb,
                       (GPU_BLENDFACTOR)st->blend_src_alpha, (GPU_BLENDFACTOR)st->blend_dst_alpha);
    } else if (st->blend_mode == 2) {
        C3D_ColorLogicOp((GPU_LOGICOP)st->logic_op);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    u32 mask = (st->color_write & 0xF) | (st->depth_write ? GPU_WRITE_DEPTH : 0);
    C3D_DepthTest(st->depth_test || st->depth_write, (GPU_TESTFUNC)(st->depth_test ? st->depth_func : GPU_ALWAYS),
                  (GPU_WRITEMASK)mask);
    C3D_CullFace(GPU_CULL_NONE);
    /* window depth = far + (z/w) * (far - near), z/w in [-1, 0] as on GX */
    float zrange = st->depth_far - st->depth_near;
    C3D_DepthMap(true, zrange, st->depth_far); /* PICA: depth = scale * z/w + offset */
    float tx, ty, tw, th;
    efb_to_target_rect(st->viewport_x, st->viewport_y, st->viewport_w, st->viewport_h, &tx, &ty, &tw, &th);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tw >= 1 && th >= 1) {
        C3D_SetViewport((u32)tx, (u32)ty, (u32)tw, (u32)th);
    }
    int sc[4];
    s_scissor_empty = scissor_rect(st, sc);
    if (!s_scissor_empty) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)sc[0], (u32)sc[1], (u32)sc[2], (u32)sc[3]);
    } else if (det) {
        /* draws are skipped; the scissor words still get fixed values (C3D_SetViewport above left the mode off and
         * the corners as set last). Mode off, corners 0. */
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 1, 1);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    }
}

/* The state's scissor in target pixels (x0 y0 x1 y1, end exclusive), clamped to the target region; 1 when empty. */
static int scissor_rect(const PlatGpuState* st, int out[4]) {
    float sx, sy, sw, sh;
    efb_to_target_rect(st->scissor_x0, st->scissor_y0, (float)(st->scissor_x1 - st->scissor_x0),
                       (float)(st->scissor_y1 - st->scissor_y0), &sx, &sy, &sw, &sh);
    /* clamp to the target region; start floors, end ceils */
    float cx0 = sx < 0 ? 0 : sx > REGION_W ? REGION_W : sx;
    float cy0 = sy < 0 ? 0 : sy > REGION_H ? REGION_H : sy;
    float cx1 = sx + sw < 0 ? 0 : sx + sw > REGION_W ? REGION_W : sx + sw;
    float cy1 = sy + sh < 0 ? 0 : sy + sh > REGION_H ? REGION_H : sy + sh;
    int x0 = (int)cx0, y0 = (int)cy0, x1 = (int)cx1, y1 = (int)cy1;
    x1 += (float)x1 < cx1;
    y1 += (float)y1 < cy1;
    s_scissor_gx[0] = x0;
    s_scissor_gx[1] = y0;
    s_scissor_gx[2] = x1;
    s_scissor_gx[3] = y1;
    out[0] = x0;
    out[1] = y0;
    out[2] = x1;
    out[3] = y1;
    return x1 <= x0 || y1 <= y0;
}

static void apply_state(const PlatGpuState* st) {
    int det = s_direct != 0;
    C3D_Tex* t[3];
    apply_tev(st, det);
    tex_prepare(st, t);
    tex_bind3(t);
    apply_pix(st, det);
}

static void draw_rect_quad(Record* r, u16 x, u16 y, u16 w, u16 h, u32 rgba, u32 depth24, int color, int depth) {
    /* A clear: a screen quad at the clear depth with every test off. */
    (void)r;
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    env->color = rgba;
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)((color ? GPU_WRITE_COLOR : 0) | (depth ? GPU_WRITE_DEPTH : 0)));
    C3D_DepthMap(true, -1.0f, 0.0f);
    float tx, ty, tw, th;
    efb_to_target_rect(x, y, w, h, &tx, &ty, &tw, &th);
    C3D_SetViewport((u32)tx, (u32)ty, (u32)tw, (u32)th);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    float z = -(float)(depth24 & 0xFFFFFF) / 16777215.0f; /* depth = -z */
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    const float c[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
    for (int i = 0; i < 4; i++) {
        C3D_ImmSendAttrib(c[i][0], c[i][1], z, 1.0f);
        C3D_ImmSendAttrib(255, 255, 255, 255);
        C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
    }
    C3D_ImmDrawEnd();
}

/* EFB rectangle -> copy texture, through a textured quad that samples the
 * EFB render target (its color buffer is a texture in VRAM). */
static C3D_Tex s_efb_tex;

/* One pass of a copy: the quad into `target`, whose viewport is vp_w x vp_h
 * texels and holds rows r0.. of the copy texture (0 and the whole texture for
 * a copy target in VRAM; a strip for the scratch target). */
static void copy_pass(C3D_RenderTarget* target, u32 vp_w, u32 vp_h, u32 r0, u16 x, u16 y, u16 w, u16 h, u32 dst_w,
                      u32 dst_h) {
    C3D_FrameDrawOn(target);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_TexSetFilter(&s_efb_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexBind(0, &s_efb_tex);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_SetViewport(0, 0, vp_w, vp_h);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* The copy texture holds the rectangle upright in GX texture space:
     * texel (s, t) of the dw x dh image is EFB pixel (x + s * w, y + t * h). */
    float fx = (float)dst_w / vp_w;
    float tx, ty, tw, th;
    efb_to_target_rect(x, y, w, h, &tx, &ty, &tw, &th);
    float u0 = tx / s_efb_tex.width, u1 = (tx + tw) / s_efb_tex.width;
    float v0 = (ty + VIEW_V0) / s_efb_tex.height, v1 = (ty + th + VIEW_V0) / s_efb_tex.height;
    /* corners: GX (s, t) = (0,0) (1,0) (0,1) (1,1) -> EFB corners -> target uv */
    float cx[4] = {0, 1, 0, 1}, cy[4] = {0, 0, 1, 1};
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot0, 1, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot1, 0, 1, 0, 0);
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        float ex = cx[i], ey = cy[i]; /* in the copied rect, 0..1 */
        float ttx, tty, dummy0, dummy1;
        efb_to_target_rect(x + ex * w, y + ey * h, 0, 0, &ttx, &tty, &dummy0, &dummy1);
        float u = ttx / s_efb_tex.width, v = (tty + VIEW_V0) / s_efb_tex.height; /* viewport pixel -> texel (see present) */
        (void)u0;
        (void)u1;
        (void)v0;
        (void)v1;
        /* destination: GX t = 0 is the texture's last memory row, which is
         * viewport y = 0 (clip y = -1); the image fills the first dst_h
         * viewport rows */
        float px = -1.0f + 2.0f * ex * fx;
        float py = -1.0f + 2.0f * (ey * (float)dst_h - (float)r0) / (float)vp_h;
        C3D_ImmSendAttrib(px, py, -0.5f, 1.0f);
        C3D_ImmSendAttrib(255, 255, 255, 255);
        C3D_ImmSendAttrib(u, v, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
    }
    C3D_ImmDrawEnd();
    rot_uniforms();
    C3D_FrameDrawOn(s_efb);
}

/* The VRAM scratch target of the strip copies: one block of VRAM seen as a
 * render target per (width, format), as many rows as it holds. */
typedef struct {
    u16 width, rows;
    u8 fmt;
    C3D_Tex tex;
    C3D_RenderTarget* target;
} Scratch;
static Scratch s_scratch[32];
static u32 s_scratch_n;

static Scratch* scratch_get(u32 width, u32 fmt) {
    for (u32 i = 0; i < s_scratch_n; i++) {
        if (s_scratch[i].width == width && s_scratch[i].fmt == fmt) {
            return &s_scratch[i];
        }
    }
    u32 bpp = fmt == GPU_RGBA8 ? 4 : 2;
    u32 rows = (SCRATCH_BYTES / (width * bpp)) & ~7u;
    if (rows > 512) {
        rows = 512;
    }
    if (s_scratch_mem == NULL || s_scratch_n >= sizeof(s_scratch) / sizeof(s_scratch[0]) || rows < 8 ||
        (fmt != GPU_RGBA8 && fmt != GPU_RGB565 && fmt != GPU_RGBA5551 && fmt != GPU_RGBA4)) {
        return NULL;
    }
    Scratch* sc = &s_scratch[s_scratch_n];
    memset(sc, 0, sizeof(*sc));
    sc->tex.data = s_scratch_mem;
    sc->tex.fmt = (GPU_TEXCOLOR)fmt;
    sc->tex.width = (u16)width;
    sc->tex.height = (u16)rows;
    sc->tex.size = width * rows * bpp;
    sc->target = C3D_RenderTargetCreateFromTex(&sc->tex, GPU_TEXFACE_2D, 0, -1);
    if (sc->target == NULL) {
        return NULL;
    }
    sc->width = (u16)width;
    sc->rows = (u16)rows;
    sc->fmt = (u8)fmt;
    s_scratch_n++;
    return sc;
}

static void copy_to_texture(u16 x, u16 y, u16 w, u16 h, u32 handle, u32 dst_w, u32 dst_h) {
    if (handle == 0 || handle >= MAX_TEX) {
        return;
    }
    TexSlot* slot = &s_tex[handle];
    /* The copy fills the whole power-of-two texture, stretched as gx_tex.c resamples a loaded texture: a
     * draw's texture coordinates are fractions of the GX size (608 x 480 is a 1024 x 512 texture here),
     * and an image kept at 1:1 in the corner came out magnified, black beyond its edge (the pause menu). */
    if (s_copy_full) {
        dst_w = slot->tex.width;
        dst_h = slot->tex.height;
    }
    if (slot->via_scratch) {
        /* Strips: the copy is rendered R rows at a time into the scratch target, and each strip is copied
         * (a raw memory copy, both are in the PICA's tiled layout) to its place in the linear texture. Memory
         * tile row 0 is the top of the image (viewport y = H - 1), so the strip of viewport rows r0.. goes to
         * tile rows counted from the end. */
        static int s_warned;
        Scratch* sc = scratch_get(slot->tex.width, slot->tex.fmt);
        u32 th = slot->tex.height, lmax = (dst_h + 7) & ~7u;
        if (lmax > th) {
            lmax = th;
        }
        u32 nstrips = sc ? (lmax + sc->rows - 1) / sc->rows : 0;

        if (sc == NULL) {
            if (!s_warned) {
                s_warned = 1;
                PLOG_E("gpu: copy target %lu (%lux%lu) cannot be filled: no scratch target", (unsigned long)handle,
                       (unsigned long)slot->tex.width, (unsigned long)th);
            }
            return;
        }
        u32 bpp = slot->tex.fmt == GPU_RGBA8 ? 4 : 2;
        u32 rowbytes = slot->tex.width * 8 * bpp; /* one tile row */
        for (u32 r0 = 0; r0 < lmax; r0 += sc->rows) {
            u32 n = lmax - r0 < sc->rows ? lmax - r0 : sc->rows;
            if (s_gx_entries + 2 > 24) {
                /* The GX queue (32 entries) only runs at C3D_FrameEnd: finish the frame so far, and open a new one
                 * (C3D_FrameBegin waits until the queue is empty). The strips so far are in memory by then. */
                PLOG_I("gpu: copy target %lu (%lux%lu) needs %lu strips: GX queue full at strip row %lu, new frame",
                       (unsigned long)handle, (unsigned long)slot->tex.width, (unsigned long)th, (unsigned long)nstrips,
                       (unsigned long)r0);
                RPHASE(7);
                gpudump_note();
                C3D_FrameEnd(0);
                RPHASE(1);
                C3D_FrameBegin(0);
                RPHASE(6);
                C3D_BindProgram(&s_prog);
                C3D_SetAttrInfo(&s_attr);
                s_gx_entries = 0;
            }
            copy_pass(sc->target, slot->tex.width, sc->rows, r0, x, y, w, h, dst_w, dst_h);
            C3D_FrameSplit(GX_CMDLIST_FLUSH);
            u8* src = (u8*)sc->tex.data + ((sc->rows - n) / 8) * rowbytes;
            u8* dst = (u8*)slot->tex.data + ((th - r0 - n) / 8) * rowbytes;
            u32 bytes = (n / 8) * rowbytes;
            GX_TextureCopy((u32*)src, GX_BUFFER_DIM(bytes, 0), (u32*)dst, GX_BUFFER_DIM(bytes, 0), bytes, 8);
            s_gx_entries += 2;
        }
        C3D_FrameDrawOn(s_efb);
        return;
    }
    if (slot->target == NULL) {
        slot->target = C3D_RenderTargetCreateFromTex(&slot->tex, GPU_TEXFACE_2D, 0, -1);
        if (slot->target == NULL) {
            PLOG_E("gpu: copy target %lu cannot be rendered to", (unsigned long)handle);
            return;
        }
    }
    copy_pass(slot->target, slot->tex.width, slot->tex.height, 0, x, y, w, h, dst_w, dst_h);
}

static void take_shot(void);
static u32 s_shot_due;
static void capture_efb(const Record* r, const char* path);
static char s_named_path[48];
static int s_named_shot; /* plat_gpu_shot: the next presented frame goes to s_named_path */

/* The top screen's render target, linked to the framebuffer: citro3d
 * transfers it and swaps at C3D_FrameEnd. */
static C3D_RenderTarget* s_screen;
#define SCREEN_TRANSFER_FLAGS                                                                                        \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                 \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                    \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static void efb_point(float x, float y, float* tx, float* ty) {
    float tw, th;
    efb_to_target_rect(x, y, 0, 0, tx, ty, &tw, &th);
}

/* Present: the shown part of the EFB target (480x800, rotated as the
 * screen) drawn into the 240x400 screen target as one textured quad. The
 * bilinear sample at each screen pixel's centre falls on the corner shared
 * by its four EFB pixels, so the halving averages them. (A display
 * transfer of the EFB into a linear buffer followed by a TextureCopy into
 * the framebuffer came out with only its first quarter in Azahar.) */
static void present(C3D_RenderTarget* screen) {
    C3D_FrameDrawOn(screen);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_TexSetFilter(&s_efb_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexBind(0, &s_efb_tex);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_SetViewport(0, 0, 240, 400);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot0, 1, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot1, 0, 1, 0, 0);
    /* Viewport y counts up from the last memory row (as GL's), and texel
     * v = 1 - row / height with row the memory row: viewport pixel (x, y)
     * of a target is texel (x / width, y / height). The screen target's
     * viewport y runs right to left on the display, the EFB's left to
     * right (efb_to_target_rect), so screen viewport pixel (sx, sy) shows
     * EFB viewport pixel (2 sx, REGION_H - 2 sy). Checked with gputest. */
    const float u1 = (float)REGION_W / TARGET_W, v1 = (float)(REGION_H + VIEW_V0) / VIEW_H, v0 = (float)VIEW_V0 / VIEW_H;
    /* with efb608 the picture covers the middle REGION_H / 2 of the 400 screen columns; the screen targets are cleared to
     * black once at their creation and the quad never draws the bars */
    const float k = (float)REGION_H / 800.0f;
    const float c[4][4] = {{-1, -k, 0, v1}, {1, -k, u1, v1}, {-1, k, 0, v0}, {1, k, u1, v0}};
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        C3D_ImmSendAttrib(c[i][0], c[i][1], -0.5f, 1.0f);
        C3D_ImmSendAttrib(255, 255, 255, 255);
        C3D_ImmSendAttrib(c[i][2], c[i][3], 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(0, 0, 0, 1);
    }
    C3D_ImmDrawEnd();
    rot_uniforms();
}

/* ---- Shader path replay (render thread) ---- */

/* The screen rotation's x and y rows (as rot_uniforms). */
static void rot_rows(float r0[4], float r1[4]) {
    static const float rows[4][2][4] = {
        {{0, 1, 0, 0}, {1, 0, 0, 0}},
        {{0, -1, 0, 0}, {1, 0, 0, 0}},
        {{0, 1, 0, 0}, {-1, 0, 0, 0}},
        {{0, -1, 0, 0}, {-1, 0, 0, 0}},
    };
    memcpy(r0, rows[s_rot][0], 16);
    memcpy(r1, rows[s_rot][1], 16);
}

/* The shader-path uniforms as last set in this record's replay (bit patterns): a value equal to the one in
 * force is not set again, so citro3d uploads only the rows that changed. Only vsh_apply_* write these rows;
 * the cache is cleared at the start of every record. */
typedef struct {
    u32 unif[96][4]; /* as last set (bit patterns, x y z w) */
    u32 val[96][4];  /* gxdirect, gxonehop: the values in citro3d's order (w, z, y, x: C3D_FVec) */
    u32 pend[3];     /* gxdirect, gxonehop: the rows not yet written to the GPU */
    u32 valid[3];    /* gxonehop: the rows set since the record's start (a stereo mark writes them again) */
    int direct;      /* rows go to pend (else C3D_FVUnifSet) */
} VUnif;
static VUnif s_vr; /* the render thread's */
static VUnif s_vg; /* gxonehop: the recording thread's */
static int s_urow = 1; /* switch gxurow (default 1); 0 sets every uniform row every time (hardware bisect) */
static void vunif_reset_u(VUnif* u) {
    memset(u->unif, 0xFF, sizeof(u->unif));
    u->valid[0] = u->valid[1] = u->valid[2] = 0;
}
static void vunif_reset(void) { vunif_reset_u(&s_vr); }
#define s_vpend (s_vr.pend)
#define s_vval (s_vr.val)
static int s_c3d_unif = 1; /* gxdirect: citro3d may hold dirty uniforms or fixed attributes its flags do not show */
static inline void vunif_u(VUnif* u, int id, float x, float y, float z, float w) {
    if (id < 0 || id >= 96) {
        return;
    }
    /* the row's four bit patterns, compared and stored as words (no libc memcmp/memcpy call per row) */
    u32 b0, b1, b2, b3;
    memcpy(&b0, &x, 4);
    memcpy(&b1, &y, 4);
    memcpy(&b2, &z, 4);
    memcpy(&b3, &w, 4);
    u32* d = u->unif[id];
    if (s_urow && d[0] == b0 && d[1] == b1 && d[2] == b2 && d[3] == b3) {
        return;
    }
    d[0] = b0;
    d[1] = b1;
    d[2] = b2;
    d[3] = b3;
    if (u->direct) {
        u32* q = u->val[id]; /* {w, z, y, x} */
        q[0] = b3;
        q[1] = b2;
        q[2] = b1;
        q[3] = b0;
        u->pend[id >> 5] |= 1u << (id & 31);
        u->valid[id >> 5] |= 1u << (id & 31);
        return;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, id, x, y, z, w);
}
#define vunif(id, x, y, z, w) vunif_u(u, id, x, y, z, w)

/* Uniforms of a CMD_VSH_CONST. The clip rows come unrotated; the rotation
 * rows hold only 0 and +-1, so composing them on the CPU is exact (a row is
 * picked and maybe negated). A second eye pass would compose its clip-space
 * offset here too, from the same record. */
static void vsh_const_rows(VUnif* u, const PlatGpuVshConst* c);
static void vsh_apply_const(const PlatGpuVshConst* c) {
    vsh_const_rows(&s_vr, c);
    if (s_uloc_lit >= 0) {
        bool lit = (c->flags & PLAT_VSH_LIT) != 0;
        /* gxdirect: only a change dirties citro3d (a dirty uniform costs a C3Di_UpdateContext before the next draw) */
        if (!s_direct || ((C3D_BoolUnifs[GPU_VERTEX_SHADER] >> (s_uloc_lit - 0x68) & 1) != 0) != lit) {
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_lit, lit);
            s_c3d_unif = 1;
        }
    }
}

static void vsh_const_rows(VUnif* u, const PlatGpuVshConst* c) {
    /* each rotation row holds one +-1 and zeros (rot_rows): the row is 0.0f + (+-1 * a clip row), which the
     * general sum below the old loop made, term for term */
    static int s_rr_rot = -1, s_rr_i[2];
    static float s_rr_s[2];
    if (s_rr_rot != s_rot) {
        float r[2][4];
        rot_rows(r[0], r[1]);
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 4; i++) {
                if (r[k][i] != 0.0f) {
                    s_rr_i[k] = i;
                    s_rr_s[k] = r[k][i];
                }
            }
        }
        s_rr_rot = s_rot;
    }
    for (int k = 0; k < 2; k++) {
        const float* src = c->clip[s_rr_i[k]];
        float sg = s_rr_s[k];
        float row[4];
        for (int j = 0; j < 4; j++) {
            row[j] = 0.0f + sg * src[j];
        }
        vunif(s_uloc_clip + k, row[0], row[1], row[2], row[3]);
    }
    vunif(s_uloc_clip + 2, c->clip[2][0], c->clip[2][1], c->clip[2][2], c->clip[2][3]);
    vunif(s_uloc_clip + 3, c->clip[3][0], c->clip[3][1], c->clip[3][2], c->clip[3][3]);
    for (int i = 0; i < 9; i++) {
        vunif(s_uloc_texrow + i, c->tex[i][0], c->tex[i][1], c->tex[i][2], c->tex[i][3]);
    }
    for (int i = 0; i < 3; i++) {
        vunif(s_uloc_tgsrc + i, c->tgsrc[i][0], c->tgsrc[i][1], c->tgsrc[i][2],
                      c->tgsrc[i][3]);
    }
    vunif(s_uloc_clrmix, c->clrmix[0], c->clrmix[1], c->clrmix[2], c->clrmix[3]);
    vunif(s_uloc_matclr, c->matclr[0], c->matclr[1], c->matclr[2], c->matclr[3]);
}

/* The light block's rows; returns the light count's int uniform x (n - 1, 0 with none). */
static int vsh_light_rows(VUnif* u, const PlatGpuVshLight* l);
static void vsh_apply_light(const PlatGpuVshLight* l) {
    if (s_uloc_lit < 0) {
        return;
    }
    int x = vsh_light_rows(&s_vr, l);
    u32 iv = IVec_Pack(x, 0, 1, 0);
    if (!s_direct || C3D_IVUnif[GPU_VERTEX_SHADER][s_uloc_lcount - 0x60] != iv) {
        C3D_IVUnifSet(GPU_VERTEX_SHADER, s_uloc_lcount, x, 0, 1, 0);
        s_c3d_unif = 1;
    }
}

static int vsh_light_rows(VUnif* u, const PlatGpuVshLight* l) {
    for (int i = 0; i < 3; i++) {
        vunif(s_uloc_eyerow + i, l->eye[i][0], l->eye[i][1], l->eye[i][2], l->eye[i][3]);
        vunif(s_uloc_nrmrow + i, l->nrm[i][0], l->nrm[i][1], l->nrm[i][2], l->nrm[i][3]);
    }
    vunif(s_uloc_amb, l->amb[0], l->amb[1], l->amb[2], l->amb[3]);
    vunif(s_uloc_dmode, l->dmode[0], l->dmode[1], l->dmode[2], l->dmode[3]);
    vunif(s_uloc_amode, l->amode[0], l->amode[1], l->amode[2], l->amode[3]);
    u32 n = l->count > PLAT_VSH_LIGHTS ? PLAT_VSH_LIGHTS : l->count;
    for (u32 i = 0; i < (n ? n : 1); i++) {
        vunif(s_uloc_lpos + i, l->light[i].pos[0], l->light[i].pos[1], l->light[i].pos[2], 0);
        vunif(s_uloc_ldir + i, l->light[i].dir[0], l->light[i].dir[1], l->light[i].dir[2], 0);
        vunif(s_uloc_lcol + i, l->light[i].color[0], l->light[i].color[1],
                      l->light[i].color[2], 0);
        vunif(s_uloc_lka + i, l->light[i].a[0], l->light[i].a[1], l->light[i].a[2], 0);
        vunif(s_uloc_lkk + i, l->light[i].k[0], l->light[i].k[1], l->light[i].k[2], 0);
    }
    /* the loop runs count + 1 times; with no light it runs once over light 0, which the front end zeroes */
    return n ? (int)n - 1 : 0;
}

static float pow2_neg(u32 k) {
    float f = 1.0f;
    while (k-- > 0) {
        f *= 0.5f;
    }
    return f;
}

static void vsh_scales_rows(VUnif* u, u32 scales) {
    float s = pow2_neg(scales & 0xFF);
    vunif(s_uloc_vscale, s, s, s, 1.0f);
    for (int k = 0; k < 3; k++) {
        u32 b = scales >> (8 + 8 * k) & 0xFF;
        u32 slot = b & 3;
        float w = pow2_neg(b >> 2);
        vunif(s_uloc_texsel + k, slot == 0 ? w : 0.0f, slot == 1 ? w : 0.0f,
                      slot == 2 ? w : 0.0f, 0.0f);
    }
}
static void vsh_apply_scales(u32 scales) { vsh_scales_rows(&s_vr, scales); }
#undef vunif

/* Vertex loaders for a stream layout: position v0, colour v1, slots v2-v4;
 * registers the stream lacks are fixed attributes (0). */
static void vsh_apply_layout(const void* stream, u32 layout, C3D_AttrInfo* ai, C3D_BufInfo* vbi) {
    int32_t off[6];
    u32 nslots = PLAT_VSH_NSLOTS(layout);
    u32 stride = plat_vsh_layout_n(layout & PLAT_VSH_POS_S16, nslots, layout >> 3 & 7, layout & PLAT_VSH_NRM, off);
    /* buffer order: the present attributes sorted by offset */
    int order[6], n = 0;
    for (int a = 0; a < 6; a++) {
        if (off[a] >= 0) {
            int k = n++;
            while (k > 0 && off[order[k - 1]] > off[a]) {
                order[k] = order[k - 1];
                k--;
            }
            order[k] = a;
        }
    }
    AttrInfo_Init(ai);
    for (int i = 0; i < n; i++) {
        int a = order[i];
        if (a == 0) {
            AttrInfo_AddLoader(ai, 0, (layout & PLAT_VSH_POS_S16) ? GPU_SHORT : GPU_FLOAT, 3);
        } else if (a == 1) {
            AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
        } else if (a == 5) {
            AttrInfo_AddLoader(ai, 5, GPU_FLOAT, 3);
        } else {
            AttrInfo_AddLoader(ai, a, (layout & PLAT_VSH_SLOT_S16(a - 2)) ? GPU_SHORT : GPU_FLOAT, 2);
        }
    }
    int fixed[4], fixed_reg[4], nfixed = 0;
    for (int a = 2; a < 6; a++) {
        if (off[a] < 0) {
            fixed_reg[nfixed] = a;
            fixed[nfixed++] = AttrInfo_AddFixed(ai, a);
        }
    }
    BufInfo_Init(vbi);
    BufInfo_Add(vbi, stream, (ptrdiff_t)stride, n, 0x543210ull & ((1ull << (4 * n)) - 1));
    C3D_SetAttrInfo(ai);
    C3D_SetBufInfo(vbi);
    for (int i = 0; i < nfixed; i++) {
        if (fixed[i] >= 0) {
            /* no normal: (0, 0, 1), as the CPU path lights a vertex without one */
            C3D_FixedAttribSet(fixed[i], 0, 0, fixed_reg[i] == 5 ? 1.0f : 0.0f, 0);
        }
    }
    s_c3d_unif = 1; /* fixed attributes */
}

/* GX cull mode -> PICA. GX's front face is counter-clockwise in its clip
 * space (clockwise on the y-down screen); the screen rotation keeps the
 * winding when its determinant is positive (rot 1, 2) and flips it otherwise.
 * The PICA's viewport y runs the other way from the clip y the winding is
 * judged in here, which flips it once more: checked in Azahar (gor_01: with
 * one flip fewer, every map face that looks at the camera was culled). */
static GPU_CULLMODE vsh_cull(u32 cull) {
    if (cull == 0) {
        return GPU_CULL_NONE;
    }
    int flips = s_rot == 1 || s_rot == 2;
    int cull_ccw = (cull == 1) != flips; /* the faces to drop are CCW for the PICA */
    return cull_ccw ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;
}

/* Stereo: where the record's 2D tail starts (stereo.h). Everything after the
 * last scene draw or EFB copy is the tail, replayed once per eye on the warped
 * image. TTYD draws its 2D layer (HUD, windows, text) and its fades with
 * perspective cameras, so the projection type does not tell them apart; the
 * viewport's depth range does: camdrv gives the 3D cameras 0-1, the 3D effect
 * cameras 0.2-0.3, the 2D camera 0.1-0.2 and the fade cameras 0-0.1. A draw
 * whose depth range ends at 0.2 or nearer is a screen layer; any other draw is
 * scene, except draws that write neither colour nor depth (camDraw's
 * sysDummyDraw runs for every camera, the debug ones after the 2D layer
 * included). NULL: no scene draw in this record, the frame stays mono. */
#define STEREO_LAYER_FAR 0.2001f
static u8* stereo_split(const Record* r, u32* tail_cmds, u32* tail_zwrites) {
    u8 *p = r->data, *end = r->data + r->used, *split = NULL;
    int scene = 0;
    const PlatGpuState* st = NULL;
    static PlatGpuState ss; /* the state the record's CMD_STATE and deltas give */
    *tail_cmds = *tail_zwrites = 0;
    /* switch stereolog=N: the record's draw order at stereo frames N and N + 300 (S/s scene draw on the CPU/shader
     * path, L/l screen layer draw, C copy, X clear; runs counted) */
    static int s_log = -2;
    static u32 s_nrec;
    if (s_log == -2) {
        s_log = plat_switch_int("stereolog", -1);
    }
    char seq[480];
    u32 nseq = 0, run = 0;
    char last = 0;
    s_nrec++;
    int logit = s_log >= 0 && (s_nrec == (u32)s_log || s_nrec == (u32)s_log + 300);
#define SEQ(c)                                                                                                  \
    do {                                                                                                        \
        if (logit) {                                                                                            \
            if ((c) == last) {                                                                                  \
                run++;                                                                                          \
            } else {                                                                                            \
                if (last && nseq < sizeof(seq) - 12) {                                                          \
                    nseq += (u32)snprintf(seq + nseq, sizeof(seq) - nseq, "%c%lu ", last, (unsigned long)run); \
                }                                                                                               \
                last = (c);                                                                                     \
                run = 1;                                                                                        \
            }                                                                                                   \
        }                                                                                                       \
    } while (0)
    seq[0] = 0;
    while (p < end) {
        CmdHead* h = (CmdHead*)p;
        u8* body = p + sizeof(CmdHead);
        p = body + h->size;
        (*tail_cmds)++;
        if (h->type == CMD_CLEAR) {
            SEQ('X');
        }
        switch (h->type) {
        case CMD_STATE:
            ss = *(const PlatGpuState*)body;
            st = &ss;
            break;
        case CMD_STATE_DELTA:
            if (st != NULL) {
                const u32* d = (const u32*)body;
                for (u32 i = 0; i < d[0]; i++) {
                    ((u32*)&ss)[d[1 + 2 * i]] = d[2 + 2 * i];
                }
            }
            break;
        case CMD_COPY: split = p; *tail_cmds = *tail_zwrites = 0; SEQ('C'); break;
        case CMD_DRAW:
        case CMD_VSH_DRAW:
        case CMD_VSH_DRAWP:
            if (st == NULL || (st->color_write == 0 && !st->depth_write)) {
                break; /* skipped by the replay, or writes nothing (sysDummyDraw, once per camera) */
            }
            if (st->depth_far > STEREO_LAYER_FAR) {
                SEQ(h->type == CMD_DRAW ? 'S' : 's');
                split = p;
                scene = 1;
                *tail_cmds = *tail_zwrites = 0;
            } else {
                SEQ(h->type == CMD_DRAW ? 'L' : 'l');
                *tail_zwrites += st->depth_write != 0;
            }
            break;
        default: break;
        }
    }
    if (logit) {
        SEQ(1);
        PLOG_I("stereolog frame %lu: %s(tail from byte %ld of %lu)", (unsigned long)s_nrec, seq,
               split ? (long)(split - r->data) : -1L, (unsigned long)r->used);
    }
#undef SEQ
    return scene ? split : NULL;
}

/* ---- gxdirect (stage 3): the replay writes the shader path's PICA words itself ----
 *
 * Every word is one citro3d or libctru would write, made by their own encoders:
 *  - a draw: C3D_DrawElements' packet for u16 triangles (citro3d 1.7.1 drawElements.c), built once at start-up with
 *    libctru's GPUCMD_* and patched with the index offset and count;
 *  - uniform rows: what C3D_UpdateUniforms writes for a run of dirty rows (GPUREG_VSH_FLOATUNIFORM_CONFIG, then the
 *    run's data through GPUCMD_AddWrites, which splits runs over 256 words as for citro3d);
 *  - the PICA state in three classes (TEV with fog mode and colour; the texture units; the pixel pipeline with
 *    viewport and scissor) and the attribute setup per (stream, layout): the first time a key is seen, citro3d's
 *    setters run and the words its C3Di_UpdateContext emits for them are captured (the command buffer pointed at a
 *    scratch array), kept with the key, and copied into the command buffer; after that the kept words are copied.
 *    The key is everything the setters read (the texture units' key is the C3D_Tex objects after the sampler
 *    setters), so equal keys give equal words (with the `det` setters, apply_tev/apply_pix).
 * citro3d's own copy of the state then lags behind the GPU. Before the replay goes back to citro3d (a CPU-path draw,
 * a clear, a copy, the stereo warp, the present: C3D_PATH in render_record), the state in force goes through its
 * setters again and every class is marked as not held by the GPU. When C3Di_UpdateContext emits a group on its own,
 * the classes are marked the same way. Blocks are copied: no GPUREG_CMDBUF_JUMP.
 * citro3d internals used: __C3D_Context's flags (word 8) and bufInfo.base_paddr (word 16), checked at start-up, and
 * C3Di_UpdateContext (a global symbol of libcitro3d.a). */
extern u32 __C3D_Context[];
void C3Di_UpdateContext(void);
#define C3DX_FLAGS (__C3D_Context[8])
#define C3DX_BUFBASE (__C3D_Context[16])
#define C3DXF_ACTIVE 0x1u
#define C3DXF_DRAWUSED 0x2u
#define C3DXF_ATTRBUF 0xCu
#define C3DXF_FOGLUT 0x20000u
/* effect, viewport, scissor, texture-environment buffer, texture status, units, stages */
#define C3DXF_CLASS (0x10u | 0x40u | 0x80u | 0x200u | 0x4000u | 0x3800000u | 0xFC000000u)

enum { DC_TEV, DC_TEX, DC_PIX, DC_LAY, DC_N };
#define DK_TEV 28 /* key words: stages .. buffer_color (26), fog enable, fog colour */
#define DK_TEX 18 /* 3 C3D_Tex */
#define DK_PIX 12 /* alpha_test_enable .. color_write (4), scissor_x0 .. depth_far (8) */
#define DK_LAY 2  /* stream, layout */
#define DK_MAX 28
_Static_assert(sizeof(C3D_Tex) == 24, "C3D_Tex is 6 words");
_Static_assert(offsetof(PlatGpuState, tex) == 26 * 4, "TEV key region");
_Static_assert(offsetof(PlatGpuState, cull) - offsetof(PlatGpuState, alpha_test_enable) == 16, "pixel key region");
_Static_assert(sizeof(PlatGpuState) - offsetof(PlatGpuState, scissor_x0) == 32, "scissor key region");

/* A table of kept words: 4-way sets, entry = [hash, words (0: empty), stamp, key..., words...] */
#define DT_WAYS 4
typedef struct {
    u32 slots, keyw, capw, stride, stamp;
    u32* mem;
} DTab;
static DTab s_dtab[DC_N];
static const char* const k_dc_name[DC_N] = {"tev", "tex", "pix", "lay"};
static u32 s_dkey[3][DK_MAX]; /* the keys of the class blocks the GPU holds */
static int s_gpu_valid;        /* bit c: s_dkey[c] is what the GPU holds */
static int s_lay_valid;        /* the GPU holds the attribute setup of v_stream / v_layout */
static int s_shadow_stale;     /* kept words went to the GPU since citro3d's setters last saw the state in force */
static int s_state_pending;    /* a CMD_STATE or delta not sent yet (gxdirect sends the state at the next draw) */
static u32 s_cap[512];         /* capture scratch */
static u32 s_cap2[512];        /* gxdirect=2: the second build */
static u32 s_tpl[32], s_tpl_n, s_tpl_idx, s_tpl_cnt; /* the draw packet */
static struct {
    u32 draws, copied[DC_N], captured[DC_N], uncached, flushes, runs, degenerate;
    u32 checks, mismatches;
    u32 maxw[DC_N];
    u32 fl_calls, fl_bytes, fl_ranges; /* GSPGPU_FlushDataCache calls, bytes, and stream ranges among them (render thread) */
} s_dst;
static u32 s_dlog_every, s_dlog_n;

static u32 dkey_hash(const u32* k, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) {
        h = (h ^ k[i]) * 16777619u;
    }
    /* FNV's low bits depend only on the inputs' low bits (aligned pointers, sizes in the upper half): mix the high
     * bits down before the set index takes the low ones */
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

static u32* dtab_find(DTab* t, const u32* key, u32 h) {
    u32 i = h & (t->slots - 1) & ~(DT_WAYS - 1u);
    for (u32 k = 0; k < DT_WAYS; k++) {
        u32* e = t->mem + (size_t)(i + k) * t->stride;
        if (e[1] != 0 && e[0] == h && memcmp(e + 3, key, t->keyw * 4) == 0) {
            e[2] = ++t->stamp;
            return e;
        }
    }
    return NULL;
}

static void dtab_put(DTab* t, u32* e, const u32* key, u32 h, const u32* w, u32 n) {
    if (n == 0 || n > t->capw) {
        s_dst.uncached++;
        return;
    }
    if (e == NULL) { /* an empty way, else the least recently used */
        u32 i = h & (t->slots - 1) & ~(DT_WAYS - 1u);
        e = t->mem + (size_t)i * t->stride;
        for (u32 k = 1; k < DT_WAYS && e[1] != 0; k++) {
            u32* x = t->mem + (size_t)(i + k) * t->stride;
            if (x[1] == 0 || (s32)(x[2] - e[2]) < 0) {
                e = x;
            }
        }
    }
    e[0] = h;
    e[1] = n;
    e[2] = ++t->stamp;
    memcpy(e + 3, key, t->keyw * 4);
    memcpy(e + 3 + t->keyw, w, n * 4);
}

/* Words into citro3d's command buffer where GPUCMD_Add would put them. GPUCMD_Add keeps the offset even, and on a
 * full buffer it stops in svcBreak: so does this. */
/* gxonehop=2: the words the gxdirect replay writes itself in this record (G), compared with the one-hop words */
static u32* s_ohv_g;
static u32 s_ohv_gn, s_ohv_gcap;
static int s_ohv_on; /* 1 collecting, 2 overflowed (no compare), 0 off */
static void ohv_g(const u32* w, u32 n) {
    if (s_ohv_on != 1) {
        return;
    }
    if (s_ohv_gn + n > s_ohv_gcap) {
        s_ohv_on = 2;
        return;
    }
    memcpy(s_ohv_g + s_ohv_gn, w, n * 4);
    s_ohv_gn += n;
}
#define OHV_FROM(off0) ohv_g(gpuCmdBuf + (off0), gpuCmdBufOffset - (off0))

static void cmd_put(const u32* w, u32 n) {
    ohv_g(w, n);
    if ((gpuCmdBufOffset & 1) || gpuCmdBufOffset + n > gpuCmdBufSize) {
        PLOG_E("gxdirect: command buffer offset %lu of %lu words, %lu more: no room", (unsigned long)gpuCmdBufOffset,
               (unsigned long)gpuCmdBufSize, (unsigned long)n);
        svcBreak(USERBREAK_PANIC);
    }
    memcpy(gpuCmdBuf + gpuCmdBufOffset, w, n * 4);
    gpuCmdBufOffset += n;
}

/* citro3d's pending work goes out now, before kept words it would otherwise overwrite later. */
static void c3d_flush(void) {
    u32 f = C3DX_FLAGS;
    if (!s_c3d_unif && (f & ~(C3DXF_ACTIVE | C3DXF_DRAWUSED)) == 0) {
        return;
    }
    if (f & C3DXF_CLASS) {
        s_gpu_valid = 0;
    }
    if (f & C3DXF_ATTRBUF) {
        s_lay_valid = 0;
    }
    C3Di_UpdateContext();
    s_c3d_unif = 0;
    s_dst.flushes++;
}

/* The words C3Di_UpdateContext emits for what the setters since the last flush dirtied. A fog LUT bound while fog
 * is off stays pending in citro3d; it is kept out of the capture (with gxdirect, fog_bind sends LUTs itself). */
static u32 c3d_capture(u32* out, u32 cap) {
    u32 *buf, size, off;
    u32 keep = C3DX_FLAGS & C3DXF_FOGLUT;
    C3DX_FLAGS &= ~C3DXF_FOGLUT;
    GPUCMD_GetBuffer(&buf, &size, &off);
    GPUCMD_SetBuffer(out, cap, 0);
    C3Di_UpdateContext();
    u32 n = gpuCmdBufOffset;
    GPUCMD_SetBuffer(buf, size, off);
    C3DX_FLAGS |= keep;
    return n;
}

/* One block: the kept words when the key is known, else citro3d's setters (`apply`) and a capture.
 * gxdirect=2 captures every time and compares with the kept words. */
static void direct_block(int c, const u32* key, void (*apply)(const void*), const void* arg) {
    DTab* t = &s_dtab[c];
    u32 h = dkey_hash(key, t->keyw);
    u32* e = dtab_find(t, key, h);
    if (e != NULL && s_direct == 1) {
        cmd_put(e + 3 + t->keyw, e[1]);
        s_dst.copied[c]++;
        return;
    }
    RPHASE(8);
    c3d_flush(); /* nothing is pending here (the draw flushed first); kept for safety */
    apply(arg);
    u32 n = c3d_capture(s_cap, sizeof(s_cap) / 4);
    s_c3d_unif = 0; /* the capture took everything citro3d had pending */
    if (n > s_dst.maxw[c]) {
        s_dst.maxw[c] = n;
    }
    if (e != NULL) {
        s_dst.checks++;
        if (e[1] != n || memcmp(e + 3 + t->keyw, s_cap, n * 4) != 0) {
            if (s_dst.mismatches++ < 8) {
                PLOG_W("gxdirect verify: %s block differs from citro3d's words (kept %lu words, now %lu)", k_dc_name[c],
                       (unsigned long)e[1], (unsigned long)n);
            }
        }
    }
    dtab_put(t, e, key, h, s_cap, n);
    cmd_put(s_cap, n);
    s_dst.captured[c]++;
    RPHASE(3);
}

static void apply_tev_cb(const void* a) { apply_tev((const PlatGpuState*)a, 1); }
static void apply_tex_cb(const void* a) { tex_bind3((C3D_Tex* const*)a); }
static void apply_pix_cb(const void* a) { apply_pix((const PlatGpuState*)a, 1); }

/* The state's three classes, each sent when the GPU does not hold its key. Returns 1 when the pixel block went out
 * (it sets the cull mode to none). */
static int direct_state(const PlatGpuState* st) {
    u32 k[DK_MAX];
    int pix = 0;
    memcpy(k, st, 26 * 4);
    k[26] = st->fog_enable;
    k[27] = st->fog_enable ? st->fog_color : 0;
    if (!(s_gpu_valid & 1) || memcmp(k, s_dkey[DC_TEV], DK_TEV * 4) != 0) {
        direct_block(DC_TEV, k, apply_tev_cb, st);
        memcpy(s_dkey[DC_TEV], k, DK_TEV * 4);
    }
    /* textures: the sampler setters first, then the objects are the key */
    C3D_Tex* t[3];
    tex_prepare(st, t);
    for (int u = 0; u < 3; u++) {
        memcpy(k + 6 * u, t[u], sizeof(C3D_Tex));
    }
    if (!(s_gpu_valid & 2) || memcmp(k, s_dkey[DC_TEX], DK_TEX * 4) != 0) {
        direct_block(DC_TEX, k, apply_tex_cb, t);
        memcpy(s_dkey[DC_TEX], k, DK_TEX * 4);
    }
    memcpy(k, &st->alpha_test_enable, 16);
    memcpy(k + 4, &st->scissor_x0, 32);
    if (!(s_gpu_valid & 4) || memcmp(k, s_dkey[DC_PIX], DK_PIX * 4) != 0) {
        /* a viewport under one pixel is not set (apply_pix): the GPU keeps the last one, which the key does not name */
        float tx, ty, tw, th;
        efb_to_target_rect(st->viewport_x, st->viewport_y, st->viewport_w, st->viewport_h, &tx, &ty, &tw, &th);
        if (!(tw >= 1 && th >= 1)) {
            s_dst.degenerate++;
        }
        direct_block(DC_PIX, k, apply_pix_cb, st);
        memcpy(s_dkey[DC_PIX], k, DK_PIX * 4);
        pix = 1;
        /* kept words do not run apply_pix: the empty-scissor flag of this key (scissor guard, c883edf) */
        int sc[4];
        s_scissor_empty = scissor_rect(st, sc);
    }
    s_gpu_valid = 7;
    s_shadow_stale = 1;
    return pix;
}

static void vsh_apply_layout(const void* stream, u32 layout, C3D_AttrInfo* ai, C3D_BufInfo* vbi);
typedef struct {
    const void* stream;
    u32 layout;
    C3D_AttrInfo* ai;
    C3D_BufInfo* vbi;
} LayArg;
static void apply_lay_cb(const void* a) {
    const LayArg* l = (const LayArg*)a;
    vsh_apply_layout(l->stream, l->layout, l->ai, l->vbi);
}

static void direct_layout(const void* stream, u32 layout, C3D_AttrInfo* ai, C3D_BufInfo* vbi) {
    u32 k[DK_LAY] = {(u32)(uintptr_t)stream, layout};
    LayArg a = {stream, layout, ai, vbi};
    direct_block(DC_LAY, k, apply_lay_cb, &a);
    s_lay_valid = 1;
    s_shadow_stale = 1;
}

/* GPUREG_FACECULLING_CONFIG as C3Di_EffectBind writes it */
static void direct_cull(GPU_CULLMODE m) {
    u32 o = gpuCmdBufOffset;
    GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, (u32)m & 3);
    OHV_FROM(o);
}

/* The pending uniform rows, as C3D_UpdateUniforms writes a run of dirty rows. citro3d's copy of the rows is kept
 * equal, so an upload of its own (after C3Di_DirtyUniforms) writes the same values. */
static void direct_unif_run(u32 i, u32 j) {
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | i);
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, &s_vval[i][0], (j - i) * 4);
}

static void direct_unif_flush(void) {
    if ((s_vpend[0] | s_vpend[1] | s_vpend[2]) == 0) {
        return;
    }
    u32 i = 0;
    while (i < 96) {
        u32 wd = s_vpend[i >> 5] >> (i & 31);
        if (wd == 0) {
            i = (i | 31) + 1;
            continue;
        }
        if (!(wd & 1)) {
            i += (u32)__builtin_ctz(wd);
            continue;
        }
        u32 j = i;
        while (j < 96 && (s_vpend[j >> 5] >> (j & 31) & 1)) {
            j++;
        }
        memcpy(&C3D_FVUnif[GPU_VERTEX_SHADER][i], s_vval[i], (j - i) * 16);
        if (s_direct == 2) {
            /* the same rows through citro3d (nothing else is dirty: the draw flushed citro3d first) */
            u32 *buf, size, off;
            GPUCMD_GetBuffer(&buf, &size, &off);
            GPUCMD_SetBuffer(s_cap, sizeof(s_cap) / 4, 0);
            direct_unif_run(i, j);
            u32 n1 = gpuCmdBufOffset;
            GPUCMD_SetBuffer(s_cap2, sizeof(s_cap2) / 4, 0);
            memset(&C3D_FVUnifDirty[GPU_VERTEX_SHADER][i], 1, j - i);
            C3D_UpdateUniforms(GPU_VERTEX_SHADER);
            u32 n2 = gpuCmdBufOffset;
            GPUCMD_SetBuffer(buf, size, off);
            s_dst.checks++;
            if (n1 != n2 || memcmp(s_cap, s_cap2, n1 * 4) != 0) {
                if (s_dst.mismatches++ < 8) {
                    PLOG_W("gxdirect verify: uniform rows %lu..%lu: %lu words, citro3d %lu", (unsigned long)i,
                           (unsigned long)j - 1, (unsigned long)n1, (unsigned long)n2);
                }
            }
        }
        u32 o = gpuCmdBufOffset;
        direct_unif_run(i, j);
        OHV_FROM(o);
        s_dst.runs++;
        i = j;
    }
    s_vpend[0] = s_vpend[1] = s_vpend[2] = 0;
}

/* C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx) after its C3Di_UpdateContext */
static void direct_draw(const u16* idx, u32 count) {
    u32 pa = osConvertVirtToPhys(idx);
    u32 base = C3DX_BUFBASE;
    if (pa < base) {
        return; /* as C3D_DrawElements */
    }
    s_dst.draws++;
    if (s_direct == 2) {
        u32 off0 = gpuCmdBufOffset;
        C3D_DrawElements(GPU_TRIANGLES, (int)count, C3D_UNSIGNED_SHORT, idx);
        memcpy(s_cap, s_tpl, s_tpl_n * 4);
        s_cap[s_tpl_idx] = (pa - base) | (u32)C3D_UNSIGNED_SHORT << 31;
        s_cap[s_tpl_cnt] = count;
        s_dst.checks++;
        if (gpuCmdBufOffset - off0 != s_tpl_n || memcmp(gpuCmdBuf + off0, s_cap, s_tpl_n * 4) != 0) {
            if (s_dst.mismatches++ < 8) {
                PLOG_W("gxdirect verify: draw packet differs from C3D_DrawElements' (%lu words, citro3d %lu)",
                       (unsigned long)s_tpl_n, (unsigned long)(gpuCmdBufOffset - off0));
            }
        }
        return;
    }
    if ((gpuCmdBufOffset & 1) || gpuCmdBufOffset + s_tpl_n > gpuCmdBufSize) {
        C3D_DrawElements(GPU_TRIANGLES, (int)count, C3D_UNSIGNED_SHORT, idx); /* libctru reports the full buffer */
        return;
    }
    u32* p = gpuCmdBuf + gpuCmdBufOffset;
    memcpy(p, s_tpl, s_tpl_n * 4);
    p[s_tpl_idx] = (pa - base) | (u32)C3D_UNSIGNED_SHORT << 31;
    p[s_tpl_cnt] = count;
    ohv_g(p, s_tpl_n);
    gpuCmdBufOffset += s_tpl_n;
    C3DX_FLAGS |= C3DXF_DRAWUSED;
}

/* The fog LUT: citro3d writes it only once fog is on (C3Di_UpdateContext); with gxdirect it goes out at once with
 * the same words (index 0, then the 128 entries) and is taken off citro3d's pending list. */
static void fog_bind(const void* body) {
    static C3D_FogLut lut;
    /* citro3d takes 256 floats: the 128 table values, then each entry's difference to the next; the record holds
     * the 129 values of PlatGpuState.fog_lut */
    const float* v = (const float*)body;
    float data[256];
    for (int i = 0; i < 128; i++) {
        data[i] = v[i];
        data[128 + i] = v[i + 1] - v[i];
    }
    FogLut_FromArray(&lut, data);
    C3D_FogLutBind(&lut);
    if (s_direct) {
        u32 o = gpuCmdBufOffset;
        GPUCMD_AddWrite(GPUREG_FOG_LUT_INDEX, 0);
        GPUCMD_AddWrites(GPUREG_FOG_LUT_DATA0, lut.data, 128);
        OHV_FROM(o);
        C3DX_FLAGS &= ~C3DXF_FOGLUT;
    }
}

#define TPL_IDX_MARK 0xA5A5A5A4u
#define TPL_CNT_MARK 0x5A5A5A5Au
static u32 s_oh_base; /* the attribute buffers' base address (BufInfo_Init's base_paddr; gxonehop's draw packets) */
static int direct_init(void) {
    /* citro3d's context words: the flags (active bit; C3D_SetBufInfo sets the buffer bit) and the buffer base
     * (C3D_SetBufInfo copies the BufInfo there) */
    u32 f0 = C3DX_FLAGS;
    C3D_BufInfo bi;
    BufInfo_Init(&bi);
    C3D_SetBufInfo(&bi);
    u32 f1 = C3DX_FLAGS;
    if (!(f0 & C3DXF_ACTIVE) || !(f1 & 0x8u) || bi.base_paddr == 0 || C3DX_BUFBASE != bi.base_paddr) {
        PLOG_W("gxdirect: citro3d's context is not laid out as expected (flags %08lx -> %08lx, base %08lx): off",
               (unsigned long)f0, (unsigned long)f1, (unsigned long)C3DX_BUFBASE);
        return 0;
    }
    s_oh_base = bi.base_paddr;
    /* C3D_DrawElements' packet for triangles (citro3d 1.7.1 drawElements.c; checked against libcitro3d.a) */
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    GPUCMD_SetBuffer(s_tpl, sizeof(s_tpl) / 4, 0);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, TPL_IDX_MARK);
    GPUCMD_AddWrite(GPUREG_NUMVERTICES, TPL_CNT_MARK);
    GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
    GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    s_tpl_n = gpuCmdBufOffset;
    GPUCMD_SetBuffer(buf, size, off);
    int fi = -1, fc = -1;
    for (u32 i = 0; i < s_tpl_n; i += 2) { /* one parameter per command: [parameter, header] pairs */
        if (s_tpl[i] == TPL_IDX_MARK) {
            fi = (int)i;
        }
        if (s_tpl[i] == TPL_CNT_MARK) {
            fc = (int)i;
        }
    }
    if (s_tpl_n != 30 || fi < 0 || fc < 0) {
        PLOG_W("gxdirect: unexpected draw packet (%lu words): off", (unsigned long)s_tpl_n);
        return 0;
    }
    s_tpl_idx = (u32)fi;
    s_tpl_cnt = (u32)fc;
    /* largest blocks (counted from the encoders): tev 42, tex 30, pix 46, lay 76 words; a larger one is not kept */
    static const u16 k_slots0[DC_N] = {256, 512, 256, 512}, k_slots2[DC_N] = {512, 2048, 512, 1024};
    const u16* k_slots = s_onehop == 2 ? k_slots2 : k_slots0; /* gxonehop=2 looks up the one-hop keys here */
    static const u16 k_keyw[DC_N] = {DK_TEV, DK_TEX, DK_PIX, DK_LAY};
    static const u16 k_capw[DC_N] = {44, 32, 48, 76};
    u32 bytes = 0;
    for (int c = 0; c < DC_N; c++) {
        DTab* t = &s_dtab[c];
        t->slots = k_slots[c];
        t->keyw = k_keyw[c];
        t->capw = k_capw[c];
        t->stride = 3 + t->keyw + t->capw;
        t->mem = calloc((size_t)t->slots * t->stride, 4);
        bytes += t->slots * t->stride * 4;
        if (t->mem == NULL) {
            PLOG_W("gxdirect: no memory for the kept words (%lu KB): off", (unsigned long)(bytes >> 10));
            for (int d = 0; d < c; d++) {
                free(s_dtab[d].mem);
                s_dtab[d].mem = NULL;
            }
            return 0;
        }
    }
    s_dlog_every = (u32)plat_switch_int("gxthreadstats", plat_switch_int("gxstats", 0));
    PLOG_I("gxdirect=%d: draw packet %lu words, kept words %lu KB", s_direct, (unsigned long)s_tpl_n,
           (unsigned long)(bytes >> 10));
    return 1;
}

static void direct_log(void) {
    if (!s_direct || s_dlog_every == 0 || ++s_dlog_n < s_dlog_every) {
        return;
    }
    u32 n = s_dlog_n;
    s_dlog_n = 0;
    /* two lines: the log holds about 250 characters per line */
    PLOG_I("gxdirect frame %lu: per frame %lu draws, kept tev %lu tex %lu pix %lu lay %lu, captured %lu %lu %lu %lu, "
           "c3d flushes %lu, unif runs %lu",
           (unsigned long)s_frame_counter, (unsigned long)(s_dst.draws / n), (unsigned long)(s_dst.copied[0] / n),
           (unsigned long)(s_dst.copied[1] / n), (unsigned long)(s_dst.copied[2] / n),
           (unsigned long)(s_dst.copied[3] / n), (unsigned long)(s_dst.captured[0] / n),
           (unsigned long)(s_dst.captured[1] / n), (unsigned long)(s_dst.captured[2] / n),
           (unsigned long)(s_dst.captured[3] / n), (unsigned long)(s_dst.flushes / n), (unsigned long)(s_dst.runs / n));
    PLOG_I("gxdirect frame %lu, in %lu frames: verify checks %lu mismatches %lu, uncached %lu, degenerate viewport %lu, "
           "largest %lu %lu %lu %lu words",
           (unsigned long)s_frame_counter, (unsigned long)n, (unsigned long)s_dst.checks,
           (unsigned long)s_dst.mismatches, (unsigned long)s_dst.uncached, (unsigned long)s_dst.degenerate,
           (unsigned long)s_dst.maxw[0], (unsigned long)s_dst.maxw[1], (unsigned long)s_dst.maxw[2],
           (unsigned long)s_dst.maxw[3]);
    PLOG_I("gxdirect frame %lu: GSP data-cache flushes per frame %lu calls (%lu stream ranges), %lu bytes",
           (unsigned long)s_frame_counter, (unsigned long)(s_dst.fl_calls / n), (unsigned long)(s_dst.fl_ranges / n),
           (unsigned long)(s_dst.fl_bytes / n));
    memset(&s_dst, 0, sizeof(s_dst));
}

/* ---- gxonehop (stage 5): the recording thread writes the shader path's PICA words ----
 *
 * With gxonehop=1 the thread that records (the game thread: gxonehop turns gxthread off) does what the gxdirect
 * replay does for shader-path draws, at the time of the call, into the record's word arena: the state's three class
 * blocks and the attribute setup copied from its own tables of kept words, the uniform rows, the cull word, the
 * draw packet, the fog LUT. Its model of what the GPU holds follows the replay's rules exactly (state sent at the
 * next draw, a block only when its key differs from the one held, a record starts with nothing held, a clear, copy or
 * CPU-path draw leaves nothing held). The record keeps only what the render thread must do itself:
 *  - CMD_OH_WORDS: a run of words, copied into citro3d's command buffer;
 *  - CMD_OH_FLUSH: before the first draw after citro3d work: the vsh switch on, citro3d's pending words out
 *    (the replay's c3d_flush);
 *  - CMD_OH_UNIF: the `lit` bool and the light count int uniform, which stay with citro3d;
 *  - CMD_OH_CAPTURE: a block whose key the tables do not hold yet: the render thread runs citro3d's setters and
 *    captures the words as gxdirect does, and the recording thread takes them into its tables when the record comes
 *    back (a later capture of the same key in the same record copies them: `ref`);
 *  - CMD_OH_MARK: the stereo boundary. After a scene draw or copy, before the next screen-layer draw, clear,
 *    CPU-path draw or the record's end. The model then holds nothing (the next draw sends its full state) and the
 *    mark carries the uniform rows, the fog LUT and the uniforms of citro3d in force, so the 2D tail after the last
 *    mark is self-contained: in stereo the render thread splits there, and replays the tail for the second eye
 *    with the mark's words first. In mono a mark does nothing.
 *  - CMD_STATE (whole) before a clear, copy or CPU-path draw when the state changed: citro3d's setters need it.
 * gxonehop=2 renders as gxdirect=1 from the full record and builds the words of mode 1 next to it (side list,
 * marks also in the record so the replay drops what it holds there as mode 1 does); after each mono record the
 * render thread compares them word for word with the words the replay wrote itself. */
#define OH_SIDE_BYTES (192u << 10)
#define OH_ROOM_BYTES 3072u /* record bytes a shader draw's commands, a CMD_STATE and a mark need at most */
#define OH_DRAW_WORDS 1024u /* arena words of one shader draw at most (3 blocks, a layout, all rows, the packet) */
#define OH_MARK_WORDS 640u  /* arena words of a mark at most (all rows, the fog LUT) */
#define OH_PENDING 0xFFFFFFFFu
#define OH_CAPW 76u

typedef struct {
    const u32* w;
    u32 n, draws;
} OhWords;
typedef struct {
    s32 lit, lcount; /* -1: unchanged */
} OhUnif;
typedef struct OhCap {
    u32 cls, n; /* n: the captured words, filled by the render thread (0: not kept) */
    const struct OhCap* ref;
    u32 key[DK_MAX];
    u32 res[OH_CAPW];
    u32 dbg[3]; /* texture handles (verify log) */
} OhCap;
typedef struct {
    s32 lit, lcount;
    const u32* w;
    u32 n;
} OhMark;

/* the recording thread's model of the GPU (mirrors the replay's s_dkey, s_gpu_valid, v_* ...) */
static struct {
    u32 dkey[3][DK_MAX];
    int gpu_valid, lay_valid, vsh_mode, c3d, scissor_empty;
    const void* v_stream;
    u32 v_layout, v_scales, v_cull;
    s32 lit, lcount; /* the values the last CMD_OH_UNIF set (-1: none in this record) */
    u32 fog[128];
    int fog_valid;
    u32 chunk0, chunk_draws; /* the open run of words starts at chunk0 */
    u32* packet;             /* the last draw packet, while a draw may merge into it */
    PlatGpuState given;      /* the last CMD_STATE (gxonehop=1) */
    int given_valid;
    int after_scene, scene_seen;
    u32 gdraw, seq;
} s_om;
static VshDraw s_oh_md; /* gxonehop=1: the last shader draw, while one may merge into it (s_last_vdraw) */
static DTab s_otab[DC_N];
static u32 s_oh_words; /* arena words per record */
static struct {
    u32 draws, merged, skipped, copied[DC_N], caps, refs, flushes, ops, marks, chunks, words, states;
} s_ost;
static struct {
    u32 chunks, words, caps, refs, marks;
    u32 checks, mismatches, unresolved, cmp_words, logged;
} s_orst;

static inline Record* oh_rec(void) { return &s_rec[s_cur]; }

/* GPUCMD_Add's encoding (libctru 2.x: runs of 256 parameters at most, padded to an even length; checked against
 * GPUCMD_Add at start-up), into a buffer of our own: the recording thread cannot use libctru's global one. */
static u32* enc_add(u32* w, u32 header, const u32* p, u32 len) {
    if (len == 0) {
        len = 1;
    }
    while (len > 0) {
        u32 n = len < 256 ? len : 256;
        w[0] = p ? p[0] : 0;
        w[1] = header | (n - 1) << 20;
        if (n > 1) {
            if (p) {
                memcpy(w + 2, p + 1, (n - 1) * 4);
            } else {
                memset(w + 2, 0, (n - 1) * 4);
            }
        }
        w += n + 1;
        if (!(n & 1)) {
            *w++ = 0;
        }
        if ((s32)header < 0) {
            header += n;
        }
        len -= n;
        if (p) {
            p += n;
        }
    }
    return w;
}
static inline u32* enc_write(u32* w, u32 reg, u32 v) {
    w[0] = v;
    w[1] = GPUCMD_HEADER(0, 0xF, reg);
    return w + 2;
}

static void* side_put(u16 type, u32 size) {
    Record* r = oh_rec();
    u32 need = (u32)sizeof(CmdHead) + ((size + 3u) & ~3u);
    if (r->side_used + need > OH_SIDE_BYTES) {
        r->side_used = OH_SIDE_BYTES + 1; /* overflowed: this record is not compared */
        return NULL;
    }
    CmdHead* h = (CmdHead*)(r->side + r->side_used);
    h->type = type;
    h->size = (u16)(need - sizeof(CmdHead));
    r->side_used += need;
    return h + 1;
}

static void* oh_put(u16 type, u32 size) {
    if (s_onehop == 1) {
        return rec_put(type, size); /* closes the open run first */
    }
    oh_close();
    return side_put(type, size);
}

static void oh_close(void) {
    Record* r = oh_rec();
    if (r->now > s_om.chunk0) {
        OhWords* c = s_onehop == 1 ? rec_put(CMD_OH_WORDS, sizeof(OhWords)) : side_put(CMD_OH_WORDS, sizeof(OhWords));
        r = oh_rec();
        if (c != NULL) {
            *c = (OhWords){r->ow + s_om.chunk0, r->now - s_om.chunk0, s_om.chunk_draws};
            s_ost.chunks++;
        }
    }
    s_om.chunk0 = r->now;
    s_om.chunk_draws = 0;
    s_om.packet = NULL;
}

static void oh_event(void) {
    s_last_draw = NULL;
    if (s_onehop == 1) {
        s_last_vdraw = NULL;
    }
    s_om.packet = NULL;
}

/* Room for one more shader draw (or what goes before a clear, copy or CPU-path draw) in this record: a segment
 * now, never in the middle of a draw's words. */
static void oh_need_room(u32 bytes, u32 words) {
    Record* r = oh_rec();
    if (r->used + bytes + OH_ROOM_BYTES + 64 > RECORD_BYTES || r->now + words + OH_DRAW_WORDS + OH_MARK_WORDS > s_oh_words ||
        (s_onehop == 2 && r->side_used + OH_ROOM_BYTES > OH_SIDE_BYTES)) {
        flush_segment();
    }
}

static void oh_record_start(void) {
    Record* r = oh_rec();
    r->now = 0;
    r->side_used = 0;
    r->oh_split = -1;
    r->oh_tail_draws = r->oh_tail_z = 0;
    r->ncaps = 0;
    memset(&s_om, 0, sizeof(s_om));
    s_om.v_scales = 0xFFFFFFFFu;
    s_om.c3d = 1;
    s_om.lit = -1;
    s_om.lcount = -1;
    s_om.seq = s_submit_seq + 1;
    vunif_reset_u(&s_vg);
    s_vg.pend[0] = s_vg.pend[1] = s_vg.pend[2] = 0;
    s_oh_state_pending = 0;
}

/* the stereo split's classes (stereo_split) */
enum { OHS_NONE, OHS_SCENE, OHS_LAYER };
static int oh_class(const PlatGpuState* st) {
    if (st->color_write == 0 && !st->depth_write) {
        return OHS_NONE;
    }
    return st->depth_far > STEREO_LAYER_FAR ? OHS_SCENE : OHS_LAYER;
}

/* runs of the rows in mask (3 words), as direct_unif_run writes them */
static u32* oh_rows(u32* w, const u32* mask) {
    u32 i = 0;
    while (i < 96) {
        u32 wd = mask[i >> 5] >> (i & 31);
        if (wd == 0) {
            i = (i | 31) + 1;
            continue;
        }
        if (!(wd & 1)) {
            i += (u32)__builtin_ctz(wd);
            continue;
        }
        u32 j = i;
        while (j < 96 && (mask[j >> 5] >> (j & 31) & 1)) {
            j++;
        }
        w = enc_write(w, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | i);
        w = enc_add(w, GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_DATA), &s_vg.val[i][0], (j - i) * 4);
        i = j;
    }
    return w;
}

static void oh_mark(void) {
    oh_close();
    Record* r = oh_rec();
    u32* w0 = r->ow + r->now;
    u32* w = oh_rows(w0, s_vg.valid);
    if (s_om.fog_valid) {
        w = enc_write(w, GPUREG_FOG_LUT_INDEX, 0);
        w = enc_add(w, GPUCMD_HEADER(0, 0xF, GPUREG_FOG_LUT_DATA0), s_om.fog, 128);
    }
    r->now = (u32)(w - r->ow);
    s_om.chunk0 = r->now; /* the mark's words are not part of the run */
    OhMark* m = oh_put(CMD_OH_MARK, sizeof(OhMark));
    r = oh_rec();
    if (m != NULL) {
        *m = (OhMark){s_om.lit, s_om.lcount, w0, (u32)(w - w0)};
        if (s_onehop == 1) {
            r->oh_split = (s32)((u8*)m - sizeof(CmdHead) - r->data);
        }
    }
    if (s_onehop == 2) {
        rec_put(CMD_OH_MARK, 0); /* the replay drops what it holds there too */
    }
    s_om.gpu_valid = 0;
    s_om.lay_valid = 0;
    s_om.c3d = 1;
    s_om.after_scene = 0;
    r->oh_tail_draws = r->oh_tail_z = 0;
    s_ost.marks++;
    oh_event();
}

static void oh_stereo_after(int cls, int zwrite) {
    Record* r = oh_rec();
    if (cls == OHS_SCENE) {
        s_om.after_scene = 1;
        s_om.scene_seen = 1;
        r->oh_tail_draws = r->oh_tail_z = 0;
    } else if (cls == OHS_LAYER) {
        r->oh_tail_draws++;
        r->oh_tail_z += zwrite != 0;
    }
}

static void oh_record_end(int present) {
    if (present && s_om.after_scene) {
        oh_mark();
    }
    oh_close();
    if (!s_om.scene_seen) {
        oh_rec()->oh_split = -1; /* no scene draw: the frame stays mono */
    }
}

static void oh_c3d_cmd(int what) {
    if (what == OHC_DRAW0 || what == OHC_DRAW) {
        if (s_last_state_valid && oh_class(&s_last_state) != OHS_SCENE && s_om.after_scene) {
            oh_mark();
        }
    } else if (what == OHC_CLEAR && s_om.after_scene) {
        oh_mark();
    }
    if (s_onehop == 1 && what != OHC_DRAW0 && s_last_state_valid &&
        (!s_om.given_valid || memcmp(&s_om.given, &s_last_state, sizeof(PlatGpuState)) != 0)) {
        PlatGpuState* d = rec_put(CMD_STATE, sizeof(PlatGpuState));
        if (d != NULL) {
            *d = s_last_state;
            s_om.given = s_last_state;
            s_om.given_valid = 1;
            s_ost.states++;
        }
    }
}

static void oh_after_c3d(int what) {
    if (what == OHC_DRAW0 || what == OHC_DRAW) {
        if (s_last_state_valid) {
            oh_stereo_after(oh_class(&s_last_state), s_last_state.depth_write);
        }
    } else if (what == OHC_COPY) {
        s_om.after_scene = 1;
        oh_rec()->oh_tail_draws = oh_rec()->oh_tail_z = 0;
    }
    if (what != OHC_DRAW0) {
        /* the replay's C3D_PATH and LEAVE_VSH: nothing is held, the vsh switch is off, citro3d has work pending */
        s_om.gpu_valid = 0;
        s_om.lay_valid = 0;
        s_om.vsh_mode = 0;
        s_om.c3d = 1;
    }
    oh_event();
}

static int scissor_rect_n(const PlatGpuState* st) {
    float sx, sy, sw, sh;
    efb_to_target_rect(st->scissor_x0, st->scissor_y0, (float)(st->scissor_x1 - st->scissor_x0),
                       (float)(st->scissor_y1 - st->scissor_y0), &sx, &sy, &sw, &sh);
    float cx0 = sx < 0 ? 0 : sx > REGION_W ? REGION_W : sx;
    float cy0 = sy < 0 ? 0 : sy > REGION_H ? REGION_H : sy;
    float cx1 = sx + sw < 0 ? 0 : sx + sw > REGION_W ? REGION_W : sx + sw;
    float cy1 = sy + sh < 0 ? 0 : sy + sh > REGION_H ? REGION_H : sy + sh;
    int x0 = (int)cx0, y0 = (int)cy0, x1 = (int)cx1, y1 = (int)cy1;
    x1 += (float)x1 < cx1;
    y1 += (float)y1 < cy1;
    return x1 <= x0 || y1 <= y0;
}

static int oh_cpu_draw_keep(void) {
    if (scissor_rect_n(&s_last_state)) {
        return 1; /* skipped by the replay before the filter */
    }
    return plat_gpudump_draw_filter(s_frame_counter, s_om.seq, s_om.gdraw++);
}

static void oh_fog(const float* lut129) {
    oh_event();
    Record* r = oh_rec();
    if (r->now + 132 + OH_DRAW_WORDS + OH_MARK_WORDS > s_oh_words) {
        flush_segment();
        r = oh_rec();
    }
    C3D_FogLut lut;
    float data[256];
    for (int i = 0; i < 128; i++) {
        data[i] = lut129[i];
        data[128 + i] = lut129[i + 1] - lut129[i];
    }
    FogLut_FromArray(&lut, data);
    u32* w = enc_write(r->ow + r->now, GPUREG_FOG_LUT_INDEX, 0);
    w = enc_add(w, GPUCMD_HEADER(0, 0xF, GPUREG_FOG_LUT_DATA0), lut.data, 128);
    r->now = (u32)(w - r->ow);
    memcpy(s_om.fog, lut.data, sizeof(s_om.fog));
    s_om.fog_valid = 1;
}

static void oh_op(s32 lit, s32 lcount) {
    OhUnif* o = oh_put(CMD_OH_UNIF, sizeof(OhUnif));
    if (o != NULL) {
        o->lit = lit;
        o->lcount = lcount;
    }
    s_om.c3d = 1;
    s_ost.ops++;
}

static void oh_const(const PlatGpuVshConst* c) {
    oh_event();
    vsh_const_rows(&s_vg, c);
    if (s_uloc_lit >= 0) {
        s32 lit = (c->flags & PLAT_VSH_LIT) != 0;
        if (lit != s_om.lit) {
            oh_op(lit, -1);
            s_om.lit = lit;
        }
    }
}

static void oh_light(const PlatGpuVshLight* l) {
    oh_event();
    if (s_uloc_lit < 0) {
        return;
    }
    s32 x = vsh_light_rows(&s_vg, l);
    if (x != s_om.lcount) {
        oh_op(-1, x);
        s_om.lcount = x;
    }
}

/* A pending entry: the capture body that will hold the words, and the record (seq) it is in. */
static void oh_pend_put(DTab* t, u32* e, const u32* key, u32 h, const OhCap* b) {
    if (e == NULL) {
        u32 i = h & (t->slots - 1) & ~(DT_WAYS - 1u);
        e = t->mem + (size_t)i * t->stride;
        for (u32 k = 1; k < DT_WAYS && e[1] != 0; k++) {
            u32* x = t->mem + (size_t)(i + k) * t->stride;
            if (x[1] == 0 || (s32)(x[2] - e[2]) < 0) {
                e = x;
            }
        }
    }
    e[0] = h;
    e[1] = OH_PENDING;
    e[2] = ++t->stamp;
    memcpy(e + 3, key, t->keyw * 4);
    e[3 + t->keyw] = (u32)(uintptr_t)b;
    e[4 + t->keyw] = s_om.seq;
}

static const PlatGpuState* s_oh_dbg_st;
static void oh_block(int c, const u32* key) {
    DTab* t = &s_otab[c];
    u32 h = dkey_hash(key, t->keyw);
    u32* e = dtab_find(t, key, h);
    Record* r = oh_rec();
    if (e != NULL && e[1] != OH_PENDING) {
        memcpy(r->ow + r->now, e + 3 + t->keyw, e[1] * 4);
        r->now += e[1];
        s_ost.copied[c]++;
        return;
    }
    OhCap* b = oh_put(CMD_OH_CAPTURE, sizeof(OhCap));
    if (b == NULL) {
        return;
    }
    r = oh_rec();
    b->cls = (u32)c;
    b->n = 0;
    b->ref = NULL;
    memcpy(b->key, key, t->keyw * 4);
    if (c == DC_TEX && s_oh_dbg_st != NULL) {
        for (int u = 0; u < 3; u++) {
            b->dbg[u] = s_oh_dbg_st->tex[u].handle;
        }
    }
    if (e != NULL && e[4 + t->keyw] == s_om.seq) {
        b->ref = (const OhCap*)(uintptr_t)e[3 + t->keyw]; /* captured earlier in this record */
        s_ost.refs++;
        return;
    }
    if (r->ncaps < sizeof(r->caps) / sizeof(r->caps[0])) {
        r->caps[r->ncaps++] = b;
        oh_pend_put(t, e, key, h, b);
    }
    s_ost.caps++;
}

/* The texture units' objects as tex_prepare leaves them, as copies (s_tex belongs to the render thread's setters). */
static void oh_tex_prepare(const PlatGpuState* st, C3D_Tex out[3]) {
    for (int u = 0; u < 3; u++) {
        const PlatGpuTexUnit* tu = &st->tex[u];
        if (tu->handle == 0 || tu->handle >= MAX_TEX || !s_tex[tu->handle].tex.data) {
            out[u] = s_blank_tex;
            continue;
        }
        C3D_Tex* t = &out[u];
        *t = s_tex[tu->handle].tex;
        C3D_TexSetFilter(t, tu->mag_linear ? GPU_LINEAR : GPU_NEAREST, tu->min_linear ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetFilterMipmap(t, tu->mip_linear && t->maxLevel != 0 ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetLodBias(t, tu->lod_bias);
        C3D_TexSetWrap(t, (GPU_TEXTURE_WRAP_PARAM)tu->wrap_s, (GPU_TEXTURE_WRAP_PARAM)tu->wrap_t);
    }
}

/* direct_state on the recording side */
static int oh_state(const PlatGpuState* st) {
    u32 k[DK_MAX];
    int pix = 0;
    memcpy(k, st, 26 * 4);
    k[26] = st->fog_enable;
    k[27] = st->fog_enable ? st->fog_color : 0;
    if (!(s_om.gpu_valid & 1) || memcmp(k, s_om.dkey[DC_TEV], DK_TEV * 4) != 0) {
        oh_block(DC_TEV, k);
        memcpy(s_om.dkey[DC_TEV], k, DK_TEV * 4);
    }
    C3D_Tex t[3];
    oh_tex_prepare(st, t);
    memcpy(k, t, sizeof(t));
    if (!(s_om.gpu_valid & 2) || memcmp(k, s_om.dkey[DC_TEX], DK_TEX * 4) != 0) {
        s_oh_dbg_st = st;
        oh_block(DC_TEX, k);
        s_oh_dbg_st = NULL;
        memcpy(s_om.dkey[DC_TEX], k, DK_TEX * 4);
    }
    memcpy(k, &st->alpha_test_enable, 16);
    memcpy(k + 4, &st->scissor_x0, 32);
    if (!(s_om.gpu_valid & 4) || memcmp(k, s_om.dkey[DC_PIX], DK_PIX * 4) != 0) {
        oh_block(DC_PIX, k);
        memcpy(s_om.dkey[DC_PIX], k, DK_PIX * 4);
        pix = 1;
        s_om.scissor_empty = scissor_rect_n(st);
    }
    s_om.gpu_valid = 7;
    return pix;
}

static void oh_unif_flush(void) {
    if ((s_vg.pend[0] | s_vg.pend[1] | s_vg.pend[2]) == 0) {
        return;
    }
    Record* r = oh_rec();
    r->now = (u32)(oh_rows(r->ow + r->now, s_vg.pend) - r->ow);
    s_vg.pend[0] = s_vg.pend[1] = s_vg.pend[2] = 0;
}

static GPU_CULLMODE vsh_cull(u32 cull);
static void oh_draw(const void* stream, u32 layout, u32 scales, u32 cull, const u16* idx, u32 count, int drawp, int merged) {
    if (!s_last_state_valid || count < 3) {
        s_ost.skipped++;
        return; /* the replay skips a draw before the record's first state */
    }
    int cls = oh_class(&s_last_state);
    if (drawp && s_ixpool_hi != NULL && ((const u8*)idx < s_ixpool_lo || (const u8*)(idx + count) > s_ixpool_hi)) {
        if (cls != OHS_SCENE && s_om.after_scene) {
            oh_mark();
        }
        oh_stereo_after(cls, s_last_state.depth_write);
        s_ost.skipped++;
        return; /* outside the index pool: skipped by the replay (which logs it) */
    }
    if (merged) {
        if (s_om.packet != NULL) {
            s_om.packet[s_tpl_cnt] += count;
        }
        s_oh_idx_end += count;
        s_ost.merged++;
        return;
    }
    if (cls != OHS_SCENE && s_om.after_scene) {
        oh_mark();
    }
    Record* r;
    if (!s_om.vsh_mode) {
        s_om.vsh_mode = 1;
        s_om.c3d = 1;
        s_om.v_stream = NULL;
    }
    if (s_om.c3d) {
        oh_put(CMD_OH_FLUSH, 0);
        s_om.c3d = 0;
        s_ost.flushes++;
    }
    if (s_oh_state_pending || s_om.gpu_valid != 7) {
        if (oh_state(&s_last_state)) {
            s_om.v_cull = 0;
        }
        s_oh_state_pending = 0;
    }
    u32* packet = NULL;
    if (!s_om.scissor_empty) {
        if (!s_om.lay_valid || stream != s_om.v_stream || layout != s_om.v_layout) {
            u32 k[DK_LAY] = {(u32)(uintptr_t)stream, layout};
            oh_block(DC_LAY, k);
            s_om.lay_valid = 1;
            s_om.v_stream = stream;
            s_om.v_layout = layout;
        }
        if (scales != s_om.v_scales) {
            vsh_scales_rows(&s_vg, scales);
            s_om.v_scales = scales;
        }
        r = oh_rec();
        if (cull != s_om.v_cull) {
            r->now = (u32)(enc_write(r->ow + r->now, GPUREG_FACECULLING_CONFIG, (u32)vsh_cull(cull) & 3) - r->ow);
            s_om.v_cull = cull;
        }
        oh_unif_flush();
        u32 pa = osConvertVirtToPhys(idx);
        if (pa >= s_oh_base && (s_onehop != 1 || plat_gpudump_draw_filter(s_frame_counter, s_om.seq, s_om.gdraw++))) {
            packet = r->ow + r->now;
            memcpy(packet, s_tpl, s_tpl_n * 4);
            packet[s_tpl_idx] = (pa - s_oh_base) | (u32)C3D_UNSIGNED_SHORT << 31;
            packet[s_tpl_cnt] = count;
            r->now += s_tpl_n;
            s_om.chunk_draws++;
            s_ost.draws++;
        }
    }
    oh_stereo_after(cls, s_last_state.depth_write);
    s_om.packet = packet;
    if (s_onehop == 1) {
        r = oh_rec();
        if (drawp) {
            s_last_vdraw = NULL;
        } else {
            s_oh_md = (VshDraw){stream, layout, scales, cull, 0, 0};
            s_last_vdraw = &s_oh_md;
            s_oh_idx_end = (u32)(idx - r->idx) + count;
        }
    }
}

/* Render thread: citro3d's `lit` and light count as the replay sets them (a change dirties citro3d). */
static void oh_apply_unif(s32 lit, s32 lcount) {
    if (lit >= 0 && s_uloc_lit >= 0 && ((C3D_BoolUnifs[GPU_VERTEX_SHADER] >> (s_uloc_lit - 0x68) & 1) != 0) != (lit != 0)) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_lit, lit != 0);
        s_c3d_unif = 1;
    }
    if (lcount >= 0 && s_uloc_lit >= 0 && C3D_IVUnif[GPU_VERTEX_SHADER][s_uloc_lcount - 0x60] != IVec_Pack(lcount, 0, 1, 0)) {
        C3D_IVUnifSet(GPU_VERTEX_SHADER, s_uloc_lcount, lcount, 0, 1, 0);
        s_c3d_unif = 1;
    }
}

static C3D_AttrInfo s_oh_vai;
static C3D_BufInfo s_oh_vbi;
static C3D_Tex s_oh_ctex[3]; /* the objects a texture capture binds (citro3d keeps the pointers) */

/* Render thread: a CMD_OH_CAPTURE, as direct_block's capture. */
static void oh_capture(OhCap* b) {
    const OhCap* src = b->ref;
    if (src != NULL && src->n > 0) {
        cmd_put(src->res, src->n);
        s_orst.refs++;
        return;
    }
    RPHASE(8);
    c3d_flush();
    PlatGpuState st;
    memset(&st, 0, sizeof(st));
    switch (b->cls) {
    case DC_TEV:
        memcpy(&st, b->key, 26 * 4);
        st.fog_enable = (u8)b->key[26];
        st.fog_color = b->key[27];
        apply_tev(&st, 1);
        break;
    case DC_TEX: {
        memcpy(s_oh_ctex, b->key, sizeof(s_oh_ctex));
        C3D_Tex* p[3] = {&s_oh_ctex[0], &s_oh_ctex[1], &s_oh_ctex[2]};
        tex_bind3(p);
        break;
    }
    case DC_PIX:
        memcpy(&st.alpha_test_enable, b->key, 16);
        memcpy(&st.scissor_x0, b->key + 4, 32);
        apply_pix(&st, 1);
        break;
    default:
        vsh_apply_layout((const void*)(uintptr_t)b->key[0], b->key[1], &s_oh_vai, &s_oh_vbi);
        break;
    }
    u32 n = c3d_capture(s_cap, sizeof(s_cap) / 4);
    s_c3d_unif = 0;
    cmd_put(s_cap, n);
    if (src == NULL && n <= OH_CAPW && n <= s_otab[b->cls].capw) {
        memcpy(b->res, s_cap, n * 4);
        b->n = n;
    }
    s_orst.caps++;
    RPHASE(3);
}

/* Recording thread, when a record comes back: its captured words into the tables. */
static void oh_import(Record* r) {
    for (u32 i = 0; i < r->ncaps; i++) {
        OhCap* b = r->caps[i];
        DTab* t = &s_otab[b->cls];
        u32 h = dkey_hash(b->key, t->keyw);
        u32* e = dtab_find(t, b->key, h);
        if (b->n > 0 && b->n <= t->capw) {
            dtab_put(t, e, b->key, h, b->res, b->n);
        } else if (e != NULL && e[1] == OH_PENDING && e[3 + t->keyw] == (u32)(uintptr_t)b) {
            e[1] = 0; /* not kept (too large, or the record was dropped): captured again when next needed */
        }
    }
    r->ncaps = 0;
}

/* gxonehop=2, render thread after a mono record: the one-hop words (side list; a capture's words from the replay's
 * own table, also handed back to the recording thread) against the words the replay wrote itself. */
static void ohv_compare(Record* r) {
    if (s_ohv_on != 1 || r->side_used > OH_SIDE_BYTES) {
        s_orst.unresolved++;
        return;
    }
    u32 pos = 0, ncmd = 0;
    int bad = 0;
    for (u8* p = r->side; p < r->side + r->side_used && !bad;) {
        CmdHead* h = (CmdHead*)p;
        u8* body = p + sizeof(CmdHead);
        p = body + h->size;
        ncmd++;
        const u32* w;
        u32 n;
        if (h->type == CMD_OH_WORDS) {
            w = ((const OhWords*)body)->w;
            n = ((const OhWords*)body)->n;
        } else if (h->type == CMD_OH_CAPTURE) {
            OhCap* b = (OhCap*)body;
            DTab* t = &s_dtab[b->cls];
            u32* e = dtab_find(t, b->key, dkey_hash(b->key, t->keyw));
            if (e == NULL) {
                s_orst.unresolved++;
                if (s_orst.logged++ < 8) {
                    PLOG_W("gxonehop verify: record %lu, side command %lu: a %s block the replay's table does not hold "
                           "(key %08lx %08lx %08lx %08lx): not compared",
                           (unsigned long)r->seq, (unsigned long)ncmd, k_dc_name[b->cls], (unsigned long)b->key[0],
                           (unsigned long)b->key[1], (unsigned long)b->key[2], (unsigned long)b->key[3]);
                    if (b->cls == DC_TEX) {
                        for (int u = 0; u < 3; u++) {
                            u32 hh = b->dbg[u] < MAX_TEX ? b->dbg[u] : 0;
                            PLOG_W("gxonehop verify:   unit %d handle %lu: key data %08lx, now %08lx live %d pending %d target %d",
                                   u, (unsigned long)b->dbg[u], (unsigned long)b->key[6 * u],
                                   (unsigned long)(uintptr_t)s_tex[hh].tex.data, s_tex[hh].live, s_tex[hh].pending_free,
                                   s_tex[hh].is_target);
                        }
                        for (u32 s = 0; s < t->slots; s++) {
                            const u32* x = t->mem + (size_t)s * t->stride + 3;
                            if (x[-2] != 0 && (x[0] == b->key[0] || x[6] == b->key[0] || x[12] == b->key[0])) {
                                PLOG_W("gxonehop verify:   replay entry %lu: %08lx %08lx %08lx %08lx %08lx %08lx | %08lx | "
                                       "%08lx; key %08lx %08lx %08lx %08lx %08lx %08lx",
                                       (unsigned long)s, (unsigned long)x[0], (unsigned long)x[1], (unsigned long)x[2],
                                       (unsigned long)x[3], (unsigned long)x[4], (unsigned long)x[5], (unsigned long)x[6],
                                       (unsigned long)x[12], (unsigned long)b->key[0], (unsigned long)b->key[1],
                                       (unsigned long)b->key[2], (unsigned long)b->key[3], (unsigned long)b->key[4],
                                       (unsigned long)b->key[5]);
                            }
                        }
                    }
                }
                return;
            }
            w = e + 3 + t->keyw;
            n = e[1];
            if (b->ref == NULL && n <= OH_CAPW) {
                memcpy(b->res, w, n * 4);
                b->n = n;
            }
        } else {
            continue;
        }
        u32 k = 0;
        if (pos + n > s_ohv_gn) {
            bad = 1;
        } else {
            while (k < n && w[k] == s_ohv_g[pos + k]) {
                k++;
            }
            bad = k < n;
        }
        if (bad && s_orst.logged++ < 8) {
            PLOG_W("gxonehop verify: record %lu, side command %lu (%s, %lu words) differs at word %lu of %lu: one-hop "
                   "%08lx, replay %08lx",
                   (unsigned long)r->seq, (unsigned long)ncmd, h->type == CMD_OH_WORDS ? "words" : "capture",
                   (unsigned long)n, (unsigned long)(pos + k), (unsigned long)s_ohv_gn,
                   (unsigned long)(k < n ? w[k] : 0), (unsigned long)(pos + k < s_ohv_gn ? s_ohv_g[pos + k] : 0));
        }
        pos += n;
    }
    if (!bad && pos != s_ohv_gn) {
        bad = 1;
        if (s_orst.logged++ < 8) {
            PLOG_W("gxonehop verify: record %lu: one-hop %lu words, replay %lu", (unsigned long)r->seq,
                   (unsigned long)pos, (unsigned long)s_ohv_gn);
        }
    }
    s_orst.checks++;
    s_orst.mismatches += bad;
    s_orst.cmp_words += pos;
}

static int oh_init(void) {
    /* the encoder against libctru's GPUCMD_Add (this thread owns citro3d's buffer until the render thread starts) */
    static u32 a[1100], b[1100], v[600];
    for (u32 i = 0; i < 600; i++) {
        v[i] = 0x9E3779B9u * (i + 1);
    }
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    for (u32 len = 0; len <= 520; len++) {
        for (int inc = 0; inc < 2; inc++) {
            u32 hdr = GPUCMD_HEADER(inc, 0xF, GPUREG_VSH_FLOATUNIFORM_DATA);
            GPUCMD_SetBuffer(a, 1100, 0);
            GPUCMD_Add(hdr, v, len);
            u32 n1 = gpuCmdBufOffset;
            u32 n2 = (u32)(enc_add(b, hdr, v, len) - b);
            if (n1 != n2 || memcmp(a, b, n1 * 4) != 0) {
                GPUCMD_SetBuffer(buf, size, off);
                PLOG_W("gxonehop: the command encoder differs from GPUCMD_Add (%lu parameters: %lu words, libctru %lu): off",
                       (unsigned long)len, (unsigned long)n2, (unsigned long)n1);
                return 0;
            }
        }
    }
    GPUCMD_SetBuffer(buf, size, off);
    /* larger than gxdirect's: with 512 texture slots a few keys of gor_01 were evicted within every frame */
    static const u16 k_slots[DC_N] = {512, 2048, 512, 1024};
    static const u16 k_keyw[DC_N] = {DK_TEV, DK_TEX, DK_PIX, DK_LAY};
    static const u16 k_capw[DC_N] = {44, 32, 48, 76};
    for (int c = 0; c < DC_N; c++) {
        DTab* t = &s_otab[c];
        t->slots = k_slots[c];
        t->keyw = k_keyw[c];
        t->capw = k_capw[c];
        t->stride = 3 + t->keyw + t->capw;
        t->mem = calloc((size_t)t->slots * t->stride, 4);
        if (t->mem == NULL) {
            PLOG_W("gxonehop: no memory for the tables: off");
            return 0;
        }
    }
    u32 kb = (u32)plat_switch_int("gxonehopkb", 512);
    if (kb < 64 || kb > 4096) {
        kb = 512;
    }
    s_oh_words = kb * 256u;
    for (int i = 0; i < 2; i++) {
        s_rec[i].ow = memalign(16, s_oh_words * 4);
        s_rec[i].side = s_onehop == 2 ? malloc(OH_SIDE_BYTES) : NULL;
        if (s_rec[i].ow == NULL || (s_onehop == 2 && s_rec[i].side == NULL)) {
            PLOG_W("gxonehop: no memory for two %lu KB word arenas: off", (unsigned long)kb);
            return 0;
        }
    }
    if (s_onehop == 2) {
        s_ohv_gcap = s_oh_words;
        s_ohv_g = malloc(s_ohv_gcap * 4);
        if (s_ohv_g == NULL) {
            PLOG_W("gxonehop=2: no memory for the compare buffer: off");
            return 0;
        }
    }
    PLOG_I("gxonehop=%d: the recording thread writes the shader path's words (2 x %lu KB arenas)%s", s_onehop,
           (unsigned long)kb, s_onehop == 2 ? ", compared with the gxdirect replay's" : "");
    return 1;
}

static void oh_log(void) {
    static u32 n;
    if (!s_onehop || s_dlog_every == 0 || ++n < s_dlog_every) {
        return;
    }
    u32 f = n;
    n = 0;
    PLOG_I("gxonehop frame %lu: per frame %lu draws (%lu merged, %lu skipped), kept tev %lu tex %lu pix %lu lay %lu, "
           "captures %lu refs %lu, flushes %lu, unif ops %lu, marks %lu, states %lu, runs %lu",
           (unsigned long)s_frame_counter, (unsigned long)(s_ost.draws / f), (unsigned long)(s_ost.merged / f),
           (unsigned long)(s_ost.skipped / f), (unsigned long)(s_ost.copied[0] / f), (unsigned long)(s_ost.copied[1] / f),
           (unsigned long)(s_ost.copied[2] / f), (unsigned long)(s_ost.copied[3] / f), (unsigned long)(s_ost.caps / f),
           (unsigned long)(s_ost.refs / f), (unsigned long)(s_ost.flushes / f), (unsigned long)(s_ost.ops / f),
           (unsigned long)(s_ost.marks / f), (unsigned long)(s_ost.states / f), (unsigned long)(s_ost.chunks / f));
    PLOG_I("gxonehop frame %lu, in %lu frames: render runs %lu words %lu captures %lu refs %lu marks used %lu; verify "
           "records %lu mismatches %lu not compared %lu words %lu",
           (unsigned long)s_frame_counter, (unsigned long)f, (unsigned long)s_orst.chunks, (unsigned long)s_orst.words,
           (unsigned long)s_orst.caps, (unsigned long)s_orst.refs, (unsigned long)s_orst.marks,
           (unsigned long)s_orst.checks, (unsigned long)s_orst.mismatches, (unsigned long)s_orst.unresolved,
           (unsigned long)s_orst.cmp_words);
    memset(&s_ost, 0, sizeof(s_ost));
    u32 logged = s_orst.logged;
    memset(&s_orst, 0, sizeof(s_orst)); /* the render thread's counters: racy, for the log only */
    s_orst.logged = logged;
}

static Record* s_render_rec; /* the record render_record replays (render thread), for gpudump_note */
static void gpudump_note(void) {
    Record* r = s_render_rec;
    if (r != NULL) {
        plat_gpudump_record(r->seq, r->frame, r->verts, r->nverts * (u32)sizeof(PlatGpuVertex), r->idx, r->nidx * 2u);
    }
}

static u64 s_begin_wait_acc; /* render thread: ticks in C3D_FrameBegin over this frame's records */
static u32 s_begin_records;

static void render_record(Record* r) {
    u64 t0 = svcGetSystemTick();
    s_rseq = r->seq;
    s_render_rec = r;
    vunif_reset();
    reap_targets();
    /* stereo (stereo.h): 0 mono, else the 2D tail's start; the 3D part is replayed once */
    u32 tail_cmds = 0, tail_zwrites = 0;
    u8* split = NULL;
    if (r->present && stereo_wanted()) {
        if (s_onehop == 1) {
            /* the last stereo mark (gxonehop): the tail after it is self-contained */
            split = r->oh_split >= 0 ? r->data + r->oh_split : NULL;
            tail_cmds = r->oh_tail_draws;
            tail_zwrites = r->oh_tail_z;
        } else {
            split = stereo_split(r, &tail_cmds, &tail_zwrites);
        }
    }
    int stereo = split != NULL && stereo_begin();
    s_ohv_on = s_onehop == 2 && !stereo; /* gxonehop=2: compare mono records */
    s_ohv_gn = 0;
    /* C3D_FrameBegin waits until the GPU has finished the work queued so far (the previous record): the fence
     * for the memory of that record (gpufence). Then the vblank wait C3D_FRAME_SYNCDRAW would have made first:
     * only before a frame's first record, since a segment or a mid-frame sync (plat_gpu_sync) after it would
     * wait for one more vblank each (gpusyncall=1: before every record, as before Step 1). */
    u64 tw = svcGetSystemTick();
    gpu_reset_apply(); /* a reset the game thread asked for since the last record */
    RPHASE(1);
    if (s_hang_fired < s_hang_times && plat_gpudump_test_hang_from(r->frame)) {
        s_hang_fired++;
        PLOG_W("gpuhangtest: the render thread stops before C3D_FrameBegin of record %lu (game frame %lu)%s",
               (unsigned long)r->seq, (unsigned long)r->frame, s_hang_recover ? ", until a GPU reset is requested" : "");
        u32 want = __atomic_load_n(&s_reset_seen, __ATOMIC_ACQUIRE);
        for (;;) {
            if (s_hang_recover && __atomic_load_n(&s_reset_req, __ATOMIC_ACQUIRE) != want) {
                break; /* the hang the test pretends to be is over: the real path continues here */
            }
            svcSleepThread(s_hang_recover ? 10000000LL : 1000000000LL);
        }
        gpu_reset_apply();
    }
    if (s_gpu_fence) {
        C3D_FrameBegin(0);
        if (gpu_reset_pending()) {
            /* The GPU was reset while this thread waited in C3D_FrameBegin for the queue: the frame is open,
             * and marking the context dirty is all it needs. */
            gpu_reset_apply();
        }
        gpu_done_publish(r->seq - 1);
        if (s_begin_records == 0 || s_sync_all) {
            C3D_FrameSync();
        }
    } else {
        C3D_FrameBegin(s_begin_records == 0 || s_sync_all ? C3D_FRAME_SYNCDRAW : 0);
    }
    RPHASE(2);
    s_begin_wait_acc += svcGetSystemTick() - tw;
    if (svcGetSystemTick() - tw > SYSCLOCK_ARM11) {
        PLOG_W("gpu: C3D_FrameBegin took %lu ms (a vblank or GX interrupt was missed, or the process was suspended)",
               (unsigned long)((svcGetSystemTick() - tw) / (SYSCLOCK_ARM11 / 1000u)));
    }
    s_begin_records++;
    if (stereo) {
        stereo_prepare();
        s_gx_entries += STEREO_GX_ENTRIES;
    } else {
        split = NULL;
    }
    C3D_FrameDrawOn(s_efb);
    C3D_BindProgram(&s_prog);
    C3D_SetAttrInfo(&s_attr);
    rot_uniforms();
    static C3D_BufInfo bi;
    BufInfo_Init(&bi);
    BufInfo_Add(&bi, r->verts, sizeof(PlatGpuVertex), 5, 0x43210);
    C3D_SetBufInfo(&bi);
    for (u32 i = 0; i < r->ndirty; i++) {
        GSPGPU_FlushDataCache((const void*)r->dirty[i].lo, (u32)(r->dirty[i].hi - r->dirty[i].lo));
        s_dst.fl_bytes += (u32)(r->dirty[i].hi - r->dirty[i].lo);
    }
    s_dst.fl_calls += r->ndirty + 1 + (r->nidx > 0);
    s_dst.fl_ranges += r->ndirty;
    r->ndirty = 0;
    GSPGPU_FlushDataCache(r->verts, r->nverts * sizeof(PlatGpuVertex));
    s_dst.fl_bytes += r->nverts * (u32)sizeof(PlatGpuVertex);
    if (r->nidx > 0) {
        GSPGPU_FlushDataCache(r->idx, r->nidx * sizeof(u16));
        s_dst.fl_bytes += r->nidx * 2u;
    }
    /* shader path: vsh is on from a CMD_VSH_DRAW until the next other draw */
    static C3D_AttrInfo vai;
    static C3D_BufInfo vbi;
    int vsh_mode = 0;
    const void* v_stream = NULL;
    u32 v_layout = 0, v_scales = 0xFFFFFFFFu, v_cull = 0;
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_vsh, false);
    if (s_direct) {
        s_gpu_valid = 0;
        s_lay_valid = 0;
        s_c3d_unif = 1;
        s_state_pending = 0;
        s_shadow_stale = 0;
        s_vpend[0] = s_vpend[1] = s_vpend[2] = 0; /* vunif_reset above: the record sets its rows again */
    }
    /* gxdirect: back to citro3d's setters (a CPU-path draw, a clear, a copy, the stereo warp, the present). The state
     * in force goes through them again when kept words or a pending state went past them. */
#define C3D_PATH()                                                          \
    do {                                                                    \
        if (s_direct) {                                                     \
            if (cur_state != NULL && (s_shadow_stale || s_state_pending)) { \
                apply_state(cur_state);                                     \
                s_state_pending = 0;                                        \
                restore = 0;                                                \
                v_cull = 0;                                                 \
            }                                                               \
            s_shadow_stale = 0;                                             \
            s_gpu_valid = 0;                                                \
            s_lay_valid = 0;                                                \
            s_c3d_unif = 1;                                                 \
        }                                                                   \
    } while (0)
#define LEAVE_VSH()                                                \
    do {                                                           \
        C3D_PATH();                                                \
        if (vsh_mode) {                                            \
            vsh_mode = 0;                                          \
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_vsh, false); \
            C3D_SetAttrInfo(&s_attr);                              \
            C3D_SetBufInfo(&bi);                                   \
            C3D_CullFace(GPU_CULL_NONE);                           \
            v_stream = NULL;                                       \
            v_cull = 0;                                            \
        }                                                          \
    } while (0)
    u8* p = r->data;
    u8* end = r->data + r->used;
    /* A clear or a copy draws with its own state (immediate mode, which
     * also replaces the vertex buffer setup): the next draw puts the last
     * GX state and the frame's vertex buffer back. */
    const PlatGpuState* cur_state = NULL;
    int restore = 0;
    u32 gdraw = 0; /* draw calls made or dropped by the gpudrawcut/gpudrawskip filter, in list order */
    u32 n_draw = 0, n_clear = 0, n_copy = 0, n_skip = 0, after_last_draw = 0; /* for the screenshot log */
    /* stereo: the state in force at the 2D tail's start, for the second eye's replay */
    const void *fog_now = NULL, *split_fog = NULL;
    const PlatGpuVshConst *vc_now = NULL, *split_vc = NULL;
    const PlatGpuState* split_state = NULL;
    static PlatGpuState split_rstate; /* s_rstate at the split (deltas change s_rstate after it) */
    const PlatGpuVshLight *vl_now = NULL, *split_vl = NULL;
    u8* tail = NULL;
    int eye = 0;
#define STEREO_SPLIT()                       \
    do {                                     \
        LEAVE_VSH();                         \
        stereo_snapshot();                   \
        stereo_warp(0);                      \
        C3D_SetAttrInfo(&s_attr);            \
        C3D_SetBufInfo(&bi);                 \
        rot_uniforms();                      \
        restore = 1;                         \
        tail = split;                        \
        split_state = cur_state;             \
        if (cur_state != NULL) {             \
            split_rstate = s_rstate;         \
        }                                    \
        split_fog = fog_now;                 \
        split_vc = vc_now;                   \
        split_vl = vl_now;                   \
        split = NULL;                        \
    } while (0)
eye_again:
    while (p < end) {
        if (p == split) {
            STEREO_SPLIT();
        }
        CmdHead* h = (CmdHead*)p;
        u8* body = p + sizeof(CmdHead);
        p = body + h->size;
        switch (h->type) {
        case CMD_STATE_DELTA: {
            if (cur_state == NULL) {
                break; /* never: a delta follows a full state in its record */
            }
            const u32* d = (const u32*)body;
            u32* w = (u32*)&s_rstate;
            for (u32 i = 0; i < d[0]; i++) {
                w[d[1 + 2 * i]] = d[2 + 2 * i];
            }
            if (s_direct) {
                s_state_pending = 1; /* sent at the next draw */
                break;
            }
            apply_state(cur_state); /* cull none */
            restore = 0;
            v_cull = 0;
            break;
        }
        case CMD_STATE:
            s_rstate = *(const PlatGpuState*)body;
            cur_state = &s_rstate;
            if (s_direct) {
                s_state_pending = 1;
                break;
            }
            apply_state(cur_state); /* cull none */
            restore = 0;
            v_cull = 0;
            break;
        case CMD_FOG:
            fog_bind(body);
            fog_now = body;
            break;
        case CMD_DRAW: {
            const u32* d = (const u32*)body;
            u32 dn = d[1] & ~CMD_DRAW_DROP;
            if (cur_state == NULL) {
                n_skip++;
            }
            if (cur_state != NULL && dn >= 3) {
                LEAVE_VSH();
                if (restore || s_state_pending) {
                    apply_state(cur_state);
                    restore = 0;
                    s_state_pending = 0;
                }
                if (s_scissor_empty) {
                    scissor_skip_log();
                    break;
                }
                /* gxonehop=1: the recording thread asked the filter (one count with the shader draws) */
                if (s_onehop == 1 ? !(d[1] & CMD_DRAW_DROP) : plat_gpudump_draw_filter(r->frame, r->seq, gdraw++)) {
                    C3D_DrawArrays(GPU_TRIANGLES, (int)d[0], (int)(dn - dn % 3));
                }
                n_draw++;
                after_last_draw = 0;
            }
            break;
        }
        case CMD_VSH_LIGHT:
            vsh_apply_light((const PlatGpuVshLight*)body);
            vl_now = (const PlatGpuVshLight*)body;
            break;
        case CMD_VSH_CONST:
            vsh_apply_const((const PlatGpuVshConst*)body);
            vc_now = (const PlatGpuVshConst*)body;
            break;
        case CMD_VSH_DRAW:
        case CMD_VSH_DRAWP: {
            /* the two bodies share their first four fields; DRAWP's indices are its own (linear memory) */
            const VshDraw* d = (const VshDraw*)body;
            const u16* d_idx;
            u32 d_count;
            if (cur_state == NULL) {
                n_skip++;
                break;
            }
            if (h->type == CMD_VSH_DRAWP) {
                d_idx = ((const VshDrawP*)body)->idx;
                d_count = ((const VshDrawP*)body)->count;
                if (d_count < 3) {
                    break;
                }
                if (s_ixpool_hi != NULL && ((const u8*)d_idx < s_ixpool_lo || (const u8*)(d_idx + d_count) > s_ixpool_hi)) {
                    static u32 s_logged;
                    if (s_logged++ == 0) {
                        PLOG_E("gpu: drawp indices %p +%lu outside the index pool %p..%p: draw skipped", (const void*)d_idx,
                               (unsigned long)d_count, (const void*)s_ixpool_lo, (const void*)s_ixpool_hi);
                    }
                    break;
                }
            } else {
                if (d->count < 3 || d->first + d->count > r->nidx) {
                    break; /* bounds: never draw past the index arena */
                }
                d_idx = r->idx + d->first;
                d_count = d->count;
            }
            if (s_direct) {
                RPHASE(9);
                if (!vsh_mode) {
                    vsh_mode = 1;
                    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_vsh, true);
                    s_c3d_unif = 1;
                    v_stream = NULL;
                }
                c3d_flush(); /* citro3d's pending words first: the words below must not be overwritten by them */
                if (restore || s_state_pending || s_gpu_valid != 7) {
                    if (direct_state(cur_state)) {
                        v_cull = 0;
                    }
                    restore = 0;
                    s_state_pending = 0;
                }
                if (s_scissor_empty) {
                    scissor_skip_log();
                    RPHASE(3);
                    break;
                }
                if (!s_lay_valid || d->stream != v_stream || d->layout != v_layout) {
                    direct_layout(d->stream, d->layout, &vai, &vbi);
                    v_stream = d->stream;
                    v_layout = d->layout;
                }
                if (d->scales != v_scales) {
                    vsh_apply_scales(d->scales);
                    v_scales = d->scales;
                }
                if (d->cull != v_cull) {
                    direct_cull(vsh_cull(d->cull));
                    v_cull = d->cull;
                }
                direct_unif_flush();
                direct_draw(d_idx, d_count);
                n_draw++;
                after_last_draw = 0;
                RPHASE(3);
                break;
            }
            if (restore) {
                apply_state(cur_state); /* also sets cull none */
                restore = 0;
                v_cull = 0;
            }
            if (s_scissor_empty) {
                scissor_skip_log();
                break;
            }
            if (!vsh_mode) {
                vsh_mode = 1;
                C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_vsh, true);
                v_stream = NULL;
            }
            if (d->stream != v_stream || d->layout != v_layout) {
                vsh_apply_layout(d->stream, d->layout, &vai, &vbi);
                v_stream = d->stream;
                v_layout = d->layout;
            }
            if (d->scales != v_scales) {
                vsh_apply_scales(d->scales);
                v_scales = d->scales;
            }
            if (d->cull != v_cull) {
                C3D_CullFace(vsh_cull(d->cull));
                v_cull = d->cull;
            }
            if (plat_gpudump_draw_filter(r->frame, r->seq, gdraw++)) {
                C3D_DrawElements(GPU_TRIANGLES, (int)d_count, C3D_UNSIGNED_SHORT, d_idx);
            }
            n_draw++;
            after_last_draw = 0;
            break;
        }
        case CMD_OH_WORDS: {
            const OhWords* c = (const OhWords*)body;
            RPHASE(9);
            cmd_put(c->w, c->n);
            if (c->draws) {
                C3DX_FLAGS |= C3DXF_DRAWUSED;
                n_draw += c->draws;
                after_last_draw = 0;
            }
            s_shadow_stale = 1;
            s_orst.chunks++;
            s_orst.words += c->n;
            RPHASE(3);
            break;
        }
        case CMD_OH_FLUSH:
            if (!vsh_mode) {
                vsh_mode = 1;
                C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_uloc_vsh, true);
                s_c3d_unif = 1;
            }
            c3d_flush();
            break;
        case CMD_OH_UNIF:
            oh_apply_unif(((const OhUnif*)body)->lit, ((const OhUnif*)body)->lcount);
            break;
        case CMD_OH_CAPTURE:
            oh_capture((OhCap*)body);
            s_shadow_stale = 1;
            break;
        case CMD_OH_MARK:
            if (s_onehop == 2) {
                s_gpu_valid = 0; /* as the one-hop words drop what they hold here */
                s_lay_valid = 0;
            } else if (stereo && (u8*)h == tail) {
                /* the split: what the tail needs that its own words do not send */
                const OhMark* m = (const OhMark*)body;
                oh_apply_unif(m->lit, m->lcount);
                cmd_put(m->w, m->n);
                s_shadow_stale = 1;
                s_orst.marks++;
            }
            break;
        case CMD_CLEAR: {
            const u32* d = (const u32*)body;
            LEAVE_VSH();
            draw_rect_quad(r, (u16)d[0], (u16)(d[0] >> 16), (u16)d[1], (u16)(d[1] >> 16), d[2], d[3] & 0xFFFFFF,
                           (d[3] >> 24) & 1, (d[3] >> 25) & 1);
            n_clear++;
            after_last_draw |= 1;
            C3D_SetAttrInfo(&s_attr);
            C3D_SetBufInfo(&bi);
            restore = 1;
            break;
        }
        case CMD_COPY: {
            const u32* d = (const u32*)body;
            LEAVE_VSH();
            RPHASE(6);
            copy_to_texture((u16)d[0], (u16)(d[0] >> 16), (u16)d[1], (u16)(d[1] >> 16), d[3], d[4] & 0xFFFF,
                            d[4] >> 16);
            RPHASE(3);
            n_copy++;
            after_last_draw |= 2;
            C3D_SetAttrInfo(&s_attr);
            C3D_SetBufInfo(&bi);
            restore = 1;
            break;
        }
        default: break;
        }
    }
    LEAVE_VSH();
    if (stereo) {
        if (split != NULL) {
            STEREO_SPLIT(); /* the record ends with its 3D part */
        }
        present(stereo_eye_target(eye));
        if (eye == 0) {
            /* the right eye: its warp, then the same 2D tail from the same state */
            eye = 1;
            stereo_warp(1);
            C3D_SetAttrInfo(&s_attr);
            C3D_SetBufInfo(&bi);
            rot_uniforms();
            vunif_reset(); /* the warp set uniforms of its own */
            if (split_state != NULL) {
                s_rstate = split_rstate;
            }
            cur_state = split_state;
            restore = 1;
            if (s_direct) {
                s_state_pending = cur_state != NULL; /* citro3d's setters saw the record's last state, not this one */
                s_gpu_valid = 0;
                s_lay_valid = 0;
                s_c3d_unif = 1;
            }
            v_stream = NULL;
            v_scales = 0xFFFFFFFFu;
            v_cull = 0;
            if (split_fog != NULL) {
                fog_bind(split_fog);
            }
            if (split_vl != NULL) {
                vsh_apply_light(split_vl);
            }
            if (split_vc != NULL) {
                vsh_apply_const(split_vc);
            }
            p = tail;
            goto eye_again;
        }
    } else if (r->present) {
        present(s_screen);
    }
#undef STEREO_SPLIT
#undef LEAVE_VSH
#undef C3D_PATH
    if (s_onehop == 2) {
        if (s_ohv_on) {
            ohv_compare(r);
        }
        s_ohv_on = 0;
    }
    RPHASE(7);
    gpudump_note();
    C3D_FrameEnd(0);
    RPHASE(0);
    if (r->present) {
        direct_log();
    }
    s_stats.render_us = (u32)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000u));
    s_stats.gpu_us = (u32)(C3D_GetDrawingTime() * 1000.0f);
    if (r->present) {
        s_stats.begin_wait_us = (u32)(s_begin_wait_acc / (SYSCLOCK_ARM11 / 1000000u));
        s_stats.records = s_begin_records;
        s_begin_wait_acc = 0;
        s_begin_records = 0;
    }
    if (r->present) {
        stereo_frame_done(stereo, s_stats.render_us, s_stats.gpu_us, tail_cmds, tail_zwrites);
    }
    if (r->present && s_shots_every) {
        take_shot();
        if (s_shot_due) {
            char path[48];
            snprintf(path, sizeof(path), "shots/%05lu.ppm", (unsigned long)s_shot_due);
            s_shot_due = 0;
            PLOG_I("shot %s: record of %lu bytes has %lu draws (%lu without a state), %lu clears, %lu copies; after the last draw:%s%s", path,
                   (unsigned long)r->used, (unsigned long)n_draw, (unsigned long)n_skip, (unsigned long)n_clear, (unsigned long)n_copy,
                   after_last_draw & 1 ? " clear" : "", after_last_draw & 2 ? " copy" : "");
            capture_efb(r, path); /* its GPU copy waits for the frame's commands */
            stereo_shot(path, stereo);
        }
    }
    if (r->present && __atomic_load_n(&s_named_shot, __ATOMIC_ACQUIRE)) {
        capture_efb(r, s_named_path);
        __atomic_store_n(&s_named_shot, 0, __ATOMIC_RELEASE);
    }
}

/* ---- APT suspend, sleep and exit ----
 *
 * The hook runs on the thread that calls aptMainLoop: the game thread, at a
 * frame boundary, so the game thread is never mid-record and never touches
 * citro3d. libctru calls the hooks newest first, before it releases the GPU
 * (suspend, sleep) or after the app asked to leave (exit). citro3d's own hook
 * (C3Di_AptEventHook) on suspend waits for its GX queue to drain and stops
 * the vblank callbacks; on restore it marks all of its cached state dirty and
 * restarts the vblank callbacks. It does not know about this render thread.
 *
 * What the render thread can be doing when the hook runs:
 *  1. waiting for a record (s_submit): nothing to close, nothing pending.
 *  2. a record submitted (s_busy) but not started: it must not start, or
 *     C3D_FrameBegin would queue GPU work after the right is released and the
 *     next C3D_FrameBegin would wait forever for a queue that never finishes.
 *     It parks in render_gate() with the record kept.
 *  3. in render_record before C3D_FrameBegin, or in C3D_FrameBegin's
 *     C3D_FrameSync/queue wait: the vblank callbacks still run (citro3d stops
 *     them only after this hook), so it gets through and renders the record.
 *  4. between C3D_FrameBegin and C3D_FrameEnd: a frame is open, so it must
 *     finish. A frame is never open across records: every record, a
 *     mid-frame segment included, is a whole FrameBegin..FrameEnd.
 *  5. after C3D_FrameEnd: the command list and the screen transfer are queued
 *     and run asynchronously. Waiting for the record does not wait for the
 *     GPU; citro3d's hook that runs right after ours does (gxCmdQueueWait),
 *     and the render thread can queue nothing more in between.
 * So the hook sets the hold first, waits until the render thread is idle or
 * parked (never waits for the game thread), and the render thread's gate keeps
 * it parked until the hold clears. Restore and wake are handled by a second
 * hook registered before C3D_Init, so it runs after citro3d's restore and the
 * render thread never races citro3d's dirty-flag update. */
#define APT_HOLD_SUSPEND 1u
#define APT_HOLD_SLEEP 2u
#define APT_HOLD_EXIT 4u
static aptHookCookie s_apt_first, s_apt_last;
/* Render thread, before a record: 1 = replay it, 0 = drop it (exiting). */
static int render_gate(void) {
    if (!__atomic_load_n(&s_apt_hold, __ATOMIC_ACQUIRE)) {
        return 1;
    }
    __atomic_store_n(&s_apt_parked, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_apt_hold, __ATOMIC_ACQUIRE) && !__atomic_load_n(&s_apt_discard, __ATOMIC_ACQUIRE)) {
        svcSleepThread(5000000LL);
    }
    __atomic_store_n(&s_apt_parked, 0, __ATOMIC_RELEASE);
    return !__atomic_load_n(&s_apt_discard, __ATOMIC_ACQUIRE);
}

/* Runs first (registered after C3D_Init). */
static void apt_hook_first(APT_HookType type, void* param) {
    (void)param;
    u32 bit;
    const char* what;
    switch (type) {
    case APTHOOK_ONSUSPEND: bit = APT_HOLD_SUSPEND; what = "suspend"; break;
    case APTHOOK_ONSLEEP: bit = APT_HOLD_SLEEP; what = "sleep"; break;
    case APTHOOK_ONEXIT: bit = APT_HOLD_EXIT; what = "exit"; break;
    default: return;
    }
    if (__atomic_load_n(&s_apt_off, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (type == APTHOOK_ONEXIT) {
        __atomic_store_n(&s_apt_discard, 1, __ATOMIC_RELEASE); /* a parked record is dropped */
    }
    __atomic_fetch_or(&s_apt_hold, bit, __ATOMIC_ACQ_REL);
    int ok = 1;
    if (s_threaded) {
        /* Idle (not busy) or parked with its record unstarted. Bounded: a wedged render
         * thread must not turn into a hang of the whole console. */
        u64 t0 = svcGetSystemTick();
        while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) && !__atomic_load_n(&s_apt_parked, __ATOMIC_ACQUIRE)) {
            if (svcGetSystemTick() - t0 > 4ull * SYSCLOCK_ARM11) {
                ok = 0;
                break;
            }
            svcSleepThread(200000LL);
        }
    }
    if (ok) {
        PLOG_I("gpu: %s: render idle, frame closed", what);
    } else {
        PLOG_W("gpu: %s: the render thread did not go idle in 4 s, going on", what);
    }
}

/* Runs last (registered before C3D_Init), after citro3d's restore. */
static void apt_hook_last(APT_HookType type, void* param) {
    (void)param;
    u32 bit;
    switch (type) {
    case APTHOOK_ONRESTORE: bit = APT_HOLD_SUSPEND; break;
    case APTHOOK_ONWAKEUP: bit = APT_HOLD_SLEEP; break;
    default: return;
    }
    if (__atomic_load_n(&s_apt_off, __ATOMIC_ACQUIRE)) {
        return;
    }
    u32 left = __atomic_and_fetch(&s_apt_hold, ~bit, __ATOMIC_ACQ_REL);
    if (left == 0) {
        PLOG_I("gpu: resume");
    }
}

/* ---- GPU hang reset ----
 * Game thread: a fence wait or a wait for the render thread timed out, so the GPU is hung (a PICA hang does not
 * end by itself). gpu_reset_try resets the GPU core and unsticks libctru's GX queue (ctrdk gpureset), which
 * lets the render thread's C3D_FrameBegin return, then asks the render thread to rebuild citro3d. At most 3
 * resets within 10 s: after that the waits go on and the watchdog's crash path takes over.
 * Render thread: gpu_reset_apply runs at the start of the next record or right after C3D_FrameBegin
 * returns. It calls citro3d's own APT hook with APTHOOK_ONRESTORE, which marks the whole context dirty so the
 * next frame sends program, shader, buffers, targets, textures and state again (what citro3d does after a
 * suspend, when the GPU lost its state). Not C3D_Fini and C3D_Init: Fini destroys all render targets and Init
 * keeps stale pointers undirtied. The record in hand is replayed whole, so nothing but the frame the GPU hung
 * on is lost. Per-record caches need nothing: the game thread starts every
 * record with a full state and the render thread applies a full state at its first command. */
static int gpu_reset_pending(void) { return __atomic_load_n(&s_reset_req, __ATOMIC_ACQUIRE) != s_reset_seen; }

static int gpu_reset_try(const char* why) {
    if (!s_threaded || __atomic_load_n(&s_apt_hold, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    /* A process that was suspended (Rosalina menu) wakes with its timeouts expired: wait a little and see
     * whether anything moves before touching the GPU. */
    u32 done0 = __atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE), rseq0 = s_rseq, ph0 = s_rphase;
    svcSleepThread(100000000LL);
    if (__atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE) != done0 || s_rseq != rseq0 || s_rphase != ph0) {
        return 0;
    }
    static int given_up; /* once given up, for good: the watchdog's crash path is the way out */
    if (given_up || !gpureset_allow(&s_reset_lim, 3, 10000)) {
        if (!given_up) {
            given_up = 1;
            PLOG_E("gpu: the GPU hung again after %lu resets within 10 s (%s): no more resets, the watchdog takes over",
                   (unsigned long)s_reset_count, why);
        }
        return 0;
    }
    u32 n = ++s_reset_count;
    Result rc = gpureset_core();
    PLOG_W("gpu: GPU hang (%s): reset #%lu done, GSPGPU_ResetGpuCore %08lX; %s", why, (unsigned long)n,
           (unsigned long)rc, plat_gpu_phase_line());
    __atomic_fetch_add(&s_reset_req, 1, __ATOMIC_ACQ_REL);
    return 1;
}

/* Render thread. */
static void gpu_reset_apply(void) {
    u32 req = __atomic_load_n(&s_reset_req, __ATOMIC_ACQUIRE);
    if (req == s_reset_seen) {
        return;
    }
    u32 old = s_rphase;
    RPHASE(8);
    /* No C3D_Fini and C3D_Init: Fini destroys every render target (and frees their VRAM), and Init keeps the
     * old bound program and buffers without marking them dirty. citro3d has its own recovery for a GPU that
     * lost its state, the APT hook's APTHOOK_ONRESTORE branch: it marks the whole context dirty (program,
     * shader code, buffers, targets, textures, texenv, effects, uniforms) so that the next frame sends
     * everything again, and starts the vblank callbacks. It is called here with the same argument. The hook is
     * found in libctru's list (the cookie that is neither of ours). The GX queue is left alone: the next
     * C3D_FrameBegin waits for it and clears it (gpureset_core already ended it if the GPU was hung). */
    aptHookCookie* c3d_hook = NULL;
    for (aptHookCookie* h = s_apt_first.next; h != NULL && c3d_hook == NULL; h = h->next) {
        if (h->callback != apt_hook_first && h->callback != apt_hook_last) {
            c3d_hook = h;
        }
    }
    for (aptHookCookie* h = s_apt_last.next; h != NULL && c3d_hook == NULL; h = h->next) {
        if (h->callback != apt_hook_first && h->callback != apt_hook_last) {
            c3d_hook = h;
        }
    }
    if (c3d_hook == NULL) {
        plat_fatal("gpu: citro3d's APT hook not found after a GPU reset");
    }
    c3d_hook->callback(APTHOOK_ONRESTORE, c3d_hook->param);
    s_begin_records = 0; /* the next record's C3D_FrameBegin takes the vblank wait again */
    s_begin_wait_acc = 0;
    s_reset_seen = req;
    RPHASE(old);
    PLOG_I("gpu: citro3d state marked dirty after GPU reset #%lu", (unsigned long)req);
}

static volatile int s_dead;
static void render_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("render", 64 * 1024);
    for (;;) {
        LightEvent_Wait(&s_submit);
        while (s_dead) {
            svcSleepThread(1000000000LL);
        }
        Record* rr = &s_rec[__atomic_load_n(&s_render_idx, __ATOMIC_ACQUIRE)];
        if (render_gate()) {
            plat_pmu_begin(PLAT_PMU_RENDER);
            render_record(rr);
            plat_pmu_end(PLAT_PMU_RENDER);
        } else {
            gpu_done_publish(rr->seq); /* not replayed (no GPU access now): nothing of it is in flight */
        }
        __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
        LightEvent_Signal(&s_done);
    }
}

/* ---- Pixel peek ---- */

uint32_t plat_gpu_peek_argb(uint16_t x, uint16_t y) {
    plat_gpu_sync();
    float tx, ty, tw, th;
    efb_to_target_rect(x, y, 1, 1, &tx, &ty, &tw, &th);
    u32 px = (u32)tx, py = (u32)TARGET_H - 1 - (u32)ty; /* viewport y counts up from the last memory row */
    const u32* buf = (const u32*)s_efb->frameBuf.colorBuf;
    u32 idx = ((py >> 3) * (TARGET_W >> 3) + (px >> 3)) * 64 + ((px & 1) | ((py & 1) << 1) | ((px & 2) << 1) |
                                                                  ((py & 2) << 2) | ((px & 4) << 2) | ((py & 4) << 3));
    u32 v = buf[idx]; /* RGBA8: R in the top byte */
    return (v & 0xFF) << 24 | v >> 8;
}

/* ---- Screenshots (switch shots=N: every Nth frame to shots/NNNNN.ppm) ---- */

/* Decides at each present whether this frame is captured; the capture
 * itself copies the EFB right after the frame is submitted, behind its commands. */
static void take_shot(void) {
    static u32 frame;
    frame++;
    if (frame < s_shots_from || frame % s_shots_every != 0 || s_shots_left == 0) {
        return;
    }
    s_shots_left--;
    s_shot_due = frame;
}

void plat_gpu_shot(const char* path) {
    if (__atomic_load_n(&s_named_shot, __ATOMIC_ACQUIRE)) {
        return; /* the last one is not taken yet */
    }
    snprintf(s_named_path, sizeof(s_named_path), "%s", path);
    __atomic_store_n(&s_named_shot, 1, __ATOMIC_RELEASE);
}

/* Switch gxdepthshot (debug only): the EFB's depth buffer next to each shot,
 * as <shot>.dep: "DEP1 608 480\n" and one little-endian word per pixel, rows
 * top to bottom (D24S8: the depth is the low 24 bits). Copied to linear
 * memory by the GPU like the colour (a CPU read of VRAM can miss what the
 * GPU drew). */
static void capture_depth(u32* buf, const char* path) {
    char dpath[56];
    size_t n = strlen(path);
    if (n < 4 || n + 1 > sizeof(dpath)) {
        return;
    }
    memcpy(dpath, path, n - 4);
    memcpy(dpath + n - 4, ".dep", 5);
    const u32 bytes = TARGET_W * (u32)TARGET_H * 4;
    const u32 dim = GX_BUFFER_DIM(TARGET_W, (u32)TARGET_H);
    GSPGPU_InvalidateDataCache(buf, bytes);
    C3D_SyncDisplayTransfer((u32*)s_efb->frameBuf.depthBuf, dim, buf, dim,
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(buf, bytes);
    u8* out = malloc(16 + 608 * 480 * 4);
    if (out == NULL) {
        return;
    }
    size_t out_n = (size_t)sprintf((char*)out, "DEP1 608 480\n");
    static u32 row[608];
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 608; x++) {
            float tx, ty, tw, th;
            efb_to_target_rect((float)x, (float)y, 1, 1, &tx, &ty, &tw, &th);
            u32 px = (u32)tx, py = (u32)TARGET_H - 1 - (u32)ty;
            row[x] = buf[py * TARGET_W + px];
        }
        memcpy(out + out_n, row, sizeof(row));
        out_n += sizeof(row);
    }
    plat_sd_write_file_own(dpath, out, out_n, 0);
}

/* The EFB (608x480, as the game drew it) to a PPM file. */
static void capture_efb(const Record* r, const char* path) {
    /* The file is built in memory and queued to the SD service (core 1): this thread makes no SD call. */
    u8* out = malloc(16 + 608 * 480 * 3);
    size_t out_n = 0;
    if (out == NULL) {
        return;
    }
    /* The GPU copies the EFB to linear memory first: a CPU read of VRAM can
     * miss what the GPU drew (Azahar keeps rendered surfaces on the host GPU
     * until a GPU transfer reads them). */
    const u32 bytes = TARGET_W * (u32)TARGET_H * 4;
    static u32* buf;
    if (buf == NULL) {
        buf = (u32*)linearAlloc(bytes);
        if (buf == NULL) {
            free(out);
            PLOG_W("shot %s: no linear memory", path);
            return;
        }
    }
    GSPGPU_InvalidateDataCache(buf, bytes);
    const u32 dim = GX_BUFFER_DIM(TARGET_W, (u32)TARGET_H);
    C3D_SyncDisplayTransfer((u32*)s_efb->frameBuf.colorBuf, dim, buf, dim,
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(buf, bytes);
    out_n = (size_t)sprintf((char*)out, "P6\n608 480\n255\n");
    static u8 row[608 * 3];
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 608; x++) {
            float tx, ty, tw, th;
            efb_to_target_rect((float)x, (float)y, 1, 1, &tx, &ty, &tw, &th);
            u32 px = (u32)tx, py = (u32)TARGET_H - 1 - (u32)ty;
            u32 v = buf[py * TARGET_W + px]; /* untiled by the transfer; RGBA8: R in the top byte */
            row[x * 3 + 0] = (u8)(v >> 24);
            row[x * 3 + 1] = (u8)(v >> 16);
            row[x * 3 + 2] = (u8)(v >> 8);
        }
        memcpy(out + out_n, row, sizeof(row));
        out_n += sizeof(row);
    }
    plat_sd_write_file_own(path, out, out_n, 0);
    if (s_depth_shots) {
        capture_depth(buf, path);
    }
    PLOG_I("shot %s: this record %lu bytes %lu vertices; %lu draws, %lu vertices, %lu states, %lu record bytes, %lu segments", path,
           (unsigned long)r->used, (unsigned long)r->nverts,
           (unsigned long)s_stats.draws, (unsigned long)s_stats.vertices, (unsigned long)s_stats.state_changes,
           (unsigned long)s_stats.record_bytes, (unsigned long)s_stats.segments);
}

/* ---- Self-test (switch gputest): orientation pattern ---- */

static void gpu_selftest(void) {
    PlatGpuState st;
    memset(&st, 0, sizeof(st));
    st.num_stages = 1;
    st.stages[0].src_rgb = 0x0; /* primary color */
    st.stages[0].src_alpha = 0x0;
    st.stages[0].func_rgb = 0;
    st.stages[0].func_alpha = 0;
    st.color_write = 0xF;
    st.depth_func = 1;
    st.scissor_x1 = 608;
    st.scissor_y1 = 480;
    st.viewport_w = 608;
    st.viewport_h = 480;
    st.depth_far = 1;
    for (int frame = 0; frame < 30; frame++) {
        plat_gpu_clear(0, 0, 608, 480, 0xFF404040u, 0xFFFFFF, 1, 1);
        plat_gpu_set_state(&st);
        /* quads: red top-left, green top-right, blue bottom-left (EFB) */
        const struct {
            float x0, y0, x1, y1;
            u32 rgba;
        } q[3] = {{-1, 1, -0.5f, 0.5f, 0xFF0000FFu}, {0.5f, 1, 1, 0.5f, 0xFF00FF00u}, {-1, -0.5f, -0.5f, -1, 0xFFFF0000u}};
        for (int i = 0; i < 3; i++) {
            PlatGpuVertex* v = plat_gpu_alloc_vertices(6);
            float xs[6] = {q[i].x0, q[i].x1, q[i].x1, q[i].x0, q[i].x1, q[i].x0};
            float ys[6] = {q[i].y0, q[i].y0, q[i].y1, q[i].y0, q[i].y1, q[i].y1};
            for (int k = 0; k < 6; k++) {
                memset(&v[k], 0, sizeof(v[k]));
                v[k].pos[0] = xs[k];
                v[k].pos[1] = ys[k];
                v[k].pos[2] = -0.5f;
                v[k].pos[3] = 1;
                memcpy(v[k].color, &q[i].rgba, 4);
            }
            plat_gpu_draw(v, 6);
        }
        plat_gpu_frame_end(608, 480);
    }
    plat_gpu_sync();
    PLOG_I("gputest: done (rot=%d)", s_rot);
}

/* ---- Init ---- */

/* Crash screen: no wait_idle (the render thread may be what crashed); the
 * render thread parks instead of touching citro3d again. */
static void gpu_abort(void) {
    s_apt_off = 1; /* the hooks stay in libctru's list, inert */
    s_dead = 1;
    /* No C3D_Fini and no gfxExit: C3D_Fini waits for the GX queue (C3Di_WaitAndClearQueue(-1)), which never
     * ends on a hung GPU, so the crash screen never came up. The crash screen draws with the CPU into the
     * libctru frame buffers instead. The queue is unbound so that a frame still in flight cannot swap the
     * screens again after the crash screen is up. */
    GX_BindQueue(NULL);
}

static void gpu_shutdown(void) {
    s_apt_discard = 1; /* a record parked by suspend or exit is dropped, not waited for */
    /* A hung GPU (the fence is overdue, or the render thread is still busy after 3 s) never lets C3D_Fini's
     * queue wait end: leave the GPU stack as it is and let the exit go on to the APT relaunch. */
    u64 t0 = svcGetSystemTick();
    while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) && svcGetSystemTick() - t0 < 3ull * SYSCLOCK_ARM11 &&
           !__atomic_load_n(&s_fence_overdue, __ATOMIC_ACQUIRE)) {
        LightEvent_WaitTimeout(&s_done, 50000000LL);
    }
    if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) || __atomic_load_n(&s_fence_overdue, __ATOMIC_ACQUIRE)) {
        PLOG_W("gpu: shutdown with a hung GPU (busy %d, fence overdue %d): no C3D_Fini, no gfxExit", (int)s_busy,
               (int)s_fence_overdue);
        gpu_abort();
        return;
    }
    wait_idle();
    s_apt_off = 1;
    C3D_Fini();
    gfxExit();
}

static void gpu_reset_log(int level, const char* text, void* user) {
    (void)user;
    if (level == 0) {
        PLOG_E("%s", text);
    } else if (level == 1) {
        PLOG_W("%s", text);
    } else {
        PLOG_I("%s", text);
    }
}

void plat_gpu_init(void) {
    LightLock_Init(&s_tex_lock);
    LightEvent_Init(&s_submit, RESET_ONESHOT);
    LightEvent_Init(&s_done, RESET_STICKY);
    /* before C3D_Init: libctru calls the newest hook first, so this one runs
     * after citro3d's on restore and wake (see the APT section) */
    aptHook(&s_apt_last, apt_hook_last, NULL);
    gfxInitDefault();
    gfxSet3D(false);
    /* A GPU hung by an earlier run stays hung across the process: its first command list would hang this one
     * too. Test it with a tiny list and reset it before citro3d starts. */
    gpureset_startup(500, gpu_reset_log, NULL);
    plat_console_init();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8)) {
        plat_fatal("gpu: citro3d init failed");
    }
    aptHook(&s_apt_first, apt_hook_first, NULL); /* after C3D_Init: runs before citro3d's on suspend, sleep, exit */
    plat_set_gpu_shutdown(gpu_shutdown);
    plat_set_gpu_abort(gpu_abort);
    plat_gpudump_init();
    s_rot = plat_switch_int("rot", 0) & 3;
    s_merge = plat_switch_int("gxmerge", 1) != 0;
    s_copy_full = plat_switch_int("copyfull", 1) != 0;
    s_efb608 = plat_switch_int("efb608", 0) != 0;
    s_target_h = s_efb608 ? 608 : 1024;
    s_region_h = s_efb608 ? 608 : 800;
    s_efb_xoff = s_efb608 ? 0 : 96;
    s_efb = C3D_RenderTargetCreate(TARGET_W, TARGET_H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (s_efb == NULL) {
        plat_fatal("gpu: no VRAM for the %dx%d EFB", TARGET_W, TARGET_H);
    }
    C3D_RenderTargetClear(s_efb, C3D_CLEAR_ALL, 0x000000FF, 0);
    if (!C3D_TexInit(&s_blank_tex, 8, 8, GPU_RGBA8)) {
        plat_fatal("gpu: no memory for the blank texture");
    }
    memset(s_blank_tex.data, 0xFF, s_blank_tex.size);
    C3D_TexFlush(&s_blank_tex);
    s_screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB8, -1);
    if (s_screen == NULL) {
        plat_fatal("gpu: no VRAM for the screen target");
    }
    C3D_RenderTargetClear(s_screen, C3D_CLEAR_COLOR, 0, 0);
    C3D_RenderTargetSetOutput(s_screen, GFX_TOP, GFX_LEFT, SCREEN_TRANSFER_FLAGS);
    /* the scratch target of the strip copies (bank A has room after the EFB and the screen) */
    s_scratch_mem = vramAllocAt(SCRATCH_BYTES, VRAM_ALLOC_A);
    if (s_scratch_mem == NULL) {
        s_scratch_mem = vramAlloc(SCRATCH_BYTES);
    }
    PLOG_I("gpu: VRAM after the EFB, the screen and a %d KB copy scratch: %lu KB free, largest block %lu KB", SCRATCH_BYTES / 1024,
           (unsigned long)(vramSpaceFree() / 1024), (unsigned long)vram_largest_kb());
    /* the EFB's color buffer viewed as a texture, for EFB copies */
    memset(&s_efb_tex, 0, sizeof(s_efb_tex));
    s_efb_tex.data = s_efb->frameBuf.colorBuf;
    s_efb_tex.fmt = GPU_RGBA8;
    s_efb_tex.width = TARGET_W;
    s_efb_tex.height = VIEW_H;
    s_efb_tex.size = TARGET_W * VIEW_H * 4;
    s_efb_tex.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                      GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
    StereoInit si = {s_efb, s_screen, &s_attr, 0, 0, SCREEN_TRANSFER_FLAGS, efb_point, TARGET_H, REGION_H};

    s_dvlb = DVLB_ParseFile((u32*)gx_shbin, gx_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    s_uloc_rot0 = shaderInstanceGetUniformLocation(s_prog.vertexShader, "rot0");
    s_uloc_rot1 = shaderInstanceGetUniformLocation(s_prog.vertexShader, "rot1");
    s_uloc_vsh = shaderInstanceGetUniformLocation(s_prog.vertexShader, "vsh");
    s_uloc_clip = shaderInstanceGetUniformLocation(s_prog.vertexShader, "clip");
    s_uloc_texrow = shaderInstanceGetUniformLocation(s_prog.vertexShader, "texrow");
    s_uloc_texsel = shaderInstanceGetUniformLocation(s_prog.vertexShader, "texsel");
    s_uloc_vscale = shaderInstanceGetUniformLocation(s_prog.vertexShader, "vscale");
    s_uloc_clrmix = shaderInstanceGetUniformLocation(s_prog.vertexShader, "clrmix");
    s_uloc_matclr = shaderInstanceGetUniformLocation(s_prog.vertexShader, "matclr");
    s_uloc_tgsrc = shaderInstanceGetUniformLocation(s_prog.vertexShader, "tgsrc");
    if (s_uloc_tgsrc < 0) {
        plat_fatal("gpu: the shader has no tgsrc uniform");
    }
    s_uloc_lit = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lit");
    s_uloc_eyerow = shaderInstanceGetUniformLocation(s_prog.vertexShader, "eyerow");
    s_uloc_nrmrow = shaderInstanceGetUniformLocation(s_prog.vertexShader, "nrmrow");
    s_uloc_amb = shaderInstanceGetUniformLocation(s_prog.vertexShader, "amb");
    s_uloc_dmode = shaderInstanceGetUniformLocation(s_prog.vertexShader, "dmode");
    s_uloc_amode = shaderInstanceGetUniformLocation(s_prog.vertexShader, "amode");
    s_uloc_lpos = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lpos");
    s_uloc_ldir = shaderInstanceGetUniformLocation(s_prog.vertexShader, "ldir");
    s_uloc_lcol = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lcol");
    s_uloc_lka = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lka");
    s_uloc_lkk = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lkk");
    s_uloc_lcount = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lcount");
    if (s_uloc_lit < 0 || s_uloc_eyerow < 0 || s_uloc_nrmrow < 0 || s_uloc_amb < 0 || s_uloc_dmode < 0 ||
        s_uloc_amode < 0 || s_uloc_lpos < 0 || s_uloc_ldir < 0 || s_uloc_lcol < 0 || s_uloc_lka < 0 ||
        s_uloc_lkk < 0 || s_uloc_lcount < 0) {
        PLOG_W("gpu: the shader has no lighting uniforms: lit draws stay on the CPU");
        s_uloc_lit = -1;
    }
    /* C3D_BoolUnifSet takes the location (b0 is 0x68); b15 must stay unused */
    if (s_uloc_vsh < 0x68 || s_uloc_vsh >= 0x68 + 15 || s_uloc_clip < 0 || s_uloc_texrow < 0 || s_uloc_texsel < 0 ||
        s_uloc_vscale < 0 || s_uloc_clrmix < 0 || s_uloc_matclr < 0) {
        plat_fatal("gpu: shader uniforms missing (vsh %d clip %d texrow %d texsel %d)", s_uloc_vsh, s_uloc_clip,
                   s_uloc_texrow, s_uloc_texsel);
    }
    si.uloc_rot0 = s_uloc_rot0;
    si.uloc_rot1 = s_uloc_rot1;
    stereo_init(&si);
    AttrInfo_Init(&s_attr);
    AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&s_attr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&s_attr, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&s_attr, 4, GPU_FLOAT, 2);

    for (int i = 0; i < 2; i++) {
        s_rec[i].data = plat_place_alloc(PLAT_PLACE_GX_RECORD, RECORD_BYTES, 64);
        s_rec[i].verts = linearAlloc(ARENA_VERTICES * sizeof(PlatGpuVertex));
        s_rec[i].idx = plat_place_alloc(PLAT_PLACE_GX_INDEX, INDEX_ARENA * sizeof(u16), 64); /* linear unless place= moves it */
        if (s_rec[i].data == NULL || s_rec[i].verts == NULL || s_rec[i].idx == NULL) {
            plat_fatal("gpu: no memory for frame records");
        }
    }
    memset(&s_last_state, 0xFF, sizeof(s_last_state));
    s_shots_every = (u32)plat_switch_int("shots", 0);
    s_shots_left = (u32)plat_switch_int("shotmax", 200);
    s_depth_shots = plat_switch_has("gxdepthshot");
    s_shots_from = (u32)plat_switch_int("shotfrom", 0); /* first presented frame a shot may be (shots=1: consecutive frames) */
    char dumpcopy[32];
    if (s_shots_every || plat_switch_value("gxdumpcopy", dumpcopy, sizeof(dumpcopy))) { /* its shots go there too */
        plat_sd_mkdir("shots");
    }
    s_threaded = !plat_switch_has("gxsync");
    s_flush_on_render = plat_switch_int("gxflushrender", 1) != 0;
    s_state_delta = plat_switch_int("gxstatedelta", 1) != 0;
    s_urow = plat_switch_int("gxurow", 1) != 0;
    s_sync_all = plat_switch_int("gpusyncall", 0) != 0;
    s_gpu_fence = plat_switch_int("gpufence", 1) != 0;
    s_direct = plat_switch_int("gxdirect", 0);
    s_onehop = plat_switch_int("gxonehop", 0);
    if (s_onehop < 0 || s_onehop > 2) {
        s_onehop = 0;
    }
    if (s_onehop) {
        s_direct = 1; /* gxonehop implies gxdirect=1 (its verify mode is gxonehop=2) */
    }
    if (s_direct < 0 || s_direct > 2 || (s_direct && !direct_init())) {
        s_direct = 0;
    }
    if (s_onehop && (!s_direct || !oh_init())) {
        s_onehop = 0;
    }
    s_vr.direct = s_direct != 0;
    s_vg.direct = 1;
    vunif_reset_u(&s_vg);
    if (s_onehop) {
        oh_record_start(); /* the first record */
    }
    s_hang_recover = plat_switch_int("gpuhangrecover", 0) != 0; /* with gpuhangtest=N: the pretend hang ends at a GPU reset */
    s_hang_times = plat_switch_int("gpuhangtimes", 1);          /* how many records pretend to hang */
    LightEvent_Init(&s_fence_ev, RESET_STICKY);
    PLOG_I("gpu: gpusyncall %d gpufence %d gxflushrender %d gxstatedelta %d gxurow %d gxsync %d", s_sync_all,
           s_gpu_fence, s_flush_on_render, s_state_delta, s_urow, !s_threaded);
    /* Switch gpucore=N: the render thread's core (default 2 on the New 3DS, 1 on the old one). */
    int core = plat_switch_int("gpucore", plat_is_new3ds() ? 2 : 1);
    if (s_threaded) {
        s_thread = threadCreate(render_thread, NULL, 64 * 1024, 0x2E, core, false);
        if (s_thread == NULL) {
            PLOG_W("gpu: no render thread on core %d, rendering on the game thread", core);
            s_threaded = 0;
        }
    }
    PLOG_I("gpu: EFB %dx%d (rotated), render thread %s %d, rot %d, draw merge %d", TARGET_W, TARGET_H,
           s_threaded ? "on core" : "off", core, s_rot, s_merge);
    if (plat_switch_has("gputest")) {
        gpu_selftest();
    }
}
