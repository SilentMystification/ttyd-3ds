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
#include <string.h>

#include "gx_internal.h"
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
} CacheEntry;

static CacheEntry s_cache[CACHE_SIZE];
static u32 s_frame = 1;
static u32 s_uploads;
static u8* s_rgba;      /* decode scratch (1024x1024 RGBA8) */
static u8* s_tiled;

/* Copies made by GXCopyTex: destination address -> render-target texture. */
typedef struct {
    const void* addr;
    u32 handle;
    u16 w, h;
    u8 fmt;
} CopyEntry;
static CopyEntry s_copies[64];

/* ---- Objects ---- */

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrap_s,
                  GXTexWrapMode wrap_t, u8 mipmap) {
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

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrap_s,
                    GXTexWrapMode wrap_t, u8 mipmap, u32 tlut_name) {
    GXInitTexObj(obj, image_ptr, width, height, (GXTexFmt)format, wrap_s, wrap_t, mipmap);
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

void GXInitTexObjData(GXTexObj* obj, void* image_ptr) { ((GxTexObj*)obj)->image = image_ptr; }
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

/* RGBA8 rows (row 0 = GX t 0) -> PICA texture. Returns the handle. */
static u32 upload_rgba8(const u8* px, u32 w, u32 h, u16* out_w, u16* out_h) {
    int gray = 1, opaque = 1, binary = 1;
    for (u32 i = 0; i < w * h; i++) {
        const u8* p = px + i * 4;
        gray &= p[0] == p[1] && p[1] == p[2];
        opaque &= p[3] == 255;
        binary &= p[3] == 0 || p[3] == 255;
    }
    u32 fmt = gray ? PLAT_TEXFMT_LA8 : opaque ? PLAT_TEXFMT_RGB565 : binary ? PLAT_TEXFMT_RGBA5551 : PLAT_TEXFMT_RGBA8;
    u32 bpp = fmt == PLAT_TEXFMT_RGBA8 ? 4 : 2;
    u32 pw = pow2(w), ph = pow2(h);
    u32 tiles_w = pw >> 3;
    for (u32 my = 0; my < ph; my++) {
        /* PICA samples t = 0 at the last row in memory */
        const u8* row = px + (size_t)(((ph - 1 - my) * h / ph) * w) * 4;
        for (u32 mx = 0; mx < pw; mx++) {
            const u8* p = row + (size_t)(pw == w ? mx : mx * w / pw) * 4;
            u32 idx = ((my >> 3) * tiles_w + (mx >> 3)) * 64 + morton8(mx & 7, my & 7);
            switch (fmt) {
            case PLAT_TEXFMT_LA8:
                s_tiled[idx * 2] = p[3];
                s_tiled[idx * 2 + 1] = p[0];
                break;
            case PLAT_TEXFMT_RGB565:
                ((u16*)s_tiled)[idx] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                break;
            case PLAT_TEXFMT_RGBA5551:
                ((u16*)s_tiled)[idx] =
                    (u16)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7));
                break;
            default: ((u32*)s_tiled)[idx] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; break;
            }
        }
    }
    *out_w = (u16)pw;
    *out_h = (u16)ph;
    s_uploads++;
    return plat_gpu_tex_create(pw, ph, fmt, s_tiled, pw * ph * bpp);
}

static u32 hash_bytes(const u8* p, u32 n) {
    u32 h = 2166136261u;
    if (n <= 32 * 1024) {
        for (u32 i = 0; i < n; i++) {
            h = (h ^ p[i]) * 16777619u;
        }
        return h;
    }
    /* large textures: 64 spread samples of 64 bytes */
    for (u32 s = 0; s < 64; s++) {
        const u8* q = p + (size_t)(n - 64) * s / 63;
        for (u32 i = 0; i < 64; i++) {
            h = (h ^ q[i]) * 16777619u;
        }
    }
    return h;
}

static void entry_free(CacheEntry* e) {
    if (e->handle != 0) {
        plat_gpu_tex_free(e->handle);
    }
    memset(e, 0, sizeof(*e));
}

/* Converts (or finds) the texture of texture map slot `map`. */
u32 gx_tex_handle_for(int map, u16* out_w, u16* out_h) {
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
            return s_copies[i].handle;
        }
    }
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
                u32 h = hash_bytes(t->image, bytes);
                if (tl) {
                    h ^= hash_bytes(tl->data, (u32)tl->n_entries * 2);
                }
                e->checked_frame = s_frame;
                if (h != e->hash) {
                    entry_free(e);
                    free_e = e;
                    break;
                }
            }
            e->last_frame = s_frame;
            *out_w = e->tex_w;
            *out_h = e->tex_h;
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
    gx_tex_decode_rgba8(t->image, t->width, t->height, t->format, tl ? tl->data : NULL, tl ? tl->format : 0,
                        s_rgba);
    CacheEntry* e = free_e;
    e->handle = upload_rgba8(s_rgba, t->width, t->height, &e->tex_w, &e->tex_h);
    if (e->handle == 0) {
        /* texture memory full: free everything not used this frame, retry */
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (s_cache[i].handle && s_cache[i].last_frame != s_frame) {
                entry_free(&s_cache[i]);
            }
        }
        e->handle = upload_rgba8(s_rgba, t->width, t->height, &e->tex_w, &e->tex_h);
        if (e->handle == 0) {
            plat_fatal("GX: no texture memory for a %ux%u texture", t->width, t->height);
        }
    }
    e->image = t->image;
    e->w = t->width;
    e->h = t->height;
    e->fmt = t->format;
    e->is_ci = t->is_ci;
    e->tlut = tl ? tl->data : NULL;
    e->tlut_fmt = tl ? tl->format : 0;
    e->hash = hash_bytes(t->image, bytes) ^ (tl ? hash_bytes(tl->data, (u32)tl->n_entries * 2) : 0);
    e->last_frame = e->checked_frame = s_frame;
    *out_w = e->tex_w;
    *out_h = e->tex_h;
    return e->handle;
}

void gx_tex_invalidate_all(void) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].handle) {
            entry_free(&s_cache[i]);
        }
    }
}

void gx_tex_frame_end(void) {
    s_frame++;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].handle && s_frame - s_cache[i].last_frame > EVICT_FRAMES) {
            entry_free(&s_cache[i]);
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
    if (slot->handle != 0 && (slot->addr != addr || slot->w != pw || slot->h != ph || slot->fmt != fmt)) {
        plat_gpu_tex_free(slot->handle);
        slot->handle = 0;
    }
    if (slot->handle == 0) {
        slot->handle = plat_gpu_tex_create_target(pw, ph, PLAT_TEXFMT_RGBA8);
        if (slot->handle == 0) {
            plat_fatal("GX: no memory for a %ux%u copy texture", w, h);
        }
    }
    slot->addr = addr;
    slot->w = pw;
    slot->h = ph;
    slot->fmt = fmt;
    return slot->handle;
}

void gx_tex_init(void) {
    s_rgba = memalign(64, 1024 * 1024 * 4);
    s_tiled = memalign(64, 1024 * 1024 * 4);
    if (s_rgba == NULL || s_tiled == NULL) {
        plat_fatal("GX: no memory for texture conversion buffers");
    }
}
