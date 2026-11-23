/* Decoded display-list cache (switch gxcache, default on).
 *
 * GXCallDisplayList on a list of primitives decodes every vertex from the
 * list's big-endian bytes and the arrays it indexes (gx_pipe.c read_vertex)
 * before the vertices are transformed. A game calls most lists again every
 * frame with the same bytes, the same vertex format and the same arrays, so
 * the decoded vertices (GxVertex, exactly what read_vertex produced) are kept
 * and handed to gx_draw_vertices again, in the same calls, without parsing
 * or decoding. The transform and everything after it run as before, so a hit
 * draws exactly what a miss draws.
 *
 * An entry is used only when every input of the decode is what it was when
 * the entry was made:
 *   - the list: address, size and a hash of all its bytes (a game that
 *     rewrites a list in place, reloads code or data over it, or reuses the
 *     memory for another list changes the hash);
 *   - the vertex description, the attribute formats of each vertex format the
 *     list uses, the current position/texture matrix indices;
 *   - for each indexed attribute: the array address and stride; and a hash
 *     of every array element the list's vertices read, at the indices they
 *     read (kept in the entry), so CPU-animated arrays or other data loaded
 *     at the address are seen, at a cost proportional to what the list uses.
 * Everything is checked on every call, except (switch gxtrust, default on)
 * when the content-stable memory registry (include/plat/memstable.h) vouches
 * that the list and every array span it reads lie in covered memory that no
 * reported path changed since the entry's last full check: then the hashes
 * are skipped. The state inputs above are still compared every call.
 * A list whose bytes or arrays changed is decoded again; after
 * DLC_DYNAMIC_AFTER such changes it is marked dynamic and always decoded.
 * A list called with another vertex state gets a second entry (up to
 * DLC_VARIANTS per list).
 *
 * Not cached: lists with port opcodes (recorded GX state calls) or raw
 * primitives, and lists of more than DLC_CAP_VERTS vertices.
 *
 * Memory: entries are malloc'd from the regular heap (the CPU reads them;
 * the GPU never does), within a budget (switch gxcachekb, default 8192),
 * evicted least recently used first. The module knows nothing about the
 * game: any game's display lists go through it the same way.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gx_bigmem.h"
#include "gx_internal.h"
#include "plat/memstable.h"
#include "plat/plat.h"

#define DLC_ENTRIES 16384
#define DLC_BUCKETS 8192
#define DLC_CAP_VERTS 8192
#define DLC_CAP_DRAWS 1024
#define DLC_VARIANTS 4
#define DLC_DYNAMIC_AFTER 4
#define DLC_MAX_RANGES 12

enum { E_FREE = 0, E_BUILT, E_NOCACHE, E_DYNAMIC };

typedef GxDlcDraw DlcDraw;

/* An indexed attribute the list reads: its array and the bytes of one element. */
typedef struct {
    const u8* base;
    u8 attr;
    u8 stride;
    u8 elem;
    u8 pad;
    u16 imin, imax; /* the lowest and highest index the list reads */
} DlcRange;

typedef struct {
    const void* list;
    u32 nbytes;
    u32 dl_hash;
    u32 arr_hash;  /* hash of every array element the list reads, in decode order */
    u16 next;      /* bucket chain, entry index + 1 */
    u16 lru_prev, lru_next; /* entry index + 1 */
    u8 state;
    u8 changes;    /* times the content was found changed */
    u8 fmt_mask;   /* vertex formats the list uses */
    u8 nranges;
    u16 ndraws;
    u32 nverts;
    u32 last_frame;
    u32 trust_gen;   /* gxtrust: the registry's generation the contents are trusted at (0: not trusted) */
    u32 trust_epoch; /* the registry epoch of the last trust request that failed */
    u32 vs_ok;       /* gx_vtx_version at which same_state last matched (0: never) */
    u32 vsh_rec;     /* the list's pass-group record index (gx_vsh.c), a hint checked on use */
    u8 vtx_desc[GX_VA_MAX_ATTR];
    u8 cur_pnmtx;
    u8 cur_texmtx[8];
    void* blob;    /* DlcDraw[ndraws], DlcRange[nranges], GxAttrFmt[popcount(fmt_mask)][GX_VA_MAX_ATTR], GxVertex[nverts],
                    u16 index[nverts][nranges] */
    u32 blob_bytes;
    GxVshStream* vsh; /* gxvsh: the vertices as a PICA stream (linear memory), or NULL */
    u32 vsh_reason;   /* why there is none (GXV_R_*) */
    void* bbx;        /* gxbbox: the list's box record (gx_draw.c), or NULL */
} DlcEntry;

static DlcEntry* s_e; /* DLC_ENTRIES, heap (gx_bigmem.h) */
static u16 s_bucket[DLC_BUCKETS];
static u16 s_free_head; /* free list through .next */
static u16 s_lru_head, s_lru_tail; /* head = most recently used */
static int s_enabled = -1;
static int s_trust;         /* switch gxtrust (default 1): skip the hashes of entries in unchanged covered memory */
static u32 s_scribble_at;   /* switch gxscribble=N: at frame N, change a byte of a trusted array without telling the registry */
static const DlcEntry* s_scribbled;
static u32 s_budget;
static u32 s_used_bytes, s_live;

