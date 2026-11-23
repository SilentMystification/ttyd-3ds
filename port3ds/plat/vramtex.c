/* VRAM texture tier and texture census (docs/PORT_3DS_QTM.md, "VRAM").
 *
 * Every texture a game uploads lives in linear FCRAM (C3D_TexInit), so the
 * GPU reads all of them over the FCRAM bus, while about 1.4 MiB of VRAM is
 * free beside the render targets. With the switch vramtex=1 the hottest
 * textures get a second copy in VRAM and the GPU samples that one:
 *
 *   - a texture is a candidate once it has lived VT_MIN_AGE frames and was
 *     bound in VT_MIN_FRAMES of them (a texture the game rewrites is freed
 *     and created again each time, so it never qualifies and stays linear);
 *   - after a frame is queued, the hottest candidates of that frame are
 *     copied linear -> VRAM by the GPU (GX_TextureCopy, at most vramtexrate
 *     KiB and VT_MAX_COPIES textures a frame) and their data pointer is
 *     swapped;
 *   - the linear copy is kept, so eviction is a pointer swap: textures not
 *     bound for VT_COLD frames make room, and when a VRAM allocation of the
 *     renderer fails (a copy target) everything is evicted, the VRAM is given
 *     back and the tier pauses. Render targets always win.
 *
 * The tier's VRAM is one block per bank, taken once and sub-allocated here,
 * so small textures never fragment the VRAM heap the copy targets use.
 *
 * It hooks citro3d by linker --wrap (C3D_TexInitWithParams, C3D_TexBind,
 * C3D_TexDelete, C3D_FrameEnd), so the renderer needs no change; the four
 * vt_* functions are what direct calls from the renderer would use instead.
 * Rule for the renderer: nothing writes a non-target texture's data after it
 * was created.
 *
 * vramstats=N logs a census every N frames, with the tier on or off: the
 * textures bound per frame and their bytes by PICA format, split by where
 * the GPU reads them (linear FCRAM or VRAM).
 *
 * Default off: whether the GPU reads VRAM faster than FCRAM under load is a
 * console measurement. Azahar shows only that the picture is unchanged. */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat_internal.h"

#define VT_SLOTS 2048u /* hash slots (power of two); at most half are used */
#define VT_MIN_AGE 30u
#define VT_MIN_FRAMES 8u
#define VT_COLD 120u
#define VT_MAX_COPIES 4
#define VT_ZOMBIES 128
#define VT_ARENAS 2
#define VT_ALIGN 0x80u
#define VT_FREE_DELAY 3u /* frames between an eviction and the reuse of its VRAM */
#define VT_PAUSE 600u    /* frames without the tier after a failed target allocation */
#define VT_FORMATS 14

typedef struct {
    C3D_Tex* tex; /* key; NULL: free slot */
    void* lin;    /* the linear data (always kept) */
    void* vram;   /* the VRAM copy the GPU samples, or NULL */
    u32 bytes;    /* all levels */
    u32 born, last_frame;
    u32 heat; /* binds per frame, smoothed, 8.8 */
    u16 frame_binds;
    u16 frames_used;
    u8 arena;
} VtEnt;

typedef struct {
    u8* base;
    u32 size;
    int release; /* give the block back once nothing is in it */
} VtArena;

typedef struct {
    u8 arena;
    u32 off, size, frame;
} VtZombie;

bool __real_C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p);
void __real_C3D_TexBind(int unitId, C3D_Tex* tex);
void __real_C3D_TexDelete(C3D_Tex* tex);
void __real_C3D_FrameEnd(u8 flags);

