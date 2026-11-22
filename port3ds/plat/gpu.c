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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "gx_shbin.h"
#include "plat/gpu.h"
#include "plat/hud.h"
#include "plat_internal.h"

#define TARGET_W 512  /* rotated render target, power of two so it can be sampled */
#define TARGET_H 1024
#define REGION_W 480  /* the part shown: twice the top screen, rotated */
#define REGION_H 800
#define EFB_X_OFFSET 96 /* (800 - 608) / 2: TTYD's 608-wide EFB centred */
#define MAX_TEX 4096
#define ARENA_VERTICES (96 * 1024)
#define RECORD_BYTES (512u << 10) /* two records fill the 1 MiB gx record budget in QTM */

enum { CMD_STATE = 1, CMD_FOG, CMD_DRAW, CMD_CLEAR, CMD_COPY, CMD_FRAME_END, CMD_SEGMENT_END };

typedef struct {
    u16 type;
    u16 size;
} CmdHead;

typedef struct {
    u8* data;
    u32 used;
    PlatGpuVertex* verts;
    u32 nverts;
    int present;
} Record;

typedef struct {
    C3D_Tex tex;
    u8 live;
    u8 is_target;
    u8 pending_free;
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
static PlatGpuStats s_stats, s_stats_frame;
static int s_rot;              /* orientation mode, see efb_to_target */
static u32 s_shots_every, s_shots_left;

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

static void* rec_put(u16 type, u32 size) {
    Record* r = &s_rec[s_cur];
    u32 need = (u32)sizeof(CmdHead) + ((size + 3u) & ~3u);
    /* keep room for the end marker; a full record is drawn as a segment */
    if (type != CMD_SEGMENT_END && type != CMD_FRAME_END && r->used + need + 64 > RECORD_BYTES) {
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
    return h + 1;
}

static void wait_idle(void) {
    while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) {
        LightEvent_Wait(&s_done);
    }
}

static void render_record(Record* r);

static void submit(int present) {
    Record* r = &s_rec[s_cur];
    r->present = present;
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
    s_rec[s_cur].used = 0;
    s_rec[s_cur].nverts = 0;
    memset(&s_last_state, 0xFF, sizeof(s_last_state));
    s_last_state_valid = 0;
    s_last_fog_hash = 0;
}

/* The record is full mid-frame: hand it to the render thread as a segment
 * (drawn, not presented) and go on in the other one, with the current GX
 * state put back at its start. */
static void end_segment(void) {
    /* The replay skips draws until a record's first state command, so the
     * state in force goes at the start of the next record too. */
    PlatGpuState keep = s_last_state;
    int have_state = s_last_state_valid;
    rec_put(CMD_SEGMENT_END, 0);
    submit(0);
    if (have_state) {
        plat_gpu_set_state(&keep);
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
    if (r->nverts + count > ARENA_VERTICES) {
        flush_segment();
        r = &s_rec[s_cur];
    }
    PlatGpuVertex* v = r->verts + r->nverts;
    r->nverts += count;
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

void plat_gpu_set_state(const PlatGpuState* state) {
    if (state->fog_enable) {
        u32 h = fog_hash(state);
        if (h != s_last_fog_hash) {
            float* lut = rec_put(CMD_FOG, sizeof(state->fog_lut) + 8);
            if (lut != NULL) {
                memcpy(lut, state->fog_lut, sizeof(state->fog_lut));
                s_last_fog_hash = h;
            }
        }
    }
    /* compare everything except the LUT */
    size_t cmp = offsetof(PlatGpuState, fog_lut);
    if (memcmp(&s_last_state, state, cmp) == 0 &&
        memcmp((const u8*)&s_last_state + offsetof(PlatGpuState, scissor_x0),
               (const u8*)state + offsetof(PlatGpuState, scissor_x0),
               sizeof(PlatGpuState) - offsetof(PlatGpuState, scissor_x0)) == 0) {
        return;
    }
    PlatGpuState* dst = rec_put(CMD_STATE, sizeof(PlatGpuState));
    if (dst != NULL) {
        *dst = *state;
        s_last_state = *state;
        s_last_state_valid = 1;
        s_stats_frame.state_changes++;
    }
}

void plat_gpu_draw(const PlatGpuVertex* vertices, uint32_t count) {
    if (vertices == NULL || count == 0) {
        return;
    }
    u32* d = rec_put(CMD_DRAW, 8);
    if (d != NULL) {
        d[0] = (u32)(vertices - s_rec[s_cur].verts);
        d[1] = count;
        s_stats_frame.draws++;
        s_stats_frame.vertices += count;
    }
}

void plat_gpu_clear(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t rgba, uint32_t depth24, int clear_color,
                    int clear_depth) {
    u32* d = rec_put(CMD_CLEAR, 16);
    if (d != NULL) {
        d[0] = (u32)x | (u32)y << 16;
        d[1] = (u32)w | (u32)h << 16;
        d[2] = rgba;
        d[3] = depth24 | (u32)(clear_color ? 1u : 0u) << 24 | (u32)(clear_depth ? 1u : 0u) << 25;
    }
}

void plat_gpu_copy_tex(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int half, uint32_t handle, uint32_t dst_w,
                       uint32_t dst_h) {
    u32* d = rec_put(CMD_COPY, 20);
    if (d != NULL) {
        d[0] = (u32)x | (u32)y << 16;
        d[1] = (u32)w | (u32)h << 16;
        d[2] = (u32)half;
        d[3] = handle;
        d[4] = dst_w | dst_h << 16;
    }
}

void plat_gpu_frame_begin(void) {}

static uint32_t s_presents;
uint32_t plat_gpu_present_count(void) { return s_presents; }

void plat_gpu_frame_end(uint16_t efb_w, uint16_t efb_h) {
    s_presents++;
    (void)efb_w;
    (void)efb_h;
    rec_put(CMD_FRAME_END, 0);
    s_stats_frame.record_bytes = s_rec[s_cur].used;
    submit(1);
    s_stats.draws = s_stats_frame.draws;
    s_stats.vertices = s_stats_frame.vertices;
    s_stats.state_changes = s_stats_frame.state_changes;
    s_stats.record_bytes = s_stats_frame.record_bytes;
    s_stats.segments = s_stats_frame.segments;
    memset(&s_stats_frame, 0, sizeof(s_stats_frame));
    s_frame_counter++;
}

void plat_gpu_sync(void) {
    end_segment();
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
static void reap_targets(void) {
    LightLock_Lock(&s_tex_lock);
    for (u32 i = 1; i < MAX_TEX; i++) {
        TexSlot* t = &s_tex[i];
        if (t->pending_free && t->target != NULL && s_frame_counter >= t->free_after_frame) {
            C3D_RenderTargetDelete(t->target);
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof(*t));
        }
    }
    LightLock_Unlock(&s_tex_lock);
}

uint32_t plat_gpu_tex_create(uint32_t width, uint32_t height, uint32_t fmt, const void* pixels, uint32_t bytes) {
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

uint32_t plat_gpu_tex_create_target(uint32_t width, uint32_t height, uint32_t fmt) {
    u32 h = tex_alloc_slot();
    if (h == 0) {
        return 0;
    }
    TexSlot* t = &s_tex[h];
    /* VRAM when it has room (the EFB and its depth buffer take 4 of its
     * 6 MB), otherwise linear FCRAM, which the GPU also renders to. */
    if (!C3D_TexInitVRAM(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt) &&
        !C3D_TexInit(&t->tex, (u16)width, (u16)height, (GPU_TEXCOLOR)fmt)) {
        t->live = 0;
        return 0;
    }
    t->is_target = 1;
    return h;
}

void plat_gpu_tex_free(uint32_t handle) {
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

static void apply_state(const PlatGpuState* st) {
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
    for (int u = 0; u < 3; u++) {
        const PlatGpuTexUnit* tu = &st->tex[u];
        if (tu->handle == 0 || tu->handle >= MAX_TEX || !s_tex[tu->handle].tex.data) {
            C3D_TexBind(u, &s_blank_tex);
            continue;
        }
        C3D_Tex* t = &s_tex[tu->handle].tex;
        C3D_TexSetFilter(t, tu->mag_linear ? GPU_LINEAR : GPU_NEAREST, tu->min_linear ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetWrap(t, (GPU_TEXTURE_WRAP_PARAM)tu->wrap_s, (GPU_TEXTURE_WRAP_PARAM)tu->wrap_t);
        C3D_TexBind(u, t);
    }
    C3D_AlphaTest(st->alpha_test_enable, (GPU_TESTFUNC)st->alpha_test_func, st->alpha_test_ref);
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
    C3D_FogGasMode(st->fog_enable ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (st->fog_enable) {
        C3D_FogColor(st->fog_color);
    }
    float tx, ty, tw, th;
    efb_to_target_rect(st->viewport_x, st->viewport_y, st->viewport_w, st->viewport_h, &tx, &ty, &tw, &th);
    if (tw >= 1 && th >= 1) {
        C3D_SetViewport((u32)tx, (u32)ty, (u32)tw, (u32)th);
    }
    float sx, sy, sw, sh;
    efb_to_target_rect(st->scissor_x0, st->scissor_y0, (float)(st->scissor_x1 - st->scissor_x0),
                       (float)(st->scissor_y1 - st->scissor_y0), &sx, &sy, &sw, &sh);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)sx, (u32)sy, (u32)(sx + sw), (u32)(sy + sh));
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

static void copy_to_texture(u16 x, u16 y, u16 w, u16 h, u32 handle, u32 dst_w, u32 dst_h) {
    if (handle == 0 || handle >= MAX_TEX) {
        return;
    }
    TexSlot* slot = &s_tex[handle];
    if (slot->target == NULL) {
        slot->target = C3D_RenderTargetCreateFromTex(&slot->tex, GPU_TEXFACE_2D, 0, -1);
        if (slot->target == NULL) {
            PLOG_E("gpu: copy target %lu cannot be rendered to", (unsigned long)handle);
            return;
        }
    }
    C3D_FrameDrawOn(slot->target);
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
    C3D_SetViewport(0, 0, slot->tex.width, slot->tex.height);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* The copy texture holds the rectangle upright in GX texture space:
     * texel (s, t) of the dw x dh image is EFB pixel (x + s * w, y + t * h). */
    float fx = (float)dst_w / slot->tex.width, fy = (float)dst_h / slot->tex.height;
    float tx, ty, tw, th;
    efb_to_target_rect(x, y, w, h, &tx, &ty, &tw, &th);
    float u0 = tx / s_efb_tex.width, u1 = (tx + tw) / s_efb_tex.width;
    float v0 = ty / s_efb_tex.height, v1 = (ty + th) / s_efb_tex.height;
    /* corners: GX (s, t) = (0,0) (1,0) (0,1) (1,1) -> EFB corners -> target uv */
    float cx[4] = {0, 1, 0, 1}, cy[4] = {0, 0, 1, 1};
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot0, 1, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_rot1, 0, 1, 0, 0);
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        float ex = cx[i], ey = cy[i]; /* in the copied rect, 0..1 */
        float ttx, tty, dummy0, dummy1;
        efb_to_target_rect(x + ex * w, y + ey * h, 0, 0, &ttx, &tty, &dummy0, &dummy1);
        float u = ttx / s_efb_tex.width, v = tty / s_efb_tex.height; /* viewport pixel -> texel (see present) */
        (void)u0;
        (void)u1;
        (void)v0;
        (void)v1;
        /* destination: GX t = 0 is the texture's last memory row, which is
         * viewport y = 0 (clip y = -1); the image fills the first dst_h
         * viewport rows */
        float px = -1.0f + 2.0f * ex * fx;
        float py = -1.0f + 2.0f * ey * fy;
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

static void take_shot(void);
static u32 s_shot_due;
static void capture_efb(const Record* r, const char* path);
static char s_named_path[48];
static int s_named_shot; /* plat_gpu_shot: the next presented frame goes to s_named_path */

/* The top screen's render target, linked to the framebuffer: citro3d
 * transfers it and swaps at C3D_FrameEnd. */
static C3D_RenderTarget* s_screen;

/* Present: the shown part of the EFB target (480x800, rotated as the
 * screen) drawn into the 240x400 screen target as one textured quad. The
 * bilinear sample at each screen pixel's centre falls on the corner shared
 * by its four EFB pixels, so the halving averages them. (A display
 * transfer of the EFB into a linear buffer followed by a TextureCopy into
 * the framebuffer came out with only its first quarter in Azahar.) */
static void present(void) {
    C3D_FrameDrawOn(s_screen);
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
    const float u1 = (float)REGION_W / TARGET_W, v1 = (float)REGION_H / TARGET_H;
    const float c[4][4] = {{-1, -1, 0, v1}, {1, -1, u1, v1}, {-1, 1, 0, 0}, {1, 1, u1, 0}};
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

static void render_record(Record* r) {
    u64 t0 = svcGetSystemTick();
    reap_targets();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameDrawOn(s_efb);
    C3D_BindProgram(&s_prog);
    C3D_SetAttrInfo(&s_attr);
    rot_uniforms();
    static C3D_BufInfo bi;
    BufInfo_Init(&bi);
    BufInfo_Add(&bi, r->verts, sizeof(PlatGpuVertex), 5, 0x43210);
    C3D_SetBufInfo(&bi);
    GSPGPU_FlushDataCache(r->verts, r->nverts * sizeof(PlatGpuVertex));
    u8* p = r->data;
    u8* end = r->data + r->used;
    /* A clear or a copy draws with its own state (immediate mode, which
     * also replaces the vertex buffer setup): the next draw puts the last
     * GX state and the frame's vertex buffer back. */
    const PlatGpuState* cur_state = NULL;
    int restore = 0;
    u32 n_draw = 0, n_clear = 0, n_copy = 0, n_skip = 0, after_last_draw = 0; /* for the screenshot log */
    while (p < end) {
        CmdHead* h = (CmdHead*)p;
        u8* body = p + sizeof(CmdHead);
        p = body + h->size;
        switch (h->type) {
        case CMD_STATE:
            cur_state = (const PlatGpuState*)body;
            apply_state(cur_state);
            restore = 0;
            break;
        case CMD_FOG: {
            static C3D_FogLut lut;
            FogLut_FromArray(&lut, (const float*)body);
            C3D_FogLutBind(&lut);
            break;
        }
        case CMD_DRAW: {
            const u32* d = (const u32*)body;
            if (cur_state == NULL) {
                n_skip++;
            }
            if (cur_state != NULL && d[1] >= 3) {
                if (restore) {
                    apply_state(cur_state);
                    restore = 0;
                }
                C3D_DrawArrays(GPU_TRIANGLES, (int)d[0], (int)(d[1] - d[1] % 3));
                n_draw++;
                after_last_draw = 0;
            }
            break;
        }
        case CMD_CLEAR: {
            const u32* d = (const u32*)body;
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
            copy_to_texture((u16)d[0], (u16)(d[0] >> 16), (u16)d[1], (u16)(d[1] >> 16), d[3], d[4] & 0xFFFF,
                            d[4] >> 16);
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
    if (r->present) {
        present();
    }
    C3D_FrameEnd(0);
    s_stats.render_us = (u32)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000u));
    s_stats.gpu_us = (u32)(C3D_GetDrawingTime() * 1000.0f);
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
        }
    }
    if (r->present && __atomic_load_n(&s_named_shot, __ATOMIC_ACQUIRE)) {
        capture_efb(r, s_named_path);
        __atomic_store_n(&s_named_shot, 0, __ATOMIC_RELEASE);
    }
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
        render_record(&s_rec[__atomic_load_n(&s_render_idx, __ATOMIC_ACQUIRE)]);
        __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
        LightEvent_Signal(&s_done);
    }
}

/* ---- Pixel peek ---- */

uint32_t plat_gpu_peek_argb(uint16_t x, uint16_t y) {
    plat_gpu_sync();
    float tx, ty, tw, th;
    efb_to_target_rect(x, y, 1, 1, &tx, &ty, &tw, &th);
    u32 px = (u32)tx, py = TARGET_H - 1 - (u32)ty; /* viewport y counts up from the last memory row */
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
    if (frame % s_shots_every != 0 || s_shots_left == 0) {
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

/* The EFB (608x480, as the game drew it) to a PPM file. */
static void capture_efb(const Record* r, const char* path) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    /* The GPU copies the EFB to linear memory first: a CPU read of VRAM can
     * miss what the GPU drew (Azahar keeps rendered surfaces on the host GPU
     * until a GPU transfer reads them). */
    const u32 bytes = TARGET_W * TARGET_H * 4;
    static u32* buf;
    if (buf == NULL) {
        buf = (u32*)linearAlloc(bytes);
        if (buf == NULL) {
            fclose(f);
            PLOG_W("shot %s: no linear memory", path);
            return;
        }
    }
    GSPGPU_InvalidateDataCache(buf, bytes);
    const u32 dim = GX_BUFFER_DIM(TARGET_W, TARGET_H);
    C3D_SyncDisplayTransfer((u32*)s_efb->frameBuf.colorBuf, dim, buf, dim,
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(buf, bytes);
    fprintf(f, "P6\n608 480\n255\n");
    static u8 row[608 * 3];
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 608; x++) {
            float tx, ty, tw, th;
            efb_to_target_rect((float)x, (float)y, 1, 1, &tx, &ty, &tw, &th);
            u32 px = (u32)tx, py = TARGET_H - 1 - (u32)ty;
            u32 v = buf[py * TARGET_W + px]; /* untiled by the transfer; RGBA8: R in the top byte */
            row[x * 3 + 0] = (u8)(v >> 24);
            row[x * 3 + 1] = (u8)(v >> 16);
            row[x * 3 + 2] = (u8)(v >> 8);
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
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
    s_dead = 1;
    C3D_Fini();
    gfxExit();
}

static void gpu_shutdown(void) {
    wait_idle();
    C3D_Fini();
    gfxExit();
}

void plat_gpu_init(void) {
    LightLock_Init(&s_tex_lock);
    LightEvent_Init(&s_submit, RESET_ONESHOT);
    LightEvent_Init(&s_done, RESET_STICKY);
    gfxInitDefault();
    gfxSet3D(false);
    plat_console_init();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8)) {
        plat_fatal("gpu: citro3d init failed");
    }
    plat_set_gpu_shutdown(gpu_shutdown);
    plat_set_gpu_abort(gpu_abort);
    s_rot = plat_switch_int("rot", 0) & 3;
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
    C3D_RenderTargetSetOutput(s_screen, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                  GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                  GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    /* the EFB's color buffer viewed as a texture, for EFB copies */
    memset(&s_efb_tex, 0, sizeof(s_efb_tex));
    s_efb_tex.data = s_efb->frameBuf.colorBuf;
    s_efb_tex.fmt = GPU_RGBA8;
    s_efb_tex.width = TARGET_W;
    s_efb_tex.height = TARGET_H;
    s_efb_tex.size = TARGET_W * TARGET_H * 4;
    s_efb_tex.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                      GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);

    s_dvlb = DVLB_ParseFile((u32*)gx_shbin, gx_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    s_uloc_rot0 = shaderInstanceGetUniformLocation(s_prog.vertexShader, "rot0");
    s_uloc_rot1 = shaderInstanceGetUniformLocation(s_prog.vertexShader, "rot1");
    AttrInfo_Init(&s_attr);
    AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&s_attr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&s_attr, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&s_attr, 4, GPU_FLOAT, 2);

    for (int i = 0; i < 2; i++) {
        s_rec[i].data = plat_place_alloc(PLAT_PLACE_GX_RECORD, RECORD_BYTES, 64);
        s_rec[i].verts = linearAlloc(ARENA_VERTICES * sizeof(PlatGpuVertex));
        if (s_rec[i].data == NULL || s_rec[i].verts == NULL) {
            plat_fatal("gpu: no memory for frame records");
        }
    }
    memset(&s_last_state, 0xFF, sizeof(s_last_state));
    s_shots_every = (u32)plat_switch_int("shots", 0);
    s_shots_left = (u32)plat_switch_int("shotmax", 200);
    char dumpcopy[32];
    if (s_shots_every || plat_switch_value("gxdumpcopy", dumpcopy, sizeof(dumpcopy))) { /* its shots go there too */
        mkdir("shots", 0777);
    }
    s_threaded = !plat_switch_has("gxsync");
    if (s_threaded) {
        s_thread = threadCreate(render_thread, NULL, 64 * 1024, 0x2E, plat_is_new3ds() ? 2 : 1, false);
        if (s_thread == NULL) {
            PLOG_W("gpu: no render thread on core 2, rendering on the game thread");
            s_threaded = 0;
        }
    }
    PLOG_I("gpu: EFB %dx%d (rotated), render thread %s, rot %d", TARGET_W, TARGET_H, s_threaded ? "on core 2" : "off",
           s_rot);
    if (plat_switch_has("gputest")) {
        gpu_selftest();
    }
}