/* capture of the list being decoded */
int gx_dlc_capturing;
u16* gx_dlc_idx; /* GX_DLC_IDX_CAP, heap */
u32 gx_dlc_nidx;
static GxVertex* s_cap_v; /* DLC_CAP_VERTS, heap */
static DlcDraw s_cap_d[DLC_CAP_DRAWS];
static u32 s_cap_nv, s_cap_nd;
static u8 s_cap_fmt_mask;
static int s_cap_abort;
static const void* s_cap_list;
static u32 s_cap_bytes, s_cap_hash;
static DlcEntry* s_cap_replace; /* entry of the same list and state whose content changed */
static DlcEntry* s_cap_verify;  /* gxcache=2: a hit decoded again, to compare with the entry */
static u32 s_cap_epoch;         /* the registry's epoch when the decode began */

/* counters (per frame, summed into the gxstats line) */
static struct {
    u32 calls, hits, built, changed, variant, nocache, dynamic_calls, too_big, sparse, evicted, alloc_fail;
    u32 hit_verts;
    u32 verified, mismatched, full;
    u32 trusted, grants, stale, scribble_hits;
} s_st, s_sum;

static inline DlcEntry* E(u16 i) { return i ? &s_e[i - 1] : NULL; }
static inline u16 IDX(const DlcEntry* e) { return (u16)(e - s_e + 1); }

/* Frees an entry's decoded vertices and its stream (whose vertices the GPU
 * may still read: gx_vsh_stream_free holds them for the frames in flight). */
static void free_blob(DlcEntry* e) {
    if (e->blob) {
        free(e->blob);
        s_used_bytes -= e->blob_bytes;
        e->blob = NULL;
        e->blob_bytes = 0;
    }
    if (e->vsh) {
        s_used_bytes -= e->vsh->bytes;
        gx_vsh_stream_free(e->vsh);
        e->vsh = NULL;
    }
    gx_bbx_free(e->bbx);
    e->bbx = NULL;
}

static u32 bucket_of(const void* list, u32 n) {
    u32 h = (u32)(uintptr_t)list * 0x9E3779B1u ^ n * 0x85EBCA77u;
    return (h >> 20) & (DLC_BUCKETS - 1);
}

static void init(void) {
    s_enabled = plat_switch_int("gxcache", 1);
    if (s_enabled < 0 || s_enabled > 2) {
        s_enabled = 1;
    }
    s_e = gx_big_alloc("gxcache entries", sizeof(DlcEntry) * DLC_ENTRIES);
    gx_dlc_idx = gx_big_alloc("gxcache indices", sizeof(u16) * GX_DLC_IDX_CAP);
    s_cap_v = gx_big_alloc("gxcache capture", sizeof(GxVertex) * DLC_CAP_VERTS);
    s_budget = (u32)plat_switch_int("gxcachekb", 8192) * 1024u;
    s_trust = plat_switch_int("gxtrust", 1) != 0;
    s_scribble_at = (u32)plat_switch_int("gxscribble", 0);
    for (int i = 0; i < DLC_ENTRIES; i++) {
        s_e[i].next = (u16)(i + 2 <= DLC_ENTRIES ? i + 2 : 0);
    }
    s_free_head = 1;
    PLOG_I("gxcache: %s, budget %lu KB, %d entries, trust %d", s_enabled == 2 ? "verify" : s_enabled ? "on" : "off",
           (unsigned long)(s_budget / 1024), DLC_ENTRIES, s_trust);
}

int gx_dlcache_enabled(void) {
    if (s_enabled < 0) {
        init();
    }
    return s_enabled;
}

/* ---- LRU and buckets ---- */

static void lru_unlink(DlcEntry* e) {
    if (e->lru_prev) {
        E(e->lru_prev)->lru_next = e->lru_next;
    } else {
        s_lru_head = e->lru_next;
    }
    if (e->lru_next) {
        E(e->lru_next)->lru_prev = e->lru_prev;
    } else {
        s_lru_tail = e->lru_prev;
    }
    e->lru_prev = e->lru_next = 0;
}

static void lru_front(DlcEntry* e) {
    e->lru_prev = 0;
    e->lru_next = s_lru_head;
    if (s_lru_head) {
        E(s_lru_head)->lru_prev = IDX(e);
    }
    s_lru_head = IDX(e);
    if (!s_lru_tail) {
        s_lru_tail = IDX(e);
    }
}

static void entry_drop(DlcEntry* e) {
    u32 b = bucket_of(e->list, e->nbytes);
    u16* link = &s_bucket[b];
    while (*link && E(*link) != e) {
        link = &E(*link)->next;
    }
    if (*link) {
        *link = e->next;
    }
    lru_unlink(e);
    free_blob(e);
    memset(e, 0, sizeof(*e));
    e->next = s_free_head;
    s_free_head = IDX(e);
    s_live--;
}

/* The least recently used entry may go to make room, unless it was used this
 * frame or the last: when a frame draws more lists than fit, the ones that do
 * not fit stay uncached rather than every list pushing out the next (an LRU
 * over a cyclic working set larger than itself never hits). */