static int s_mode = -1; /* 0 off, 1 census only, 2 tier on */
static LightLock s_lock;
static VtEnt* s_ent;
static u32 s_count;
static u32 s_frame;
static C3D_Tex* s_bound[3];
static C3D_Tex* s_vram_failed; /* the texture whose VRAM allocation just failed: a copy target */
static VtArena s_arena[VT_ARENAS];
static int s_arenas_made;
static VtZombie s_zombie[VT_ZOMBIES];
static u32 s_zombies;
static volatile int s_pressure;
static u32 s_pause_until;
static u32 s_reserve, s_cap, s_rate, s_budget;
static u32 s_stats_every;
/* census, summed over the interval */
static struct {
    u32 frames;
    u32 tex_lin, tex_vram;          /* distinct tracked textures bound per frame */
    u64 bytes_lin, bytes_vram;      /* their bytes */
    u64 fmt_lin[VT_FORMATS], fmt_vram[VT_FORMATS];
    u32 binds, binds_other;
    u32 promoted, evicted, pressure;
    u64 promoted_bytes;
    float cmdbuf; /* sum of citro3d's command buffer usage (fraction of its active half) */
} s_st;

static const char* const k_fmt[VT_FORMATS] = {"RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8",
                                              "L8",    "A8",   "LA4",      "L4",     "A4",    "ETC1", "ETC1A4"};

static void vt_init(void) {
    LightLock_Init(&s_lock);
    s_stats_every = (u32)plat_switch_int("vramstats", 0);
    int on = plat_switch_int("vramtex", 0) != 0;
    if (on && plat_switch_int("vramfail", 0) == 1) {
        PLOG_W("vramtex: off with vramfail=1 (copy targets in linear memory cannot be told from textures)");
        on = 0;
    }
    s_reserve = (u32)plat_switch_int("vramtexreserve", 512) << 10;
    s_cap = (u32)plat_switch_int("vramtexmax", 256);
    if (s_cap > 1024) {
        s_cap = 1024; /* the stereo colour snapshot (2 MiB, written by the GPU) must never qualify */
    }
    s_cap <<= 10;
    s_rate = (u32)plat_switch_int("vramtexrate", 256) << 10;
    s_budget = (u32)plat_switch_int("vramtexkb", 6144) << 10;
    int mode = on ? 2 : s_stats_every != 0 ? 1 : 0;
    if (mode != 0) {
        s_ent = calloc(VT_SLOTS, sizeof(VtEnt));
        if (s_ent == NULL) {
            PLOG_W("vramtex: no memory for the table: off");
            mode = 0;
        }
    }
    if (mode == 2) {
        PLOG_I("vramtex: on, reserve %lu KB, textures up to %lu KB, %lu KB copied per frame at most",
               (unsigned long)(s_reserve >> 10), (unsigned long)(s_cap >> 10), (unsigned long)(s_rate >> 10));
    }
    s_mode = mode;
}

static inline int vt_on(void) {
    if (__builtin_expect(s_mode < 0, 0)) {
        vt_init();
    }
    return s_mode;
}

static inline u32 vt_hash(const C3D_Tex* tex) { return (u32)(((uintptr_t)tex >> 3) * 2654435761u) >> 21; }

static VtEnt* vt_find(const C3D_Tex* tex) {
    for (u32 i = vt_hash(tex) & (VT_SLOTS - 1);; i = (i + 1) & (VT_SLOTS - 1)) {
        if (s_ent[i].tex == tex) {
            return &s_ent[i];
        }
        if (s_ent[i].tex == NULL) {
            return NULL;
        }
    }
}

static void vt_remove(VtEnt* e) {
    /* linear probing with backward-shift deletion */
    u32 i = (u32)(e - s_ent);
    for (u32 j = (i + 1) & (VT_SLOTS - 1); s_ent[j].tex != NULL; j = (j + 1) & (VT_SLOTS - 1)) {
        u32 home = vt_hash(s_ent[j].tex) & (VT_SLOTS - 1);
        if (((j - home) & (VT_SLOTS - 1)) >= ((j - i) & (VT_SLOTS - 1))) {
            s_ent[i] = s_ent[j];
            i = j;
        }
    }
    memset(&s_ent[i], 0, sizeof(VtEnt));
    s_count--;
}

/* The VRAM of an evicted or deleted texture: free once the frames that may still sample it have left the GPU. */
static int vt_zombie(const VtEnt* e) {
    if (s_zombies >= VT_ZOMBIES) {
        return 0;
    }
    VtZombie* z = &s_zombie[s_zombies++];
    z->arena = e->arena;
    z->off = (u32)((u8*)e->vram - s_arena[e->arena].base);
    z->size = e->bytes;
    z->frame = s_frame;
    return 1;
}

