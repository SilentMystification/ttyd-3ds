/* GX textures: texture and palette objects, GameCube texel decoding, and
 * the cache of converted PICA200 textures.
 *
 * A texture is converted the first time a draw uses it: decoded from its
 * GameCube format (big-endian, tiled) to RGBA8, put in the smallest PICA
 * format that holds it exactly (LA8 for grey, RGB565 for opaque, RGBA5551
 * for one-bit alpha, else RGBA8; the same choice as the Animal Crossing 3DS
 * port), resampled to power-of-two sizes when needed and Morton-tiled. The
 * cache is keyed by image address, format, size and palette. Game code
 * rewrites some textures in place (fonts, generated images), so an entry is
 * re-validated by a hash of its texels the first time it is used in a
 * frame; entries unused for 120 frames are freed. Textures made by
 * GXCopyTex are GPU render targets and are used directly.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"
#include "plat/job.h"
#include "plat/memstable.h"
#include "plat/options.h"
#include "plat/plat.h"

_Static_assert(sizeof(GxTexObj) <= sizeof(GXTexObj), "GxTexObj must fit GXTexObj");
_Static_assert(sizeof(GxTlutObj) <= sizeof(GXTlutObj), "GxTlutObj must fit GXTlutObj");

#define CACHE_SIZE 1024
#define EVICT_FRAMES 120

typedef struct {
    const void* image;
    u16 w, h;
    u8 fmt, is_ci;
    u16 tlut_entries;
    const void* tlut;
    u32 tlut_fmt;
    u32 hash;
    u32 handle;
    u16 tex_w, tex_h;
    u32 last_frame;   /* frame of the last use */
    u32 checked_frame;/* frame of the last hash check */
    u32 trust_gen;    /* textrust: the memstable generation the image and TLUT are vouched stable in, or 0 */
    u32 trust_epoch;  /* epoch of the last refused trust request (asked again only once covered memory changed) */
} CacheEntry;

/* ---- textrust (switch, default 1): the content-stable memory registry (plat/memstable.h) as for display lists.
 * After a hash check (or the upload's hash) that began at epoch E, the entry asks the registry to vouch that its
 * image and TLUT spans lie in covered memory (disc fills) with no write, release or fill reported since E. While
 * the generation is unchanged the per-frame hash is skipped. Memory the game writes without DCFlushRange is
 * not revoked by a report; texverify=1 hashes anyway and counts trust decisions the hash contradicts
 * (texstats "stale", must be 0). */
static int s_trust_on = 1, s_verify, s_copy_evict = 1; /* switch copyevict: per-frame age eviction of copy slots */
static unsigned s_st_trusted, s_st_stale, s_st_grants;

/* Per cache entry, parallel to s_cache but on the heap (the exe's static data must stay under 16 MB). */
typedef struct {
    u8 levels;             /* mip levels after level 0 that the PICA texture has (texfilter=mip) */
    u32 bytes0, bytes_mip; /* PICA texture memory: level 0, and the levels after it */
} CacheExtra;
static CacheExtra* s_x;

/* ---- texframe=1: one log line per frame with the texture work of that frame (counts and game-thread time) ---- */
static int s_tf_on;
static int s_async;
static struct {
    unsigned queued, taken, stale_gen, verify_bad, dropped, unused, wait_n, inline_n, calls, rej_clut, rej_fmt, rej_size, rej_cov, rej_dup, rej_full;
    u64 wait_ticks;
} s_as;
static u32 s_pre_bytes_hw; /* texasync: the most its results took at once (bytes; below) */
static struct {
    unsigned lookups, uploads, hashes, hash_bytes, async_hit, async_miss;
    u64 t_total, t_dec, t_conv, t_create, t_hash;
} s_tf;
#define TF_T() (s_tf_on ? plat_ticks() : 0)

static CacheEntry s_cache[CACHE_SIZE];
static u32 s_frame = 1;
static u32 s_uploads;
static u8* s_rgba;      /* decode scratch (1024x1024 RGBA8) */
static u8* s_tiled;     /* level 0 of a texture with a mip chain (1024x1024 RGBA8), with the mip scratch; a texture
                         * without one is tiled straight into its own memory */

/* ---- texfilter: the sampling mode (option "texfilter", switch texfilter=, texlodbias=) ----
 * game: GX's filters as the game sets them; linear / nearest: forced; mip: bilinear plus a mip chain
 * (GX's own levels where the texture has them, else a box filter at upload) with a linear level filter.
 * Never applied (always "game"): GXCopyTex targets (screen effects, render to texture), textures that
 * a TEV stage reads as an indirect offset map, and textures with a side of 4 texels or less (ramps and
 * lookup strips, where filtering changes what the value means). A texture that GX gave as CI
 * (palette indexed) is decoded through its TLUT at upload, so what the unit samples is colour and
 * filtering it matches what GX's own bilinear did after the palette lookup. */
enum { TF_GAME, TF_LINEAR, TF_MIP, TF_NEAREST };
static u8 s_mode = TF_GAME;
static int s_mode_forced;      /* texfilter= switch given: the option is ignored */
static float s_lod_bias;       /* texlodbias= */
static u32 s_mip_extra_made;   /* uploads in mip mode, whose texture got a chain */
static u32 s_live_n, s_live_bytes, s_live_mip;
static u8 *s_mip_out, *s_mip_a, *s_mip_b; /* mip chain scratch, allocated on the first use of mip mode */

/* what gx_tex_handle_for found for the lookup in progress, read by gx_tex_unit_sampler */
static struct {
    u8 copy, levels;
} s_last;

/* texcensus=N: what TTYD asks for. Per distinct texture at upload and per sampler setup. */
static int s_census_every;
static u32 s_census_frames;
typedef struct {
    u32 up[6][3][2];   /* uploads: min filter (GX_NEAR..GX_LIN_MIP_LIN), mag (near/linear/other), has GX mips */
    u32 use[6][3][2];  /* sampler setups (a draw's state build) */
    u32 copy_use, ind_use, tiny_use, up_total, use_total;
    u32 fmt[16], fmt_mip[16];
    u32 bias_nz, edge;
} Census;
static Census* s_cen; /* heap, only with texcensus */

/* Copies made by GXCopyTex: destination address -> render-target texture. */
typedef struct {
    const void* addr;
    u32 handle;
    u16 w, h;
    u8 fmt;
    u32 last_frame; /* last frame the slot was made or sampled */
} CopyEntry;
static CopyEntry s_copies[64];

/* ---- texstats=N: measurement of the per-frame texture re-validation ----
 * Every N frames logs the hashes made, bytes hashed, cache lookups and hash
 * mismatches (a cached texture whose bytes changed). The first mismatch of
 * each texture is logged with its address, size, format, memory and the
 * caller of the GXInitTexObj* call that last set that image address. */
extern void* OSGetArenaHi(void);
#include "port3ds/rel_static.h"
extern char __end__[];
#define ARENA_BYTES 0x013B4880u /* GAME_ARENA_BYTES in os.c */

static int s_stats_every;      /* 0 = off */
static u32 s_arena_lo, s_arena_hi;
static unsigned s_st_hashes, s_st_bytes, s_st_lookups, s_st_checks, s_st_mism, s_st_uploads, s_st_frames;
static unsigned s_st_tot_hashes, s_st_tot_bytes, s_st_tot_lookups, s_st_tot_mism, s_st_tot_frames;
static u32 s_dbg_tlut_hash[CACHE_SIZE];
static int s_compact;
static unsigned s_cv_n[16], s_cv_bad[16], s_cv_texels[16], s_cv_old_bad[16], s_cv_saved;


typedef struct {
    const void* image;
    void* init_ra;
    unsigned mism;
    unsigned interval_mism;
    u8 fmt;
    u16 w, h;
} StatTex;
#define STAT_TEX_MAX 512
static StatTex s_stat_tex[STAT_TEX_MAX];

static StatTex* stat_find(const void* image, int create) {
    u32 slot = ((u32)image >> 5) % STAT_TEX_MAX;
    for (u32 i = 0; i < 16; i++) {
        StatTex* st = &s_stat_tex[(slot + i) % STAT_TEX_MAX];
        if (st->image == image) {
            return st;
        }
        if (st->image == NULL) {
            if (create) {
                st->image = image;
            }
            return create ? st : NULL;
        }
    }
    return NULL;
}

static void stat_note_init(const void* image, void* ra) {
    if (s_stats_every && image != NULL) {
        StatTex* st = stat_find(image, 1);
        if (st != NULL) {
            st->init_ra = ra;
        }
    }
}

static const char* stat_region(u32 a, char* buf, size_t n) {
    for (int i = 0; i < port_rel_count; i++) {
        const PortRelStatic* r = &port_rel_table[i];
        if (a >= (u32)r->data_start && a < (u32)r->data_end) {
            snprintf(buf, n, "REL data (%s) +0x%x", r->name, (unsigned)(a - (u32)r->data_start));
            return buf;
        }
        if (a >= (u32)r->bss_start && a < (u32)r->bss_end) {
            snprintf(buf, n, "REL bss (%s) +0x%x", r->name, (unsigned)(a - (u32)r->bss_start));
            return buf;
        }
    }
    if (a >= 0x00100000u && a < (u32)__end__) {
        return "exe image (static data/bss)";
    }
    if (a >= s_arena_lo && a < s_arena_hi) {
        snprintf(buf, n, "game arena +0x%x", (unsigned)(a - s_arena_lo));
        return buf;
    }
    if (a >= 0x08000000u && a < 0x14000000u) {
        return "other heap";
    }
    if (a >= 0x14000000u && a < 0x40000000u) {
        return "linear";
    }
    return "unknown";
}

static void stat_mismatch(const CacheEntry* e, const GxTexObj* t, u32 bytes, int tlut_changed) {
    s_st_mism++;
    StatTex* st = stat_find(t->image, 1);
    if (st == NULL) {
        return;
    }
    if (st->mism == 0) {
        char rb[64];
        st->fmt = t->format;
        st->w = t->width;
        st->h = t->height;
        PLOG_I("texstats: FIRST CHANGE image %p %ux%u fmt %u ci %u bytes %u (%s) in %s; changed part: %s; init caller %p; "
               "frame %u",
               t->image, t->width, t->height, t->format, t->is_ci, (unsigned)bytes, "main memory",
               stat_region((u32)t->image, rb, sizeof(rb)), tlut_changed ? "TLUT" : "image", st->init_ra,
               (unsigned)s_frame);
    }
    st->mism++;
    st->interval_mism++;
    (void)e;
}