static int evictable(const DlcEntry* e) { return e != NULL && gx_perf_frame_no() - e->last_frame > 1; }

static DlcEntry* entry_new(const void* list, u32 nbytes) {
    if (!s_free_head) {
        if (!evictable(E(s_lru_tail))) {
            s_st.full++;
            return NULL;
        }
        entry_drop(E(s_lru_tail));
        s_st.evicted++;
    }
    DlcEntry* e = E(s_free_head);
    s_free_head = e->next;
    memset(e, 0, sizeof(*e));
    e->list = list;
    e->nbytes = nbytes;
    u32 b = bucket_of(list, nbytes);
    e->next = s_bucket[b];
    s_bucket[b] = IDX(e);
    lru_front(e);
    s_live++;
    return e;
}

/* ---- Entry layout ---- */

static inline DlcDraw* e_draws(const DlcEntry* e) { return (DlcDraw*)e->blob; }
static inline DlcRange* e_ranges(const DlcEntry* e) { return (DlcRange*)(e_draws(e) + e->ndraws); }
static inline GxAttrFmt* e_vat(const DlcEntry* e) { return (GxAttrFmt*)(e_ranges(e) + e->nranges); }
static inline GxVertex* e_verts(const DlcEntry* e) {
    uintptr_t p = (uintptr_t)(e_vat(e) + (u32)__builtin_popcount(e->fmt_mask) * GX_VA_MAX_ATTR);
    return (GxVertex*)((p + 7) & ~(uintptr_t)7);
}
static inline u16* e_idx(const DlcEntry* e) { return (u16*)(e_verts(e) + e->nverts); }

/* Hash of the array elements the list reads: for each vertex, each indexed
 * attribute's element at its recorded index. Only what the decode read is
 * hashed, so a list indexing a few vertices of a large array costs only those. */
static inline u32 mix(u32 h, u32 w) {
    h = (h ^ w) * 0x9E3779B1u;
    return h ^ (h >> 15);
}
static u32 gather_hash(const DlcRange* r, u32 nr, const u16* idx, u32 nverts) {
    u32 h = 0x5A17u;
    for (u32 v = 0; v < nverts; v++) {
        for (u32 a = 0; a < nr; a++) {
            const u8* p = r[a].base + (u32)*idx++ * r[a].stride;
            u32 w;
            switch (r[a].elem) {
            case 12:
                __builtin_memcpy(&w, p + 8, 4);
                h = mix(h, w);
                /* fall through */
            case 8:
                __builtin_memcpy(&w, p + 4, 4);
                h = mix(h, w);
                /* fall through */
            case 4:
                __builtin_memcpy(&w, p, 4);
                h = mix(h, w);
                break;
            default:
                for (u32 i = 0; i < r[a].elem; i++) {
                    h = mix(h, p[i]);
                }
                break;
            }
        }
    }
    return h;
}

/* The decode's inputs other than memory contents: the same as when the entry was made? */
static int same_state_full(const GxState* st, const DlcEntry* e) {
    if (!gx_mem_equal(e->vtx_desc, st->vtx_desc, sizeof(e->vtx_desc)) || e->cur_pnmtx != (u8)st->cur_pnmtx ||
        !gx_mem_equal(e->cur_texmtx, st->cur_texmtx, sizeof(e->cur_texmtx))) {
        return 0;
    }
    if (e->state != E_BUILT) {
        return 1;
    }
    const GxAttrFmt* v = e_vat(e);
    for (u32 f = 0; f < GX_MAX_VTXFMT; f++) {
        if (e->fmt_mask & (1u << f)) {
            if (!gx_mem_equal(v, st->vat[f], sizeof(st->vat[f]))) {
                return 0;
            }
            v += GX_VA_MAX_ATTR;
        }
    }
    const DlcRange* r = e_ranges(e);
    for (u32 i = 0; i < e->nranges; i++) {
        if (st->array_base[r[i].attr] != r[i].base || st->array_stride[r[i].attr] != r[i].stride) {
            return 0;
        }
    }
    return 1;
}

/* The full compare only when the vertex state changed since this entry last matched (st is always &gx). */
static int same_state(const GxState* st, DlcEntry* e) {
    if (e->vs_ok == gx_vtx_version && st == &gx) {
        return 1;
    }
    if (!same_state_full(st, e)) {
        return 0;
    }
    e->vs_ok = st == &gx ? gx_vtx_version : 0;
    return 1;
}

static int same_arrays(const DlcEntry* e) {
    return e->nranges == 0 || gather_hash(e_ranges(e), e->nranges, e_idx(e), e->nverts) == e->arr_hash;
}

/* gxtrust: after a full check that began at `epoch`, asks the registry
 * whether the list and every array span the list reads lie in covered memory
 * unchanged since then. Returns the trust generation, or 0. */