static void vt_rebind(C3D_Tex* tex) {
    for (int u = 0; u < 3; u++) {
        if (s_bound[u] == tex) {
            __real_C3D_TexBind(u, tex); /* marks the unit dirty: the next draw sends the new address */
        }
    }
}

/* Back to the linear copy. 0 when the zombie list is full (nothing changed). */
static int vt_evict(VtEnt* e) {
    if (!vt_zombie(e)) {
        return 0;
    }
    e->tex->data = e->lin;
    e->vram = NULL;
    vt_rebind(e->tex);
    s_st.evicted++;
    return 1;
}

typedef struct {
    u32 off, end;
} VtSpan;

static int span_cmp(const void* a, const void* b) {
    u32 x = ((const VtSpan*)a)->off, y = ((const VtSpan*)b)->off;
    return x < y ? -1 : x > y;
}

/* First fit in an arena among the promoted textures and the zombies. */
static void* vt_alloc(u32 bytes, u8* arena_out) {
    static VtSpan spans[VT_SLOTS / 2 + VT_ZOMBIES];
    u32 need = (bytes + VT_ALIGN - 1) & ~(VT_ALIGN - 1);
    for (u32 a = 0; a < VT_ARENAS; a++) {
        if (s_arena[a].base == NULL || s_arena[a].release) {
            continue;
        }
        u32 n = 0;
        for (u32 i = 0; i < VT_SLOTS; i++) {
            if (s_ent[i].tex != NULL && s_ent[i].vram != NULL && s_ent[i].arena == a) {
                spans[n].off = (u32)((u8*)s_ent[i].vram - s_arena[a].base);
                spans[n].end = spans[n].off + ((s_ent[i].bytes + VT_ALIGN - 1) & ~(VT_ALIGN - 1));
                n++;
            }
        }
        for (u32 i = 0; i < s_zombies; i++) {
            if (s_zombie[i].arena == a) {
                spans[n].off = s_zombie[i].off;
                spans[n].end = spans[n].off + ((s_zombie[i].size + VT_ALIGN - 1) & ~(VT_ALIGN - 1));
                n++;
            }
        }
        qsort(spans, n, sizeof(VtSpan), span_cmp);
        u32 at = 0;
        for (u32 i = 0; i <= n; i++) {
            u32 limit = i < n ? spans[i].off : s_arena[a].size;
            if (limit >= at && limit - at >= need) {
                *arena_out = (u8)a;
                return s_arena[a].base + at;
            }
            if (i < n && spans[i].end > at) {
                at = spans[i].end;
            }
        }
    }
    return NULL;
}

static u32 vt_arena_used(u32 a) {
    u32 used = 0;
    for (u32 i = 0; i < VT_SLOTS; i++) {
        if (s_ent[i].tex != NULL && s_ent[i].vram != NULL && s_ent[i].arena == a) {
            used += s_ent[i].bytes;
        }
    }
    return used;
}

/* One block per VRAM bank, as large as the reserve and the budget allow. */
static void vt_make_arenas(void) {
    static const vramAllocPos k_bank[VT_ARENAS] = {VRAM_ALLOC_B, VRAM_ALLOC_A};
    u32 free_now = vramSpaceFree();
    u32 left = free_now > s_reserve ? free_now - s_reserve : 0;
    if (left > s_budget) {
        left = s_budget;
    }
    for (u32 a = 0; a < VT_ARENAS; a++) {
        s_arena[a].release = 0;
        if (s_arena[a].base != NULL) {
            continue;
        }
        for (u32 size = left & ~0xFFFFu; size >= 0x20000u; size -= 0x10000u) {
            void* p = vramAllocAt(size, k_bank[a]);
            if (p != NULL) {
                s_arena[a].base = p;
                s_arena[a].size = size;
                left -= size;
                break;
            }
        }
    }
    s_arenas_made = 1;
    PLOG_I("vramtex: VRAM %lu KB free before, tier blocks %lu + %lu KB, %lu KB left for targets",
           (unsigned long)(free_now >> 10), (unsigned long)(s_arena[0].size >> 10), (unsigned long)(s_arena[1].size >> 10),
           (unsigned long)(vramSpaceFree() >> 10));
}

