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
#include "pica_enc.h"
#include "pica_model.h"
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
    int shot;          /* plat_gpu_shot: capture this record's frame to shot_path (set at submit, with present) */
    char shot_path[48];
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
    /* gxchunk=2: the per-draw words beside each batch chunk (ckref), the chunks' places in ow and ckref, and per draw
     * the end of its words in each (ckend: pairs) */
    struct CkReg* ckr;
    u32* ckref;
    u32* ckend;
    u32 nckr, nckref, nckend;
    int ckover; /* a verify buffer overflowed: the record is not compared */
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
extern int plat_gpudump_filter_on;
#define plat_gpudump_draw_filter(f, s, i) (!plat_gpudump_filter_on || plat_gpudump_draw_filter_fn(f, s, i))
int plat_gpudump_draw_filter_fn(u32 frame, u32 seq, u32 idx); /* switches gpudrawcut / gpudrawskip: 0 drops the draw */
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
    u8 dt_busy;     /* texdt: transfer jobs (queued, in flight or being verified) that use this memory: not freed meanwhile */
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
static u32 s_tex_epoch;              /* gxone: bumped when a slot is used again or a texture's data moves */
static u32 s_tex_seen[MAX_TEX / 32]; /* slots that held a texture before (under s_tex_lock) */
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
static VshDrawP* s_last_vdrawp;       /* body of the last command when it is a CMD_VSH_DRAWP from plat_gpu_vsh_drawl */
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
static int s_one; /* switch gxone (GX native step 2; the gxone section after oh_import) */
static int s_chunk; /* switch gxchunk (GX native step 3; the gxchunk section after the gxone section) */
static int s_immfl; /* switch gximmfl (GX native step 4; the gximmfl section after plat_gpu_chunk_draw) */
static int s_ck_tier_linear = 1; /* the frame list is in linear memory (C3D_FrameEnd(0) flushes it) */
static void ck_verify(Record* r);
static void ck_log(u32 frames);
static u32 s_one_cur[3];
static void one_vfy(int c, const u32* key, const PlatGpuState* st, int key_sends);
static int oh_state_one(const PlatGpuState* st);
struct OhCap;
static void one_import(const struct OhCap* b);
static void one_log(u32 frames);
static const u16* s_oh_idx_end; /* index end (arena or linear range) of the last mergeable shader draw (gxonehop=1) */

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
    s_last_vdrawp = NULL;
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

static void shot_attach(Record* r);