static u32 trust(DlcEntry* e, u32 epoch) {
    PlatMemSpan sp[1 + DLC_MAX_RANGES];
    sp[0] = (PlatMemSpan){e->list, e->nbytes};
    const DlcRange* r = e_ranges(e);
    for (u32 i = 0; i < e->nranges; i++) {
        sp[1 + i] = (PlatMemSpan){r[i].base + (u32)r[i].imin * r[i].stride,
                                  (u32)(r[i].imax - r[i].imin) * r[i].stride + r[i].elem};
    }
    u32 g = plat_memstable_trust(sp, 1 + e->nranges, epoch);
    if (g != 0) {
        s_st.grants++;
    } else {
        e->trust_epoch = epoch; /* asked again only once covered memory changed */
    }
    return g;
}

/* gxthread=3: the array spans of the entry this frame used for `list` (the memory its decode reads). */
u32 gx_dlcache_spans(const void* list, u32 nbytes, PlatMemSpan* sp, u32 max) {
    if (s_enabled <= 0) {
        return 0;
    }
    u32 frame = gx_perf_frame_no();
    for (DlcEntry* e = E(s_bucket[bucket_of(list, nbytes)]); e; e = E(e->next)) {
        if (e->list != list || e->nbytes != nbytes || e->state != E_BUILT || e->last_frame != frame) {
            continue;
        }
        const DlcRange* r = e_ranges(e);
        u32 n = 0;
        for (u32 i = 0; i < e->nranges && n < max; i++) {
            sp[n++] = (PlatMemSpan){r[i].base + (u32)r[i].imin * r[i].stride,
                                    (u32)(r[i].imax - r[i].imin) * r[i].stride + r[i].elem};
        }
        return n;
    }
    return 0;
}

static void replay(DlcEntry* e) {
    const DlcDraw* d = e_draws(e);
    const GxVertex* v = e_verts(e);
    if (gx_perf_on > 0) {
        gx_perf.dl_verts += e->nverts;
    }
    for (u32 i = 0; i < e->ndraws; i++) {
        gx_draw_vertices(d[i].prim, v + d[i].first, d[i].count);
    }
}

/* ---- Lookup (GXCallDisplayList) ---- */

int gx_dlcache_call(const GxState* st, const void* list, u32 nbytes) {
    u64 t0 = GX_PERF_T();
    u32 frame = gx_perf_frame_no();
    s_st.calls++;
    u32 h = 0;
    int have_h = 0;
#define LIST_HASH() (have_h ? h : (have_h = 1, h = gx_hash32(list, nbytes, 0xD15Cu)))
    DlcEntry* same_list_changed = NULL;
    int variants = 0;
    for (DlcEntry* e = E(s_bucket[bucket_of(list, nbytes)]); e; e = E(e->next)) {
        if (e->list != list || e->nbytes != nbytes) {
            continue;
        }
        variants++;
        if (e->state == E_NOCACHE) {
            /* not hashed again: a list of state calls stays one (if the memory is
             * reused for another list, that one goes uncached until the entry
             * is evicted) */
            s_st.nocache++;
            e->last_frame = frame;
            lru_unlink(e);
            lru_front(e);
            goto decode_plain;
        }
        if (e->state == E_DYNAMIC) {
            s_st.dynamic_calls++;
            e->last_frame = frame;
            lru_unlink(e);
            lru_front(e);
            goto decode_plain;
        }
        if (!same_state(st, e)) {
            continue;
        }
        int ok = 0;
        if (e->state == E_BUILT) {
            if (s_trust && e->trust_gen != 0 && e->trust_gen == plat_memstable_gen()) {
                /* the registry vouches for the list and its arrays: no hashing */
                ok = 1;
                s_st.trusted++;
                if (e == s_scribbled) {
                    s_st.scribble_hits++;
                }
                if (s_enabled == 2 && (e->dl_hash != LIST_HASH() || !same_arrays(e))) {
                    s_st.stale++; /* verify: trust skipped a change (the decode below compares too) */
                }
            } else {
                u32 epoch = plat_memstable_epoch();
                ok = e->dl_hash == LIST_HASH() && same_arrays(e);
                e->trust_gen = 0;
                if (ok && s_trust && e->trust_epoch != epoch) {
                    e->trust_gen = trust(e, epoch);
                }
            }
        }
        if (ok) {
            s_st.hits++;
            s_st.hit_verts += e->nverts;
            e->last_frame = frame;
            lru_unlink(e);
            lru_front(e);
            if (s_enabled == 2) {
                /* verify: decode it again and compare (gx_dlc_cap_finish) */
                s_cap_verify = e;
                s_cap_replace = NULL;
                s_cap_list = list;
                s_cap_bytes = nbytes;
                s_cap_hash = e->dl_hash;
                s_cap_nv = s_cap_nd = 0;
                s_cap_fmt_mask = 0;
                s_cap_abort = 0;
                gx_dlc_nidx = 0;
                gx_dlc_capturing = 1;
                goto decode_plain;
            }
            if (gx_perf_on > 0) {
                gx_perf.t_cache += plat_ticks() - t0;
            }
            if (gx_vsh_on) {
                /* the pass group decides: the shader draws the stream, or the CPU replays as before */
                gx_vsh_rec_hint = &e->vsh_rec;
                gx_bbx_hint = &e->bbx;
                if (gx_draw_vsh(e->vsh, e->vsh_reason, e->list, e->nbytes, e->nverts, e_verts(e), e_draws(e), e->ndraws)) {
                    if (gx_perf_on > 0) {
                        gx_perf.dl_verts += e->nverts;
                    }
                    return 1;
                }
                gx_vsh_noted = 1;
                replay(e);
                gx_vsh_noted = 0;
                return 1;
            }
            replay(e);
            return 1;
        }
        same_list_changed = e; /* same list and state, other bytes or array contents */
    }
    if (same_list_changed != NULL) {
        s_st.changed++;
        if (s_scribbled != NULL) {
            static int told;
            if (!told) {
                told = 1;
                PLOG_I("gxscribble: the full check saw list %p change (decoded again)", list);
            }
        }
        if (same_list_changed->changes + 1 >= DLC_DYNAMIC_AFTER) {
            /* keeps changing: decode it every time from now on */
            DlcEntry* e = same_list_changed;
            free_blob(e);
            e->state = E_DYNAMIC;
            e->changes++;
            e->last_frame = frame;
            goto decode_plain;
        }
    } else if (variants >= DLC_VARIANTS) {
        s_st.variant++;
        goto decode_plain;
    } else if (variants > 0) {
        s_st.variant++;
    }
    /* decode it, capturing the vertices */
    s_cap_epoch = plat_memstable_epoch();
    gx_dlc_capturing = 1;
    s_cap_list = list;
    s_cap_bytes = nbytes;
    s_cap_hash = LIST_HASH();
#undef LIST_HASH
    s_cap_replace = same_list_changed;
    s_cap_nv = s_cap_nd = 0;
    s_cap_fmt_mask = 0;
    s_cap_abort = 0;
    gx_dlc_nidx = 0;
decode_plain:
    if (gx_perf_on > 0) {
        gx_perf.t_cache += plat_ticks() - t0;
    }
    return 0;
}