static void stat_frame_end(void) {
    s_st_frames++;
    if (s_compact == 2 && s_st_tot_frames % 120 == 119) {
        PLOG_I("texcompact: verify (new fmt: textures/texels/mismatched texels) L4 %u/%u/%u LA4 %u/%u/%u L8 %u/%u/%u RGBA4 %u/%u/%u; "
               "bytes saved at upload %u; baseline lossy textures (old fmt LA8 %u RGB565 %u 5551 %u)",
               s_cv_n[10], s_cv_texels[10], s_cv_bad[10], s_cv_n[9], s_cv_texels[9], s_cv_bad[9], s_cv_n[7], s_cv_texels[7],
               s_cv_bad[7], s_cv_n[4], s_cv_texels[4], s_cv_bad[4], s_cv_saved, s_cv_old_bad[5], s_cv_old_bad[3], s_cv_old_bad[2]);
    }
    s_st_tot_frames++;
    if (s_stats_every && s_st_frames >= (u32)s_stats_every) {
        unsigned f = s_st_frames;
        PLOG_I("texstats: frames %u: per frame hashes %u.%02u bytes %u lookups %u.%02u first-use-checks %u.%02u "
               "mismatches %u.%02u uploads %u.%02u trusted %u.%02u grants %u stale %u",
               (unsigned)f, s_st_hashes / f, s_st_hashes * 100 / f % 100, s_st_bytes / f, s_st_lookups / f,
               s_st_lookups * 100 / f % 100, s_st_checks / f, s_st_checks * 100 / f % 100, s_st_mism / f,
               s_st_mism * 100 / f % 100, s_st_uploads / f, s_st_uploads * 100 / f % 100, s_st_trusted / f,
               s_st_trusted * 100 / f % 100, s_st_grants, s_st_stale);
        s_st_tot_hashes += s_st_hashes;
        s_st_tot_bytes += s_st_bytes;
        s_st_tot_lookups += s_st_lookups;
        s_st_tot_mism += s_st_mism;
        for (int i = 0; i < STAT_TEX_MAX; i++) {
            StatTex* st = &s_stat_tex[i];
            if (st->interval_mism != 0) {
                PLOG_I("texstats:   changed %p %ux%u fmt %u: %u times in last %u frames (%u total)", st->image, st->w,
                       st->h, st->fmt, st->interval_mism, (unsigned)f, st->mism);
                st->interval_mism = 0;
            }
        }
        if (s_async) {
            PLOG_I("texasync: queued %u taken %u stale-gen %u verify-bad %u dropped %u unused %u, binds that waited %u "
                   "(%u us), decoded at the bind %u; prefetch calls %u: clut %u fmt %u size %u dup %u full %u uncovered %u; "
                   "results at most %u KB", s_as.queued, s_as.taken, s_as.stale_gen, s_as.verify_bad, s_as.dropped, s_as.unused,
                   s_as.wait_n, (unsigned)(s_as.wait_ticks / (PLAT_TICKS_PER_SEC / 1000000u)), s_as.inline_n, s_as.calls, s_as.rej_clut,
                   s_as.rej_fmt, s_as.rej_size, s_as.rej_dup, s_as.rej_full, s_as.rej_cov, (unsigned)(s_pre_bytes_hw >> 10));
        }
        s_st_hashes = s_st_bytes = s_st_lookups = s_st_checks = s_st_mism = s_st_uploads = s_st_frames = 0;
        s_st_trusted = s_st_stale = s_st_grants = 0;
    }
}

/* ---- Objects ---- */

static void tex_obj_init(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXTexFmt format,
                         GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap, void* ra) {
    stat_note_init(image_ptr, ra);
    GxTexObj* t = (GxTexObj*)obj;
    memset(obj, 0, sizeof(*obj));
    t->image = image_ptr;
    t->width = width;
    t->height = height;
    t->format = (u8)format;
    t->wrap_s = (u8)wrap_s;
    t->wrap_t = (u8)wrap_t;
    t->mipmap = mipmap;
    t->min_filter = mipmap ? GX_LIN_MIP_LIN : GX_LINEAR;
    t->mag_filter = GX_LINEAR;
    t->max_lod_q = mipmap ? 10 * 16 : 0;
}

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrap_s,
                  GXTexWrapMode wrap_t, u8 mipmap) {
    tex_obj_init(obj, image_ptr, width, height, format, wrap_s, wrap_t, mipmap, __builtin_return_address(0));
}

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrap_s,
                    GXTexWrapMode wrap_t, u8 mipmap, u32 tlut_name) {
    tex_obj_init(obj, image_ptr, width, height, (GXTexFmt)format, wrap_s, wrap_t, mipmap,
                 __builtin_return_address(0));
    GxTexObj* t = (GxTexObj*)obj;
    t->is_ci = 1;
    t->tlut_name = tlut_name;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso) {
    GxTexObj* t = (GxTexObj*)obj;
    t->min_filter = (u8)min_filt;
    t->mag_filter = (u8)mag_filt;
    t->min_lod_q = (u8)(min_lod < 0 ? 0 : min_lod > 10 ? 160 : min_lod * 16.0f);
    t->max_lod_q = (u8)(max_lod < 0 ? 0 : max_lod > 10 ? 160 : max_lod * 16.0f);
    t->lod_bias_q = (s8)(lod_bias < -4 ? -128 : lod_bias > 3.99f ? 127 : lod_bias * 32.0f);
    t->bias_clamp = bias_clamp;
    t->edge_lod = do_edge_lod;
    t->max_aniso = (u8)max_aniso;
}

void GXInitTexObjData(GXTexObj* obj, void* image_ptr) {
    stat_note_init(image_ptr, __builtin_return_address(0));
    ((GxTexObj*)obj)->image = image_ptr;
}
void* GXGetTexObjData(const GXTexObj* to) { return ((const GxTexObj*)to)->image; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* to) { return (GXTexFmt)((const GxTexObj*)to)->format; }
u16 GXGetTexObjWidth(const GXTexObj* to) { return ((const GxTexObj*)to)->width; }
u16 GXGetTexObjHeight(const GXTexObj* to) { return ((const GxTexObj*)to)->height; }

void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, GXTlutFmt fmt, u16 n_entries) {
    GxTlutObj* t = (GxTlutObj*)tlut_obj;
    memset(tlut_obj, 0, sizeof(*tlut_obj));
    t->data = lut;
    t->format = (u8)fmt;
    t->n_entries = n_entries;
}

void GXInitTexPreLoadRegion(GXTexRegion* region, u32 tmem_even, u32 size_even, u32 tmem_odd, u32 size_odd) {
    memset(region, 0, sizeof(*region));
    (void)tmem_even;
    (void)size_even;
    (void)tmem_odd;
    (void)size_odd;
}

/* Preloading into TMEM only changes where the GameCube keeps the texels;
 * the port converts from main memory either way. */
void GXPreLoadEntireTexture(GXTexObj* tex_obj, GXTexRegion* region) {
    GX_CLOSE_PRIMITIVE();
    (void)tex_obj;
    (void)region;
}

void GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion* region, GXTexMapID id) {
    GX_CLOSE_PRIMITIVE();
    (void)region;
    GXLoadTexObj(obj, id);
}

/* TMEM cache invalidation: the converted textures are re-validated by hash
 * once per frame, which covers what an invalidation is for. */
void GXInvalidateTexAll(void) {
    GX_CLOSE_PRIMITIVE();
}
void GXTexModeSync(void) {}

/* Bytes a texture takes in main memory (the SDK's GXGetTexBufferSize). */
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod) {
    u32 tile_w, tile_h, bpt = 32;
    switch (format) {
    case GX_TF_I4:
    case GX_TF_C4:
    case GX_TF_CMPR:
    case GX_CTF_R4:
    case GX_CTF_Z4:
        tile_w = 8;
        tile_h = 8;
        break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_C8:
    case GX_TF_Z8:
    case GX_CTF_RA4:
    case GX_CTF_A8:
    case GX_CTF_R8:
    case GX_CTF_G8:
    case GX_CTF_B8:
    case GX_CTF_Z8M:
    case GX_CTF_Z8L:
        tile_w = 8;
        tile_h = 4;
        break;
    case GX_TF_RGBA8:
    case GX_TF_Z24X8:
        tile_w = 4;
        tile_h = 4;
        bpt = 64;
        break;
    default:
        tile_w = 4;
        tile_h = 4;
        break;
    }
    u32 size = 0;
    u32 levels = mipmap ? max_lod : 1;
    if (levels == 0) {
        levels = 1;
    }
    for (u32 l = 0; l < levels; l++) {
        u32 tw = (width + tile_w - 1) / tile_w;
        u32 th = (height + tile_h - 1) / tile_h;
        size += tw * th * bpt;
        if (width == 1 && height == 1) {
            break;
        }
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return size;
}

/* ---- Decoding ---- */

static inline u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }

static void put(u8* out, u32 w, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
    u8* p = out + ((size_t)y * w + x) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

static void rgb5a3(u16 v, u8* c) {
    if (v & 0x8000) {
        c[0] = (u8)((v >> 10 & 31) * 255 / 31);
        c[1] = (u8)((v >> 5 & 31) * 255 / 31);
        c[2] = (u8)((v & 31) * 255 / 31);
        c[3] = 255;
    } else {
        c[0] = (u8)((v >> 8 & 15) * 17);
        c[1] = (u8)((v >> 4 & 15) * 17);
        c[2] = (u8)((v & 15) * 17);
        c[3] = (u8)((v >> 12 & 7) * 255 / 7);
    }
}

static void rgb565(u16 v, u8* c) {
    c[0] = (u8)((v >> 11 & 31) * 255 / 31);
    c[1] = (u8)((v >> 5 & 63) * 255 / 63);
    c[2] = (u8)((v & 31) * 255 / 31);
    c[3] = 255;
}

static void tlut_color(const u16* tlut, u32 tlut_fmt, u32 index, u8* c) {
    u16 v = be16((const u8*)&tlut[index]);
    switch (tlut_fmt) {
    case GX_TL_IA8:
        c[0] = c[1] = c[2] = (u8)v;
        c[3] = (u8)(v >> 8);
        break;
    case GX_TL_RGB565: rgb565(v, c); break;
    default: rgb5a3(v, c); break;
    }
}

/* Decodes a whole texture of the given GameCube format into RGBA8 rows. */
void gx_tex_decode_rgba8(const u8* src, u32 w, u32 h, u32 fmt, const u16* tlut, u32 tlut_fmt, u8* out) {
    u32 bw, bh;
    switch (fmt) {
    case GX_TF_I4:
    case GX_TF_C4:
    case GX_TF_CMPR: bw = 8; bh = 8; break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_C8: bw = 8; bh = 4; break;
    default: bw = 4; bh = 4; break;
    }
    const u8* p = src;
    for (u32 by = 0; by < h; by += bh) {
        for (u32 bx = 0; bx < w; bx += bw) {
            if (fmt == GX_TF_CMPR) {
                /* 2x2 DXT1 blocks per 8x8 tile */
                for (u32 sub = 0; sub < 4; sub++) {
                    u32 ox = bx + (sub & 1) * 4, oy = by + (sub >> 1) * 4;
                    u16 c0 = be16(p), c1 = be16(p + 2);
                    u8 pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; k++) {
                            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; k++) {
                            pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    for (u32 y = 0; y < 4; y++) {
                        u8 bits = p[4 + y];
                        for (u32 x = 0; x < 4; x++) {
                            u32 i = (bits >> (6 - 2 * x)) & 3;
                            if (ox + x < w && oy + y < h) {
                                put(out, w, ox + x, oy + y, pal[i][0], pal[i][1], pal[i][2], pal[i][3]);
                            }
                        }
                    }
                    p += 8;
                }
                continue;
            }
            if (fmt == GX_TF_RGBA8) {
                /* 32 bytes AR then 32 bytes GB */
                for (u32 i = 0; i < 16; i++) {
                    u32 x = bx + (i & 3), y = by + (i >> 2);
                    if (x < w && y < h) {
                        put(out, w, x, y, p[i * 2 + 1], p[32 + i * 2], p[32 + i * 2 + 1], p[i * 2]);
                    }
                }
                p += 64;
                continue;
            }
            for (u32 y = 0; y < bh; y++) {
                for (u32 x = 0; x < bw; x++) {
                    u32 px = bx + x, py = by + y;
                    u8 c[4];
                    switch (fmt) {
                    case GX_TF_I4: {
                        u8 b = p[(y * bw + x) / 2];
                        u8 i4 = (x & 1) ? (b & 15) : (b >> 4);
                        c[0] = c[1] = c[2] = c[3] = (u8)(i4 * 17);
                        break;
                    }
                    case GX_TF_I8: c[0] = c[1] = c[2] = c[3] = p[y * bw + x]; break;
                    case GX_TF_IA4: {
                        u8 b = p[y * bw + x];
                        c[0] = c[1] = c[2] = (u8)((b & 15) * 17);
                        c[3] = (u8)((b >> 4) * 17);
                        break;
                    }
                    case GX_TF_IA8: {
                        const u8* q = p + (y * bw + x) * 2;
                        c[0] = c[1] = c[2] = q[1];
                        c[3] = q[0];
                        break;
                    }
                    case GX_TF_RGB565: rgb565(be16(p + (y * bw + x) * 2), c); break;
                    case GX_TF_RGB5A3: rgb5a3(be16(p + (y * bw + x) * 2), c); break;
                    case GX_TF_C4: {
                        u8 b = p[(y * bw + x) / 2];
                        tlut_color(tlut, tlut_fmt, (x & 1) ? (b & 15) : (b >> 4), c);
                        break;
                    }
                    case GX_TF_C8: tlut_color(tlut, tlut_fmt, p[y * bw + x], c); break;
                    case GX_TF_C14X2: tlut_color(tlut, tlut_fmt, be16(p + (y * bw + x) * 2) & 0x3FFF, c); break;
                    default: c[0] = c[1] = c[2] = 255; c[3] = 255; break;
                    }
                    if (px < w && py < h) {
                        put(out, w, px, py, c[0], c[1], c[2], c[3]);
                    }
                }
            }
            switch (fmt) {
            case GX_TF_I4:
            case GX_TF_C4: p += 32; break;
            case GX_TF_I8:
            case GX_TF_IA4:
            case GX_TF_C8: p += 32; break;
            default: p += 32; break; /* IA8, RGB565, RGB5A3, C14X2: 4x4 x 2 bytes */
            }
        }
    }
}

/* ---- Conversion to PICA ---- */

static u32 pow2(u32 v) {
    u32 p = 8;
    while (p < v && p < 1024) {
        p <<= 1;
    }
    return p;
}

static u32 morton8(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

/* ---- texcompact (switch, default 0): smaller PICA formats where the texels are exactly representable ----
 * 0: the formats above (LA8 / RGB565 / RGBA5551 / RGBA8). 1: also L8 for an opaque grey texture (L8 reads as
 * L,L,L,255, the same as LA8 with alpha 255), LA4 / L4 for grey texels that are all multiples of 17 (the PICA
 * widens a nibble n to n*17, GX's I4/IA4 decode gives n*17 too) and RGBA4 where RGBA8 would be used and every
 * channel is a multiple of 17. 4-bit formats only without a mip chain (a filtered level is not a multiple of 17).
 * 2: as 1, and every upload is checked: both formats are put in tiled form and decoded as the sampler does; the
 * mismatches are counted per format ("texcompact: verify ..." in the log, must be 0). */

static u32 fmt_bits(u32 fmt) {
    switch (fmt) {
    case PLAT_TEXFMT_RGBA8: return 32;
    case PLAT_TEXFMT_L4: return 4;
    case PLAT_TEXFMT_L8:
    case PLAT_TEXFMT_LA4: return 8;
    default: return 16;
    }
}

/* Level 0 of a texture in this PICA format: gray, opaque, one-bit alpha or full. `mips`: 0 no chain, 1 a chain
 * made by box filter, 2 a chain taken from the game's own levels. */
static u32 pick_format_ex(const u8* px, u32 w, u32 h, int compact, int mips) {
    int gray = 1, opaque = 1, binary = 1, n17 = 1;
    for (u32 i = 0; i < w * h; i++) {
        const u8* p = px + i * 4;
        gray &= p[0] == p[1] && p[1] == p[2];
        opaque &= p[3] == 255;
        binary &= p[3] == 0 || p[3] == 255;
        n17 &= p[0] % 17 == 0 && p[1] % 17 == 0 && p[2] % 17 == 0 && p[3] % 17 == 0;
    }
    if (compact) {
        if (gray && n17 && mips == 0) {
            return opaque ? PLAT_TEXFMT_L4 : PLAT_TEXFMT_LA4;
        }
        if (gray && opaque && mips != 2) {
            return PLAT_TEXFMT_L8;
        }
    }
    u32 f = gray ? PLAT_TEXFMT_LA8 : opaque ? PLAT_TEXFMT_RGB565 : binary ? PLAT_TEXFMT_RGBA5551 : PLAT_TEXFMT_RGBA8;
    if (compact && f == PLAT_TEXFMT_RGBA8 && n17 && mips == 0) {
        f = PLAT_TEXFMT_RGBA4;
    }
    return f;
}
static u32 pick_format(const u8* px, u32 w, u32 h) { return pick_format_ex(px, w, h, s_compact, 0); }

/* RGBA8 rows (row 0 = GX t 0), w x h, resampled (nearest) to pw x ph and Morton tiled into `out`. */
static void tile_level(const u8* px, u32 w, u32 h, u32 pw, u32 ph, u32 fmt, u8* out) {
    u32 tiles_w = pw >> 3;
    if (fmt == PLAT_TEXFMT_L4) {
        memset(out, 0, (size_t)pw * ph / 2); /* nibbles are OR-ed in */
    }
    for (u32 my = 0; my < ph; my++) {
        /* PICA samples t = 0 at the last row in memory */
        const u8* row = px + (size_t)(((ph - 1 - my) * h / ph) * w) * 4;
        for (u32 mx = 0; mx < pw; mx++) {
            const u8* p = row + (size_t)(pw == w ? mx : mx * w / pw) * 4;
            u32 idx = ((my >> 3) * tiles_w + (mx >> 3)) * 64 + morton8(mx & 7, my & 7);
            switch (fmt) {
            case PLAT_TEXFMT_LA8:
                out[idx * 2] = p[3];
                out[idx * 2 + 1] = p[0];
                break;
            case PLAT_TEXFMT_L8: out[idx] = p[0]; break;
            case PLAT_TEXFMT_LA4: out[idx] = (u8)((p[0] / 17) << 4 | (p[3] / 17)); break;
            case PLAT_TEXFMT_L4: out[idx >> 1] |= (u8)((p[0] / 17) << ((idx & 1) * 4)); break;
            case PLAT_TEXFMT_RGBA4:
                ((u16*)out)[idx] = (u16)((p[0] / 17) << 12 | (p[1] / 17) << 8 | (p[2] / 17) << 4 | (p[3] / 17));
                break;
            case PLAT_TEXFMT_RGB565:
                ((u16*)out)[idx] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                break;
            case PLAT_TEXFMT_RGBA5551:
                ((u16*)out)[idx] =
                    (u16)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7));
                break;
            default: ((u32*)out)[idx] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; break;
            }
        }
    }
}

static u32 level_bytes(u32 fmt, u32 pw, u32 ph) { return pw * ph * fmt_bits(fmt) / 8; }

/* Box filter of a 2x2 block, colour weighted by alpha (so a transparent texel's colour does not bleed in). */
static inline void box4(const u8* a, const u8* b, const u8* c, const u8* d, u8* o) {
    u32 wa = a[3], wb = b[3], wc = c[3], wd = d[3], sw = wa + wb + wc + wd;
    for (int k = 0; k < 3; k++) {
        o[k] = (u8)(sw != 0 ? (a[k] * wa + b[k] * wb + c[k] * wc + d[k] * wd + sw / 2) / sw
                            : (u32)(a[k] + b[k] + c[k] + d[k] + 2) >> 2);
    }
    o[3] = (u8)((sw + 2) >> 2);
}

/* Level 1 from level 0 (w x h, resampled as tile_level does to pw x ph): (pw/2) x (ph/2) rows into dst. */
static void box_first(const u8* px, u32 w, u32 h, u32 pw, u32 ph, u8* dst) {
    u32 qw = pw >> 1, qh = ph >> 1;
    for (u32 y = 0; y < qh; y++) {
        const u8* r0 = px + (size_t)(((2 * y) * h / ph) * w) * 4;
        const u8* r1 = px + (size_t)(((2 * y + 1) * h / ph) * w) * 4;
        for (u32 x = 0; x < qw; x++) {
            u32 x0 = (2 * x) * w / pw, x1 = (2 * x + 1) * w / pw;
            box4(r0 + x0 * 4, r0 + x1 * 4, r1 + x0 * 4, r1 + x1 * 4, dst + ((size_t)y * qw + x) * 4);
        }
    }
}

/* The next level of a linear RGBA8 level (sw x sh, even sides). */
static void box_next(const u8* src, u32 sw, u32 sh, u8* dst) {
    u32 qw = sw >> 1, qh = sh >> 1;
    for (u32 y = 0; y < qh; y++) {
        const u8* r0 = src + (size_t)(2 * y) * sw * 4;
        const u8* r1 = r0 + (size_t)sw * 4;
        for (u32 x = 0; x < qw; x++) {
            box4(r0 + x * 8, r0 + x * 8 + 4, r1 + x * 8, r1 + x * 8 + 4, dst + ((size_t)y * qw + x) * 4);
        }
    }
}