static void vt_zombies_reap(void) {
    u32 n = 0;
    for (u32 i = 0; i < s_zombies; i++) {
        if (s_frame - s_zombie[i].frame < VT_FREE_DELAY) {
            s_zombie[n++] = s_zombie[i];
        }
    }
    s_zombies = n;
    for (u32 a = 0; a < VT_ARENAS; a++) {
        if (s_arena[a].base == NULL || !s_arena[a].release) {
            continue;
        }
        int busy = vt_arena_used(a) != 0;
        for (u32 i = 0; i < s_zombies; i++) {
            busy |= s_zombie[i].arena == a;
        }
        if (!busy) {
            vramFree(s_arena[a].base);
            memset(&s_arena[a], 0, sizeof(s_arena[a]));
            s_arenas_made = 0;
        }
    }
}

static void vt_stats_line(void) {
    u32 f = s_st.frames ? s_st.frames : 1;
    u32 live = 0, live_vram = 0;
    u64 live_bytes = 0, live_vram_bytes = 0, live_fmt[VT_FORMATS] = {0};
    for (u32 i = 0; i < VT_SLOTS; i++) {
        const VtEnt* e = &s_ent[i];
        if (e->tex == NULL) {
            continue;
        }
        live++;
        live_bytes += e->bytes;
        live_fmt[e->tex->fmt < VT_FORMATS ? e->tex->fmt : 0] += e->bytes;
        if (e->vram != NULL) {
            live_vram++;
            live_vram_bytes += e->bytes;
        }
    }
    PLOG_I("vramstats: frame %lu: per frame %lu textures bound from linear (%lu KB), %lu from VRAM (%lu KB), %lu binds "
           "(+%lu of targets); live %lu textures %lu KB, %lu of them in VRAM (%lu KB of %lu KB); "
           "promoted %lu (%lu KB), evicted %lu, target pressure %lu; VRAM free %lu KB",
           (unsigned long)s_frame, (unsigned long)(s_st.tex_lin / f), (unsigned long)(s_st.bytes_lin / f >> 10),
           (unsigned long)(s_st.tex_vram / f), (unsigned long)(s_st.bytes_vram / f >> 10), (unsigned long)(s_st.binds / f),
           (unsigned long)(s_st.binds_other / f), (unsigned long)live, (unsigned long)(live_bytes >> 10),
           (unsigned long)live_vram, (unsigned long)(live_vram_bytes >> 10),
           (unsigned long)((s_arena[0].size + s_arena[1].size) >> 10), (unsigned long)s_st.promoted,
           (unsigned long)(s_st.promoted_bytes >> 10), (unsigned long)s_st.evicted, (unsigned long)s_st.pressure,
           (unsigned long)(vramSpaceFree() >> 10));
    char line[320];
    int n = 0;
    for (int k = 0; k < VT_FORMATS && n < (int)sizeof(line) - 48; k++) {
        if (live_fmt[k] != 0 || s_st.fmt_lin[k] != 0 || s_st.fmt_vram[k] != 0) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %lu/%lu", k_fmt[k],
                          (unsigned long)((s_st.fmt_lin[k] + s_st.fmt_vram[k]) / f >> 10), (unsigned long)(live_fmt[k] >> 10));
        }
    }
    PLOG_I("vramstats:   KB by format (bound per frame/live):%s; command buffer %lu/1000 of its half per record",
           n ? line : " none", (unsigned long)(s_st.cmdbuf * 1000.0f / (float)f));
    memset(&s_st, 0, sizeof(s_st));
}

/* ---- The four entry points (what direct calls from a renderer would use) ---- */