/* ---- Capture (gx_pipe.c draw_from_reader) ---- */

GxVertex* gx_dlc_cap_reserve(u8 fmt, u32 n) {
    if (s_cap_abort || s_cap_nv + n > DLC_CAP_VERTS || s_cap_nd >= DLC_CAP_DRAWS) {
        if (!s_cap_abort) {
            s_cap_abort = 2; /* too big */
        }
        return NULL;
    }
    s_cap_fmt_mask |= (u8)(1u << (fmt & 7));
    return &s_cap_v[s_cap_nv];
}

void gx_dlc_cap_draw(u8 prim, const GxVertex* v, u32 n) {
    if (s_cap_abort || v != &s_cap_v[s_cap_nv]) {
        return;
    }
    s_cap_d[s_cap_nd++] = (DlcDraw){prim, 0, 0, s_cap_nv, n};
    s_cap_nv += n;
}

static int same_vertex(const GxVertex* a, const GxVertex* b) {
    if (memcmp(a->pos, b->pos, sizeof(a->pos)) != 0 || a->has_nrm != b->has_nrm || a->pnmtx != b->pnmtx ||
        memcmp(a->texmtx, b->texmtx, sizeof(a->texmtx)) != 0 || memcmp(a->clr, b->clr, sizeof(a->clr)) != 0 ||
        memcmp(a->tex, b->tex, sizeof(a->tex)) != 0) {
        return 0;
    }
    return !a->has_nrm || memcmp(a->nrm, b->nrm, sizeof(a->nrm)) == 0;
}

/* The capture (a fresh decode) equals entry e: the same draws and vertices. */
static int same_decode(const DlcEntry* e) {
    if (s_cap_abort || s_cap_nd != e->ndraws || s_cap_nv != e->nverts) {
        return 0;
    }
    const DlcDraw* d = e_draws(e);
    for (u32 i = 0; i < s_cap_nd; i++) {
        if (d[i].prim != s_cap_d[i].prim || d[i].first != s_cap_d[i].first || d[i].count != s_cap_d[i].count) {
            return 0;
        }
    }
    const GxVertex* v = e_verts(e);
    for (u32 i = 0; i < s_cap_nv; i++) {
        if (!same_vertex(&v[i], &s_cap_v[i])) {
            return 0;
        }
    }
    return 1;
}

/* ---- Exactness of a fixed-point stream (gxstats only) ----
 * Fractional bits a float needs (0..149), or -1 for a value that is not finite. */
static int frac_bits(f32 f) {
    union {
        f32 f;
        u32 u;
    } c = {f};
    u32 e = c.u >> 23 & 0xFF;
    u32 m = c.u & 0x7FFFFF;
    if (e == 0xFF) {
        return -1;
    }
    if (e == 0 && m == 0) {
        return 0;
    }
    if (e != 0) {
        m |= 0x800000;
    } else {
        e = 1;
    }
    int k = 23 - ((int)e - 127) - __builtin_ctz(m);
    return k > 0 ? k : 0;
}