/* Bytes of GX texture data before mip level `lvl` of a w x h image. */
static u32 gx_level_offset(u32 w, u32 h, u32 fmt, u32 lvl) {
    u32 off = 0;
    for (u32 l = 0; l < lvl; l++) {
        off += GXGetTexBufferSize((u16)(w > 1 ? w : 1), (u16)(h > 1 ? h : 1), fmt, GX_FALSE, 0);
        w >>= 1;
        h >>= 1;
    }
    return off;
}

static int mip_scratch(void) {
    if (s_mip_out == NULL) {
        s_mip_out = memalign(64, 1024 * 1024 * 4 / 3 + 4096);
        s_mip_a = memalign(64, 512 * 512 * 4);
        s_mip_b = memalign(64, 256 * 256 * 4);
        s_tiled = memalign(64, 1024 * 1024 * 4);
        if (s_mip_out == NULL || s_mip_a == NULL || s_mip_b == NULL || s_tiled == NULL) {
            PLOG_W("texfilter: no memory for the mip scratch; mip levels off");
            free(s_mip_out);
            free(s_mip_a);
            free(s_mip_b);
            free(s_tiled);
            s_mip_out = s_mip_a = s_mip_b = s_tiled = NULL;
            return 0;
        }
    }
    return 1;
}

/* What the sampler returns for texel `idx` of a tiled level in PICA format `fmt` (the PICA widens n-bit fields by
 * bit replication; L4/LA4 nibble order as citro3d: LA4 L high, A low; L4 even texel in the low nibble). */
static void sample_tiled(const u8* t, u32 fmt, u32 idx, u8* c) {
    switch (fmt) {
    case PLAT_TEXFMT_RGBA8: {
        u32 v = ((const u32*)t)[idx];
        c[0] = (u8)(v >> 24); c[1] = (u8)(v >> 16); c[2] = (u8)(v >> 8); c[3] = (u8)v;
        break;
    }
    case PLAT_TEXFMT_L8: c[0] = c[1] = c[2] = t[idx]; c[3] = 255; break;
    case PLAT_TEXFMT_LA8: c[0] = c[1] = c[2] = t[idx * 2 + 1]; c[3] = t[idx * 2]; break;
    case PLAT_TEXFMT_LA4: c[0] = c[1] = c[2] = (u8)((t[idx] >> 4) * 17); c[3] = (u8)((t[idx] & 15) * 17); break;
    case PLAT_TEXFMT_L4: c[0] = c[1] = c[2] = (u8)((t[idx >> 1] >> ((idx & 1) * 4) & 15) * 17); c[3] = 255; break;
    case PLAT_TEXFMT_RGBA4: {
        u16 v = ((const u16*)t)[idx];
        c[0] = (u8)((v >> 12) * 17); c[1] = (u8)((v >> 8 & 15) * 17); c[2] = (u8)((v >> 4 & 15) * 17); c[3] = (u8)((v & 15) * 17);
        break;
    }
    case PLAT_TEXFMT_RGB565: {
        u16 v = ((const u16*)t)[idx];
        u32 r = v >> 11, g = v >> 5 & 63, b = v & 31;
        c[0] = (u8)(r << 3 | r >> 2); c[1] = (u8)(g << 2 | g >> 4); c[2] = (u8)(b << 3 | b >> 2); c[3] = 255;
        break;
    }
    default: {
        u16 v = ((const u16*)t)[idx];
        u32 r = v >> 11, g = v >> 6 & 31, b = v >> 1 & 31;
        c[0] = (u8)(r << 3 | r >> 2); c[1] = (u8)(g << 3 | g >> 2); c[2] = (u8)(b << 3 | b >> 2); c[3] = (v & 1) ? 255 : 0;
        break;
    }
    }
}

/* texcompact=2: the level-0 texels the sampler gets from `nf` against those from `of` (today's choice), and
 * today's choice against the decoded RGBA8 (information: how lossy the baseline already is). */
static void compact_verify(const u8* px, u32 w, u32 h, u32 pw, u32 ph, u32 nf, u32 of) {
    u32 nb = level_bytes(nf, pw, ph), ob = level_bytes(of, pw, ph);
    u8* a = malloc(nb + 4);
    u8* b = malloc(ob + 4);
    if (a != NULL && b != NULL) {
        tile_level(px, w, h, pw, ph, nf, a);
        tile_level(px, w, h, pw, ph, of, b);
        u32 bad = 0, oldbad = 0;
        for (u32 my = 0; my < ph; my++) {
            const u8* row = px + (size_t)(((ph - 1 - my) * h / ph) * w) * 4;
            for (u32 mx = 0; mx < pw; mx++) {
                const u8* e = row + (size_t)(pw == w ? mx : mx * w / pw) * 4;
                u32 idx = ((my >> 3) * (pw >> 3) + (mx >> 3)) * 64 + morton8(mx & 7, my & 7);
                u8 cn[4], co[4];
                sample_tiled(a, nf, idx, cn);
                sample_tiled(b, of, idx, co);
                bad += memcmp(cn, co, 4) != 0;
                oldbad += memcmp(co, e, 4) != 0;
            }
        }
        s_cv_n[nf & 15]++;
        s_cv_texels[nf & 15] += (unsigned)(pw * ph);
        s_cv_bad[nf & 15] += (unsigned)bad;
        s_cv_old_bad[of & 15] += oldbad != 0;
        s_cv_saved += (unsigned)(ob - nb);
        if (bad != 0) {
            PLOG_W("texcompact: verify MISMATCH %u texels of %ux%u, new fmt %u old fmt %u", (unsigned)bad, (unsigned)pw, (unsigned)ph, (unsigned)nf, (unsigned)of);
        }
    }
    free(a);
    free(b);
}

/* RGBA8 rows (row 0 = GX t 0) -> PICA texture. Returns the handle. `gxt` is the GX texture when the mip
 * chain may use the game's own levels (its data at gxt->image); `want_mips` asks for a chain at all. */
static u32 upload_rgba8(const u8* px, u32 w, u32 h, u16* out_w, u16* out_h, const GxTexObj* gxt, const GxTlutObj* tl,
                        int want_mips, u8* out_levels, u32* out_bytes0, u32* out_mip_bytes) {
    int mipk = want_mips ? (gxt != NULL && gxt->mipmap ? 2 : 1) : 0;
    u32 fmt = pick_format_ex(px, w, h, s_compact, mipk);
    u32 pw = pow2(w), ph = pow2(h);
    if (s_compact == 2) {
        compact_verify(px, w, h, pw, ph, fmt, pick_format_ex(px, w, h, 0, mipk));
    }
    *out_levels = 0;
    *out_mip_bytes = 0;
    *out_bytes0 = level_bytes(fmt, pw, ph);
    *out_w = (u16)pw;
    *out_h = (u16)ph;
    s_uploads++;
    /* a one-bit alpha texture keeps no chain (a cut-out: a filtered alpha would thin it, and RGBA5551
     * cannot hold the in-between alphas; the next format up is twice the memory) */
    u32 levels = want_mips && fmt != PLAT_TEXFMT_RGBA5551 ? plat_gpu_tex_max_levels(pw, ph) : 0;
    if (levels != 0 && !mip_scratch()) {
        levels = 0;
    }
    if (levels == 0) {
        /* tiled straight into the texture's memory: the bytes plat_gpu_tex_create would have copied there */
        u64 tc = TF_T();
        void* dst = NULL;
        u32 size = 0, bytes = level_bytes(fmt, pw, ph);
        u32 hd = plat_gpu_tex_create_begin(pw, ph, fmt, &dst, &size);
        s_tf.t_create += s_tf_on ? plat_ticks() - tc : 0;
        if (hd == 0) {
            return 0;
        }
        if (size >= bytes) {
            tile_level(px, w, h, pw, ph, fmt, dst);
        } else {
            /* not seen (the PICA size is the tiled size); as plat_gpu_tex_create did: the first `size` bytes */
            u8* tmp = malloc(bytes);
            if (tmp == NULL) {
                plat_fatal("GX: no memory to tile a %lux%lu texture", (unsigned long)pw, (unsigned long)ph);
            }
            tile_level(px, w, h, pw, ph, fmt, tmp);
            memcpy(dst, tmp, size);
            free(tmp);
        }
        plat_gpu_tex_create_end(hd);
        return hd;
    }
    int from_gx = gxt != NULL && gxt->mipmap;
    const u8* lin = NULL; /* the level being tiled and filtered, linear rows, lw x lh */
    u32 lw = pw >> 1, lh = ph >> 1, off = 0;
    if (!from_gx) {
        box_first(px, w, h, pw, ph, s_mip_a);
        lin = s_mip_a;
    }
    tile_level(px, w, h, pw, ph, fmt, s_tiled); /* level 0; s_rgba is free after this */
    for (u32 l = 1; l <= levels; l++, lw >>= 1, lh >>= 1) {
        if (from_gx) {
            /* the game's own level l: decode, resample to the padded size */
            u32 gw = (w >> l) ? (w >> l) : 1, gh = (h >> l) ? (h >> l) : 1;
            const u8* src = (const u8*)gxt->image + gx_level_offset(w, h, gxt->format, l);
            gx_tex_decode_rgba8(src, gw, gh, gxt->format, tl ? tl->data : NULL, tl ? tl->format : 0, s_rgba);
            tile_level(s_rgba, gw, gh, lw, lh, fmt, s_mip_out + off);
        } else {
            tile_level(lin, lw, lh, lw, lh, fmt, s_mip_out + off);
            if (l < levels) {
                u8* nxt = lin == s_mip_a ? s_mip_b : s_mip_a;
                box_next(lin, lw, lh, nxt);
                lin = nxt;
            }
        }
        off += level_bytes(fmt, lw, lh);
    }
    *out_levels = (u8)levels;
    *out_mip_bytes = off;
    s_mip_extra_made++;
    u64 tc = TF_T();
    u32 hd = plat_gpu_tex_create_mip(pw, ph, fmt, s_tiled, level_bytes(fmt, pw, ph), s_mip_out, off, levels);
    s_tf.t_create += s_tf_on ? plat_ticks() - tc : 0;
    return hd;
}

typedef u32 __attribute__((may_alias)) u32_alias;

/* Every byte of the span goes into the hash: the unaligned head and the tail
 * byte by byte, the aligned middle a word at a time on two independent lanes
 * (multiply, xor-shift), which the ARM11 pipelines. */