/* A texture was created in linear memory and filled. */
static void vt_add(C3D_Tex* tex) {
    if (tex->data == NULL || s_count >= VT_SLOTS / 2) {
        return;
    }
    u32 i = vt_hash(tex) & (VT_SLOTS - 1);
    while (s_ent[i].tex != NULL) {
        i = (i + 1) & (VT_SLOTS - 1);
    }
    VtEnt* e = &s_ent[i];
    memset(e, 0, sizeof(*e));
    e->tex = tex;
    e->lin = tex->data;
    e->bytes = C3D_TexCalcTotalSize(tex->size, tex->maxLevel);
    e->born = e->last_frame = s_frame;
    s_count++;
}

/* A texture is about to be deleted: its data pointer is the linear one again. */
static void vt_forget(C3D_Tex* tex, int restore) {
    VtEnt* e = vt_find(tex);
    if (e == NULL) {
        return;
    }
    if (e->vram != NULL) {
        if (!vt_zombie(e)) {
            /* no room to remember the block: it stays allocated until the arena is released */
            s_arena[e->arena].release = 1;
            s_pressure = 1;
        }
        if (restore) {
            tex->data = e->lin;
        }
    }
    for (int u = 0; u < 3; u++) {
        if (s_bound[u] == tex) {
            s_bound[u] = NULL;
        }
    }
    vt_remove(e);
}

/* A texture is bound to a unit for the draws that follow. */
static void vt_use(int unit, C3D_Tex* tex) {
    if (unit >= 0 && unit < 3) {
        s_bound[unit] = tex;
    }
    VtEnt* e = vt_find(tex);
    if (e == NULL) {
        s_st.binds_other++;
        return;
    }
    s_st.binds++;
    if (e->frame_binds != 0xFFFF) {
        e->frame_binds++;
    }
    if (e->last_frame != s_frame || e->frames_used == 0) {
        e->last_frame = s_frame;
        if (e->frames_used != 0xFFFF) {
            e->frames_used++;
        }
        u32 k = tex->fmt < VT_FORMATS ? tex->fmt : 0;
        if (e->vram != NULL) {
            s_st.tex_vram++;
            s_st.bytes_vram += e->bytes;
            s_st.fmt_vram[k] += e->bytes;
        } else {
            s_st.tex_lin++;
            s_st.bytes_lin += e->bytes;
            s_st.fmt_lin[k] += e->bytes;
        }
    }
}