/* Could these n x k floats be s16 values times one power of two, exactly? */
static int exact_s16(const f32* first, u32 n, u32 k, u32 stride_f32) {
    int need = 0;
    f32 big = 0;
    for (u32 i = 0; i < n; i++) {
        const f32* v = first + i * stride_f32;
        for (u32 j = 0; j < k; j++) {
            int b = frac_bits(v[j]);
            if (b < 0) {
                return 0;
            }
            need = b > need ? b : need;
            f32 a = v[j] < 0 ? -v[j] : v[j];
            big = a > big ? a : big;
        }
    }
    if (need > 60) {
        return 0;
    }
    /* |v| * 2^need must fit s16 */
    f32 lim = 32767.0f;
    for (int i = 0; i < need; i++) {
        lim *= 0.5f;
    }
    return big <= lim;
}

static u32 s_tot_entries, s_tot_verts, s_pos_exact_entries, s_pos_exact_verts, s_tex_exact_entries,
    s_tex_exact_verts, s_tex_entries, s_tex_verts;

static void exactness(const GxVertex* v, u32 n, int has_tex0) {
    const u32 stride = sizeof(GxVertex) / sizeof(f32);
    s_tot_entries++;
    s_tot_verts += n;
    if (exact_s16(v->pos, n, 3, stride)) {
        s_pos_exact_entries++;
        s_pos_exact_verts += n;
    }
    if (has_tex0) {
        s_tex_entries++;
        s_tex_verts += n;
        if (exact_s16(v->tex[0], n, 2, stride)) {
            s_tex_exact_entries++;
            s_tex_exact_verts += n;
        }
    }
}

static u32 comp_bytes(u8 type) { return type == GX_U8 || type == GX_S8 ? 1 : type == GX_U16 || type == GX_S16 ? 2 : 4; }

/* Bytes read_vertex reads from one array element of attribute a in format fmt. */
static u32 elem_bytes(int a, const GxAttrFmt* f) {
    if (a == GX_VA_POS) {
        return (f->cnt == GX_POS_XY ? 2 : 3) * comp_bytes(f->type);
    }
    if (a == GX_VA_NRM) {
        return 3 * comp_bytes(f->type);
    }
    if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
        switch (f->type) {
        case GX_RGB565:
        case GX_RGBA4: return 2;
        case GX_RGBA6: return 3;
        case GX_RGB8:
        case GX_RGBX8:
        case GX_RGBA8: return 4;
        default: return 0;
        }
    }
    return (f->cnt == GX_TEX_S ? 1 : 2) * comp_bytes(f->type);
}