static u32 hash_span(u32 h, const u8* p, u32 n) {
    while (n != 0 && ((u32)p & 3) != 0) {
        h = (h ^ *p++) * 16777619u;
        n--;
    }
    const u32_alias* w = (const u32_alias*)p;
    u32 h1 = h, h2 = h ^ 0x9E3779B9u;
    for (; n >= 8; n -= 8, w += 2) {
        h1 = (h1 ^ w[0]) * 0x85EBCA6Bu;
        h2 = (h2 ^ w[1]) * 0xC2B2AE35u;
        h1 ^= h1 >> 15;
        h2 ^= h2 >> 13;
    }
    if (n >= 4) {
        h1 = (h1 ^ w[0]) * 0x85EBCA6Bu;
        h1 ^= h1 >> 15;
        w++;
        n -= 4;
    }
    h = (h1 ^ (h2 * 0x27D4EB2Fu)) * 0x165667B1u;
    h ^= h >> 16;
    p = (const u8*)w;
    while (n-- != 0) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

static u32 hash_bytes_raw(const u8* p, u32 n);
static u32 hash_bytes(const u8* p, u32 n) {
    if (s_stats_every) {
        s_st_hashes++;
        s_st_bytes += n <= 32 * 1024 ? n : 64 * 64;
    }
    if (!s_tf_on) {
        return hash_bytes_raw(p, n);
    }
    u64 t0 = plat_ticks();
    u32 r = hash_bytes_raw(p, n);
    s_tf.t_hash += plat_ticks() - t0;
    s_tf.hashes++;
    s_tf.hash_bytes += n <= 32 * 1024 ? n : 64 * 64;
    return r;
}

static u32 hash_bytes_raw(const u8* p, u32 n) {
    u32 h = 2166136261u;
    if (n <= 32 * 1024) {
        return hash_span(h, p, n);
    }
    /* large textures: 64 spread samples of 64 bytes */
    for (u32 s = 0; s < 64; s++) {
        h = hash_span(h, p + (size_t)(n - 64) * s / 63, 64);
    }
    return h;
}

static void entry_free(CacheEntry* e) {
    if (e->handle != 0) {
        plat_gpu_tex_free(e->handle);
        CacheExtra* x = &s_x[e - s_cache];
        s_live_n--;
        s_live_bytes -= x->bytes0 + x->bytes_mip;
        s_live_mip -= x->bytes_mip;
        memset(x, 0, sizeof(*x));
    }
    memset(e, 0, sizeof(*e));
}

/* Whether a texture of this GX size gets a chain in mip mode (the sampler exempts the tiny ones anyway). */
static int wants_mips(const GxTexObj* t) { return s_mode == TF_MIP && t->width > 4 && t->height > 4; }



/* After a check or upload hash that began at `epoch` found the entry current: asks the registry to vouch for it. */
static void tex_trust(CacheEntry* e, const GxTexObj* t, const GxTlutObj* tl, u32 bytes, u32 epoch) {
    PlatMemSpan sp[2] = {{t->image, bytes}, {NULL, 0}};
    u32 n = 1;
    if (tl) {
        sp[n++] = (PlatMemSpan){tl->data, (u32)tl->n_entries * 2};
    }
    e->trust_gen = plat_memstable_trust(sp, n, epoch);
    if (e->trust_gen != 0) {
        s_st_grants++;
    } else {
        e->trust_epoch = epoch;
    }
}


/* ---- texasync (switch, default 0): textures decoded on the core-2 job queue before their first bind ----
 * The game tells the GX layer about a texture set (GXPrefetchTexPort, called when a TPL is unpacked). Each plain
 * (non-CI) texture whose image lies in covered memory is queued: decode to RGBA8, pick the PICA format, tile and hash,
 * all on private buffers, by a runner job on the job worker (one job walks the queue; the entries are claimed one at a
 * time by compare-and-swap). The input is vouched stable by memstable at queue time: plat_memstable_trust marks the
 * span relied and returns the trust generation G. At the first bind the result is used only when the generation is
 * still G (no relied range written, released or refilled since the queue time, so the bytes the worker read are the
 * bytes there now); otherwise it is dropped and the texture is converted at the bind as before. The result goes through
 * the same plat_gpu_tex_create, so the picture is the one the synchronous path makes. A bind of a texture the worker
 * has not started decodes it on the game thread; one the worker is decoding is waited for. texverify=1 also
 * re-hashes at the bind. Only plain textures without mip chains (texfilter=mip converts at the bind), up to 640x512 texels (1024x512 padded). Results unused after PRE_AGE frames are dropped. */
#define PRE_MAX 1024
#define PRE_BYTES_MAX (6u << 20)
#define PRE_MAX_PIX (640u * 512u)  /* texels of a texture (the decode scratch is 4 bytes each) */
#define PRE_MAX_PAD (1024u * 512u) /* texels after padding to powers of two */
#define PRE_AGE 60
enum { PS_FREE = 0, PS_TODO, PS_RUN, PS_DONE };
typedef struct {
    u32 state; /* atomic: FREE -> TODO (game thread) -> RUN (worker or game thread) -> DONE -> FREE (game thread) */
    const void* image;
    u16 w, h;
    u8 fmt, ok;
    u32 gx_bytes, gen, frame, est_bytes;
    u8* out;
    u32 out_bytes, pfmt, hash;
    u16 pw, ph;
} PreTex;
static PreTex* s_pre;
static u8* s_pre_rgba; /* the runner's decode scratch; a decode at the bind uses the bind's own (s_rgba) */
static u32 s_pre_bytes;    /* atomic: the results' heap bytes (a queued entry counts its most, a decoded one its own) */
static u32 s_pre_active; /* a runner job is walking the queue */
static PlatJob s_pre_runner;
extern void svcSleepThread(long long ns);

static inline int pre_cas(PreTex* p, u32 from, u32 to) {
    return __atomic_compare_exchange_n(&p->state, &from, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Decodes one claimed (PS_RUN) entry into `rgba` (PRE_MAX_PIX texels): the result stays in the entry, DONE is
 * published last. One runner decodes at a time (pre_kick joins the last one before it submits another), with
 * s_pre_rgba; the bind (pre_take) decodes with the scratch of the thread that binds. */
static void pre_decode(PreTex* p, u8* rgba) {
    gx_tex_decode_rgba8(p->image, p->w, p->h, p->fmt, NULL, 0, rgba);
    u32 fmt = pick_format(rgba, p->w, p->h);
    u32 pw = pow2(p->w), ph = pow2(p->h);
    u32 bytes = level_bytes(fmt, pw, ph);
    u8* out = malloc(bytes);
    if (out != NULL) {
        tile_level(rgba, p->w, p->h, pw, ph, fmt, out);
        p->hash = hash_bytes_raw(p->image, p->gx_bytes);
        p->out = out;
        p->out_bytes = bytes;
        p->pfmt = fmt;
        p->pw = (u16)pw;
        p->ph = (u16)ph;
        p->ok = 1;
    }
    /* the entry counted its most (RGBA8); now its own bytes, or none */
    u32 real = out != NULL ? bytes : 0;
    __atomic_sub_fetch(&s_pre_bytes, p->est_bytes - real, __ATOMIC_RELAXED);
    p->est_bytes = real;
    __atomic_store_n(&p->state, PS_DONE, __ATOMIC_RELEASE);
}

static int pre_any_todo(void) {
    for (int i = 0; i < PRE_MAX; i++) {
        if (__atomic_load_n(&s_pre[i].state, __ATOMIC_ACQUIRE) == PS_TODO) {
            return 1;
        }
    }
    return 0;
}

/* The runner job: claims queued entries in order until none is left. */
static void pre_runner(void* unused) {
    (void)unused;
    for (;;) {
        for (int i = 0; i < PRE_MAX; i++) {
            PreTex* p = &s_pre[i];
            if (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == PS_TODO && pre_cas(p, PS_TODO, PS_RUN)) {
                pre_decode(p, s_pre_rgba);
            }
        }
        __atomic_store_n(&s_pre_active, 0u, __ATOMIC_RELEASE);
        u32 z = 0;
        if (!pre_any_todo() || !__atomic_compare_exchange_n(&s_pre_active, &z, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return; /* nothing new, or the game thread started another runner */
        }
    }
}

/* Game thread: makes sure a runner is walking the queue. */
static void pre_kick(void) {
    u32 z = 0;
    if (__atomic_compare_exchange_n(&s_pre_active, &z, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (s_pre_runner != 0) {
            plat_job_join(s_pre_runner); /* the previous runner has finished (it cleared the flag) or is about to */
        }
        s_pre_runner = plat_job_submit(pre_runner, NULL);
    }
}

static void pre_free(PreTex* p) {
    __atomic_sub_fetch(&s_pre_bytes, p->est_bytes, __ATOMIC_RELAXED);
    free(p->out);
    memset(p, 0, sizeof(*p)); /* state FREE */
}

static int cache_has(const void* image, u16 w, u16 h, u8 fmt) {
    u32 slot = ((u32)image >> 5) % CACHE_SIZE;
    for (u32 probe = 0; probe < 16; probe++) {
        const CacheEntry* e = &s_cache[(slot + probe) % CACHE_SIZE];
        if (e->handle != 0 && e->image == image && e->w == w && e->h == h && e->fmt == fmt && !e->is_ci) {
            return 1;
        }
    }
    return 0;
}

static PreTex* pre_find(const void* image, u16 w, u16 h, u8 fmt) {
    for (int i = 0; i < PRE_MAX; i++) {
        PreTex* p = &s_pre[i];
        if (p->state != PS_FREE && p->image == image && p->w == w && p->h == h && p->fmt == fmt) {
            return p;
        }
    }
    return NULL;
}

/* Public: the game has a plain texture in memory that it will probably draw soon. */
void GXPrefetchTexPort(const void* image, u16 w, u16 h, u32 format, int has_clut) {
    if (!s_async || s_pre == NULL || image == NULL || s_mode == TF_MIP) {
        return;
    }
    s_as.calls++;
    if (has_clut) {
        s_as.rej_clut++;
        return;
    }
    switch (format) {
    case GX_TF_I4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3:
    case GX_TF_RGBA8: case GX_TF_CMPR: break;
    default: s_as.rej_fmt++; return;
    }
    if (w == 0 || h == 0 || (u32)w * h > PRE_MAX_PIX || pow2(w) * pow2(h) > PRE_MAX_PAD) {
        s_as.rej_size++;
        return;
    }
    u32 est = pow2(w) * pow2(h) * 4; /* the most a result takes (RGBA8): the cap holds for the real bytes */
    while (__atomic_load_n(&s_pre_bytes, __ATOMIC_RELAXED) + est > PRE_BYTES_MAX) {
        /* make room: the oldest finished result that no bind has asked for in a while goes first */
        PreTex* old = NULL;
        for (int i = 0; i < PRE_MAX; i++) {
            PreTex* q = &s_pre[i];
            if (q->state == PS_DONE && s_frame - q->frame >= 2 && (old == NULL || q->frame < old->frame)) {
                old = q;
            }
        }
        if (old == NULL) {
            s_as.rej_full++;
            return;
        }
        s_as.unused++;
        pre_free(old);
    }
    if (cache_has(image, w, h, (u8)format) || pre_find(image, w, h, (u8)format) != NULL) {
        s_as.rej_dup++;
        return;
    }
    PreTex* p = NULL;
    for (int i = 0; i < PRE_MAX; i++) {
        if (s_pre[i].state == PS_FREE) {
            p = &s_pre[i];
            break;
        }
    }
    if (p == NULL) {
        s_as.rej_full++;
        return;
    }
    u32 bytes = GXGetTexBufferSize(w, h, format, GX_FALSE, 0);
    PlatMemSpan sp = {image, bytes};
    u32 g = plat_memstable_trust(&sp, 1, plat_memstable_epoch());
    if (g == 0) {
        s_as.rej_cov++;
        return; /* not covered memory: the bind hashes it as before */
    }
    memset(p, 0, sizeof(*p));
    p->image = image;
    p->w = w;
    p->h = h;
    p->fmt = (u8)format;
    p->gx_bytes = bytes;
    p->gen = g;
    p->frame = s_frame;
    p->est_bytes = est;
    u32 now = __atomic_add_fetch(&s_pre_bytes, est, __ATOMIC_RELAXED);
    if (now > s_pre_bytes_hw) {
        s_pre_bytes_hw = now;
    }
    s_as.queued++;
    __atomic_store_n(&p->state, PS_TODO, __ATOMIC_RELEASE);
    pre_kick();
}

/* The result for texture `t` if one is valid: creates the PICA texture from it and fills the handle and sizes; 0
 * when the bind must convert. The entry is gone afterwards either way. */
static int pre_take(const GxTexObj* t, u32* handle, u16* tw, u16* th, u32* bytes0, u32* hash, u32* gen) {
    PreTex* p = pre_find(t->image, t->width, t->height, t->format);
    if (p == NULL) {
        return 0;
    }
    int ok = 0;
    if (pre_cas(p, PS_TODO, PS_RUN)) {
        pre_decode(p, s_rgba); /* the worker has not started it: here, as the synchronous path would */
        s_as.inline_n++;
    } else if (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == PS_RUN) {
        u64 t0 = plat_ticks();
        while (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == PS_RUN) {
            svcSleepThread(20000);
        }
        s_as.wait_n++;
        s_as.wait_ticks += plat_ticks() - t0;
    }
    if (!p->ok || s_mode == TF_MIP || p->gen != plat_memstable_gen()) {
        s_as.stale_gen += p->ok;
    } else if (s_verify && hash_bytes_raw(t->image, p->gx_bytes) != p->hash) {
        s_as.verify_bad++;
        s_st_stale++;
        PLOG_W("texverify: texasync result stale: image %p %ux%u fmt %u frame %u", t->image, t->width, t->height,
               t->format, (unsigned)s_frame);
    } else {
        u32 h = plat_gpu_tex_create(p->pw, p->ph, p->pfmt, p->out, p->out_bytes);
        if (h != 0) {
            *handle = h;
            *tw = p->pw;
            *th = p->ph;
            *bytes0 = p->out_bytes;
            *hash = p->hash;
            *gen = p->gen;
            ok = 1;
            s_as.taken++;
            s_uploads++;
        }
    }
    pre_free(p);
    return ok;
}

static void pre_frame_end(void) {
    if (s_pre == NULL) {
        return;
    }
    for (int i = 0; i < PRE_MAX; i++) {
        PreTex* p = &s_pre[i];
        u32 st = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE);
        if (st == PS_FREE || s_frame - p->frame <= PRE_AGE) {
            continue;
        }
        if (st == PS_TODO && pre_cas(p, PS_TODO, PS_RUN)) {
            st = PS_DONE; /* claimed by us: nothing was made */
        }
        if (st == PS_DONE) {
            s_as.unused++;
            pre_free(p);
        }
    }
    if (pre_any_todo()) {
        pre_kick();
    }
}

static u32 handle_for_impl(int map, u16* out_w, u16* out_h);

/* ---- texfast (switch, default 0): direct-mapped lookup cache in front of the copy scan and the hash-table probe.
 * Key: the texture object's identity (image, size, format, CI flag, TLUT data and format). Value: the s_cache (or
 * s_copies) slot the full lookup found. A hit is used only if the slot still holds exactly that key, the copy
 * registry is unchanged (s_copy_gen) and the entry needs no hash this frame (checked this frame, or trusted by
 * the current memstable generation and texverify off); otherwise the full lookup runs. texfast=2: both lookups
 * run, the results are compared and mismatches counted (must be 0). */
#define FAST_SIZE 256
typedef struct {
    const void* image;
    const void* tlut;
    u32 tlut_fmt;
    u16 w, h;
    u8 fmt, is_ci;
    u8 kind; /* 0 empty, 1 s_cache, 2 s_copies */
    u16 idx;
    u32 cgen;
} FastEntry;
static FastEntry s_fast[FAST_SIZE];
static int s_fast_mode;
static u32 s_copy_gen = 1;
static int s_found_kind, s_found_idx; /* set by handle_for_impl for the fast cache to record */
static unsigned s_fast_mism, s_fast_hits, s_fast_total;

static inline FastEntry* fast_slot(const GxTexObj* t, const GxTlutObj* tl) {
    u32 k = ((u32)t->image >> 5) ^ ((u32)t->width * 31u) ^ ((u32)t->height * 17u) ^ ((u32)t->format * 7u) ^
            (tl ? ((u32)tl->data >> 5) : 0);
    return &s_fast[(k ^ (k >> 8)) % FAST_SIZE];
}

/* Returns 1 and fills the result on a safe hit. */
static int fast_try(const GxTexObj* t, u16* out_w, u16* out_h, u32* handle, u32* levels, int* copy) {
    const GxTlutObj* tl = t->is_ci ? &gx.tlut[t->tlut_name < 20 ? t->tlut_name : 0] : NULL;
    FastEntry* f = fast_slot(t, tl);
    if (f->kind == 0 || f->cgen != s_copy_gen || f->image != t->image || f->w != t->width || f->h != t->height ||
        f->fmt != t->format || f->is_ci != t->is_ci || f->tlut != (tl ? tl->data : NULL) ||
        f->tlut_fmt != (tl ? tl->format : 0)) {
        return 0;
    }
    if (f->kind == 2) {
        CopyEntry* c = &s_copies[f->idx];
        if (c->addr != t->image || c->handle == 0) {
            return 0;
        }
        if (s_fast_mode == 1) {
            c->last_frame = s_frame;
        }
        *out_w = c->w;
        *out_h = c->h;
        *handle = c->handle;
        *levels = 0;
        *copy = 1;
        return 1;
    }
    CacheEntry* e = &s_cache[f->idx];
    if (e->handle == 0 || e->image != t->image || e->w != t->width || e->h != t->height || e->fmt != t->format ||
        e->is_ci != t->is_ci || e->tlut != f->tlut || e->tlut_fmt != f->tlut_fmt) {
        return 0;
    }
    if (e->checked_frame != s_frame) {
        if (s_verify || !s_trust_on || e->trust_gen == 0 || e->trust_gen != plat_memstable_gen()) {
            return 0;
        }
        if (s_fast_mode == 1) {
            e->checked_frame = s_frame;
            s_st_trusted++;
        }
    }
    if (s_fast_mode == 1) {
        e->last_frame = s_frame;
    }
    *out_w = e->tex_w;
    *out_h = e->tex_h;
    *handle = e->handle;
    *levels = s_x[f->idx].levels;
    *copy = 0;
    return 1;
}

static void fast_record(const GxTexObj* t) {
    if (s_found_kind == 0) {
        return;
    }
    const GxTlutObj* tl = t->is_ci ? &gx.tlut[t->tlut_name < 20 ? t->tlut_name : 0] : NULL;
    FastEntry* f = fast_slot(t, tl);
    f->image = t->image;
    f->tlut = tl ? tl->data : NULL;
    f->tlut_fmt = tl ? tl->format : 0;
    f->w = t->width;
    f->h = t->height;
    f->fmt = t->format;
    f->is_ci = t->is_ci;
    f->kind = (u8)s_found_kind;
    f->idx = (u16)s_found_idx;
    f->cgen = s_copy_gen;
}

/* Converts (or finds) the texture of texture map slot `map`. */
static u32 handle_for_full(int map, u16* out_w, u16* out_h) {
    if (!s_fast_mode) {
        return handle_for_impl(map, out_w, out_h);
    }
    const GxTexObj* t = &gx.tex_map[map];
    int valid = map >= 0 && map < GX_MAX_TEXMAP && gx.tex_map_valid[map] && t->image != NULL && t->width != 0 &&
                t->height != 0 && t->width <= 1024 && t->height <= 1024;
    u16 fw = 0, fh = 0;
    u32 fh_, fl = 0;
    int fc = 0, hit = valid && fast_try(t, &fw, &fh, &fh_, &fl, &fc);
    if (hit && s_fast_mode == 1) {
        s_last.copy = fc;
        s_last.levels = (u8)fl;
        s_st_lookups += s_stats_every != 0;
        s_fast_hits++;
        *out_w = fw;
        *out_h = fh;
        return fh_;
    }
    s_found_kind = 0;
    u32 r = handle_for_impl(map, out_w, out_h);
    if (valid) {
        s_fast_total++;
        if (hit) {
            s_fast_hits++;
            if (r != fh_ || *out_w != fw || *out_h != fh || s_last.levels != fl || s_last.copy != fc) {
                s_fast_mism++;
                if (s_fast_mism <= 8) {
                    PLOG_W("texfast: MISMATCH map %d image %p fast h%u %ux%u l%u c%d, full h%u %ux%u l%u c%d", map,
                           t->image, (unsigned)fh_, (unsigned)fw, (unsigned)fh, (unsigned)fl, fc, (unsigned)r,
                           (unsigned)*out_w, (unsigned)*out_h, (unsigned)s_last.levels, (int)s_last.copy);
                }
            }
        } else if (s_found_kind && r != 0) {
            fast_record(t);
        }
    }
    return r;
}

u32 gx_tex_handle_for(int map, u16* out_w, u16* out_h) {
    if (!s_tf_on) {
        return handle_for_full(map, out_w, out_h);
    }
    u64 t0 = plat_ticks();
    u32 r = handle_for_full(map, out_w, out_h);
    s_tf.t_total += plat_ticks() - t0;
    s_tf.lookups++;
    return r;
}

static u32 handle_for_impl(int map, u16* out_w, u16* out_h) {
    s_last.copy = 0;
    s_last.levels = 0;
    if (map < 0 || map >= GX_MAX_TEXMAP || !gx.tex_map_valid[map]) {
        return 0;
    }
    const GxTexObj* t = &gx.tex_map[map];
    if (t->image == NULL || t->width == 0 || t->height == 0 || t->width > 1024 || t->height > 1024) {
        return 0;
    }
    for (int i = 0; i < (int)(sizeof(s_copies) / sizeof(s_copies[0])); i++) {
        if (s_copies[i].addr == t->image && s_copies[i].handle != 0) {
            *out_w = s_copies[i].w;
            *out_h = s_copies[i].h;
            s_copies[i].last_frame = s_frame;
            s_last.copy = 1;
            s_found_kind = 2;
            s_found_idx = i;
            return s_copies[i].handle;
        }
    }
    s_st_lookups += s_stats_every != 0;
    const GxTlutObj* tl = t->is_ci ? &gx.tlut[t->tlut_name < 20 ? t->tlut_name : 0] : NULL;
    u32 bytes = GXGetTexBufferSize(t->width, t->height, t->format, GX_FALSE, 0);
    u32 slot = ((u32)t->image >> 5) % CACHE_SIZE;
    CacheEntry* free_e = NULL;
    for (u32 probe = 0; probe < 16; probe++) {
        CacheEntry* e = &s_cache[(slot + probe) % CACHE_SIZE];
        if (e->handle == 0) {
            if (free_e == NULL) {
                free_e = e;
            }
            continue;
        }
        if (e->image == t->image && e->w == t->width && e->h == t->height && e->fmt == t->format &&
            e->is_ci == t->is_ci && (!tl || (e->tlut == tl->data && e->tlut_fmt == tl->format))) {
            if (e->checked_frame != s_frame) {
                int trusted = s_trust_on && e->trust_gen != 0 && e->trust_gen == plat_memstable_gen();
                if (!trusted || s_verify) {
                    u32 epoch = plat_memstable_epoch();
                    u32 h = hash_bytes(t->image, bytes);
                    u32 th = 0;
                    if (tl) {
                        th = hash_bytes(tl->data, (u32)tl->n_entries * 2);
                        h ^= th;
                    }
                    e->checked_frame = s_frame;
                    s_st_checks += s_stats_every != 0;
                    s_st_trusted += trusted; /* texverify: the decisions the hash checks */
                    if (trusted && h != e->hash) {
                        s_st_stale++;
                        PLOG_W("texverify: trust skipped a change: image %p %ux%u fmt %u frame %u", t->image,
                               t->width, t->height, t->format, (unsigned)s_frame);
                    }
                    if (h != e->hash) {
                        if (s_stats_every) {
                            stat_mismatch(e, t, bytes, tl && th != s_dbg_tlut_hash[e - s_cache]);
                        }
                        entry_free(e);
                        free_e = e;
                        break;
                    }
                    if (!trusted) {
                        e->trust_gen = 0;
                        if (s_trust_on && e->trust_epoch != epoch) {
                            tex_trust(e, t, tl, bytes, epoch);
                        }
                    }
                } else {
                    e->checked_frame = s_frame;
                    s_st_trusted++;
                }
            }
            e->last_frame = s_frame;
            *out_w = e->tex_w;
            *out_h = e->tex_h;
            s_last.levels = s_x[e - s_cache].levels;
            s_found_kind = 1;
            s_found_idx = (int)(e - s_cache);
            return e->handle;
        }
    }
    if (free_e == NULL) {
        /* evict the least recently used of the probe window */
        CacheEntry* lru = &s_cache[slot];
        for (u32 probe = 1; probe < 16; probe++) {
            CacheEntry* e = &s_cache[(slot + probe) % CACHE_SIZE];
            if (e->last_frame < lru->last_frame) {
                lru = e;
            }
        }
        entry_free(lru);
        free_e = lru;
    }
    u32 up_epoch = plat_memstable_epoch();
    CacheEntry* e = free_e;
    int mips = wants_mips(t);
    CacheExtra* xe = &s_x[free_e - s_cache];
    u32 pre_hash = 0, pre_gen = 0;
    int from_pre = 0;
    u64 tu0 = TF_T();
    if (s_async && !tl && !mips && s_pre != NULL) {
        from_pre = pre_take(t, &e->handle, &e->tex_w, &e->tex_h, &xe->bytes0, &pre_hash, &pre_gen);
        if (from_pre) {
            xe->levels = 0;
            xe->bytes_mip = 0;
            s_tf.async_hit++;
        } else {
            s_tf.async_miss++;
        }
    }
    if (!from_pre) {
        u64 td = TF_T();
        gx_tex_decode_rgba8(t->image, t->width, t->height, t->format, tl ? tl->data : NULL, tl ? tl->format : 0,
                            s_rgba);
        s_tf.t_dec += s_tf_on ? plat_ticks() - td : 0;
        e->handle = upload_rgba8(s_rgba, t->width, t->height, &e->tex_w, &e->tex_h, t, tl, mips, &xe->levels,
                                 &xe->bytes0, &xe->bytes_mip);
    }
    if (s_tf_on) {
        s_tf.uploads++;
        s_tf.t_conv += plat_ticks() - tu0;
    }
    if (e->handle == 0) {
        /* texture memory full: free everything not used this frame, retry. At most once per frame: a second
         * miss in the same frame would free nothing new and repeat the full GPU sync every state rebuild. */
        static u32 s_reclaim_frame = ~0u;
        if (s_reclaim_frame != (u32)s_frame) {
            s_reclaim_frame = (u32)s_frame;
            for (int i = 0; i < CACHE_SIZE; i++) {
                if (s_cache[i].handle && s_cache[i].last_frame != s_frame) {
                    entry_free(&s_cache[i]);
                }
            }
            /* the freed textures wait out their frames in flight: wait for the GPU and release them now */
            plat_gpu_tex_reclaim();
            e->handle = upload_rgba8(s_rgba, t->width, t->height, &e->tex_w, &e->tex_h, t, tl, mips, &xe->levels,
                                     &xe->bytes0, &xe->bytes_mip);
        }
        if (e->handle == 0) {
            /* A texture that cannot be made is not worth a crash (hardware build #461: a 608x480 texture in the
             * opening demo with linear memory fragmented): the draw goes without it. */
            static u32 s_fail_logged;
            if (s_fail_logged++ < 8) {
                PLOG_E("GX: no texture memory for a %ux%u texture (image %p, fmt %u): drawn without it",
                       t->width, t->height, t->image, (unsigned)t->format);
            }
            memset(e, 0, sizeof(*e));
            memset(xe, 0, sizeof(*xe));
            s_last.levels = 0;
            return 0;
        }
    }
    if (s_census_every) {
        u32 mn = t->min_filter < 6 ? t->min_filter : 0, mg = t->mag_filter < 2 ? t->mag_filter : 2;
        s_cen->up[mn][mg][t->mipmap != 0]++;
        s_cen->up_total++;
        s_cen->fmt[t->format & 15]++;
        s_cen->fmt_mip[t->format & 15] += t->mipmap != 0;
        s_cen->bias_nz += t->lod_bias_q != 0;
        s_cen->edge += t->edge_lod != 0;
    }
    s_live_n++;
    s_live_bytes += xe->bytes0 + xe->bytes_mip;
    s_live_mip += xe->bytes_mip;
    s_last.levels = xe->levels;
    e->image = t->image;
    e->w = t->width;
    e->h = t->height;
    e->fmt = t->format;
    e->is_ci = t->is_ci;
    e->tlut = tl ? tl->data : NULL;
    e->tlut_fmt = tl ? tl->format : 0;
    s_st_uploads += s_stats_every != 0;
    e->last_frame = e->checked_frame = s_frame;
    if (from_pre) {
        s_dbg_tlut_hash[e - s_cache] = 0;
        e->hash = pre_hash;
        e->trust_gen = pre_gen; /* still the current generation: checked in pre_take */
    } else {
        u32 th0 = tl ? hash_bytes(tl->data, (u32)tl->n_entries * 2) : 0;
        s_dbg_tlut_hash[e - s_cache] = th0;
        e->hash = hash_bytes(t->image, bytes) ^ th0;
        if (s_trust_on) {
            tex_trust(e, t, tl, bytes, up_epoch);
        }
    }
    *out_w = e->tex_w;
    *out_h = e->tex_h;
    s_found_kind = 1;
    s_found_idx = (int)(e - s_cache);
    return e->handle;
}

/* A TEV stage of the current setup reads `map` as an indirect offset texture. */
static int map_is_indirect(int map) {
    int num = gx.num_tev ? gx.num_tev : 1;
    for (int s = 0; s < num && s < GX_MAX_TEV; s++) {
        if (gx.tev[s].ind_enabled && gx.tev[s].ind_stage < 4 && gx.ind[gx.tev[s].ind_stage].map == map) {
            return 1;
        }
    }
    return 0;
}

static void census_sample(const GxTexObj* t, int copy, int ind, int tiny) {
    u32 mn = t->min_filter < 6 ? t->min_filter : 0, mg = t->mag_filter < 2 ? t->mag_filter : 2;
    s_cen->use[mn][mg][t->mipmap != 0]++;
    s_cen->use_total++;
    s_cen->copy_use += copy;
    s_cen->ind_use += ind;
    s_cen->tiny_use += tiny;
}

/* The filter fields of texture unit `tu` for map `map` (texture `t`), after gx_tex_handle_for(map). */
void gx_tex_unit_sampler(PlatGpuTexUnit* tu, const GxTexObj* t, int map) {
    int copy = s_last.copy, ind = 0, tiny = t->width <= 4 || t->height <= 4;
    if (s_mode != TF_GAME || s_census_every) {
        ind = map_is_indirect(map);
    }
    if (s_census_every) {
        census_sample(t, copy, ind, tiny);
    }
    /* what GX asks for */
    tu->mag_linear = t->mag_filter != GX_NEAR;
    tu->min_linear = t->min_filter == GX_LINEAR || t->min_filter == GX_LIN_MIP_NEAR || t->min_filter == GX_LIN_MIP_LIN;
    tu->mip_linear = 0;
    tu->lod_bias = 0.0f;
    if (s_mode == TF_GAME || copy || ind || tiny) {
        return; /* see the rule above s_mode */
    }
    switch (s_mode) {
    case TF_NEAREST:
        tu->mag_linear = tu->min_linear = 0;
        break;
    case TF_MIP:
        tu->mag_linear = tu->min_linear = 1;
        tu->mip_linear = s_last.levels != 0;
        tu->lod_bias = (f32)t->lod_bias_q / 32.0f + s_lod_bias;
        break;
    default: /* TF_LINEAR */
        tu->mag_linear = tu->min_linear = 1;
        break;
    }
    if (s_mode != TF_MIP) {
        tu->lod_bias = s_lod_bias;
    }
}

static const char* const k_mode_names[] = {"game", "linear", "mip", "nearest"};

/* Once per frame: the option (or the switch) -> mode; a change drops every converted texture. */
static void mode_refresh(void) {
    if (s_mode_forced) {
        return;
    }
    int v = opt_get_texfilter();
    if (v != s_mode) {
        PLOG_I("texfilter: %s -> %s", k_mode_names[s_mode], k_mode_names[v]);
        s_mode = (u8)v;
        gx_tex_invalidate_all();
    }
}

static void mode_init(void) {
    char v[24];
    s_lod_bias = 0.0f;
    if (plat_switch_value("texlodbias", v, sizeof(v))) {
        s_lod_bias = (float)atof(v);
    }
    if (plat_switch_value("texfilter", v, sizeof(v))) {
        for (int i = 0; i < 4; i++) {
            if (strcmp(v, k_mode_names[i]) == 0 || v[0] == (char)('0' + i)) {
                s_mode = (u8)i;
                s_mode_forced = 1;
            }
        }
        if (!s_mode_forced) {
            PLOG_W("texfilter: unknown mode \"%s\" (game, linear, mip, nearest)", v);
        }
    }
    s_census_every = plat_switch_int("texcensus", 0);
    if (!s_mode_forced) {
        s_mode = (u8)opt_get_texfilter();
    }
    PLOG_I("texfilter: %s%s, lod bias %d/100", k_mode_names[s_mode], s_mode_forced ? " (switch)" : "",
           (int)(s_lod_bias * 100.0f));
}

static void census_report(void) {
    static const char* const mn[6] = {"NEAR", "LINEAR", "NEAR_MIP_NEAR", "LIN_MIP_NEAR", "NEAR_MIP_LIN", "LIN_MIP_LIN"};
    static const char* const gn[3] = {"NEAR", "LINEAR", "other"};
    PLOG_I("texcensus: frame %u: %u distinct uploads, %u sampler setups (copy targets %u, indirect %u, tiny %u)",
           (unsigned)s_frame, (unsigned)s_cen->up_total, (unsigned)s_cen->use_total, (unsigned)s_cen->copy_use,
           (unsigned)s_cen->ind_use, (unsigned)s_cen->tiny_use);
    for (int a = 0; a < 6; a++) {
        for (int b = 0; b < 3; b++) {
            for (int m = 0; m < 2; m++) {
                if (s_cen->up[a][b][m] || s_cen->use[a][b][m]) {
                    PLOG_I("texcensus:   min %-13s mag %-6s gxmip %d: uploads %u, setups %u", mn[a], gn[b], m,
                           (unsigned)s_cen->up[a][b][m], (unsigned)s_cen->use[a][b][m]);
                }
            }
        }
    }
    for (int f = 0; f < 16; f++) {
        if (s_cen->fmt[f]) {
            PLOG_I("texcensus:   gx format %d: %u uploads, %u with mip levels", f, (unsigned)s_cen->fmt[f],
                   (unsigned)s_cen->fmt_mip[f]);
        }
    }
    PLOG_I("texcensus:   uploads with LOD bias != 0: %u, with edge LOD: %u", (unsigned)s_cen->bias_nz,
           (unsigned)s_cen->edge);
    PLOG_I("texmem: %s: %u textures live, %u KB (of which mip levels %u KB), %u mip chains built", k_mode_names[s_mode],
           (unsigned)s_live_n, (unsigned)(s_live_bytes >> 10), (unsigned)(s_live_mip >> 10),
           (unsigned)s_mip_extra_made);
}

void gx_tex_invalidate_all(void) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].handle) {
            entry_free(&s_cache[i]);
        }
    }
    gx_dirty |= GXD_TEX; /* gxdirty: the texture units are looked up again */
}