/* A frame was queued to the GPU (render thread, outside a frame). */
static void vt_frame(void) {
    u32 done = s_frame; /* the frame the binds since the last call belong to */
    s_frame++;
    s_st.frames++;
    vt_zombies_reap();
    if (s_pressure) {
        s_pressure = 0;
        s_st.pressure++;
        u32 n = 0;
        for (u32 i = 0; i < VT_SLOTS; i++) {
            if (s_ent[i].tex != NULL && s_ent[i].vram != NULL && vt_evict(&s_ent[i])) {
                n++;
            }
        }
        for (u32 a = 0; a < VT_ARENAS; a++) {
            s_arena[a].release = s_arena[a].base != NULL;
        }
        s_pause_until = s_frame + VT_PAUSE;
        PLOG_W("vramtex: a VRAM allocation of the renderer failed: %lu textures back to linear, VRAM released, "
               "tier paused for %u frames",
               (unsigned long)n, VT_PAUSE);
    }
    /* smooth the per-frame bind counts; find the candidates of the frame just queued */
    VtEnt* best[VT_MAX_COPIES] = {0};
    for (u32 i = 0; i < VT_SLOTS; i++) {
        VtEnt* e = &s_ent[i];
        if (e->tex == NULL) {
            continue;
        }
        e->heat = e->heat - (e->heat >> 3) + ((u32)e->frame_binds << 5);
        e->frame_binds = 0;
        if (s_mode != 2 || e->vram != NULL || e->last_frame != done || e->bytes > s_cap || e->bytes < 16 ||
            done - e->born < VT_MIN_AGE || e->frames_used < VT_MIN_FRAMES) {
            continue;
        }
        VtEnt* c = e;
        for (int k = 0; k < VT_MAX_COPIES; k++) {
            if (best[k] == NULL || c->heat > best[k]->heat) {
                VtEnt* t = best[k];
                best[k] = c;
                c = t;
                if (c == NULL) {
                    break;
                }
            }
        }
    }
    /* no GPU copies while the application does not hold the GPU (suspended, sleeping) */
    if (s_mode == 2 && best[0] != NULL && (s32)(s_frame - s_pause_until) >= 0 && gspHasGpuRight()) {
        if (!s_arenas_made) {
            vt_make_arenas();
        }
        u32 copied = 0;
        for (int k = 0; k < VT_MAX_COPIES && best[k] != NULL; k++) {
            VtEnt* e = best[k];
            if (copied + e->bytes > s_rate && copied != 0) {
                continue;
            }
            u8 arena = 0;
            void* v = vt_alloc(e->bytes, &arena);
            if (v == NULL) {
                /* make room from textures that went cold; their VRAM is free VT_FREE_DELAY frames from now */
                for (u32 i = 0; i < VT_SLOTS; i++) {
                    if (s_ent[i].tex != NULL && s_ent[i].vram != NULL && done - s_ent[i].last_frame >= VT_COLD) {
                        vt_evict(&s_ent[i]);
                    }
                }
                continue;
            }
            /* raw copy, contiguous (gap 0); the linear data was flushed when it was filled */
            u32 width = e->bytes & 0xFFFFu ? e->bytes & 0xFFFFu : 0x8000u;
            C3D_SyncTextureCopy((u32*)e->lin, GX_BUFFER_DIM(width, 0), (u32*)v, GX_BUFFER_DIM(width, 0), e->bytes, 8);
            e->vram = v;
            e->arena = arena;
            e->tex->data = v;
            vt_rebind(e->tex);
            copied += e->bytes;
            s_st.promoted++;
            s_st.promoted_bytes += e->bytes;
        }
    }
    if (s_stats_every != 0 && s_frame % s_stats_every == 0) {
        vt_stats_line();
    }
}

/* ---- citro3d hooks (linker --wrap) ---- */

bool __wrap_C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p) {
    if (vt_on() == 0) {
        return __real_C3D_TexInitWithParams(tex, cube, p);
    }
    LightLock_Lock(&s_lock);
    vt_forget(tex, 0); /* a slot reused without a delete: its old data pointer is gone already */
    LightLock_Unlock(&s_lock);
    bool ok = __real_C3D_TexInitWithParams(tex, cube, p);
    LightLock_Lock(&s_lock);
    if (p.onVram) {
        s_vram_failed = ok ? NULL : tex;
        if (!ok && (s_arena[0].base != NULL || s_arena[1].base != NULL)) {
            s_pressure = 1; /* the caller falls back this once; the next frame gives the VRAM back */
        }
    } else {
        if (ok && cube == NULL && p.type == GPU_TEX_2D && s_vram_failed != tex) {
            vt_add(tex);
        }
        s_vram_failed = NULL; /* else: a copy target in linear memory, which the GPU writes */
    }
    LightLock_Unlock(&s_lock);
    return ok;
}

void __wrap_C3D_TexBind(int unitId, C3D_Tex* tex) {
    if (vt_on() != 0 && tex != NULL) {
        LightLock_Lock(&s_lock);
        vt_use(unitId, tex);
        LightLock_Unlock(&s_lock);
    }
    __real_C3D_TexBind(unitId, tex);
}

void __wrap_C3D_TexDelete(C3D_Tex* tex) {
    if (vt_on() != 0) {
        LightLock_Lock(&s_lock);
        vt_forget(tex, 1);
        LightLock_Unlock(&s_lock);
    }
    __real_C3D_TexDelete(tex);
}

void __wrap_C3D_FrameEnd(u8 flags) {
    __real_C3D_FrameEnd(flags);
    if (vt_on() != 0) {
        LightLock_Lock(&s_lock);
        s_st.cmdbuf += C3D_GetCmdBufUsage();
        vt_frame();
        LightLock_Unlock(&s_lock);
    }
}