/* After the list ran: `kind` is what it held (1 primitives, 2 port opcodes, 4 raw primitives). */
void gx_dlc_cap_finish(const GxState* st, u32 kind) {
    if (!gx_dlc_capturing) {
        return;
    }
    gx_dlc_capturing = 0;
    if (s_cap_verify != NULL) {
        DlcEntry* e = s_cap_verify;
        s_cap_verify = NULL;
        s_st.verified++;
        if (!same_decode(e)) {
            if (s_st.mismatched++ == 0 && s_sum.mismatched == 0) {
                PLOG_W("gxcache: cached list %p (%lu bytes) decodes differently now", e->list, (unsigned long)e->nbytes);
            }
        }
        return;
    }
    u64 t0 = GX_PERF_T();
    u32 frame = gx_perf_frame_no();
    DlcEntry* old = s_cap_replace;
    s_cap_replace = NULL;
    int cacheable = kind == 1 && !s_cap_abort && s_cap_nd > 0;
    if (s_cap_abort == 2) {
        s_st.too_big++;
    }
    /* ranges of the indexed attributes */
    DlcRange ranges[DLC_MAX_RANGES];
    u32 nr = 0;
    for (int a = GX_VA_POS; cacheable && a <= GX_VA_TEX7; a++) {
        u8 d = st->vtx_desc[a];
        int attr = a;
        if (a == GX_VA_NRM && d == GX_NONE) {
            d = st->vtx_desc[GX_VA_NBT];
        }
        if (d != GX_INDEX8 && d != GX_INDEX16) {
            continue;
        }
        u32 eb = 0;
        for (u32 f = 0; f < GX_MAX_VTXFMT; f++) {
            if (s_cap_fmt_mask & (1u << f)) {
                u32 b = elem_bytes(attr, &st->vat[f][attr]);
                eb = b > eb ? b : eb;
            }
        }
        if (nr == DLC_MAX_RANGES) {
            cacheable = 0;
            break;
        }
        ranges[nr++] = (DlcRange){st->array_base[attr], (u8)attr, st->array_stride[attr], (u8)eb, 0, 0xFFFF, 0};
    }
    /* every vertex read one index per indexed attribute, in attribute order */
    if (cacheable && (gx_dlc_nidx > GX_DLC_IDX_CAP || gx_dlc_nidx != s_cap_nv * nr)) {
        s_st.sparse++; /* counted with the odd cases */
        cacheable = 0;
    }
    for (u32 v = 0; cacheable && v < s_cap_nv; v++) {
        for (u32 a = 0; a < nr; a++) {
            u16 i = gx_dlc_idx[v * nr + a];
            ranges[a].imin = i < ranges[a].imin ? i : ranges[a].imin;
            ranges[a].imax = i > ranges[a].imax ? i : ranges[a].imax;
        }
    }
    DlcEntry* e = old;
    if (e != NULL) {
        /* the same list and state with other contents: replace that entry */
        u8 changes = e->changes;
        free_blob(e);
        e->changes = (u8)(changes + 1);
    }
    if (!cacheable) {
        if (e == NULL) {
            e = entry_new(s_cap_list, s_cap_bytes);
        }
        if (e != NULL) {
            /* remembered so the next call skips the capture (and its state is not checked) */
            e->state = E_NOCACHE;
            e->dl_hash = s_cap_hash;
            memcpy(e->vtx_desc, st->vtx_desc, sizeof(e->vtx_desc)); e->vs_ok = 0;
            e->cur_pnmtx = (u8)st->cur_pnmtx;
            memcpy(e->cur_texmtx, st->cur_texmtx, sizeof(e->cur_texmtx));
            e->last_frame = frame;
        }
        goto out;
    }
    u32 nfmt = (u32)__builtin_popcount(s_cap_fmt_mask);
    u32 head = s_cap_nd * sizeof(DlcDraw) + nr * sizeof(DlcRange) + nfmt * sizeof(st->vat[0]);
    head = (head + 7) & ~7u;
    u32 total = head + s_cap_nv * sizeof(GxVertex) + s_cap_nv * nr * sizeof(u16) + 8;
    if (total > s_budget / 4) {
        s_st.too_big++;
        if (e != NULL) {
            e->state = E_NOCACHE;
        }
        goto out;
    }
    while (s_used_bytes + total > s_budget) {
        DlcEntry* victim = E(s_lru_tail);
        if (victim == e) {
            lru_unlink(e);
            lru_front(e);
            continue;
        }
        if (!evictable(victim)) {
            /* the budget holds only lists in use: this one stays uncached */
            s_st.full++;
            if (e != NULL) {
                entry_drop(e); /* its old vertices are gone already */
            }
            goto out;
        }
        entry_drop(victim);
        s_st.evicted++;
    }
    if (e == NULL) {
        e = entry_new(s_cap_list, s_cap_bytes);
        if (e == NULL) {
            goto out;
        }
    }
    e->blob = malloc(total);
    if (e->blob == NULL) {
        s_st.alloc_fail++;
        entry_drop(e);
        goto out;
    }
    e->blob_bytes = total;
    s_used_bytes += total;
    e->state = E_BUILT;
    e->dl_hash = s_cap_hash;
    e->ndraws = (u16)s_cap_nd;
    e->nranges = (u8)nr;
    e->fmt_mask = s_cap_fmt_mask;
    e->nverts = s_cap_nv;
    e->last_frame = frame;
    memcpy(e->vtx_desc, st->vtx_desc, sizeof(e->vtx_desc)); e->vs_ok = 0;
    e->cur_pnmtx = (u8)st->cur_pnmtx;
    memcpy(e->cur_texmtx, st->cur_texmtx, sizeof(e->cur_texmtx));
    memcpy(e_draws(e), s_cap_d, s_cap_nd * sizeof(DlcDraw));
    memcpy(e_ranges(e), ranges, nr * sizeof(DlcRange));
    GxAttrFmt* v = e_vat(e);
    for (u32 f = 0; f < GX_MAX_VTXFMT; f++) {
        if (s_cap_fmt_mask & (1u << f)) {
            memcpy(v, st->vat[f], sizeof(st->vat[f]));
            v += GX_VA_MAX_ATTR;
        }
    }
    memcpy(e_verts(e), s_cap_v, s_cap_nv * sizeof(GxVertex));
    memcpy(e_idx(e), gx_dlc_idx, s_cap_nv * nr * sizeof(u16));
    e->arr_hash = nr ? gather_hash(ranges, nr, gx_dlc_idx, s_cap_nv) : 0;
    lru_unlink(e);
    lru_front(e);
    s_st.built++;
    e->trust_gen = 0;
    e->trust_epoch = 0;
    if (s_trust) {
        e->trust_gen = trust(e, s_cap_epoch);
    }
    e->vsh = NULL;
    e->vsh_reason = 0;
    if (gx_vsh_on) {
        e->vsh = gx_vsh_stream_build(e_verts(e), e->nverts, e_draws(e), e->ndraws, st->vtx_desc,
                                     (const GxAttrFmt(*)[GX_VA_MAX_ATTR])e_vat(e), e->fmt_mask, &e->vsh_reason);
        if (e->vsh != NULL) {
            s_used_bytes += e->vsh->bytes;
        }
    }
    if (gx_perf_on > 0 && sizeof(GxVertex) % sizeof(f32) == 0) {
        exactness(e_verts(e), e->nverts, st->vtx_desc[GX_VA_TEX0] != GX_NONE);
    }
out:
    if (gx_perf_on > 0) {
        gx_perf.t_cache += plat_ticks() - t0;
    }
}

/* ---- Counters ---- */