void gx_tex_frame_end(void) {
    stat_frame_end();
    mode_refresh();
    if (s_census_every && ++s_census_frames % (u32)s_census_every == 0) {
        census_report();
    } else if (s_stats_every && s_census_frames++ % (u32)s_stats_every == 0) {
        PLOG_I("texmem: %s: %u textures live, %u KB (of which mip levels %u KB), %u mip chains built",
               k_mode_names[s_mode], (unsigned)s_live_n, (unsigned)(s_live_bytes >> 10), (unsigned)(s_live_mip >> 10),
               (unsigned)s_mip_extra_made);
    }
    if (s_tf_on) {
        const u32 tpu = PLAT_TICKS_PER_SEC / 1000000u;
        PLOG_I("texframe: %u lk %u up %u hash %u/%uB async %u/%u | us: total %u conv %u (dec %u create %u) hash %u",
               (unsigned)s_frame, s_tf.lookups, s_tf.uploads, s_tf.hashes, s_tf.hash_bytes, s_tf.async_hit,
               s_tf.async_hit + s_tf.async_miss, (unsigned)(s_tf.t_total / tpu), (unsigned)(s_tf.t_conv / tpu),
               (unsigned)(s_tf.t_dec / tpu), (unsigned)(s_tf.t_create / tpu), (unsigned)(s_tf.t_hash / tpu));
        memset(&s_tf, 0, sizeof(s_tf));
    }
    if (s_fast_mode && s_frame % 300 == 0) {
        PLOG_I("texfast: mode %d frame %u: lookups %u fast-hits %u mismatches %u", s_fast_mode, (unsigned)s_frame,
               s_fast_total, s_fast_hits, s_fast_mism);
    }
    pre_frame_end();
    s_frame++;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].handle && s_frame - s_cache[i].last_frame > EVICT_FRAMES) {
            entry_free(&s_cache[i]);
        }
    }
    for (int i = 0; i < (int)(sizeof(s_copies) / sizeof(s_copies[0])); i++) {
        /* Only big copies age out: the game keeps a pointer to a small copy (extdrv's crowd sprites: one texture
         * per animation frame, sampled again many frames later) and a freed target would sample stale memory. */
        if (s_copy_evict && s_copies[i].handle && s_frame - s_copies[i].last_frame > EVICT_FRAMES &&
            (u32)s_copies[i].w * s_copies[i].h > 128u * 128u) {
            plat_gpu_tex_free(s_copies[i].handle);
            s_copies[i].handle = 0;
            s_copies[i].addr = NULL;
            s_copy_gen++;
        }
    }
    s_uploads = 0;
}