static void submit(int present) {
    Record* r = &s_rec[s_cur];
    r->present = present;
    shot_attach(r);
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
        if (s_chunk == 2 || s_immfl == 2) {
            ck_verify(&s_rec[s_cur]); /* before the import: it reads the record's captures */
        }
        oh_import(&s_rec[s_cur]); /* the render thread is done with it: its captured words go into the tables */
    }
    s_rec[s_cur].used = 0;
    s_rec[s_cur].nverts = 0;
    s_rec[s_cur].nidx = 0;
    s_last_draw = NULL;
    s_last_vdraw = NULL;
    s_last_vdrawp = NULL;
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
    if (s_one) {
        memset(s_one_cur, 0, sizeof(s_one_cur)); /* gxone: a state without ids; its classes are encoded from it */
    }
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
    if (idx != r->idx + first) { /* plat_gpu_vsh_idx_reserve'd indices are in the arena already */
        memcpy(r->idx + first, idx, nidx * sizeof(u16));
    }
    r->nidx += nidx;
    s_stats_frame.vsh_calls++;
    s_stats_frame.vsh_indices += nidx;
    if (s_onehop == 1) {
        /* the merge test of the record below, on what the words hold */
        VshDraw* l = s_last_vdraw;
        int merged = s_merge && l != NULL && l->stream == stream && l->layout == layout && l->scales == scales &&
                     l->cull == cull && s_oh_idx_end == r->idx + first;
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

/* The linear index ranges DRAWP indices may come from (gxbatch's pool, the stream index segments of gx_vsh.c). */
#define IXPOOL_MAX 80
static struct {
    const u8 *lo, *hi;
} s_ixpool[IXPOOL_MAX];
static u32 s_ixpool_n;
void plat_gpu_set_index_pool(const void* base, uint32_t bytes) {
    if (base == NULL || bytes == 0) {
        return;
    }
    u32 n = s_ixpool_n;
    if (n == IXPOOL_MAX) {
        PLOG_E("gpu: index pool ranges full (%d)", IXPOOL_MAX);
        return;
    }
    s_ixpool[n].lo = base;
    s_ixpool[n].hi = (const u8*)base + bytes;
    __atomic_store_n(&s_ixpool_n, n + 1, __ATOMIC_RELEASE);
}
/* 1 when [idx, idx + count) lies in one range (or no range is registered: unchecked). */
static int ixpool_ok(const u16* idx, u32 count) {
    u32 n = __atomic_load_n(&s_ixpool_n, __ATOMIC_ACQUIRE);
    if (n == 0) {
        return 1;
    }
    for (u32 i = 0; i < n; i++) {
        if ((const u8*)idx >= s_ixpool[i].lo && (const u8*)(idx + count) <= s_ixpool[i].hi) {
            return 1;
        }
    }
    return 0;
}

uint16_t* plat_gpu_vsh_idx_reserve(uint32_t max) {
    Record* r = &s_rec[s_cur];
    if (max > INDEX_ARENA || r->nidx + max > INDEX_ARENA) {
        return NULL; /* plat_gpu_vsh_draw would flush first: the caller writes elsewhere and it copies */
    }
    return r->idx + r->nidx;
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

void plat_gpu_vsh_drawl(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, const uint16_t* idx,
                        uint32_t nidx) {
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
    s_stats_frame.vsh_calls++;
    s_stats_frame.vsh_indices += nidx;
    if (s_onehop == 1) {
        VshDraw* l = s_last_vdraw;
        int merged = s_merge && l != NULL && l->stream == stream && l->layout == layout && l->scales == scales &&
                     l->cull == cull && s_oh_idx_end == idx;
        if (!merged) {
            s_stats_frame.vsh_draws++;
        }
        oh_draw(stream, layout, scales, cull, idx, nidx, 2, merged);
        return;
    }
    VshDrawP* l = s_last_vdrawp;
    if (s_merge && l != NULL && l->stream == stream && l->layout == layout && l->scales == scales && l->cull == cull &&
        l->idx + l->count == idx) {
        l->count += nidx; /* the next range follows in the index memory: one draw */
        if (s_onehop) {
            oh_draw(stream, layout, scales, cull, idx, nidx, 2, 1);
        }
        return;
    }
    if (s_onehop) {
        oh_draw(stream, layout, scales, cull, idx, nidx, 2, 0);
    }
    VshDrawP* d = rec_put(CMD_VSH_DRAWP, sizeof(VshDrawP));
    if (d != NULL) {
        *d = (VshDrawP){stream, layout, scales, cull, idx, nidx};
        s_last_vdrawp = d;
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
        if (t->pending_free && t->target == NULL && s_frame_counter >= t->free_after_frame &&
            __atomic_load_n(&t->dt_busy, __ATOMIC_ACQUIRE) == 0) {
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof(*t));
        }
    }
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        if (!t->live && !t->pending_free) {
            t->live = 1;
            if (s_tex_seen[i >> 5] & 1u << (i & 31)) {
                plat_tex_epoch_bump(); /* gxone: blocks that named this handle hold the old texture's words */
            }
            s_tex_seen[i >> 5] |= 1u << (i & 31);
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

/* ---- texdt (switch, default 0): texture tiling by the GPU transfer engine (plan step 3b) ----
 * The caller decodes a texture into a linear staging block (a small linear-memory arena, 16 KB granules) in the
 * texture's own format (RGBA8, RGB565, RGBA5551, RGBA4; top row first, no padding). plat_gpu_tex_create_xfer makes
 * the texture and queues a job; the render thread issues it right after C3D_FrameBegin of its next record as a
 * GX_DisplayTransfer (linear to 8x8 Morton tiles, FLIP_VERT so that row 0 lands in the last tile row, as the CPU
 * tiler's t = 0 convention). The GX queue runs entries in order, so the transfer is done before that record's
 * command list, and every record that samples the texture is submitted after its creation. Nothing on the game
 * thread waits for the GPU: a job that finds no free staging block is not made (the caller tiles on the CPU).
 * The staging block and the texture's memory are busy until the record that carried the transfer is done on the
 * GPU (s_gpu_done_seq, the gpufence); a texture freed meanwhile is released only afterwards (TexSlot.dt_busy).
 * texdt=2 verifies: the texture is tiled by the caller on the CPU as before, the same input is also transferred
 * into a second block, and when the transfer is done both are compared byte for byte (mismatches are logged and
 * counted; the picture is the CPU's). A job the record cannot issue (the 32-entry GX queue is nearly full, budget
 * s_gx_entries) is tiled on the render thread by the CPU from the same staging block.
 * Switches: texdt (0/1/2), texdtflip (default 1: FLIP_VERT), texdtarena (staging arena KB, default 512, 256..1024),
 * texdtmin (smallest texture in texels that uses it, default 2048; texdt=2 tests from 64). Needs gpufence=1. */
#define DT_GRAN (16u * 1024u)
#define DT_JOBS 64
typedef struct {
    u8 state; /* 0 free, 1 queued, 2 issued */
    u8 verify;
    u8 fmt;
    u32 handle, w, h, bytes, seq;
    u8 *stage, *vbuf;
} DtJob;
static int s_dt_on, s_dt_inited, s_dt_flip = 1;
static u32 s_dt_min = 2048;
static u8* s_dt_arena;
static u32 s_dt_ngran;
static u64 s_dt_map;      /* bit per granule: in use */
static u8 s_dt_len[64];   /* granules of the block that starts at each index */
static LightLock s_dt_lock;
static DtJob s_dtj[DT_JOBS];
static struct {
    unsigned made, issued, cpu_render, cpu_nojob, vqueued, vok, vbad, vskip, nostage, hw_peak;
    u32 records;
} s_dts;

static int dt_gx_fmt(u32 fmt) {
    switch (fmt) {
    case PLAT_TEXFMT_RGBA8: return GX_TRANSFER_FMT_RGBA8;
    case PLAT_TEXFMT_RGB565: return GX_TRANSFER_FMT_RGB565;
    case PLAT_TEXFMT_RGBA5551: return GX_TRANSFER_FMT_RGB5A1;
    case PLAT_TEXFMT_RGBA4: return GX_TRANSFER_FMT_RGBA4;
    default: return -1;
    }
}

int plat_gpu_xfer_mode(void) {
    if (!s_dt_inited) {
        s_dt_inited = 1;
        int m = plat_switch_int("texdt", 0);
        if (m < 0 || m > 2) {
            m = 0;
        }
        if (m != 0 && plat_switch_int("gpufence", 1) == 0) {
            PLOG_W("texdt: off (needs gpufence=1)");
            m = 0;
        }
        if (m != 0) {
            u32 kb = (u32)plat_switch_int("texdtarena", 512);
            kb = kb < 256 ? 256 : kb > 1024 ? 1024 : kb & ~15u;
            s_dt_arena = linearMemAlign(kb * 1024u, 128);
            if (s_dt_arena == NULL) {
                PLOG_W("texdt: off (no %u KB of linear memory for the staging arena)", (unsigned)kb);
                m = 0;
            } else {
                s_dt_ngran = kb * 1024u / DT_GRAN;
                s_dt_flip = plat_switch_int("texdtflip", 1) != 0;
                s_dt_min = (u32)plat_switch_int("texdtmin", 2048);
                if (m == 2 && s_dt_min > 64) {
                    s_dt_min = 64;
                }
                LightLock_Init(&s_dt_lock);
                PLOG_I("texdt: mode %d, staging arena %u KB, flip %d, from %u texels", m, (unsigned)kb, s_dt_flip,
                       (unsigned)s_dt_min);
            }
        }
        s_dt_on = m;
    }
    return s_dt_on;
}

uint32_t plat_gpu_xfer_min_texels(void) { return s_dt_min; }

/* A linear staging block of `bytes` (at most half the arena), 16 KB aligned, or NULL. `reserve_half`: only if at
 * least half the arena stays free (the core-2 decoder, which holds blocks until the bind). Any thread. */
void* plat_gpu_stage_alloc(uint32_t bytes, int reserve_half) {
    if (!s_dt_on || bytes == 0 || bytes > s_dt_ngran * DT_GRAN / 2) {
        return NULL;
    }
    u32 n = (bytes + DT_GRAN - 1) / DT_GRAN, ng = s_dt_ngran;
    u64 want = (n >= 64 ? ~0ull : ((1ull << n) - 1));
    void* res = NULL;
    LightLock_Lock(&s_dt_lock);
    u32 used = (u32)__builtin_popcountll(s_dt_map);
    if (!reserve_half || used + n <= ng / 2) {
        for (u32 i = 0; i + n <= ng; i++) {
            if ((s_dt_map & (want << i)) == 0) {
                s_dt_map |= want << i;
                s_dt_len[i] = (u8)n;
                res = s_dt_arena + (size_t)i * DT_GRAN;
                if (used + n > s_dts.hw_peak) {
                    s_dts.hw_peak = used + n;
                }
                break;
            }
        }
    }
    if (res == NULL) {
        s_dts.nostage++;
    }
    LightLock_Unlock(&s_dt_lock);
    return res;
}

static void dt_stage_free_locked(void* p) {
    u32 i = (u32)((u8*)p - s_dt_arena) / DT_GRAN, n = s_dt_len[i];
    s_dt_map &= ~((n >= 64 ? ~0ull : ((1ull << n) - 1)) << i);
    s_dt_len[i] = 0;
}

void plat_gpu_stage_free(void* p) {
    if (p == NULL) {
        return;
    }
    LightLock_Lock(&s_dt_lock);
    dt_stage_free_locked(p);
    LightLock_Unlock(&s_dt_lock);
}

/* The writer's cache lines go to memory (call on the thread that wrote the block: the cores' L1 caches are not
 * cleaned by another core's maintenance). */
void plat_gpu_stage_flush(void* p, uint32_t bytes) { GSPGPU_FlushDataCache(p, bytes); }

static u32 dt_morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

/* what the transfer engine makes, by the CPU: linear rows (row 0 first) into 8x8 Morton tiles */
static void dt_cpu_tile(const u8* lin, u32 w, u32 h, u32 bpp, u8* out) {
    u32 tw = w >> 3;
    for (u32 y = 0; y < h; y++) {
        u32 my = s_dt_flip ? h - 1 - y : y;
        const u8* row = lin + (size_t)y * w * bpp;
        for (u32 x = 0; x < w; x++) {
            u32 idx = ((my >> 3) * tw + (x >> 3)) * 64 + dt_morton(x & 7, my & 7);
            if (bpp == 4) {
                ((u32*)out)[idx] = ((const u32*)row)[x];
            } else {
                ((u16*)out)[idx] = ((const u16*)row)[x];
            }
        }
    }
}

static void dt_job_done(DtJob* j) {
    TexSlot* t = &s_tex[j->handle];
    dt_stage_free_locked(j->stage);
    if (j->vbuf != NULL) {
        dt_stage_free_locked(j->vbuf);
    }
    __atomic_sub_fetch(&t->dt_busy, 1, __ATOMIC_RELEASE);
    memset(j, 0, sizeof(*j));
}

/* Locked: a free job slot, or NULL. */
static DtJob* dt_job_new(void) {
    for (u32 i = 0; i < DT_JOBS; i++) {
        if (s_dtj[i].state == 0) {
            return &s_dtj[i];
        }
    }
    return NULL;
}

static void dt_job_fill(DtJob* j, u32 handle, u32 w, u32 h, u32 fmt, u32 bytes, u8* stage, u8* vbuf) {
    j->state = 1;
    j->verify = vbuf != NULL;
    j->fmt = (u8)fmt;
    j->handle = handle;
    j->w = w;
    j->h = h;
    j->bytes = bytes;
    j->stage = stage;
    j->vbuf = vbuf;
    __atomic_add_fetch(&s_tex[handle].dt_busy, 1, __ATOMIC_RELEASE);
}

/* The texture `stage` is the linear form of. Takes the block over (freed on failure too). 0: not made, the caller
 * tiles on the CPU. */
uint32_t plat_gpu_tex_create_xfer(uint32_t width, uint32_t height, uint32_t fmt, void* stage, uint32_t bytes) {
    GMARK("tex_create_xfer");
    if (!s_dt_on || dt_gx_fmt(fmt) < 0 || width < 8 || height < 8 || width > 1024 || height > 1024) {
        plat_gpu_stage_free(stage);
        return 0;
    }
    u32 h = tex_alloc_slot();
    if (h == 0) {
        plat_gpu_stage_free(stage);
        return 0;
    }
    TexSlot* t = &s_tex[h];
    if (!C3D_TexInit(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt) || t->tex.size != bytes) {
        if (t->tex.data != NULL) {
            C3D_TexDelete(&t->tex);
        }
        memset(t, 0, sizeof(*t));
        plat_gpu_stage_free(stage);
        return 0;
    }
    GSPGPU_InvalidateDataCache(t->tex.data, t->tex.size); /* no stale line of the memory's earlier use is written back over the transfer */
    LightLock_Lock(&s_dt_lock);
    DtJob* j = dt_job_new();
    if (j != NULL) {
        dt_job_fill(j, h, width, height, fmt, bytes, stage, NULL);
        s_dts.made++;
    }
    LightLock_Unlock(&s_dt_lock);
    if (j == NULL) {
        /* no job slot: tile here from the block (CPU, no wait) */
        dt_cpu_tile(stage, width, height, bytes / (width * height), t->tex.data);
        C3D_TexFlush(&t->tex);
        plat_gpu_stage_free(stage);
        s_dts.cpu_nojob++;
    }
    return h;
}

/* texdt=2: `handle` was tiled by the CPU (create_begin/end); `stage` is the same texels linear. The transfer into a
 * second block is compared with the texture's memory when it is done. Takes the block over. */
void plat_gpu_tex_xfer_verify(uint32_t handle, void* stage, uint32_t bytes) {
    TexSlot* t = &s_tex[handle];
    u8* vbuf = plat_gpu_stage_alloc(bytes, 0);
    LightLock_Lock(&s_dt_lock);
    DtJob* j = vbuf != NULL ? dt_job_new() : NULL;
    if (j != NULL) {
        GSPGPU_InvalidateDataCache(vbuf, bytes);
        dt_job_fill(j, handle, t->tex.width, t->tex.height, (u32)t->tex.fmt, bytes, stage, vbuf);
        s_dts.vqueued++;
    } else {
        s_dts.vskip++;
        dt_stage_free_locked(stage);
        if (vbuf != NULL) {
            dt_stage_free_locked(vbuf);
        }
    }
    LightLock_Unlock(&s_dt_lock);
}

/* Render thread, after C3D_FrameBegin of record `seq`: the queued jobs become transfers in this frame's GX queue. */
static void dt_issue(u32 seq) {
    if (!s_dt_on) {
        return;
    }
    s_dts.records++;
    LightLock_Lock(&s_dt_lock);
    for (u32 i = 0; i < DT_JOBS; i++) {
        DtJob* j = &s_dtj[i];
        if (j->state != 1) {
            continue;
        }
        TexSlot* t = &s_tex[j->handle];
        if (s_gx_entries + 1 > 24) {
            if (!j->verify) {
                dt_cpu_tile(j->stage, j->w, j->h, j->bytes / (j->w * j->h), t->tex.data);
                GSPGPU_FlushDataCache(t->tex.data, j->bytes);
                s_dts.cpu_render++;
            } else {
                s_dts.vskip++;
            }
            dt_job_done(j);
            continue;
        }
        u32* dst = (u32*)(j->verify ? j->vbuf : (u8*)t->tex.data);
        int gf = dt_gx_fmt(j->fmt);
        GX_DisplayTransfer((u32*)j->stage, GX_BUFFER_DIM(j->w, j->h), dst, GX_BUFFER_DIM(j->w, j->h),
                           GX_TRANSFER_FLIP_VERT(s_dt_flip) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                               GX_TRANSFER_IN_FORMAT(gf) | GX_TRANSFER_OUT_FORMAT(gf) |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
        s_gx_entries++;
        j->seq = seq;
        j->state = 2;
        s_dts.issued++;
    }
    LightLock_Unlock(&s_dt_lock);
}

/* The jobs whose record is done on the GPU: verify compares, the blocks and the texture are released. */
static void dt_reap(void) {
    if (!s_dt_on) {
        return;
    }
    u32 done = __atomic_load_n(&s_gpu_done_seq, __ATOMIC_ACQUIRE);
    LightLock_Lock(&s_dt_lock);
    for (u32 i = 0; i < DT_JOBS; i++) {
        DtJob* j = &s_dtj[i];
        if (j->state != 2 || (s32)(done - j->seq) < 0) {
            continue;
        }
        if (j->verify) {
            TexSlot* t = &s_tex[j->handle];
            GSPGPU_InvalidateDataCache(j->vbuf, j->bytes);
            if (memcmp(j->vbuf, t->tex.data, j->bytes) == 0) {
                s_dts.vok++;
            } else {
                u32 bad = 0, first = 0;
                for (u32 k = 0; k < j->bytes; k++) {
                    if (j->vbuf[k] != ((u8*)t->tex.data)[k] && bad++ == 0) {
                        first = k;
                    }
                }
                if (s_dts.vbad++ < 24) {
                    PLOG_W("texdt: verify MISMATCH %ux%u fmt %u flip %d: %u of %u bytes differ, first at %u (engine %02X, cpu %02X)",
                           (unsigned)j->w, (unsigned)j->h, (unsigned)j->fmt, s_dt_flip, (unsigned)bad, (unsigned)j->bytes,
                           (unsigned)first, (unsigned)j->vbuf[first], (unsigned)((u8*)t->tex.data)[first]);
                }
            }
        }
        dt_job_done(j);
    }
    LightLock_Unlock(&s_dt_lock);
    if (s_dts.records != 0 && s_dts.records % 600 == 0) {
        PLOG_I("texdt: made %u, issued %u, cpu on render thread %u (GX queue full), cpu no job slot %u, no staging block %u, "
               "arena peak %u of %u granules; verify queued %u ok %u BAD %u skipped %u",
               s_dts.made, s_dts.issued, s_dts.cpu_render, s_dts.cpu_nojob, s_dts.nostage, s_dts.hw_peak, (unsigned)s_dt_ngran,
               s_dts.vqueued, s_dts.vok, s_dts.vbad, s_dts.vskip);
    }
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
    dt_reap(); /* texdt: the transfers of the records that sync waited for are done */
    LightLock_Lock(&s_tex_lock);
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        /* copy targets are released by the render thread (reap_targets) */
        if (t->pending_free && t->target == NULL && __atomic_load_n(&t->dt_busy, __ATOMIC_ACQUIRE) == 0) {
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
static int s_named_shot; /* plat_gpu_shot: the next record submitted with a present takes s_named_path */
static int (*s_shot_hook)(const char* path);

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
    u32 nf_skip, nf_skip_bytes; /* gxnoflush: ranges the heap flush covers (skipped with =1) */
    u32 fl_calls, fl_bytes, fl_ranges; /* GSPGPU_FlushDataCache calls, bytes, and stream ranges among them (render thread) */
} s_dst;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
static int s_noflush; /* gxnoflush: 0 off, 1 skip the flushes C3D_FrameEnd(0) covers, 2 flush all and count the covered */
/* 1: the range needs its explicit flush (outside the linear heap, which C3D_FrameEnd(0) flushes whole). */
static int nf_flush(const void* p, u32 n) {
    uintptr_t a = (uintptr_t)p;
    if (a < __ctru_linear_heap || a + n > (uintptr_t)__ctru_linear_heap + __ctru_linear_heap_size) {
        return 1;
    }
    s_dst.nf_skip++;
    s_dst.nf_skip_bytes += n;
    return s_noflush != 1;
}
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
static int dtab_alloc(void);
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
    s_dlog_every = (u32)plat_switch_int("gxthreadstats", plat_switch_int("gxstats", 0));
    if (s_onehop == 1) {
        /* gxonehop=1 draws nothing through the replay's tables (the shader path's words come from s_otab): they are
         * allocated only if gxonehop turns out off (dtab_alloc) */
        PLOG_I("gxdirect=%d: draw packet %lu words, kept words 0 KB (gxonehop=1 uses its own tables)", s_direct,
               (unsigned long)s_tpl_n);
        return 1;
    }
    return dtab_alloc();
}

/* The replay's tables of kept words (gxdirect: direct_block; gxonehop=2: the compare). 1 on success. */
static int dtab_alloc(void) {
    if (s_dtab[0].mem != NULL) {
        return 1;
    }
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
    PLOG_I("gxdirect frame %lu: GSP data-cache flushes per frame %lu calls (%lu stream ranges), %lu bytes, gxnoflush %d: %lu ranges (%lu bytes) covered by the heap flush",
           (unsigned long)s_frame_counter, (unsigned long)(s_dst.fl_calls / n), (unsigned long)(s_dst.fl_ranges / n),
           (unsigned long)(s_dst.fl_bytes / n), s_noflush, (unsigned long)(s_dst.nf_skip / n),
           (unsigned long)(s_dst.nf_skip_bytes / n));
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
    u32 tail; /* gxchunk: the list end after the n words (pica_enc_list_end): the run is a GPU command list */
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
    u32 encn;   /* gxone=2: the encoder's words for this key, compared with res when the record comes back */
    u32 enc[48];
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
    u32 held[3], held_ep; /* gxone: the class block ids the GPU holds (valid with gpu_valid), the texture epoch */
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
            u32 n = r->now - s_om.chunk0, tail = 0;
            if (s_chunk) {
                /* the run becomes a command list of its own: citro3d's split end after it (the words stay as they
                 * are; the render thread copies only the n words when it cannot submit the list) */
                tail = pica_enc_list_end(r->ow + r->now, n, s_om.chunk_draws != 0);
                r->now += tail;
            }
            *c = (OhWords){r->ow + s_om.chunk0, n, s_om.chunk_draws, tail};
            s_ost.chunks++;
        }
    }
    if (s_chunk) {
        r->now = (r->now + 3u) & ~3u; /* a list starts on a 16-byte boundary (the gap is never executed) */
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
    r->nckr = r->nckref = r->nckend = 0;
    r->ckover = 0;
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
static u32* oh_rows_u(u32* w, const u32* mask, const VUnif* u);
static inline u32* oh_rows(u32* w, const u32* mask) { return oh_rows_u(w, mask, &s_vg); }
static u32* oh_rows_u(u32* w, const u32* mask, const VUnif* u) {
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
        w = enc_add(w, GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_DATA), &u->val[i][0], (j - i) * 4);
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
    if (s_chunk) {
        r->now = (r->now + 3u) & ~3u; /* the next list starts on a 16-byte boundary */
    }
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

/* scissor_rect without its log variables (any thread); `out` (x0 y0 x1 y1) may be NULL */
static int scissor_rect_n(const PlatGpuState* st, int* out) {
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
    if (out != NULL) {
        out[0] = x0;
        out[1] = y0;
        out[2] = x1;
        out[3] = y1;
    }
    return x1 <= x0 || y1 <= y0;
}

static int oh_cpu_draw_keep(void) {
    if (scissor_rect_n(&s_last_state, NULL)) {
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
static OhCap* s_oh_lastcap; /* the capture the last oh_block made (NULL: none; gxone=2 hands it the encoder's words) */
static void oh_block(int c, const u32* key) {
    s_oh_lastcap = NULL;
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
    b->encn = 0;
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
        s_oh_lastcap = b;
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
        if (s_one) {
            /* gxone: the lookup mode as tex_prepare sets it (the copy's mode bits are otherwise whatever the render
             * thread's setters left on the object) */
            t->param = (t->param & ~GPU_TEXTURE_MODE(7)) |
                       GPU_TEXTURE_MODE(u == 0 && tu->proj ? GPU_TEX_PROJECTION : GPU_TEX_2D);
        }
    }
}

/* direct_state on the recording side (gxone=2: every class is also checked against the id path, one_vfy) */
static int oh_state(const PlatGpuState* st) {
    u32 k[DK_MAX];
    int pix = 0;
    memcpy(k, st, 26 * 4);
    k[26] = st->fog_enable;
    k[27] = st->fog_enable ? st->fog_color : 0;
    int send = !(s_om.gpu_valid & 1) || memcmp(k, s_om.dkey[DC_TEV], DK_TEV * 4) != 0;
    if (send) {
        oh_block(DC_TEV, k);
        memcpy(s_om.dkey[DC_TEV], k, DK_TEV * 4);
    }
    if (s_one == 2) {
        one_vfy(DC_TEV, k, st, send);
    }
    C3D_Tex t[3];
    oh_tex_prepare(st, t);
    memcpy(k, t, sizeof(t));
    send = !(s_om.gpu_valid & 2) || memcmp(k, s_om.dkey[DC_TEX], DK_TEX * 4) != 0;
    if (send) {
        s_oh_dbg_st = st;
        oh_block(DC_TEX, k);
        s_oh_dbg_st = NULL;
        memcpy(s_om.dkey[DC_TEX], k, DK_TEX * 4);
    }
    if (s_one == 2) {
        one_vfy(DC_TEX, k, st, send);
    }
    memcpy(k, &st->alpha_test_enable, 16);
    memcpy(k + 4, &st->scissor_x0, 32);
    send = !(s_om.gpu_valid & 4) || memcmp(k, s_om.dkey[DC_PIX], DK_PIX * 4) != 0;
    if (send) {
        oh_block(DC_PIX, k);
        memcpy(s_om.dkey[DC_PIX], k, DK_PIX * 4);
        pix = 1;
        s_om.scissor_empty = scissor_rect_n(st, NULL);
    }
    if (s_one == 2) {
        one_vfy(DC_PIX, k, st, send);
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
    if (drawp && !ixpool_ok(idx, count)) {
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
        if (s_one == 1 ? oh_state_one(&s_last_state) : oh_state(&s_last_state)) {
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
        if (drawp == 1) {
            s_last_vdraw = NULL;
        } else {
            s_oh_md = (VshDraw){stream, layout, scales, cull, 0, 0};
            s_last_vdraw = &s_oh_md;
            s_oh_idx_end = idx + count;
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
        if (b->encn != 0) {
            one_import(b);
        }
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

/* ---- switch gxone: class blocks by id (GX native step 2, docs/PORT_3DS_GX_NATIVE.md) ----
 * The front end interns each state class (TEV with the fog mode and colour, the texture units, the pixel pipeline
 * with viewport and scissor) at its draw boundary and gives the ids with the state (plat_gpu_set_state_one). An id
 * owns a block of PICA words that the port's own encoders (pica_enc.c) build once from the class's state region,
 * kept in linear memory, 16-byte aligned, handed out in order. Below the front end only ids are compared: the region
 * copy on an id change replaces plat_gpu_set_state_dirty's word compare, and at a draw a class goes out when its id
 * differs from the one the GPU holds, which replaces oh_state's key compares, oh_tex_prepare and the DTab hash.
 * gxone=1 copies the blocks into the record's words (COPY mode; gxonehop=1 is implied). gxone=2 writes the record as
 * gxonehop=1 does (keys, citro3d captures) and checks the id path beside it: every block the key path sends is
 * compared word for word with the id's block (with the kept words, or with the capture when the record comes back),
 * the id path's send decision with the key path's, and the regions of ids that did not change with the state given.
 * Texture blocks hold the objects' address, size and sampler words: a texture slot used again or texture data moved
 * (vramtex) bumps s_tex_epoch, and a block built in an older epoch is encoded again before it goes out. */
#define ONE_CLS 3
_Static_assert((int)DC_TEV == (int)PLAT_GPU_C_TEV && (int)DC_TEX == (int)PLAT_GPU_C_TEX &&
                   (int)DC_PIX == (int)PLAT_GPU_C_PIX,
               "class order");
_Static_assert(PICA_TEV_WORDS <= 44 && PICA_TEX_WORDS <= 32 && PICA_PIX_WORDS_MAX <= 48, "block strides");
static PicaEncFixed s_one_fx;
typedef struct {
    u32* mem;  /* cap blocks of stride words */
    u32* meta; /* per block: words | scissor empty << 8 */
    u32* ep;   /* per block: the texture epoch its words were built in (TEX) */
    u32 stride, cap, used, gen;
    int linear;
} OneStore;
static OneStore s_one_st[ONE_CLS];
static u32 s_one_cur[ONE_CLS]; /* the ids of the state given last (0: none, the class is encoded from the state) */
static struct {
    u32 built[ONE_CLS], sent[ONE_CLS], direct[ONE_CLS], reenc, resets[ONE_CLS], changes;
    u32 checks, bad, missed, stale, unver, logged;
} s_onec;
static const char* const k_one_name[ONE_CLS] = {"tev", "tex", "pix"};

void plat_tex_epoch_bump(void) { __atomic_add_fetch(&s_tex_epoch, 1, __ATOMIC_RELEASE); }
static inline u32 tex_epoch(void) { return __atomic_load_n(&s_tex_epoch, __ATOMIC_ACQUIRE); }

/* apply_tev's setters (det) as register values */
static u32 one_enc_tev(const PlatGpuState* st, u32* w) {
    PicaTevStage s[6];
    for (int i = 0; i < 6; i++) {
        u32* v = s[i].w;
        if (i < st->num_stages) {
            const PlatGpuTevStage* g = &st->stages[i];
            v[0] = (u32)g->src_rgb | (u32)g->src_alpha << 16;
            v[1] = ((u32)g->op_rgb & 0xFFFu) | ((u32)g->op_alpha & 0xFFFu) << 12;
            v[2] = (u32)g->func_rgb | (u32)g->func_alpha << 16;
            v[3] = g->constant;
            v[4] = (u32)g->scale_rgb | (u32)g->scale_alpha << 16;
        } else { /* C3D_TexEnvInit */
            v[0] = 0x000F000Fu;
            v[1] = 0;
            v[2] = 0;
            v[3] = 0xFFFFFFFFu;
            v[4] = 0;
        }
    }
    u32 bits = (st->fog_enable ? (u32)GPU_FOG : (u32)GPU_NO_FOG) | (st->buffer_input_rgb & 0xFu) << 8 |
               (st->buffer_input_alpha & 0xFu) << 12;
    return pica_enc_tev(w, s, bits, st->buffer_color, st->fog_enable ? st->fog_color : 0, &s_one_fx);
}

/* tex_prepare + tex_bind3 as register values (the objects as oh_tex_prepare copies them) */
static u32 one_enc_tex(const PlatGpuState* st, u32* w) {
    C3D_Tex t[3];
    oh_tex_prepare(st, t);
    PicaTexUnit u[3];
    for (int i = 0; i < 3; i++) {
        u[i] = (PicaTexUnit){t[i].border, t[i].dim, t[i].param, t[i].lodParam, osConvertVirtToPhys(t[i].data) >> 3,
                             (u32)t[i].fmt};
    }
    return pica_enc_tex(w, u, &s_one_fx);
}

/* apply_pix's setters (det) as register values */
static u32 one_enc_pix(const PlatGpuState* st, u32* w, int* sc_empty) {
    PicaPixIn in;
    memset(&in, 0, sizeof(in));
    in.alpha_test = (st->alpha_test_enable ? 1u : 0u) | ((u32)st->alpha_test_func & 7u) << 4 | (u32)st->alpha_test_ref << 8;
    if (st->blend_mode == 1) {
        in.alpha_blend = (u32)st->blend_eq_rgb | (u32)st->blend_eq_alpha << 8 | (u32)st->blend_src_rgb << 16 |
                         (u32)st->blend_dst_rgb << 20 | (u32)st->blend_src_alpha << 24 | (u32)st->blend_dst_alpha << 28;
    } else {
        in.alpha_blend = 0x01010000u; /* ADD, ADD, ONE, ZERO, ONE, ZERO */
    }
    in.logic_op = st->blend_mode == 2 ? st->logic_op : (u32)GPU_LOGICOP_COPY;
    in.blend_on = st->blend_mode != 2;
    u32 mask = (st->color_write & 0xFu) | (st->depth_write ? (u32)GPU_WRITE_DEPTH : 0);
    u32 func = st->depth_test ? st->depth_func : (u32)GPU_ALWAYS;
    in.depth_test = (st->depth_test || st->depth_write ? 1u : 0u) | (func & 7u) << 4 | mask << 8;
    in.zscale = st->depth_far - st->depth_near;
    in.zoffset = st->depth_far;
    float tx, ty, tw, th;
    efb_to_target_rect(st->viewport_x, st->viewport_y, st->viewport_w, st->viewport_h, &tx, &ty, &tw, &th);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tw >= 1 && th >= 1) {
        in.viewport = 1;
        in.vx = (u32)tx;
        in.vy = (u32)ty;
        in.vw = (u32)tw;
        in.vh = (u32)th;
    }
    int sc[4];
    *sc_empty = scissor_rect_n(st, sc);
    if (!*sc_empty) {
        in.sc_mode = GPU_SCISSOR_NORMAL;
        in.sc_x0 = (u32)sc[0];
        in.sc_y0 = (u32)sc[1];
        in.sc_x1 = (u32)sc[2];
        in.sc_y1 = (u32)sc[3];
    } else { /* mode off, corners 0 (apply_pix's det path) */
        in.sc_mode = GPU_SCISSOR_DISABLE;
        in.sc_x1 = in.sc_y1 = 1;
    }
    return pica_enc_pix(w, &in, &s_one_fx);
}

static u32 one_enc(int c, const PlatGpuState* st, u32* w, int* sc_empty) {
    *sc_empty = 0;
    if (c == DC_TEV) {
        return one_enc_tev(st, w);
    }
    if (c == DC_TEX) {
        return one_enc_tex(st, w);
    }
    return one_enc_pix(st, w, sc_empty);
}

int plat_gpu_one_active(void) { return s_one; }

uint32_t plat_gpu_one_capacity(int cls) { return s_one && (u32)cls < ONE_CLS ? s_one_st[cls].cap : 0; }

uint32_t plat_gpu_one_block(int cls, const PlatGpuState* st) {
    if (!s_one || (u32)cls >= ONE_CLS) {
        return 0;
    }
    OneStore* s = &s_one_st[cls];
    if (s->used >= s->cap) {
        return 0;
    }
    u32 i = s->used++;
    s->ep[i] = tex_epoch(); /* before the encoding reads the texture objects */
    int sce;
    u32 n = one_enc(cls, st, s->mem + (size_t)i * s->stride, &sce);
    s->meta[i] = n | (u32)sce << 8;
    s_onec.built[cls]++;
    return s->gen << 16 | (i + 1);
}

void plat_gpu_one_reset(int cls) {
    if (!s_one || (u32)cls >= ONE_CLS) {
        return;
    }
    OneStore* s = &s_one_st[cls];
    s->used = 0;
    s->gen = (s->gen + 1) & 0xFFFFu; /* no id of the old blocks equals a new one */
    s_one_cur[cls] = 0;
    s_om.held[cls] = 0;
    s_onec.resets[cls]++;
}

/* The words of class c for id (the state's region is the id's): the block's, encoded again first when it is a
 * texture block of an older epoch; without a valid id, encoded from the state. */
static u32 one_put(int c, u32 id, const PlatGpuState* st, u32* dst, u32 ep, int* sc_empty) {
    OneStore* s = &s_one_st[c];
    u32 i = (id & 0xFFFFu) - 1;
    s_onec.sent[c]++;
    if (id == 0 || id >> 16 != s->gen || i >= s->used) {
        s_onec.direct[c]++;
        return one_enc(c, st, dst, sc_empty);
    }
    u32* b = s->mem + (size_t)i * s->stride;
    if (c == DC_TEX && s->ep[i] != ep) {
        int x;
        s->ep[i] = ep;
        s->meta[i] = one_enc(c, st, b, &x) | (u32)x << 8;
        s_onec.reenc++;
    }
    u32 n = s->meta[i] & 0xFFu;
    *sc_empty = (int)(s->meta[i] >> 8 & 1u);
    memcpy(dst, b, n * 4);
    return n;
}

/* oh_state with ids: a class goes out when the GPU does not hold its id (or its texture epoch) */
static int oh_state_one(const PlatGpuState* st) {
    int pix = 0;
    u32 ep = tex_epoch();
    for (int c = 0; c < ONE_CLS; c++) {
        u32 id = s_one_cur[c];
        if ((s_om.gpu_valid >> c & 1) && id != 0 && s_om.held[c] == id && (c != DC_TEX || s_om.held_ep == ep)) {
            continue;
        }
        Record* r = oh_rec();
        int sce;
        r->now += one_put(c, id, st, r->ow + r->now, ep, &sce);
        s_om.held[c] = id;
        if (c == DC_TEX) {
            s_om.held_ep = ep;
        }
        if (c == DC_PIX) {
            pix = 1;
            s_om.scissor_empty = sce;
        }
    }
    s_om.gpu_valid = 7;
    return pix;
}

static void one_bad_log(int c, const char* what, const u32* ref, u32 rn, const u32* w, u32 n) {
    if (s_onec.logged >= 16) {
        return;
    }
    s_onec.logged++;
    u32 k = 0;
    while (k < rn && k < n && ref[k] == w[k]) {
        k++;
    }
    PLOG_W("gxone verify: frame %lu, %s block differs from %s (%lu words, encoder %lu) at word %lu: %08lx, encoder %08lx",
           (unsigned long)s_frame_counter, k_one_name[c], what, (unsigned long)rn, (unsigned long)n, (unsigned long)k,
           (unsigned long)(k < rn ? ref[k] : 0), (unsigned long)(k < n ? w[k] : 0));
}

/* gxone=2, in oh_state after class c: the id path's decision and words against the key path's */
static void one_vfy(int c, const u32* key, const PlatGpuState* st, int key_sends) {
    u32 id = s_one_cur[c];
    u32 ep = tex_epoch();
    int id_sends = !(s_om.gpu_valid >> c & 1) || id == 0 || s_om.held[c] != id || (c == DC_TEX && s_om.held_ep != ep);
    if (id_sends) {
        s_om.held[c] = id;
        if (c == DC_TEX) {
            s_om.held_ep = ep;
        }
    }
    if (key_sends && !id_sends) {
        s_onec.missed++;
        if (s_onec.logged < 16) {
            s_onec.logged++;
            PLOG_W("gxone verify: frame %lu, the %s key changed but its id %08lx did not", (unsigned long)s_frame_counter,
                   k_one_name[c], (unsigned long)id);
        }
    }
    if (!key_sends) {
        return;
    }
    u32 w[64];
    int sce;
    u32 n = one_put(c, id, st, w, ep, &sce);
    if (c == DC_PIX && sce != s_om.scissor_empty) {
        s_onec.bad++;
        if (s_onec.logged < 16) {
            s_onec.logged++;
            PLOG_W("gxone verify: frame %lu, pix block's empty-scissor flag %d, key path %d", (unsigned long)s_frame_counter,
                   sce, s_om.scissor_empty);
        }
    }
    DTab* t = &s_otab[c];
    u32* e = dtab_find(t, key, dkey_hash(key, t->keyw));
    if (e != NULL && e[1] != OH_PENDING && e[1] != 0) {
        s_onec.checks++;
        if (e[1] != n || memcmp(e + 3 + t->keyw, w, n * 4) != 0) {
            s_onec.bad++;
            one_bad_log(c, "the kept capture", e + 3 + t->keyw, e[1], w, n);
        }
    } else if (s_oh_lastcap != NULL && n <= sizeof(s_oh_lastcap->enc) / 4) {
        s_oh_lastcap->encn = n; /* compared with the capture when the record comes back (one_import) */
        memcpy(s_oh_lastcap->enc, w, n * 4);
    } else {
        s_onec.unver++;
    }
}

static void one_import(const OhCap* b) {
    if (b->n == 0) {
        s_onec.unver++;
        return;
    }
    s_onec.checks++;
    if (b->n != b->encn || memcmp(b->res, b->enc, b->n * 4) != 0) {
        s_onec.bad++;
        one_bad_log((int)b->cls, "citro3d's capture", b->res, b->n, b->enc, b->encn);
    }
}

/* the bytes of PlatGpuState each class's id stands for: TEV with the fog enable and colour, TEX, PIX with the
 * scissor, viewport and depth range (the fog LUT and its id travel apart, as with gxdirty) */
#define ONE_OFF_TEX offsetof(PlatGpuState, tex)
#define ONE_OFF_PIX offsetof(PlatGpuState, alpha_test_enable)
#define ONE_OFF_FOG offsetof(PlatGpuState, fog_enable)
#define ONE_OFF_LUT offsetof(PlatGpuState, fog_lut_id)
#define ONE_OFF_SCI offsetof(PlatGpuState, scissor_x0)
static const u16 k_one_rg[ONE_CLS][2][2] = {
    {{0, ONE_OFF_TEX}, {ONE_OFF_FOG, ONE_OFF_LUT}},
    {{ONE_OFF_TEX, ONE_OFF_PIX}, {0, 0}},
    {{ONE_OFF_PIX, ONE_OFF_FOG}, {ONE_OFF_SCI, sizeof(PlatGpuState)}},
};

static int one_rg_same(int c, const PlatGpuState* st) {
    for (int k = 0; k < 2; k++) {
        u32 a = k_one_rg[c][k][0], b = k_one_rg[c][k][1];
        if (b > a && memcmp((const u8*)&s_last_state + a, (const u8*)st + a, b - a) != 0) {
            return 0;
        }
    }
    return 1;
}

static void one_rg_copy(int c, const PlatGpuState* st) {
    for (int k = 0; k < 2; k++) {
        u32 a = k_one_rg[c][k][0], b = k_one_rg[c][k][1];
        if (b > a) {
            memcpy((u8*)&s_last_state + a, (const u8*)st + a, b - a);
        }
    }
}

void plat_gpu_set_state_one(const PlatGpuState* st, uint32_t regions, const uint32_t ids[3]) {
    if (s_one == 0) {
        plat_gpu_set_state_dirty(st, regions);
        return;
    }
    if (!s_last_state_valid) {
        plat_gpu_set_state(st); /* the record's first state goes whole */
        memcpy(s_one_cur, ids, sizeof(s_one_cur));
        return;
    }
    if ((regions & PLAT_GPU_R_PIX) && st->fog_enable) { /* the LUT as plat_gpu_set_state_dirty sends it */
        u32 id = st->fog_lut_id;
        u32 h = id != 0 ? 0 : fog_hash(st);
        if (id != 0 ? id != s_last_fog_id : h != s_last_fog_hash) {
            oh_fog(st->fog_lut);
            s_last_fog_hash = h;
            s_last_fog_id = id;
        }
        if (st->fog_lut_id == 0 || st->fog_lut_id != s_last_state.fog_lut_id) {
            memcpy(&s_last_state.fog_lut_id, &st->fog_lut_id, ONE_OFF_SCI - ONE_OFF_LUT);
        }
    }
    int changed = 0;
    for (int c = 0; c < ONE_CLS; c++) {
        if (s_one == 2) {
            /* the key path draws from s_last_state: every class is compared; a change under an unchanged id is stale */
            if (!one_rg_same(c, st)) {
                if (ids[c] != 0 && ids[c] == s_one_cur[c]) {
                    s_onec.stale++;
                    if (s_onec.logged < 16) {
                        s_onec.logged++;
                        PLOG_W("gxone verify: frame %lu, the %s region changed under the same id %08lx",
                               (unsigned long)s_frame_counter, k_one_name[c], (unsigned long)ids[c]);
                    }
                }
                one_rg_copy(c, st);
                changed = 1;
            }
            s_one_cur[c] = ids[c];
        } else if (ids[c] != s_one_cur[c] || ids[c] == 0) {
            one_rg_copy(c, st);
            s_one_cur[c] = ids[c];
            changed = 1;
        }
    }
    if (changed) {
        oh_event();
        s_oh_state_pending = 1;
        s_stats_frame.state_changes++;
        s_onec.changes++;
    }
}

static void one_log(u32 f) {
    PLOG_I("gxone frame %lu: per frame blocks built tev %lu tex %lu pix %lu, sent tev %lu tex %lu pix %lu (encoded at the "
           "draw %lu), tex re-encoded %lu, state changes %lu; resets %lu %lu %lu; verify checks %lu mismatches %lu missed "
           "%lu stale %lu not compared %lu",
           (unsigned long)s_frame_counter, (unsigned long)(s_onec.built[0] / f), (unsigned long)(s_onec.built[1] / f),
           (unsigned long)(s_onec.built[2] / f), (unsigned long)(s_onec.sent[0] / f), (unsigned long)(s_onec.sent[1] / f),
           (unsigned long)(s_onec.sent[2] / f),
           (unsigned long)((s_onec.direct[0] + s_onec.direct[1] + s_onec.direct[2]) / f), (unsigned long)(s_onec.reenc / f),
           (unsigned long)(s_onec.changes / f), (unsigned long)s_onec.resets[0], (unsigned long)s_onec.resets[1],
           (unsigned long)s_onec.resets[2], (unsigned long)s_onec.checks, (unsigned long)s_onec.bad,
           (unsigned long)s_onec.missed, (unsigned long)s_onec.stale, (unsigned long)s_onec.unver);
    u32 logged = s_onec.logged;
    memset(&s_onec, 0, sizeof(s_onec));
    s_onec.logged = logged;
}

/* Start-up: the float conversions against libctru's, citro3d's fixed context values, the block stores. */
static int one_init(void) {
    for (u32 i = 0; i < 8192; i++) {
        u32 b = i < 4096 ? (i * 0x9E3779B9u) ^ (i << 11) : (0x3A000000u + (i - 4096) * 0x00041000u) ^ ((i & 1) << 31);
        float v;
        memcpy(&v, &b, 4);
        if (pica_f24(v) != f32tof24(v) || pica_f31(v) != f32tof31(v)) {
            PLOG_W("gxone: the float encoders differ from libctru's at %08lx (f24 %08lx/%08lx, f31 %08lx/%08lx): off",
                   (unsigned long)b, (unsigned long)pica_f24(v), (unsigned long)f32tof24(v), (unsigned long)pica_f31(v),
                   (unsigned long)f32tof31(v));
            return 0;
        }
    }
    /* __C3D_Context fields (citro3d 1.7.1, from the disassembly): texEnvBuf +412, texConfig +272, texShadow +276,
     * effect: fragOpMode +216, fragOpShadow +220, earlyDepth +234 (byte), earlyDepthFunc +235 (byte), earlyDepthRef
     * +236, stencilTest +244, stencilOp +248, blendClr +256 */
    const u8* cb = (const u8*)__C3D_Context;
    s_one_fx.texenv_buf_hi = __C3D_Context[103] & 0xFFFE0000u;
    s_one_fx.tex_cfg_hi = __C3D_Context[68] & ~0x10007u;
    s_one_fx.tex_shadow = __C3D_Context[69];
    s_one_fx.fragop_lo = __C3D_Context[54] & ~0xFF00u;
    s_one_fx.fragop_shadow = __C3D_Context[55];
    s_one_fx.early_depth = cb[234];
    s_one_fx.early_func = cb[235];
    s_one_fx.early_ref = __C3D_Context[59];
    s_one_fx.stencil_test = __C3D_Context[61];
    s_one_fx.stencil_op = __C3D_Context[62];
    s_one_fx.blend_color = __C3D_Context[64];
    static const u16 k_cap[ONE_CLS] = {512, 1024, 512};
    static const u16 k_stride[ONE_CLS] = {44, 32, 48};
    u32 kb = 0;
    int heap = 0;
    for (int c = 0; c < ONE_CLS; c++) {
        OneStore* s = &s_one_st[c];
        s->cap = k_cap[c];
        s->stride = k_stride[c];
        size_t bytes = (size_t)s->cap * s->stride * 4;
        s->mem = linearMemAlign(bytes, 16);
        s->linear = s->mem != NULL;
        if (s->mem == NULL) {
            s->mem = memalign(16, bytes); /* COPY mode reads them with the CPU only */
            heap = 1;
        }
        s->meta = calloc(s->cap, 4);
        s->ep = calloc(s->cap, 4);
        if (s->mem == NULL || s->meta == NULL || s->ep == NULL) {
            PLOG_W("gxone: no memory for the class blocks: off");
            return 0;
        }
        kb += (u32)(bytes >> 10);
    }
    PLOG_I("gxone=%d: class blocks by id, own encoders (%lu KB of blocks%s); fixed: texenv %08lx texcfg %08lx shadow %08lx "
           "fragop %08lx %08lx early %lu %lu %lu stencil %08lx %08lx blend colour %08lx",
           s_one, (unsigned long)kb, heap ? ", some in the heap" : ", linear memory",
           (unsigned long)s_one_fx.texenv_buf_hi, (unsigned long)s_one_fx.tex_cfg_hi, (unsigned long)s_one_fx.tex_shadow,
           (unsigned long)s_one_fx.fragop_lo, (unsigned long)s_one_fx.fragop_shadow, (unsigned long)s_one_fx.early_depth,
           (unsigned long)s_one_fx.early_func, (unsigned long)s_one_fx.early_ref, (unsigned long)s_one_fx.stencil_test,
           (unsigned long)s_one_fx.stencil_op, (unsigned long)s_one_fx.blend_color);
    return 1;
}

/* ---- switch gxchunk: batch chunks and the frame list (GX native step 3, docs/PORT_3DS_GX_NATIVE.md) ----
 * Requires gxone=1 (so gxonehop=1: the game thread writes the shader path's words, the frame list).
 *  - Batch chunks: a gxbatch batch (a run of DRAWP draws under one vertex state) gets one chunk, built once by
 *    plat_gpu_chunk_build: per draw the attribute setup when it changes (the layout block's kept words), the four
 *    scale rows when they change (the same 32-bit values vsh_scales_rows makes), the cull word (first draw), the draw
 *    packet with the index address (offset from the attribute base) and count. 16-byte aligned and padded, in a
 *    plat/memplace placement ("gx chunks", linear memory by default). An id is the store's generation and an index:
 *    plat_gpu_chunk_draw checks it with one compare; a full store starts again (every id becomes invalid, each batch
 *    builds again at its next use).
 *  - Frame list: the record's word arena (the gxonehop words) is a placement too ("gx frame list", 2 x gxonehopkb,
 *    linear memory by default; QTM only with place=gx_frame_list:qtm and qtmgpu, which needs the cb_qtm probe).
 *    At a chunk use the game thread does what oh_draw does before the first draw (stereo mark, flush, the class
 *    blocks whose ids the GPU does not hold), writes the pending uniform rows, then copies the chunk's words (COPY
 *    mode; the last draw's count patched for a batch called in part). No gx_rec record for these draws.
 *  - Each run of words ends with citro3d's split end (pica_enc_list_end): the render thread submits it as a command
 *    list of its own (GX_ProcessCommandList into citro3d's GX queue, after citro3d's pending words as their own
 *    list), and does only the citro3d work between runs. With the queue near full (20 of 32 entries) it copies the
 *    run into citro3d's buffer as gxonehop=1 does.
 *  - Flushes: C3D_FrameEnd(0) flushes the whole linear heap before the queue runs: that one flush per record covers
 *    the frame list. Outside linear memory (place=) the render thread flushes the frame list once per record.
 *  - GPU reset: every record starts with no held id (oh_record_start), the reset is applied at a record's start.
 *  - gxchunk=2: at every chunk use the per-draw words (what gxchunk=0 would write) are built beside it, and when the
 *    record comes back both word streams go through a register-file model (pica_model.c), compared at every draw. */
#define CK_MAX_DRAWS 64u
#define CK_DRAW_WORDS (OH_CAPW + 32u + 2u + 32u) /* a layout block, 4 scale rows (4 runs at most), cull, a packet */
#define CK_VREG 4096u                            /* gxchunk=2: chunk uses per record */
#define CK_VEND 16384u                           /* gxchunk=2: chunk draws per record */
typedef struct CkReg {
    u32 a0, b0, nd, e; /* start in ow and in ckref, draws, first pair in ckend */
} CkReg;
typedef struct {
    u32 off;  /* word offset in s_ck.mem */
    u32 cull; /* the GX cull mode the chunk's cull word is for */
    u16 nd, d0;
} CkHdr;
typedef struct {
    u16 end, cnt;  /* words up to the end of this draw's packet; the offset of its count word */
    u32 nidx, cum; /* its indices (trimmed), those of the draws up to it */
    const void* stream;
    u32 layout, scales;
    const u16* idx;
} CkDraw;
static struct {
    u32* mem;
    u32 cap, used; /* words */
    CkHdr* h;
    CkDraw* d;
    u32 hcap, hused, dcap, dused, gen;
    u32 smask[3]; /* the scale rows */
} s_ck;
static VUnif s_ck_vu, s_ck_rvu; /* scratch rows: the chunk's, gxchunk=2's per-draw words */
static struct {
    u32 built, notnow, never, resets, uses, stale, draws, words;
    u32 subs, subcopy, splits; /* render thread */
    u32 vchecks, vdiffer, vone, vunres, vover, logged;
} s_ckst;
static PicaModel *s_ck_ma, *s_ck_mb;

int plat_gpu_chunk_active(void) { return s_chunk; }

/* n less its remainder mod 3 (whole triangles), without a divide */
static inline u32 trim3(u32 n) { return (u32)(((u64)n * 0xAAAAAAABull) >> 33) * 3u; }

static void ck_reset(void) {
    s_ck.used = s_ck.hused = s_ck.dused = 0;
    s_ck.gen = (s_ck.gen + 1) & 0xFFFFu;
    if (s_ck.gen == 0) {
        s_ck.gen = 1;
    }
    s_ckst.resets++;
}

/* The kept layout block of (stream, layout); NULL while its capture is pending or not made yet. */
static const u32* ck_lay(const void* stream, u32 layout, u32* n) {
    DTab* t = &s_otab[DC_LAY];
    u32 k[DK_LAY] = {(u32)(uintptr_t)stream, layout};
    u32* e = dtab_find(t, k, dkey_hash(k, DK_LAY));
    if (e == NULL || e[1] == OH_PENDING || e[1] == 0) {
        return NULL;
    }
    *n = e[1];
    return e + 3 + t->keyw;
}

/* All four scale rows of `scales`, as oh_rows writes rows */
static u32* ck_scales(u32* w, u32 scales) {
    vunif_reset_u(&s_ck_vu);
    s_ck_vu.pend[0] = s_ck_vu.pend[1] = s_ck_vu.pend[2] = 0;
    vsh_scales_rows(&s_ck_vu, scales);
    return oh_rows_u(w, s_ck_vu.pend, &s_ck_vu);
}

/* C3D_DrawElements' packet for u16 triangles at idx */
static u32* ck_packet(u32* w, const u16* idx, u32 count) {
    memcpy(w, s_tpl, s_tpl_n * 4);
    w[s_tpl_idx] = (osConvertVirtToPhys(idx) - s_oh_base) | (u32)C3D_UNSIGNED_SHORT << 31;
    w[s_tpl_cnt] = count;
    return w + s_tpl_n;
}

uint32_t plat_gpu_chunk_build(const PlatGpuChunkDraw* d, uint32_t n, uint32_t cull) {
    if (!s_chunk || n == 0 || n > CK_MAX_DRAWS) {
        s_ckst.never++;
        return PLAT_GPU_CHUNK_NEVER;
    }
    for (u32 i = 0; i < n; i++) {
        u32 c = trim3(d[i].nidx);
        const u8* lo = (const u8*)d[i].idx;
        if (d[i].stream == NULL || lo == NULL || c < 3 || PLAT_VSH_STRIDE(d[i].layout) == 0 ||
            !ixpool_ok(d[i].idx, c) ||
            osConvertVirtToPhys(d[i].idx) < s_oh_base) {
            s_ckst.never++;
            return PLAT_GPU_CHUNK_NEVER; /* a draw plat_gpu_vsh_drawp would skip: stays on the per-draw path */
        }
    }
    if (s_ck.hused >= s_ck.hcap || s_ck.dused + n > s_ck.dcap || s_ck.used + n * CK_DRAW_WORDS + 4 > s_ck.cap) {
        ck_reset();
        if (s_ck.used + n * CK_DRAW_WORDS + 4 > s_ck.cap) {
            s_ckst.never++;
            return PLAT_GPU_CHUNK_NEVER;
        }
    }
    u32* w0 = s_ck.mem + s_ck.used;
    u32* w = w0;
    CkDraw* cd = s_ck.d + s_ck.dused;
    u32 cum = 0;
    for (u32 i = 0; i < n; i++) {
        if (i == 0 || d[i].stream != d[i - 1].stream || d[i].layout != d[i - 1].layout) {
            u32 ln;
            const u32* lw = ck_lay(d[i].stream, d[i].layout, &ln);
            if (lw == NULL) {
                s_ckst.notnow++;
                return 0; /* the attribute setup is captured first (per-draw path), then the chunk is built */
            }
            memcpy(w, lw, ln * 4);
            w += ln;
        }
        if (i == 0 || d[i].scales != d[i - 1].scales) {
            w = ck_scales(w, d[i].scales);
        }
        if (i == 0) {
            w = enc_write(w, GPUREG_FACECULLING_CONFIG, (u32)vsh_cull(cull) & 3);
        }
        u32 c = trim3(d[i].nidx);
        cum += c;
        u32 at = (u32)(w - w0);
        w = ck_packet(w, d[i].idx, c);
        cd[i] = (CkDraw){(u16)(w - w0), (u16)(at + s_tpl_cnt), c, cum, d[i].stream, d[i].layout, d[i].scales, d[i].idx};
    }
    u32 nw = (u32)(w - w0);
    if (nw & 3u) { /* 16-byte padding: a write with byte mask 0 (does nothing) */
        w = enc_write(w, GPUREG_FACECULLING_CONFIG, 0);
        w[-1] &= ~0xF0000u;
        nw += 2;
    }
    CkHdr* h = &s_ck.h[s_ck.hused];
    *h = (CkHdr){s_ck.used, cull, (u16)n, (u16)s_ck.dused};
    s_ck.used += nw;
    s_ck.dused += n;
    s_ckst.built++;
    return s_ck.gen << 16 | ++s_ck.hused;
}

/* gxchunk=2: the per-draw words oh_draw would write for the chunk's draws from here (s_om and s_vg before the pending
 * rows go out), into the record's ckref. The region (NULL: not compared). */
static CkReg* ck_ref(Record* r, const CkDraw* d, u32 nd, u32 last, u32 cull, u32 a0) {
    if (r->ckover || r->nckr >= CK_VREG || r->nckend + nd > CK_VEND ||
        r->nckref + OH_DRAW_WORDS + nd * CK_DRAW_WORDS > s_oh_words) {
        r->ckover = 1;
        return NULL;
    }
    memcpy(&s_ck_rvu, &s_vg, sizeof(VUnif));
    int lv = s_om.lay_valid;
    const void* vs = s_om.v_stream;
    u32 vl = s_om.v_layout, vsc = s_om.v_scales, vc = s_om.v_cull;
    u32* w = r->ckref + r->nckref;
    CkReg* g = (CkReg*)r->ckr + r->nckr;
    *g = (CkReg){a0, r->nckref, nd, r->nckend};
    for (u32 j = 0; j < nd; j++) {
        if (!lv || d[j].stream != vs || d[j].layout != vl) {
            u32 ln;
            const u32* lw = ck_lay(d[j].stream, d[j].layout, &ln);
            if (lw == NULL) {
                s_ckst.vunres++;
                return NULL; /* evicted since the build: this use is not compared */
            }
            memcpy(w, lw, ln * 4);
            w += ln;
            lv = 1;
            vs = d[j].stream;
            vl = d[j].layout;
        }
        if (d[j].scales != vsc) {
            vsh_scales_rows(&s_ck_rvu, d[j].scales);
            vsc = d[j].scales;
        }
        if (cull != vc) {
            w = enc_write(w, GPUREG_FACECULLING_CONFIG, (u32)vsh_cull(cull) & 3);
            vc = cull;
        }
        if ((s_ck_rvu.pend[0] | s_ck_rvu.pend[1] | s_ck_rvu.pend[2]) != 0) {
            w = oh_rows_u(w, s_ck_rvu.pend, &s_ck_rvu);
            s_ck_rvu.pend[0] = s_ck_rvu.pend[1] = s_ck_rvu.pend[2] = 0;
        }
        w = ck_packet(w, d[j].idx, j == nd - 1 ? last : d[j].nidx);
        r->ckend[(g->e + j) * 2 + 1] = (u32)(w - r->ckref);
    }
    r->nckref = (u32)(w - r->ckref);
    r->nckr++;
    r->nckend += nd;
    return g;
}

int plat_gpu_chunk_draw(uint32_t id, uint32_t cull, uint32_t nd, uint32_t last, uint32_t calls) {
    u32 hi = (id & 0xFFFFu) - 1;
    if (s_chunk == 0 || id >> 16 != s_ck.gen || hi >= s_ck.hused) {
        s_ckst.stale++;
        return 0;
    }
    const CkHdr* h = &s_ck.h[hi];
    last = trim3(last);
    const CkDraw* d = s_ck.d + h->d0;
    if (h->cull != cull || nd == 0 || nd > h->nd || last < 3 || last > d[nd - 1].nidx) {
        s_ckst.stale++;
        return 0;
    }
    u32 nw = d[nd - 1].end;
    /* plat_gpu_vsh_drawp's room checks, once for the whole chunk */
    Record* r = oh_rec();
    if (r->used + sizeof(CmdHead) + sizeof(VshDrawP) + 64 > RECORD_BYTES) {
        flush_segment();
    }
    oh_need_room(0, nw + PICA_LIST_END_MAX + 4);
    if (!s_last_vconst_valid) {
        return 1;
    }
    s_stats_frame.vsh_calls += calls;
    s_stats_frame.vsh_indices += (nd > 1 ? d[nd - 2].cum : 0) + last;
    s_stats_frame.vsh_draws += nd;
    s_ckst.uses++;
    if (!s_last_state_valid) {
        s_ost.skipped += nd;
        return 1; /* oh_draw: the replay skips a draw before the record's first state */
    }
    /* what oh_draw does before the first draw (for the later draws of a batch it does nothing more) */
    int cls = oh_class(&s_last_state);
    if (cls != OHS_SCENE && s_om.after_scene) {
        oh_mark();
    }
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
        if (oh_state_one(&s_last_state)) {
            s_om.v_cull = 0;
        }
        s_oh_state_pending = 0;
    }
    if (!s_om.scissor_empty) {
        r = oh_rec();
        u32 a0 = r->now;
        CkReg* g = s_chunk == 2 ? ck_ref(r, d, nd, last, cull, a0) : NULL;
        oh_unif_flush();
        u32* w = r->ow + r->now;
        memcpy(w, s_ck.mem + h->off, nw * 4);
        if (last != d[nd - 1].nidx) {
            w[d[nd - 1].cnt] = last; /* the batch was called in part */
        }
        if (g != NULL) {
            for (u32 j = 0; j < nd; j++) {
                r->ckend[(g->e + j) * 2] = r->now + d[j].end;
            }
        }
        r->now += nw;
        s_om.chunk_draws += nd;
        s_ost.draws += nd;
        s_om.lay_valid = 1;
        s_om.v_stream = d[nd - 1].stream;
        s_om.v_layout = d[nd - 1].layout;
        s_om.v_cull = cull;
        /* the rows now as the chunk left them: the last scales, not pending */
        vsh_scales_rows(&s_vg, d[nd - 1].scales);
        s_om.v_scales = d[nd - 1].scales;
        for (int k = 0; k < 3; k++) {
            s_vg.pend[k] &= ~s_ck.smask[k];
        }
        s_ckst.words += nw;
    }
    s_om.gdraw += nd; /* gpudrawcut/gpudrawskip numbering (gxchunk is off with them) */
    if (cls == OHS_SCENE) {
        oh_stereo_after(cls, 0);
    } else if (cls == OHS_LAYER) {
        r = oh_rec();
        r->oh_tail_draws += nd;
        r->oh_tail_z += s_last_state.depth_write ? nd : 0;
    }
    s_om.packet = NULL;
    s_last_vdraw = NULL;
    s_ckst.draws += nd;
    return 1;
}

/* ---- switch gximmfl: immediate draws straight into the frame list (GX native step 4, docs/PORT_3DS_GX_NATIVE.md) ----
 * Needs gxchunk (so gxone=1, gxonehop=1, gxthread=0: the game thread writes the frame list). plat_gpu_imm_begin
 * gives the front end the place in the record's index arena for the draw's indices (it writes them there: nothing is
 * copied), after the light and uniform block (sent when their ids changed, as plat_gpu_vsh_light/const) and
 * plat_gpu_vsh_draw's room checks; plat_gpu_imm_end writes the draw as oh_draw writes a shader draw (stereo mark,
 * flush, the class blocks whose ids the GPU does not hold, the layout block, the cull word, the pending uniform rows,
 * the packet; or the merge into the last packet). The index address comes from the arena's physical base taken once
 * per record (linear memory is physically contiguous), not osConvertVirtToPhys per draw.
 * gximmfl=2: before the draw's words, the words the old path (plat_gpu_vsh_draw's oh_draw) writes from there are
 * built from the old inputs into the record's ckref (as gxchunk=2's, whose buffers and ck_verify it shares); a merge
 * patches the old packet's count too. When the record comes back both streams go through pica_model, compared at
 * every immediate draw ("gximmfl frame" line: draws, merged; verify draws, differ). */
static struct {
    u32 draws, merged, none;
    u32 fchecks, froute, fidx; /* the front end's totals (plat_gpu_immfl_note) */
} s_imst;
static u32 s_im_pa[2];   /* physical address of each record's index arena (0: not taken yet) */
static u32* s_imv_pkt;   /* gximmfl=2: the last packet written by plat_gpu_imm_end ... */
static u32 s_imv_cnt;    /* ... and the offset of its count word in the record's ckref (~0: none) */

int plat_gpu_immfl_active(void) { return s_immfl; }

void plat_gpu_immfl_note(uint32_t checks, uint32_t route_differ, uint32_t idx_differ) {
    s_imst.fchecks = checks;
    s_imst.froute = route_differ;
    s_imst.fidx = idx_differ;
}

uint16_t* plat_gpu_imm_begin(const PlatGpuVshConst* c, uint32_t cid, const PlatGpuVshLight* l, uint32_t lid,
                             uint32_t layout, uint32_t nidx) {
    if (l != NULL) {
        plat_gpu_vsh_light(l, lid);
    }
    plat_gpu_vsh_const(c, cid);
    /* plat_gpu_vsh_draw's checks, in its order */
    if (nidx < 3 || nidx > INDEX_ARENA || PLAT_VSH_STRIDE(layout) == 0) {
        s_imst.none++;
        return NULL;
    }
    Record* r = &s_rec[s_cur];
    if (r->nidx + nidx > INDEX_ARENA || r->used + sizeof(CmdHead) + sizeof(VshDraw) + 64 > RECORD_BYTES) {
        flush_segment();
    }
    oh_need_room(0, 0);
    if (!s_last_vconst_valid) {
        s_imst.none++;
        return NULL;
    }
    r = &s_rec[s_cur];
    return r->idx + r->nidx;
}

/* gximmfl=2: the words oh_draw writes for this draw from here (s_om and s_vg as they are, after the layout block),
 * into the record's ckref: the scale rows when the scales changed, the cull word when it changed, the pending rows,
 * the packet (index address by osConvertVirtToPhys). The region; NULL: not compared. */
static CkReg* imv_ref(Record* r, u32 scales, u32 cull, const u16* idx, u32 count, u32 a0) {
    s_imv_cnt = ~0u;
    if (r->ckover || r->nckr >= CK_VREG || r->nckend + 1 > CK_VEND ||
        r->nckref + OH_DRAW_WORDS + CK_DRAW_WORDS > s_oh_words) {
        r->ckover = 1;
        return NULL;
    }
    memcpy(&s_ck_rvu, &s_vg, sizeof(VUnif));
    u32* w = r->ckref + r->nckref;
    CkReg* g = (CkReg*)r->ckr + r->nckr;
    *g = (CkReg){a0, r->nckref, 1, r->nckend};
    if (scales != s_om.v_scales) {
        vsh_scales_rows(&s_ck_rvu, scales);
    }
    if (cull != s_om.v_cull) {
        w = enc_write(w, GPUREG_FACECULLING_CONFIG, (u32)vsh_cull(cull) & 3);
    }
    if ((s_ck_rvu.pend[0] | s_ck_rvu.pend[1] | s_ck_rvu.pend[2]) != 0) {
        w = oh_rows_u(w, s_ck_rvu.pend, &s_ck_rvu);
    }
    s_imv_cnt = (u32)(w - r->ckref) + s_tpl_cnt;
    w = ck_packet(w, idx, count);
    r->ckend[g->e * 2] = a0; /* the new stream's end: set by the caller */
    r->ckend[g->e * 2 + 1] = (u32)(w - r->ckref);
    r->nckref = (u32)(w - r->ckref);
    r->nckr++;
    r->nckend++;
    return g;
}

void plat_gpu_imm_end(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, uint32_t nidx) {
    Record* r = &s_rec[s_cur];
    u32 first = r->nidx;
    const u16* idx = r->idx + first;
    r->nidx += nidx;
    s_stats_frame.vsh_calls++;
    s_stats_frame.vsh_indices += nidx;
    /* plat_gpu_vsh_draw's merge test (gxonehop=1), on what the words hold */
    VshDraw* l = s_last_vdraw;
    if (s_merge && l != NULL && l->stream == stream && l->layout == layout && l->scales == scales && l->cull == cull &&
        s_oh_idx_end == idx) { /* D2: the end is an address (arena or a stream's linear index segment) */
        if (!s_last_state_valid) {
            s_ost.skipped++;
            return;
        }
        if (s_om.packet != NULL) {
            s_om.packet[s_tpl_cnt] += nidx;
            if (s_immfl == 2 && s_om.packet == s_imv_pkt && s_imv_cnt != ~0u) {
                r->ckref[s_imv_cnt] += nidx; /* the old path merges the same way */
            }
        }
        s_oh_idx_end += nidx;
        s_ost.merged++;
        s_imst.merged++;
        return;
    }
    s_stats_frame.vsh_draws++;
    if (!s_last_state_valid) {
        s_ost.skipped++;
        return; /* the replay skips a draw before the record's first state */
    }
    /* oh_draw, a draw from the index arena (drawp 0) */
    int cls = oh_class(&s_last_state);
    if (cls != OHS_SCENE && s_om.after_scene) {
        oh_mark();
    }
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
        if (oh_state_one(&s_last_state)) {
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
        r = oh_rec();
        CkReg* g = s_immfl == 2 ? imv_ref(r, scales, cull, idx, nidx, r->now) : NULL;
        if (scales != s_om.v_scales) {
            vsh_scales_rows(&s_vg, scales);
            s_om.v_scales = scales;
        }
        u32* w = r->ow + r->now;
        if (cull != s_om.v_cull) {
            w = enc_write(w, GPUREG_FACECULLING_CONFIG, (u32)vsh_cull(cull) & 3);
            s_om.v_cull = cull;
        }
        if ((s_vg.pend[0] | s_vg.pend[1] | s_vg.pend[2]) != 0) {
            w = oh_rows(w, s_vg.pend);
            s_vg.pend[0] = s_vg.pend[1] = s_vg.pend[2] = 0;
        }
        u32 ai = (u32)(r - s_rec) & 1u;
        if (s_im_pa[ai] == 0) {
            /* linear memory, VRAM and QTM are mapped physically contiguous (at 0x14000000 and above); the heap is
             * not: there (place=) the address is converted per draw (1 marks it) */
            s_im_pa[ai] = (uintptr_t)r->idx >= 0x14000000u ? osConvertVirtToPhys(r->idx) : 1u;
        }
        u32 pa = s_im_pa[ai] != 1u ? s_im_pa[ai] + first * 2u : osConvertVirtToPhys(idx);
        if (pa >= s_oh_base && plat_gpudump_draw_filter(s_frame_counter, s_om.seq, s_om.gdraw++)) {
            packet = w;
            memcpy(packet, s_tpl, s_tpl_n * 4);
            packet[s_tpl_idx] = (pa - s_oh_base) | (u32)C3D_UNSIGNED_SHORT << 31;
            packet[s_tpl_cnt] = nidx;
            w += s_tpl_n;
            s_om.chunk_draws++;
            s_ost.draws++;
        }
        r->now = (u32)(w - r->ow);
        if (g != NULL) {
            r->ckend[g->e * 2] = r->now;
        }
    }
    oh_stereo_after(cls, s_last_state.depth_write);
    s_om.packet = packet;
    s_imv_pkt = packet;
    s_oh_md = (VshDraw){stream, layout, scales, cull, 0, 0};
    s_last_vdraw = &s_oh_md;
    s_oh_idx_end = idx + nidx;
    s_imst.draws++;
}

/* Render thread: a run of words as a command list of its own (1), or 0 when the GX queue has no room for it (the
 * caller copies the words into citro3d's buffer). citro3d's words since its last split go first. */
static int ck_submit(const OhWords* c) {
    /* the queue holds 32 and runs only at C3D_FrameEnd: 20 for the lists, the rest for the strip copies (which start
     * a new frame past 24), the frame's last split, the present and the stereo transfers */
    if (s_gx_entries + 2 > 20) {
        s_ckst.subcopy++;
        return 0;
    }
    if (gpuCmdBufOffset != 0) {
        C3D_FrameSplit(0);
        s_gx_entries++;
        s_ckst.splits++;
    }
    GX_ProcessCommandList((u32*)c->w, (c->n + c->tail) * 4u, 0);
    s_gx_entries++;
    s_ckst.subs++;
    return 1;
}

static void ck_feed2(const u32* w, u32 n) {
    pica_model_feed(s_ck_ma, w, n);
    pica_model_feed(s_ck_mb, w, n);
}

/* gxchunk=2, game thread, when the record comes back (its captures filled): both word streams through the model. */
static void ck_verify(Record* r) {
    if (r->ckover) {
        s_ckst.vover++;
        return;
    }
    if (r->nckr == 0) {
        return;
    }
    pica_model_reset(s_ck_ma);
    pica_model_reset(s_ck_mb);
    const CkReg* g = r->ckr;
    u32 ri = 0;
    for (u8* p = r->data; p < r->data + r->used;) {
        CmdHead* hd = (CmdHead*)p;
        u8* body = p + sizeof(CmdHead);
        p = body + hd->size;
        if (hd->type == CMD_OH_CAPTURE) {
            const OhCap* b = (const OhCap*)body;
            const OhCap* src = b->ref != NULL && b->ref->n > 0 ? b->ref : b;
            if (src->n > 0) {
                ck_feed2(src->res, src->n);
            } else {
                pica_model_reset(s_ck_ma); /* words the model did not see: both start again */
                pica_model_reset(s_ck_mb);
            }
            continue;
        }
        if (hd->type == CMD_OH_MARK) {
            continue; /* mono: its words are not applied */
        }
        if (hd->type != CMD_OH_WORDS) {
            /* citro3d writes registers here (its flush, setters, a clear, copy or CPU-path draw) that the model does
             * not see: both start again (after it both word streams send every block again) */
            pica_model_reset(s_ck_ma);
            pica_model_reset(s_ck_mb);
            continue;
        }
        const OhWords* c = (const OhWords*)body;
        u32 o = (u32)(c->w - r->ow), end = o + c->n;
        while (o < end) {
            while (ri < r->nckr && g[ri].a0 < o) {
                ri++;
                s_ckst.vunres++; /* a region of words the record did not keep */
            }
            if (ri < r->nckr && g[ri].a0 == o) {
                const u32* ends = r->ckend + g[ri].e * 2;
                u32 b = g[ri].b0;
                for (u32 j = 0; j < g[ri].nd; j++) {
                    pica_model_feed(s_ck_ma, r->ow + o, ends[2 * j] - o);
                    pica_model_feed(s_ck_mb, r->ckref + b, ends[2 * j + 1] - b);
                    o = ends[2 * j];
                    b = ends[2 * j + 1];
                    PicaModelDiff df;
                    pica_model_cmp(s_ck_ma, s_ck_mb, &df);
                    s_ckst.vchecks++;
                    s_ckst.vone += df.one_sided;
                    if (df.differ != 0) {
                        s_ckst.vdiffer++;
                        if (s_ckst.logged < 16) {
                            s_ckst.logged++;
                            PLOG_W("gxchunk verify: frame %lu, chunk draw %lu of %lu: %lu differ, first %04lx: chunk "
                                   "%08lx, per draw %08lx",
                                   (unsigned long)r->frame, (unsigned long)j, (unsigned long)g[ri].nd,
                                   (unsigned long)df.differ, (unsigned long)df.first_reg, (unsigned long)df.first_a,
                                   (unsigned long)df.first_b);
                        }
                    }
                    if (s_ck_ma->draws != s_ck_mb->draws && s_ckst.logged < 16) {
                        s_ckst.logged++;
                        PLOG_W("gxchunk verify: frame %lu, draw counts differ (%lu, %lu)", (unsigned long)r->frame,
                               (unsigned long)s_ck_ma->draws, (unsigned long)s_ck_mb->draws);
                    }
                }
                ri++;
                continue;
            }
            u32 nx = ri < r->nckr && g[ri].a0 < end ? g[ri].a0 : end;
            ck_feed2(r->ow + o, nx - o);
            o = nx;
        }
    }
    s_ckst.vunres += r->nckr - ri;
}

static void ck_log(u32 f) {
    PLOG_I("gxchunk frame %lu: per frame chunk uses %lu (draws %lu, words %lu), built %lu, not yet %lu, never %lu, "
           "stale %lu; store resets %lu (%lu of %lu words, %lu chunks)",
           (unsigned long)s_frame_counter, (unsigned long)(s_ckst.uses / f), (unsigned long)(s_ckst.draws / f),
           (unsigned long)(s_ckst.words / f), (unsigned long)(s_ckst.built / f), (unsigned long)(s_ckst.notnow / f),
           (unsigned long)(s_ckst.never / f), (unsigned long)(s_ckst.stale / f), (unsigned long)s_ckst.resets,
           (unsigned long)s_ck.used, (unsigned long)s_ck.cap, (unsigned long)s_ck.hused);
    PLOG_I("gxchunk frame %lu: lists submitted %lu, citro3d splits %lu, copied %lu; verify draws %lu differ %lu "
           "one-sided %lu not compared %lu records over %lu",
           (unsigned long)s_frame_counter, (unsigned long)(s_ckst.subs / f), (unsigned long)(s_ckst.splits / f),
           (unsigned long)(s_ckst.subcopy / f), (unsigned long)s_ckst.vchecks, (unsigned long)s_ckst.vdiffer,
           (unsigned long)s_ckst.vone, (unsigned long)s_ckst.vunres, (unsigned long)s_ckst.vover);
    if (s_immfl) {
        /* with gxchunk=1 the verify counts of the line above are the immediate draws' alone */
        PLOG_I("gximmfl frame %lu: per frame draws %lu merged %lu none %lu; front-end checks %lu route differ %lu "
               "index differ %lu (total)",
               (unsigned long)s_frame_counter, (unsigned long)(s_imst.draws / f), (unsigned long)(s_imst.merged / f),
               (unsigned long)(s_imst.none / f), (unsigned long)s_imst.fchecks, (unsigned long)s_imst.froute,
               (unsigned long)s_imst.fidx);
        s_imst.draws = s_imst.merged = s_imst.none = 0;
    }
    u32 logged = s_ckst.logged;
    memset(&s_ckst, 0, sizeof(s_ckst)); /* the render thread's counters: racy, for the log only */
    s_ckst.logged = logged;
}

/* Start-up, after oh_init (the frame list): the chunk store, the scale rows, gxchunk=2's buffers. */
static int ck_init(void) {
    u32 kb = (u32)plat_switch_int("gxchunkkb", 256);
    if (kb < 64 || kb > 2048) {
        kb = 256;
    }
    int h = plat_place_register("gx chunks", kb << 10, 16, PLAT_PREFS(PLAT_TIER_LINEAR, PLAT_TIER_HEAP),
                                PLAT_PF_GPU_READ);
    const char* tier = "none";
    s_ck.mem = h >= 0 ? plat_place_alloc(h, kb << 10, 16) : NULL;
    if (h >= 0) {
        plat_place_info(h, NULL, NULL, NULL, &tier);
    }
    s_ck.cap = kb << 8;
    s_ck.hcap = 2048;
    s_ck.dcap = 4096;
    s_ck.h = malloc(s_ck.hcap * sizeof(CkHdr));
    s_ck.d = malloc(s_ck.dcap * sizeof(CkDraw));
    s_ck.gen = 1;
    if (s_ck.mem == NULL || s_ck.h == NULL || s_ck.d == NULL) {
        PLOG_W("gxchunk: no memory for the chunk store: off");
        return 0;
    }
    s_ck_vu.direct = 1;
    s_ck_rvu.direct = 1;
    const int rows[4] = {s_uloc_vscale, s_uloc_texsel, s_uloc_texsel + 1, s_uloc_texsel + 2};
    for (int i = 0; i < 4; i++) {
        if (rows[i] < 0 || rows[i] >= 96) {
            PLOG_W("gxchunk: scale row %d out of range: off", rows[i]);
            return 0;
        }
        s_ck.smask[rows[i] >> 5] |= 1u << (rows[i] & 31);
    }
    if (s_chunk == 2 || s_immfl == 2) {
        s_ck_ma = malloc(sizeof(PicaModel));
        s_ck_mb = malloc(sizeof(PicaModel));
        for (int i = 0; i < 2; i++) {
            s_rec[i].ckr = malloc(CK_VREG * sizeof(CkReg));
            s_rec[i].ckref = malloc(s_oh_words * 4);
            s_rec[i].ckend = malloc(CK_VEND * 2 * 4);
            if (s_rec[i].ckr == NULL || s_rec[i].ckref == NULL || s_rec[i].ckend == NULL) {
                s_ck_ma = NULL;
            }
        }
        if (s_ck_ma == NULL || s_ck_mb == NULL) {
            PLOG_W("gxchunk=2 / gximmfl=2: no memory for the verify buffers: off");
            return 0;
        }
    }
    PLOG_I("gxchunk=%d: batch chunks (%lu KB, %s), the frame list submitted as command lists (%s)%s", s_chunk,
           (unsigned long)kb, tier, s_ck_tier_linear ? "linear memory" : "outside linear memory, flushed per record",
           s_chunk == 2 ? ", per-draw words compared through the register model" : "");
    return 1;
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
        if (s_one == 1 && c != DC_LAY) {
            continue; /* gxone=1: the three state classes go by id; only the layout keeps its table */
        }
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
    int fl = -1; /* gxchunk: the frame list placement (the GPU reads the words) */
    if (s_chunk) {
        fl = plat_place_register("gx frame list", 2u * s_oh_words * 4u, 16, PLAT_PREFS(PLAT_TIER_LINEAR, PLAT_TIER_HEAP),
                                 PLAT_PF_GPU_READ);
        const char* tier = "none";
        if (fl >= 0) {
            plat_place_info(fl, NULL, NULL, NULL, &tier);
        }
        if (fl < 0 || strcmp(tier, "HEAP") == 0) {
            PLOG_W("gxchunk: the frame list is not in memory the GPU reads (%s): off", tier);
            s_chunk = 0;
        }
        s_ck_tier_linear = strcmp(tier, "LINEAR") == 0;
    }
    for (int i = 0; i < 2; i++) {
        s_rec[i].ow = fl >= 0 ? plat_place_alloc(fl, s_oh_words * 4, 16) : memalign(16, s_oh_words * 4);
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
    if (s_one) {
        one_log(f);
    }
    if (s_chunk) {
        ck_log(f);
    }
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
        dt_reap();
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
    dt_issue(r->seq); /* texdt: the transfers that tile the textures made since the last record, before this record's lists */
    C3D_FrameDrawOn(s_efb);
    C3D_BindProgram(&s_prog);
    C3D_SetAttrInfo(&s_attr);
    rot_uniforms();
    static C3D_BufInfo bi;
    BufInfo_Init(&bi);
    BufInfo_Add(&bi, r->verts, sizeof(PlatGpuVertex), 5, 0x43210);
    C3D_SetBufInfo(&bi);
    /* gxnoflush: ranges inside the linear heap are flushed by C3D_FrameEnd(0) (the whole heap, before the GX queue
     * runs; libcitro3d 1.7.1 renderqueue.o: GSPGPU_FlushDataCache(__ctru_linear_heap, __ctru_linear_heap_size)):
     * the explicit flush is skipped for them. QTM and heap ranges keep theirs. =2: flush all as before, and count
     * the ranges the heap flush covers. */
    for (u32 i = 0; i < r->ndirty; i++) {
        u32 n = (u32)(r->dirty[i].hi - r->dirty[i].lo);
        if (nf_flush((const void*)r->dirty[i].lo, n)) {
            GSPGPU_FlushDataCache((const void*)r->dirty[i].lo, n);
            s_dst.fl_calls++;
            s_dst.fl_bytes += n;
        }
    }
    s_dst.fl_ranges += r->ndirty;
    r->ndirty = 0;
    if (nf_flush(r->verts, r->nverts * (u32)sizeof(PlatGpuVertex))) {
        GSPGPU_FlushDataCache(r->verts, r->nverts * sizeof(PlatGpuVertex));
        s_dst.fl_calls++;
        s_dst.fl_bytes += r->nverts * (u32)sizeof(PlatGpuVertex);
    }
    if (r->nidx > 0 && nf_flush(r->idx, r->nidx * 2u)) {
        GSPGPU_FlushDataCache(r->idx, r->nidx * sizeof(u16));
        s_dst.fl_calls++;
        s_dst.fl_bytes += r->nidx * 2u;
    }
    if (s_chunk && !s_ck_tier_linear && r->now > 0) {
        /* gxchunk: the frame list outside linear memory (place=): its one flush. In linear memory C3D_FrameEnd(0)
         * flushes the whole linear heap before the queue runs, which covers it. */
        GSPGPU_FlushDataCache(r->ow, r->now * 4u);
        s_dst.fl_calls++;
        s_dst.fl_bytes += r->now * 4u;
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
                if (!ixpool_ok(d_idx, d_count)) {
                    static u32 s_logged;
                    if (s_logged++ == 0) {
                        PLOG_E("gpu: drawp indices %p +%lu outside the index ranges: draw skipped", (const void*)d_idx,
                               (unsigned long)d_count);
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
            if (c->tail != 0 && ck_submit(c)) {
                /* gxchunk: submitted as a command list of its own (its end flushes the framebuffer after draws) */
                n_draw += c->draws;
                after_last_draw = c->draws ? 0 : after_last_draw;
            } else {
                cmd_put(c->w, c->n);
                if (c->draws) {
                    C3DX_FLAGS |= C3DXF_DRAWUSED;
                    n_draw += c->draws;
                    after_last_draw = 0;
                }
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
    if (r->shot) {
        capture_efb(r, r->shot_path);
        r->shot = 0;
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

void plat_gpu_set_shot_hook(int (*hook)(const char* path)) {
    s_shot_hook = hook;
}

void plat_gpu_shot(const char* path) {
    if (s_shot_hook != NULL && s_shot_hook(path)) {
        return; /* recorded: the submitting thread asks again in order */
    }
    if (__atomic_load_n(&s_named_shot, __ATOMIC_ACQUIRE)) {
        return; /* the last one is not taken yet */
    }
    snprintf(s_named_path, sizeof(s_named_path), "%s", path);
    __atomic_store_n(&s_named_shot, 1, __ATOMIC_RELEASE);
}

/* At submit (recording thread): the request goes with the next record that presents, not with whichever record
 * the render thread replays next, so the picture is the same frame however far the replay lags. */
static void shot_attach(Record* r) {
    r->shot = 0;
    if (r->present && __atomic_load_n(&s_named_shot, __ATOMIC_ACQUIRE)) {
        memcpy(r->shot_path, s_named_path, sizeof(r->shot_path));
        r->shot = 1;
        __atomic_store_n(&s_named_shot, 0, __ATOMIC_RELEASE);
    }
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
    /* through plat_log_always: in retail mode too, the line tells a runner the shot is queued (determ_ab.py) */
    plat_log_always("shot %s: this record %lu bytes %lu vertices; %lu draws, %lu vertices, %lu states, %lu record bytes, %lu segments", path,
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
    s_noflush = plat_switch_int("gxnoflush", 0);
    s_state_delta = plat_switch_int("gxstatedelta", 1) != 0;
    s_urow = plat_switch_int("gxurow", 1) != 0;
    s_sync_all = plat_switch_int("gpusyncall", 0) != 0;
    s_gpu_fence = plat_switch_int("gpufence", 1) != 0;
    s_direct = plat_switch_int("gxdirect", 1);
    s_onehop = plat_switch_int("gxonehop", 0);
    if (s_onehop < 0 || s_onehop > 2) {
        s_onehop = 0;
    }
    s_one = plat_switch_int("gxone", 0);
    if (s_one < 0 || s_one > 2) {
        s_one = 0;
    }
    if (s_one) {
        if (one_init()) {
            s_onehop = 1; /* gxone builds on gxonehop=1 (its verify mode is gxone=2) */
        } else {
            s_one = 0;
        }
    }
    s_chunk = plat_switch_int("gxchunk", 0);
    if (s_chunk < 0 || s_chunk > 2) {
        s_chunk = 0;
    }
    if (s_chunk && (s_one != 1 || !s_gpu_fence || plat_switch_has("gpudrawcut") || plat_switch_has("gpudrawskip"))) {
        PLOG_W("gxchunk: off (it needs gxone=1 and gpufence=1, and no gpudrawcut/gpudrawskip)");
        s_chunk = 0;
    }
    if (s_onehop) {
        s_direct = 1; /* gxonehop implies gxdirect=1 (its verify mode is gxonehop=2) */
    }
    if (s_direct < 0 || s_direct > 2 || (s_direct && !direct_init())) {
        s_direct = 0;
    }
    if (s_onehop && (!s_direct || !oh_init())) {
        s_onehop = 0;
        if (s_direct && !dtab_alloc()) {
            s_direct = 0; /* gxonehop=1 had left the replay's tables out */
        }
    }
    if (!s_onehop && s_one) {
        PLOG_W("gxone: off (gxonehop could not start)");
        s_one = 0;
    }
    s_immfl = plat_switch_int("gximmfl", 0);
    if (s_immfl < 0 || s_immfl > 2) {
        s_immfl = 0;
    }
    if (s_immfl && !s_chunk) {
        PLOG_W("gximmfl: off (it needs gxchunk=1 or 2)");
        s_immfl = 0;
    }
    if (s_chunk && (!s_one || !ck_init())) {
        s_chunk = 0;
        s_immfl = 0;
    }
    if (s_immfl) {
        PLOG_I("gximmfl=%d: immediate draws straight into the frame list%s", s_immfl,
               s_immfl == 2 ? ", their words compared with the old path's through the register model" : "");
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