/* switch gxscribble=N (a test of the verify mode, not for play): at frame N,
 * one array byte that a cached list reads is changed with a plain CPU store,
 * without a cache flush or any report to the registry. The lowest byte of
 * the first element is xored with 1 (a position's lowest mantissa bit).
 * gxcache=2 must count the entry as mismatched (and stale, when it was
 * trusted); gxcache=1 with trust keeps drawing the old vertices (the
 * contract: covered memory changes only through reported paths), and the log
 * counts those hits. Without trust the hash sees the change. */
static void scribble(void) {
    DlcEntry* pick = NULL;
    for (DlcEntry* e = E(s_lru_head); e != NULL; e = E(e->lru_next)) {
        if (e->state != E_BUILT || e->nranges == 0) {
            continue;
        }
        if (e->trust_gen != 0 && e->trust_gen == plat_memstable_gen()) {
            pick = e;
            break;
        }
        if (pick == NULL) {
            pick = e;
        }
    }
    if (pick == NULL) {
        PLOG_W("gxscribble: no cached list with arrays");
        return;
    }
    const DlcRange* r = e_ranges(pick);
    u8* p = (u8*)r[0].base + (u32)e_idx(pick)[0] * r[0].stride;
    *p ^= 1;
    s_scribbled = pick;
    PLOG_I("gxscribble: frame %lu: byte %p (attr %u of list %p, %lu bytes) xored with 1, entry %s", (unsigned long)gx_perf_frame_no(),
           (void*)p, r[0].attr, pick->list, (unsigned long)pick->nbytes,
           pick->trust_gen != 0 && pick->trust_gen == plat_memstable_gen() ? "trusted" : "not trusted");
}

void gx_dlcache_frame(void) {
    u32* d = (u32*)&s_sum;
    const u32* s = (const u32*)&s_st;
    for (size_t i = 0; i < sizeof(s_st) / 4; i++) {
        d[i] += s[i];
    }
    memset(&s_st, 0, sizeof(s_st));
    if (s_scribble_at != 0 && s_enabled > 0 && gx_perf_frame_no() == s_scribble_at) {
        scribble();
    }
}

void gx_dlcache_log(u32 frames) {
    if (s_enabled <= 0) {
        return;
    }
    u32 f = frames ? frames : 1;
    const typeof(s_sum)* a = &s_sum;
    u32 pct = a->calls ? (u32)((u64)a->hits * 100u / a->calls) : 0;
    PLOG_I("gxstats   cache: calls %lu hits %lu (%lu%%, %lu verts) built %lu changed %lu variant %lu nocache %lu "
           "dynamic %lu too-big %lu odd %lu evicted %lu full %lu alloc-fail %lu verified %lu mismatched %lu | "
           "%lu entries %lu KB of %lu KB",
           (unsigned long)(a->calls / f), (unsigned long)(a->hits / f), (unsigned long)pct,
           (unsigned long)(a->hit_verts / f), (unsigned long)(a->built / f), (unsigned long)(a->changed / f),
           (unsigned long)(a->variant / f), (unsigned long)(a->nocache / f), (unsigned long)(a->dynamic_calls / f),
           (unsigned long)(a->too_big / f), (unsigned long)(a->sparse / f), (unsigned long)a->evicted,
           (unsigned long)a->full,
           (unsigned long)a->alloc_fail, (unsigned long)a->verified, (unsigned long)a->mismatched,
           (unsigned long)s_live, (unsigned long)(s_used_bytes / 1024),
           (unsigned long)(s_budget / 1024));
    PlatMemStableStats ms;
    plat_memstable_stats(&ms);
    PLOG_I("gxstats   trust: %lu hits unhashed (%lu%%), grants %lu, stale %lu | registry: %lu ranges %lu KB, per frame "
           "fills %lu writes %lu releases %lu; trust revoked %lu times in %lu frames, fills dropped %lu",
           (unsigned long)(a->trusted / f), (unsigned long)(a->hits ? (u64)a->trusted * 100u / a->hits : 0),
           (unsigned long)(a->grants / f), (unsigned long)a->stale,
           (unsigned long)ms.ranges,
           (unsigned long)ms.covered_kb, (unsigned long)(ms.fills / f), (unsigned long)(ms.writes / f),
           (unsigned long)(ms.releases / f), (unsigned long)ms.revokes, (unsigned long)f, (unsigned long)ms.dropped);
    if (s_scribbled) {
        PLOG_I("gxstats   scribble: %lu trusted hits on the scribbled entry, %lu mismatched, %lu stale in these frames",
               (unsigned long)a->scribble_hits, (unsigned long)a->mismatched, (unsigned long)a->stale);
    }
    if (s_tot_entries) {
        PLOG_I("gxstats   cache entries built so far: %lu (%lu verts); positions exact as s16 x 2^-k: %lu entries "
               "(%lu verts); with TEX0: %lu (%lu verts), TEX0 exact: %lu (%lu verts)",
               (unsigned long)s_tot_entries, (unsigned long)s_tot_verts, (unsigned long)s_pos_exact_entries,
               (unsigned long)s_pos_exact_verts, (unsigned long)s_tex_entries, (unsigned long)s_tex_verts,
               (unsigned long)s_tex_exact_entries, (unsigned long)s_tex_exact_verts);
    }
    memset(&s_sum, 0, sizeof(s_sum));
}