u32 gx_tex_uploads_this_frame(void) { return s_uploads; }

/* ---- Copies ---- */

/* Registers a GXCopyTex destination; returns the render-target handle. */
u32 gx_tex_copy_target(const void* addr, u16 w, u16 h, u8 fmt) {
    CopyEntry* slot = NULL;
    for (int i = 0; i < (int)(sizeof(s_copies) / sizeof(s_copies[0])); i++) {
        if (s_copies[i].addr == addr) {
            slot = &s_copies[i];
            break;
        }
        if (slot == NULL && s_copies[i].handle == 0) {
            slot = &s_copies[i];
        }
    }
    if (slot == NULL) {
        slot = &s_copies[0];
    }
    u16 pw = (u16)pow2(w), ph = (u16)pow2(h);
    if (slot->addr != addr || slot->handle == 0) {
        s_copy_gen++; /* a new address joins the registry: the fast cache's non-copy hits may be shadowed */
    }
    if (slot->handle != 0 && (slot->addr != addr || slot->w != pw || slot->h != ph || slot->fmt != fmt)) {
        plat_gpu_tex_free(slot->handle);
        slot->handle = 0;
    }
    if (slot->handle == 0) {
        /* RGB565 copies stay RGB565 (the GameCube drops the alpha too, and the target is half the size);
         * every other copy format is kept as RGBA8 (RGB5A3's 3-bit alpha has no PICA equivalent) */
        slot->handle = plat_gpu_tex_create_target(pw, ph, fmt == GX_TF_RGB565 ? PLAT_TEXFMT_RGB565 : PLAT_TEXFMT_RGBA8);
        if (slot->handle == 0) {
            /* out of memory: drop every copy target not used this frame, release them now, retry once */
            u32 n = 0, kb = 0;
            for (int i = 0; i < (int)(sizeof(s_copies) / sizeof(s_copies[0])); i++) {
                CopyEntry* o = &s_copies[i];
                if (o != slot && o->handle != 0 && o->last_frame != s_frame) {
                    kb += (u32)o->w * o->h * (o->fmt == GX_TF_RGB565 ? 2 : 4) / 1024;
                    n++;
                    plat_gpu_tex_free(o->handle);
                    o->handle = 0;
                    o->addr = NULL;
                    s_copy_gen++;
                }
            }
            PLOG_I("GX: copy target memory low: evicted %u stale copy slots, %u KB", (unsigned)n, (unsigned)kb);
            plat_gpu_tex_reclaim();
            slot->handle = plat_gpu_tex_create_target(pw, ph, fmt == GX_TF_RGB565 ? PLAT_TEXFMT_RGB565 : PLAT_TEXFMT_RGBA8);
        }
        if (slot->handle == 0) {
            plat_fatal("GX: no memory for a %ux%u copy texture", w, h);
        }
    }
    slot->addr = addr;
    slot->w = pw;
    slot->h = ph;
    slot->fmt = fmt;
    slot->last_frame = s_frame;
    return slot->handle;
}

void gx_tex_init(void) {
    s_stats_every = plat_switch_int("texstats", 0);
    s_trust_on = plat_switch_int("textrust", 1) != 0;
    s_compact = plat_switch_int("texcompact", 0);
    if (s_compact) {
        PLOG_I("texcompact: %d", s_compact);
    }
    s_verify = plat_switch_int("texverify", 0) != 0;
    s_copy_evict = plat_switch_int("copyevict", 1) != 0;
    s_tf_on = plat_switch_int("texframe", 0) != 0;
    s_fast_mode = plat_switch_int("texfast", 0);
    s_async = plat_switch_int("texasync", 0) != 0 && s_trust_on;
    mode_init();
    s_x = calloc(CACHE_SIZE, sizeof(CacheExtra));
    if (s_x == NULL) {
        plat_fatal("GX: no memory for the texture cache");
    }
    if (s_census_every) {
        s_cen = calloc(1, sizeof(Census));
        if (s_cen == NULL) {
            s_census_every = 0;
        }
    }
    s_arena_hi = (u32)OSGetArenaHi();
    s_arena_lo = s_arena_hi - ARENA_BYTES;
    if (s_stats_every) {
        PLOG_I("texstats: every %d frames; arena %08x..%08x", s_stats_every, (unsigned)s_arena_lo,
               (unsigned)s_arena_hi);
    }
    s_rgba = memalign(64, 1024 * 1024 * 4);
    if (s_rgba == NULL) {
        plat_fatal("GX: no memory for texture conversion buffers");
    }
    if (s_async) {
        s_pre = calloc(PRE_MAX, sizeof(PreTex));
        s_pre_rgba = memalign(64, PRE_MAX_PIX * 4);
        if (s_pre == NULL || s_pre_rgba == NULL || !plat_job_enabled()) {
            PLOG_W("texasync: off (no memory or no job worker)");
            free(s_pre);
            free(s_pre_rgba);
            s_pre = NULL;
            s_pre_rgba = NULL;
            s_async = 0;
        } else {
            PLOG_I("texasync: on (%u slots, %u KB result cap)", (unsigned)PRE_MAX, (unsigned)(PRE_BYTES_MAX >> 10));
        }
    }
}
