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
    u32 gen;          /* gxbatch: bumped whenever the entry's content or stream goes (kept across reuse of the slot) */
    u16 bat_head;     /* gxbatch: chain of the batches that start with this entry (index + 1) */
    u16 bat;          /* gxbatch: a batch that holds this entry (index + 1): while it is in use the entry is too */
    u16 sdl;          /* gxsdl: the slot of the list's pre-decoded state calls (index + 1), a hint checked on use */
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
static u32 s_spread; /* switch dlspread=N (default 0: off): at most N new entries are built per frame; the other lists draw uncached that frame */
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
    u32 deferred; /* dlspread: builds left for a later frame */
    u32 relinks; /* lru_skip_batched: entries moved back to the front */
    u32 hit_verts;
    u32 verified, mismatched, full;
    u32 trusted, grants, stale, scribble_hits;
} s_st, s_sum;

static inline DlcEntry* E(u16 i) { return i ? &s_e[i - 1] : NULL; }
static inline u16 IDX(const DlcEntry* e) { return (u16)(e - s_e + 1); }

/* gxbatch (below) */
static int s_bat_on;        /* switch gxbatch: 0 off, 1 on, 2 verify */
static u32 s_dlc_epoch = 1; /* bumped whenever any entry's gen is */
static void bat_kill_chain(DlcEntry* e);
static void bat_init(void);
static int bat_open_chain(DlcEntry* e);
static void bat_log(u32 frames);
static u32 s_bat_log_every, s_bat_log_frames; /* the gxbatch line every gxthreadstats (or gxstats) frames */

/* Frees an entry's decoded vertices and its stream (whose vertices the GPU
 * may still read: gx_vsh_stream_free holds them for the frames in flight). */
static void free_blob(DlcEntry* e) {
    e->gen++;
    s_dlc_epoch++;
    if (e->bat_head) {
        bat_kill_chain(e);
    }
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
    s_spread = (u32)plat_switch_int("dlspread", 0);
    for (int i = 0; i < DLC_ENTRIES; i++) {
        s_e[i].next = (u16)(i + 2 <= DLC_ENTRIES ? i + 2 : 0);
    }
    s_free_head = 1;
    PLOG_I("gxcache: %s, budget %lu KB, %d entries, trust %d", s_enabled == 2 ? "verify" : s_enabled ? "on" : "off",
           (unsigned long)(s_budget / 1024), DLC_ENTRIES, s_trust);
    bat_init();
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
    u32 gen = e->gen;
    memset(e, 0, sizeof(*e));
    e->gen = gen;
    e->next = s_free_head;
    s_free_head = IDX(e);
    s_live--;
}

/* The least recently used entry may go to make room, unless it was used this
 * frame or the last: when a frame draws more lists than fit, the ones that do
 * not fit stay uncached rather than every list pushing out the next (an LRU
 * over a cyclic working set larger than itself never hits). */
static int evictable(const DlcEntry* e) {
    /* a batch draws its members without stamping them: a member of a recently used batch is in use */
    return e != NULL && gx_perf_frame_no() - e->last_frame > 1 && !(e->bat && gx_batch_recent(e->bat));
}

/* gxbatch: a batch's draws do not move its members in the LRU list or stamp them, so the tail can be an entry
 * a batch still draws. Such entries go back to the front (they are in use); what is left at the tail is the
 * least recently used entry no batch needs. */
#define LRU_SKIP_MAX 256
static u32 s_skip_full_frame; /* the frame in which a scan found nothing but batched entries (+1) */
static int lru_skip_batched(void) {
    u32 frame = gx_perf_frame_no();
    if (s_skip_full_frame == frame + 1) {
        return 0; /* the tail was all batched earlier this frame: nothing new can have left it */
    }
    for (u32 n = 0; n < LRU_SKIP_MAX && s_lru_tail; n++) {
        DlcEntry* t = E(s_lru_tail);
        if (!t->bat || !gx_batch_recent(t->bat)) {
            return 1;
        }
        s_st.relinks++;
        t->last_frame = frame;
        lru_unlink(t);
        lru_front(t);
    }
    /* A tail of batched entries this long: all of them are in use. Allocation fails (cheaply) for the rest of
     * the frame instead of walking the whole list again at every new entry. */
    s_skip_full_frame = frame + 1;
    return 0;
}

static DlcEntry* entry_new(const void* list, u32 nbytes) {
    if (!s_free_head) {
        if (!lru_skip_batched() || !evictable(E(s_lru_tail))) {
            s_st.full++;
            return NULL;
        }
        entry_drop(E(s_lru_tail));
        s_st.evicted++;
    }
    DlcEntry* e = E(s_free_head);
    s_free_head = e->next;
    u32 gen = e->gen;
    memset(e, 0, sizeof(*e));
    e->gen = gen;
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

/* The per-list path. CALL_HIT_FORMABLE: a trusted entry (s_form_e) the shader drew, a member for a batch;
 * CALL_BATCH: a batch that starts with this list was opened instead. */
enum { CALL_MISS = 0, CALL_HIT, CALL_HIT_FORMABLE, CALL_BATCH };
static DlcEntry* s_form_e;
u16* gx_sdl_hint;

static inline __attribute__((always_inline)) int call_slow_body(const GxState* st, const void* list, u32 nbytes);
static int call_slow(const GxState* st, const void* list, u32 nbytes) {
    int p_ = gx_ph_nested(1, GPH_T_SLOW);
    int r_ = call_slow_body(st, list, nbytes);
    gx_ph_leave(1, p_);
    return r_;
}

static inline __attribute__((always_inline)) int call_slow_body(const GxState* st, const void* list, u32 nbytes) {
    u64 t0 = GX_PERF_T();
    gx_sdl_hint = NULL;
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
            gx_sdl_hint = &e->sdl;
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
        if (ok && s_bat_on && e->bat_head && e->trust_gen != 0 && bat_open_chain(e)) {
            return CALL_BATCH; /* the batch that starts here is open; its draws are recorded when it ends */
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
                    if (gx_vsh_last.ok && e->trust_gen != 0 && e->vsh != NULL) {
                        s_form_e = e;
                        return CALL_HIT_FORMABLE;
                    }
                    return CALL_HIT;
                }
                gx_vsh_noted = 1;
                replay(e);
                gx_vsh_noted = 0;
                return CALL_HIT;
            }
            replay(e);
            return CALL_HIT;
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
    if (s_spread != 0 && s_st.built >= s_spread) {
        s_st.deferred++;
        goto decode_plain; /* this frame's build budget is used: the plain decode draws the same picture */
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
    return CALL_MISS;
}

/* ---- gxbatch: prebuilt batches of cached lists (PORT_3DS_GX_PERF.md, "Core-2 round 2") ----
 *
 * A run of consecutive hits on trusted entries, all drawn by the shader with no GX state, matrix or vertex-state
 * change between them, is collected while it is drawn the per-list way (the "forming" run) and then kept as a
 * batch: the member lists, the vertex state they matched, the GPU draws they merge into, and all their indices
 * concatenated in linear memory. From the next frame on, a call of the batch's first list opens it after one
 * validation of the whole batch; each following call that is the next member only moves a counter; when the
 * run ends (another list, a state change seen at the next call, any other draw, a copy, the frame's end) the
 * draws of the members called so far are recorded, without copying an index.
 *
 * What a batch relies on, and how each is checked once per use instead of once per member:
 *   - the members' cache entries: s_dlc_epoch (any entry's content or stream going bumps it; then each member's
 *     generation is compared once);
 *   - the members' contents: every member trusted at the registry's current generation (an entry that is not
 *     trusted never joins a batch and is hashed on the per-list path as before);
 *   - the vertex state: the batch's own copy against the state in force (vs_ok: unchanged since it last matched);
 *   - the pass-group route: every member's record decided "shader" (gx_vsh_dec_epoch), and the group's path this
 *     frame is the shader (gx_vsh_batch_route); otherwise the members are routed one by one as before;
 *   - the texture-coordinate mapping the scale words were made for (unit_attr).
 * Not used: gxcache=2, gxthread=3 (their per-list checks), gxscribble, an armed bounding box, gxdump frames. */
#define BAT_SLOTS 2048
#define BAT_MAX_MEMBERS 1024
#define BAT_MAX_DRAWS 64
#define BAT_CHAIN 4
#define BAT_ARRAYS 12
#define BAT_IX_RUNS 256
#define BAT_IX_PENDING 1024

typedef struct {
    const void* list;
    u32 nbytes;
    u16 eidx;       /* cache entry index (not + 1) */
    u16 pad;
    u32 egen;
    u32 idx_end;    /* indices of members 0..this, from the batch's first index */
    u32 verts_end;  /* vertices of members 0..this */
} BatMember;

typedef struct {
    const void* stream;
    u32 layout, scales;
    u32 idx_first;  /* in the batch's indices */
    u32 m_end;      /* one past its last member */
} BatDraw;

typedef struct {
    u8 live;
    u8 ndraws;
    u16 n;
    u16 next_head;  /* next batch with the same first entry (index + 1) */
    u16 succ;       /* the batch that came after this one last time (index + 1): tried before any lookup */
    u32 dlc_epoch, trust_gen, dec_epoch, vs_ok;
    u32 last_frame;
    u8 unit_attr[3];
    u8 fmt_mask;
    u8 cur_pnmtx;
    u8 narr;
    u8 arr_attr[BAT_ARRAYS];
    u8 arr_stride[BAT_ARRAYS];
    const u8* arr_base[BAT_ARRAYS];
    u8 vtx_desc[GX_VA_MAX_ATTR];
    u8 cur_texmtx[8];
    void* blob;     /* BatMember[n], BatDraw[ndraws], GxAttrFmt[popcount(fmt_mask)][GX_VA_MAX_ATTR] */
    u32 ix_first, ix_count; /* its range of the index pool */
} Batch;

int gx_batch_pending;
static Batch* s_bat;       /* BAT_SLOTS, heap */
static u16 s_bat_free;     /* free slots through .next_head (index + 1) */
static u32 s_bat_hand;     /* eviction scan position */
/* the index pool (linear memory): first fit over free runs, a bump pointer, frees held for the frames in flight */
static u16* s_ix;
static u32 s_ix_cap, s_ix_bump, s_ix_nruns, s_ix_npending;
static u32 s_ix_lost, s_ix_lost_logged; /* indices given up (free runs or pending list full), and the log-once count */
static struct {
    u32 start, len;
} s_ix_runs[BAT_IX_RUNS];
static struct {
    u32 start, len, frame;
} s_ix_pending[BAT_IX_PENDING];
/* the open batch */
static Batch* s_act;
static u32 s_act_pos, s_act_ver, s_act_xfv, s_act_vtxv, s_act_cull;
static void* s_act_group;
static Batch* s_bat_prev;  /* the batch drawn or formed last (for .succ) */
/* the run being collected */
typedef struct {
    const void* stream;
    u32 layout, scales;
    const u16* idx;
    u32 nidx, nverts;
} FormX;
static struct {
    u32 n, ver, xfv, vtxv, trust_gen, cull;
    u8 unit_attr[3];
    u8 fmt_mask;
    u8 cur_pnmtx;
    u8 vtx_desc[GX_VA_MAX_ATTR];
    u8 cur_texmtx[8];
    const u8* array_base[GX_VA_MAX_ATTR];
    u8 array_stride[GX_VA_MAX_ATTR];
    GxAttrFmt vat[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
    BatMember* m; /* BAT_MAX_MEMBERS, heap */
    FormX* x;
} s_form;
enum { OF_EPOCH, OF_TRUST, OF_STATE, OF_DECISION, OF_BEGIN, OF_COUNT };
static struct {
    u32 opens, members, partial, formed, refreshed, killed, form_fail, open_fail[OF_COUNT], evicted, bad;
} s_bst, s_bsum;

static inline Batch* B(u32 i) { return i ? &s_bat[i - 1] : NULL; }
static inline u32 BIDX(const Batch* b) { return (u32)(b - s_bat) + 1; }
static inline BatMember* b_members(const Batch* b) { return (BatMember*)b->blob; }
static inline BatDraw* b_draws(const Batch* b) { return (BatDraw*)(b_members(b) + b->n); }
static inline GxAttrFmt* b_vat(const Batch* b) { return (GxAttrFmt*)(b_draws(b) + b->ndraws); }

static void bat_init(void) {
    s_bat_on = plat_switch_int("gxbatch", 1);
    if (s_bat_on < 0 || s_bat_on > 2 || s_enabled != 1 || s_scribble_at != 0 || plat_switch_int("gxthread", 0) == 3) {
        s_bat_on = 0;
    }
    if (!s_bat_on) {
        return;
    }
    u32 kb = (u32)plat_switch_int("gxbatchkb", 512);
    s_ix_cap = kb * 1024u / sizeof(u16);
    s_ix = plat_gpu_stream_alloc(s_ix_cap * sizeof(u16));
    if (s_ix != NULL) {
        plat_gpu_set_index_pool(s_ix, s_ix_cap * sizeof(u16));
    }
    s_bat = gx_big_alloc("gxbatch slots", sizeof(Batch) * BAT_SLOTS);
    s_form.m = gx_big_alloc("gxbatch run", sizeof(BatMember) * BAT_MAX_MEMBERS);
    s_form.x = gx_big_alloc("gxbatch run draws", sizeof(FormX) * BAT_MAX_MEMBERS);
    if (s_ix == NULL || s_bat == NULL || s_form.m == NULL || s_form.x == NULL) {
        PLOG_W("gxbatch: no memory (%lu KB of indices): off", (unsigned long)kb);
        s_bat_on = 0;
        return;
    }
    memset(s_bat, 0, sizeof(Batch) * BAT_SLOTS);
    for (u32 i = 0; i < BAT_SLOTS; i++) {
        s_bat[i].next_head = (u16)(i + 2 <= BAT_SLOTS ? i + 2 : 0);
    }
    s_bat_free = 1;
    s_bat_log_every = (u32)plat_switch_int("gxthreadstats", plat_switch_int("gxstats", 0));
    PLOG_I("gxbatch: %s, %d slots, %lu KB of indices in linear memory", s_bat_on == 2 ? "verify" : "on", BAT_SLOTS,
           (unsigned long)kb);
}

int gx_batch_recent(u32 bat) {
    if (bat == 0 || bat > BAT_SLOTS || s_bat == NULL) {
        return 0;
    }
    const Batch* b = &s_bat[bat - 1];
    return b->live && gx_perf_frame_no() - b->last_frame <= 2;
}

int gx_batch_member(u32 bat, u32 k, const void** list, u32* nbytes, u32** hint) {
    if (bat == 0 || bat > BAT_SLOTS || s_bat == NULL || !s_bat[bat - 1].live || k >= s_bat[bat - 1].n) {
        return 0;
    }
    const BatMember* m = &b_members(&s_bat[bat - 1])[k];
    *list = m->list;
    *nbytes = m->nbytes;
    DlcEntry* e = &s_e[m->eidx];
    *hint = e->list == m->list && e->nbytes == m->nbytes ? &e->vsh_rec : NULL;
    return 1;
}

/* ---- index pool ---- */

static void ix_release(u32 start, u32 len) {
    if (start + len == s_ix_bump) {
        s_ix_bump = start;
        while (s_ix_nruns > 0 && s_ix_runs[s_ix_nruns - 1].start + s_ix_runs[s_ix_nruns - 1].len == s_ix_bump) {
            s_ix_bump = s_ix_runs[--s_ix_nruns].start;
        }
        return;
    }
    u32 i = 0;
    while (i < s_ix_nruns && s_ix_runs[i].start < start) {
        i++;
    }
    if (i > 0 && s_ix_runs[i - 1].start + s_ix_runs[i - 1].len == start) {
        s_ix_runs[i - 1].len += len;
        if (i < s_ix_nruns && s_ix_runs[i - 1].start + s_ix_runs[i - 1].len == s_ix_runs[i].start) {
            s_ix_runs[i - 1].len += s_ix_runs[i].len;
            memmove(&s_ix_runs[i], &s_ix_runs[i + 1], (s_ix_nruns - i - 1) * sizeof(s_ix_runs[0]));
            s_ix_nruns--;
        }
        return;
    }
    if (i < s_ix_nruns && start + len == s_ix_runs[i].start) {
        s_ix_runs[i].start = start;
        s_ix_runs[i].len += len;
        return;
    }
    if (s_ix_nruns == BAT_IX_RUNS) {
        /* too fragmented: the smallest range (the new one or a held one) is given up until the pool empties */
        u32 small = 0;
        for (u32 k = 1; k < s_ix_nruns; k++) {
            if (s_ix_runs[k].len < s_ix_runs[small].len) {
                small = k;
            }
        }
        s_ix_lost += s_ix_runs[small].len < len ? s_ix_runs[small].len : len;
        if (s_ix_lost_logged++ == 0) {
            PLOG_W("gxbatch: index pool free runs full: a range is given up until the pool empties");
        }
        if (len <= s_ix_runs[small].len) {
            return;
        }
        memmove(&s_ix_runs[small], &s_ix_runs[small + 1], (s_ix_nruns - small - 1) * sizeof(s_ix_runs[0]));
        s_ix_nruns--;
        if (small < i) {
            i--;
        }
    }
    memmove(&s_ix_runs[i + 1], &s_ix_runs[i], (s_ix_nruns - i) * sizeof(s_ix_runs[0]));
    s_ix_runs[i].start = start;
    s_ix_runs[i].len = len;
    s_ix_nruns++;
}

static void ix_reap(void) {
    u32 now = plat_gpu_present_count(), k = 0;
    for (u32 i = 0; i < s_ix_npending; i++) {
        if ((s32)(now - s_ix_pending[i].frame) >= 0) {
            ix_release(s_ix_pending[i].start, s_ix_pending[i].len);
        } else {
            s_ix_pending[k++] = s_ix_pending[i];
        }
    }
    s_ix_npending = k;
}

/* n indices (rounded up to a multiple of 4, so every range starts 8-byte aligned), or ~0. */
static u32 ix_alloc(u32 n) {
    n = (n + 3u) & ~3u;
    ix_reap();
    for (u32 i = 0; i < s_ix_nruns; i++) {
        if (s_ix_runs[i].len >= n) {
            u32 at = s_ix_runs[i].start;
            s_ix_runs[i].start += n;
            s_ix_runs[i].len -= n;
            if (s_ix_runs[i].len == 0) {
                memmove(&s_ix_runs[i], &s_ix_runs[i + 1], (s_ix_nruns - i - 1) * sizeof(s_ix_runs[0]));
                s_ix_nruns--;
            }
            return at;
        }
    }
    if (s_ix_bump + n <= s_ix_cap) {
        u32 at = s_ix_bump;
        s_ix_bump += n;
        return at;
    }
    return ~0u;
}

/* ---- batch slots ---- */

static void bat_kill(Batch* b) {
    /* out of its first entry's chain (the chain is intact while the batch lives: free_blob kills it first) */
    DlcEntry* e0 = &s_e[b_members(b)[0].eidx];
    u16 me = (u16)BIDX(b);
    for (u16* link = &e0->bat_head; *link; link = &B(*link)->next_head) {
        if (*link == me) {
            *link = b->next_head;
            break;
        }
    }
    if (s_ix_npending == BAT_IX_PENDING) {
        ix_reap();
    }
    u32 pstart = b->ix_first, plen = (b->ix_count + 3u) & ~3u, pframe = plat_gpu_present_count() + 3;
    if (s_ix_npending == BAT_IX_PENDING) {
        /* still full: fold the range into an adjacent pending free (held until the later of the two frames) */
        for (u32 k = 0; k < s_ix_npending; k++) {
            if (s_ix_pending[k].start + s_ix_pending[k].len == pstart) {
                s_ix_pending[k].len += plen;
                s_ix_pending[k].frame = pframe;
                plen = 0;
                break;
            }
            if (pstart + plen == s_ix_pending[k].start) {
                s_ix_pending[k].start = pstart;
                s_ix_pending[k].len += plen;
                s_ix_pending[k].frame = pframe;
                plen = 0;
                break;
            }
        }
    }
    if (plen != 0 && s_ix_npending < BAT_IX_PENDING) {
        /* the record being written and the frames in flight may draw from it */
        s_ix_pending[s_ix_npending].start = pstart;
        s_ix_pending[s_ix_npending].len = plen;
        s_ix_pending[s_ix_npending].frame = pframe;
        s_ix_npending++;
    } else if (plen != 0) {
        /* no adjacent entry either: the range is given up until the pool empties (counted, logged once) */
        s_ix_lost += plen;
        if (s_ix_lost_logged++ == 0) {
            PLOG_W("gxbatch: index pool pending frees full: a range is given up until the pool empties");
        }
    }
    /* the members were in use up to the batch's last use, which the batch's draws did not stamp on them */
    {
        const BatMember* km = b_members(b);
        for (u32 i = 0; i < b->n; i++) {
            DlcEntry* me_e = &s_e[km[i].eidx];
            if (me_e->gen == km[i].egen && me_e->last_frame < b->last_frame) {
                me_e->last_frame = b->last_frame;
            }
        }
    }
    free(b->blob);
    if (s_bat_prev == b) {
        s_bat_prev = NULL;
    }
    memset(b, 0, sizeof(*b));
    b->next_head = s_bat_free;
    s_bat_free = me;
    s_bst.killed++;
}

static void bat_kill_chain(DlcEntry* e) {
    while (e->bat_head) {
        Batch* b = B(e->bat_head);
        if (b == s_act) {
            s_act = NULL; /* never: entries change on the per-list path, after the open batch ended */
        }
        bat_kill(b);
    }
}

/* Batches not drawn for two frames go, up to `want` of them (for a slot, or for room in the index pool). */
static void bat_evict_old(u32 want) {
    u32 frame = gx_perf_frame_no();
    for (u32 n = 0; n < BAT_SLOTS && want > 0; n++) {
        Batch* b = &s_bat[s_bat_hand];
        s_bat_hand = (s_bat_hand + 1) % BAT_SLOTS;
        if (b->live && b != s_act && frame - b->last_frame > 2) {
            bat_kill(b);
            s_bst.evicted++;
            want--;
        }
    }
}

/* ---- validation and use ---- */

static int bat_same_state(Batch* b) {
    if (b->vs_ok == gx_vtx_version) {
        return 1;
    }
    if (!gx_mem_equal(b->vtx_desc, gx.vtx_desc, sizeof(b->vtx_desc)) || b->cur_pnmtx != (u8)gx.cur_pnmtx ||
        !gx_mem_equal(b->cur_texmtx, gx.cur_texmtx, sizeof(b->cur_texmtx))) {
        return 0;
    }
    const GxAttrFmt* v = b_vat(b);
    for (u32 f = 0; f < GX_MAX_VTXFMT; f++) {
        if (b->fmt_mask & (1u << f)) {
            if (!gx_mem_equal(v, gx.vat[f], sizeof(gx.vat[f]))) {
                return 0;
            }
            v += GX_VA_MAX_ATTR;
        }
    }
    for (u32 i = 0; i < b->narr; i++) {
        if (gx.array_base[b->arr_attr[i]] != b->arr_base[i] || gx.array_stride[b->arr_attr[i]] != b->arr_stride[i]) {
            return 0;
        }
    }
    b->vs_ok = gx_vtx_version;
    return 1;
}

/* Opens batch b for a call of its first list: 1 when every check passed (the call is consumed). */
static inline __attribute__((always_inline)) int bat_open_body(Batch* b);
static int bat_open(Batch* b) {
    int p_ = gx_ph_nested(1, GPH_T_BOPEN);
    int r_ = bat_open_body(b);
    gx_ph_leave(1, p_);
    return r_;
}

static inline __attribute__((always_inline)) int bat_open_body(Batch* b) {
    BatMember* m = b_members(b);
    u32 fail;
    if (b->dlc_epoch != s_dlc_epoch) {
        for (u32 i = 0; i < b->n; i++) {
            const DlcEntry* e = &s_e[m[i].eidx];
            if (e->gen != m[i].egen || e->state != E_BUILT || e->vsh == NULL) {
                bat_kill(b); /* a member's entry or stream is gone: the run is collected again */
                s_bst.open_fail[OF_EPOCH]++;
                return 0;
            }
        }
        b->dlc_epoch = s_dlc_epoch;
    }
    u32 tg = plat_memstable_gen();
    if (b->trust_gen != tg) {
        for (u32 i = 0; i < b->n; i++) {
            if (s_e[m[i].eidx].trust_gen != tg) {
                fail = OF_TRUST; /* the per-list path checks the members in full and asks for trust again */
                goto no;
            }
        }
        b->trust_gen = tg;
    }
    if (!bat_same_state(b)) {
        fail = OF_STATE;
        goto no;
    }
    if (b->dec_epoch != gx_vsh_dec_epoch) {
        for (u32 i = 0; i < b->n; i++) {
            DlcEntry* e = &s_e[m[i].eidx];
            if (!gx_vsh_rec_decided(&e->vsh_rec, m[i].list, m[i].nbytes, BIDX(b))) {
                fail = OF_DECISION;
                goto no;
            }
        }
        b->dec_epoch = gx_vsh_dec_epoch;
    }
    if (!gx_draw_batch_begin(b->unit_attr, BIDX(b), m[0].list, m[0].nbytes, &s_e[m[0].eidx].vsh_rec, &s_act_group,
                             &s_act_cull)) {
        fail = OF_BEGIN;
        goto no;
    }
    if (s_bat_prev != NULL && s_bat_prev != b) {
        s_bat_prev->succ = (u16)BIDX(b);
    }
    s_act = b;
    s_act_pos = 1;
    s_act_ver = gx.version;
    s_act_xfv = gx_xf_version;
    s_act_vtxv = gx_vtx_version;
    gx_batch_pending = 1;
    s_bst.opens++;
    return 1;
no:
    s_bst.open_fail[fail]++;
    return 0;
}

static int bat_open_chain(DlcEntry* e) {
    for (u16 bi = e->bat_head, next; bi; bi = next) {
        Batch* b = B(bi);
        next = b->next_head; /* bat_open may kill b */
        if (bat_open(b)) {
            return 1;
        }
    }
    return 0;
}

/* gxbatch=2: the open batch's index range is live: below the bump pointer, in no free run, and no pending free
 * waits longer than present+3 (the frames in flight). Logs the first failures. */
static void bat_verify_life(const Batch* b) {
    static u32 s_fail_logged;
    u32 first = b->ix_first, end = first + ((b->ix_count + 3u) & ~3u), now = plat_gpu_present_count();
    int bad = !b->live || end > s_ix_bump;
    for (u32 i = 0; i < s_ix_nruns && !bad; i++) {
        bad = first < s_ix_runs[i].start + s_ix_runs[i].len && s_ix_runs[i].start < end;
    }
    for (u32 i = 0; i < s_ix_npending && !bad; i++) {
        bad = (s32)(s_ix_pending[i].frame - now) > 3;
    }
    if (bad && s_fail_logged++ < 8) {
        PLOG_E("gxbatch verify: batch %lu index range %lu..%lu not live (bump %lu, %lu free runs, %lu pending, present %lu)",
               (unsigned long)BIDX(b), (unsigned long)first, (unsigned long)end, (unsigned long)s_ix_bump,
               (unsigned long)s_ix_nruns, (unsigned long)s_ix_npending, (unsigned long)now);
        s_bst.bad++;
    }
}

/* Records the draws of the open batch's members called so far. */
static inline __attribute__((always_inline)) void bat_close_body(void);
static void bat_close(void) {
    int p_ = gx_ph_nested(1, GPH_T_BCLOSE);
    bat_close_body();
    gx_ph_leave(1, p_);
}

static inline __attribute__((always_inline)) void bat_close_body(void) {
    Batch* b = s_act;
    if (s_bat_on == 2) {
        bat_verify_life(b);
    }
    u32 p = s_act_pos;
    s_act = NULL;
    const BatMember* m = b_members(b);
    const BatDraw* d = b_draws(b);
    u32 idx_end = m[p - 1].idx_end;
    for (u32 i = 0; i < b->ndraws; i++) {
        u32 first = d[i].idx_first;
        if (first >= idx_end) {
            break;
        }
        u32 m0 = i ? d[i - 1].m_end : 0;
        u32 mend = d[i].m_end < p ? d[i].m_end : p;
        u32 end = m[mend - 1].idx_end;
        plat_gpu_vsh_drawp(d[i].stream, d[i].layout, d[i].scales, s_act_cull, s_ix + b->ix_first + first, end - first,
                           mend - m0);
    }
    gx_draw_batch_end(s_act_group, p, m[p - 1].verts_end, idx_end);
    s_st.hits += p;
    s_st.trusted += p;
    s_st.hit_verts += m[p - 1].verts_end;
    if (gx_perf_on > 0) {
        gx_perf.dl_verts += m[p - 1].verts_end;
    }
    b->last_frame = gx_perf_frame_no();
    s_bst.members += p;
    if (p == b->n) {
        s_bat_prev = b;
    } else {
        s_bat_prev = NULL;
        s_bst.partial++;
    }
}

/* Makes the collected run a batch. */
static inline __attribute__((always_inline)) void form_end_body(void);
static void form_end(void) {
    int p_ = gx_ph_nested(1, GPH_T_FORM);
    form_end_body();
    gx_ph_leave(1, p_);
}

static inline __attribute__((always_inline)) void form_end_body(void) {
    u32 n = s_form.n;
    s_form.n = 0;
    if (n == 0) {
        return;
    }
    BatMember* fm = s_form.m;
    const FormX* fx = s_form.x;
    DlcEntry* e0 = &s_e[fm[0].eidx];
    for (u32 i = 0; i < n; i++) {
        const DlcEntry* e = &s_e[fm[i].eidx];
        if (e->gen != fm[i].egen || e->state != E_BUILT || e->vsh == NULL) {
            s_bst.form_fail++;
            return; /* an entry changed while the run was collected */
        }
    }
    /* the same run again (its batch failed a check this frame and the members went one by one): keep that batch */
    u32 chain = 0;
    Batch* last = NULL;
    for (u16 bi = e0->bat_head; bi; bi = B(bi)->next_head) {
        Batch* b = B(bi);
        chain++;
        last = b;
        if (b->n != n || b->fmt_mask != s_form.fmt_mask || b->unit_attr[0] != s_form.unit_attr[0] ||
            b->unit_attr[1] != s_form.unit_attr[1] || b->unit_attr[2] != s_form.unit_attr[2] ||
            b->cur_pnmtx != s_form.cur_pnmtx || !gx_mem_equal(b->vtx_desc, s_form.vtx_desc, sizeof(b->vtx_desc)) ||
            !gx_mem_equal(b->cur_texmtx, s_form.cur_texmtx, sizeof(b->cur_texmtx))) {
            continue;
        }
        const BatMember* bm = b_members(b);
        u32 same = 1;
        for (u32 i = 0; i < n && same; i++) {
            same = bm[i].list == fm[i].list && bm[i].nbytes == fm[i].nbytes && bm[i].eidx == fm[i].eidx &&
                   bm[i].egen == fm[i].egen;
        }
        const BatDraw* bd = b_draws(b);
        for (u32 i = 0, k = 0; i < n && same; i++) {
            if (i == bd[k].m_end) {
                k++;
            }
            same = bd[k].stream == fx[i].stream && bd[k].layout == fx[i].layout && bd[k].scales == fx[i].scales;
        }
        const GxAttrFmt* v = b_vat(b);
        for (u32 f = 0; f < GX_MAX_VTXFMT && same; f++) {
            if (b->fmt_mask & (1u << f)) {
                same = gx_mem_equal(v, s_form.vat[f], sizeof(s_form.vat[f]));
                v += GX_VA_MAX_ATTR;
            }
        }
        for (u32 i = 0; i < b->narr && same; i++) {
            same = s_form.array_base[b->arr_attr[i]] == b->arr_base[i] &&
                   s_form.array_stride[b->arr_attr[i]] == b->arr_stride[i];
        }
        if (same) {
            b->trust_gen = s_form.trust_gen;
            b->dlc_epoch = s_dlc_epoch;
            b->last_frame = gx_perf_frame_no();
            if (s_bat_prev != NULL && s_bat_prev != b) {
                s_bat_prev->succ = (u16)BIDX(b);
            }
            s_bat_prev = b;
            s_bst.refreshed++;
            return;
        }
    }
    if (chain >= BAT_CHAIN && last != NULL) {
        bat_kill(last); /* the oldest of the batches that start here */
    }
    /* the GPU draws: members that share stream, layout and scale word merge */
    u32 nd = 0, nidx = 0;
    for (u32 i = 0; i < n; i++) {
        if (i == 0 || fx[i].stream != fx[i - 1].stream || fx[i].layout != fx[i - 1].layout ||
            fx[i].scales != fx[i - 1].scales) {
            nd++;
        }
        nidx += fx[i].nidx;
    }
    if (nd > BAT_MAX_DRAWS) {
        s_bst.form_fail++;
        return;
    }
    if (!s_bat_free) {
        bat_evict_old(8);
    }
    u32 ix = ix_alloc(nidx);
    if (ix == ~0u) {
        bat_evict_old(BAT_SLOTS); /* their ranges come back after the frames in flight */
        ix = ix_alloc(nidx);
    }
    u32 nfmt = (u32)__builtin_popcount(s_form.fmt_mask);
    u32 bytes = n * sizeof(BatMember) + nd * sizeof(BatDraw) + nfmt * sizeof(s_form.vat[0]);
    void* blob = ix != ~0u && s_bat_free ? malloc(bytes) : NULL;
    if (blob == NULL) {
        if (ix != ~0u) {
            ix_release(ix, (nidx + 3u) & ~3u); /* never drawn from: free at once */
        }
        s_bst.form_fail++;
        return;
    }
    Batch* b = B(s_bat_free);
    s_bat_free = b->next_head;
    memset(b, 0, sizeof(*b));
    b->live = 1;
    b->n = (u16)n;
    b->ndraws = (u8)nd;
    b->blob = blob;
    b->ix_first = ix;
    b->ix_count = nidx;
    b->dlc_epoch = s_dlc_epoch;
    b->trust_gen = s_form.trust_gen;
    b->last_frame = gx_perf_frame_no();
    memcpy(b->unit_attr, s_form.unit_attr, 3);
    b->fmt_mask = s_form.fmt_mask;
    b->cur_pnmtx = s_form.cur_pnmtx;
    memcpy(b->vtx_desc, s_form.vtx_desc, sizeof(b->vtx_desc));
    memcpy(b->cur_texmtx, s_form.cur_texmtx, sizeof(b->cur_texmtx));
    BatMember* bm = b_members(b);
    BatDraw* bd = b_draws(b);
    u16* out = s_ix + ix;
    u32 at = 0, verts = 0, k = 0;
    for (u32 i = 0; i < n; i++) {
        if (i == 0 || fx[i].stream != fx[i - 1].stream || fx[i].layout != fx[i - 1].layout ||
            fx[i].scales != fx[i - 1].scales) {
            if (i != 0) {
                bd[k - 1].m_end = i;
            }
            bd[k].stream = fx[i].stream;
            bd[k].layout = fx[i].layout;
            bd[k].scales = fx[i].scales;
            bd[k].idx_first = at;
            k++;
        }
        memcpy(out + at, fx[i].idx, fx[i].nidx * sizeof(u16));
        at += fx[i].nidx;
        verts += fx[i].nverts;
        bm[i] = fm[i];
        bm[i].idx_end = at;
        bm[i].verts_end = verts;
        s_e[fm[i].eidx].bat = (u16)BIDX(b);
    }
    bd[k - 1].m_end = n;
    plat_gpu_stream_commit(out, nidx * sizeof(u16));
    GxAttrFmt* v = b_vat(b);
    for (u32 f = 0; f < GX_MAX_VTXFMT; f++) {
        if (s_form.fmt_mask & (1u << f)) {
            memcpy(v, s_form.vat[f], sizeof(s_form.vat[f]));
            v += GX_VA_MAX_ATTR;
        }
    }
    /* the arrays the members' vertex state names (every indexed attribute of the description) */
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        u8 dsc = s_form.vtx_desc[a];
        if (a == GX_VA_NRM && dsc == GX_NONE) {
            dsc = s_form.vtx_desc[GX_VA_NBT];
        }
        if ((dsc == GX_INDEX8 || dsc == GX_INDEX16) && b->narr < BAT_ARRAYS) {
            b->arr_attr[b->narr] = (u8)a;
            b->arr_base[b->narr] = s_form.array_base[a];
            b->arr_stride[b->narr] = s_form.array_stride[a];
            b->narr++;
        }
    }
    b->next_head = e0->bat_head;
    e0->bat_head = (u16)BIDX(b);
    if (s_bat_prev != NULL) {
        s_bat_prev->succ = (u16)BIDX(b);
    }
    s_bat_prev = b;
    s_bst.formed++;
}

/* A trusted list the shader just drew (s_form_e, gx_vsh_last): the next member of the run, or the first of a new one. */
static inline __attribute__((always_inline)) void form_add_body(const void* list, u32 nbytes);
static void form_add(const void* list, u32 nbytes) {
    int p_ = gx_ph_nested(1, GPH_T_FORM);
    form_add_body(list, nbytes);
    gx_ph_leave(1, p_);
}

static inline __attribute__((always_inline)) void form_add_body(const void* list, u32 nbytes) {
    DlcEntry* e = s_form_e;
    u32 tg = plat_memstable_gen();
    if (s_form.n != 0 && (s_form.ver != gx.version || s_form.xfv != gx_xf_version || s_form.vtxv != gx_vtx_version ||
                          s_form.trust_gen != tg || s_form.n == BAT_MAX_MEMBERS)) {
        form_end();
    }
    if (e->trust_gen != tg) {
        form_end();
        gx_batch_pending = 0;
        return;
    }
    if (s_form.n == 0) {
        s_form.ver = gx.version;
        s_form.xfv = gx_xf_version;
        s_form.vtxv = gx_vtx_version;
        s_form.trust_gen = tg;
        s_form.cull = gx_vsh_last.cull;
        memcpy(s_form.unit_attr, gx_vsh_last.unit_attr, 3);
        s_form.fmt_mask = 0;
        s_form.cur_pnmtx = (u8)gx.cur_pnmtx;
        memcpy(s_form.vtx_desc, gx.vtx_desc, sizeof(s_form.vtx_desc));
        memcpy(s_form.cur_texmtx, gx.cur_texmtx, sizeof(s_form.cur_texmtx));
        memcpy(s_form.array_base, gx.array_base, sizeof(s_form.array_base));
        memcpy(s_form.array_stride, gx.array_stride, sizeof(s_form.array_stride));
        memcpy(s_form.vat, gx.vat, sizeof(s_form.vat));
    }
    u32 i = s_form.n++;
    s_form.m[i] = (BatMember){list, nbytes, (u16)(e - s_e), 0, e->gen, 0, 0};
    s_form.x[i] = (FormX){gx_vsh_last.stream, gx_vsh_last.layout, gx_vsh_last.scales, e->vsh->idx, e->vsh->nidx,
                          e->nverts};
    s_form.fmt_mask |= e->fmt_mask;
    gx_batch_pending = 1;
}

void gx_batch_flush_slow(void) {
    if (s_act != NULL) {
        bat_close();
    }
    form_end();
    gx_batch_pending = 0;
}

/* gxbatch=2: what the per-list path would have checked for this member of the open batch. */
static void bat_verify_member(const GxState* st, const Batch* b, u32 k, const void* list, u32 nbytes) {
    const BatMember* m = &b_members(b)[k];
    const DlcEntry* e = &s_e[m->eidx];
    if (e->list != list || e->nbytes != nbytes || e->gen != m->egen || e->state != E_BUILT || e->vsh == NULL ||
        !same_state_full(st, e) || e->trust_gen != plat_memstable_gen() ||
        e->dl_hash != gx_hash32(list, nbytes, 0xD15Cu) || !same_arrays(e)) {
        if (s_bst.bad++ == 0 && s_bsum.bad == 0) {
            PLOG_W("gxbatch verify: member %lu of a batch (list %p, %lu bytes) fails the per-list checks",
                   (unsigned long)k, list, (unsigned long)nbytes);
        }
    }
}

int gx_dlcache_call(const GxState* st, const void* list, u32 nbytes) {
    if (!s_bat_on) {
        return call_slow(st, list, nbytes) != CALL_MISS;
    }
    if (s_act != NULL) {
        Batch* b = s_act;
        if (s_act_pos < b->n && gx.version == s_act_ver && gx_xf_version == s_act_xfv && gx_vtx_version == s_act_vtxv &&
            !gx_bbox_active) {
            const BatMember* m = &b_members(b)[s_act_pos];
            if (m->list == list && m->nbytes == nbytes) {
                if (s_bat_on == 2) {
                    bat_verify_member(st, b, s_act_pos, list, nbytes);
                }
                s_act_pos++;
                s_st.calls++;
                return 1;
            }
        }
        bat_close();
        gx_batch_pending = 0; /* no run is being collected while a batch is open */
    }
    if (st != &gx || gx_bbox_active) {
        form_end();
        gx_batch_pending = 0;
        return call_slow(st, list, nbytes) != CALL_MISS;
    }
    /* the batch that followed the last one the last time, before any lookup */
    if (s_bat_prev != NULL && s_bat_prev->succ != 0) {
        Batch* b = B(s_bat_prev->succ);
        if (b->live && b_members(b)[0].list == list && b_members(b)[0].nbytes == nbytes) {
            form_end();
            gx_batch_pending = 0;
            if (bat_open(b)) {
                s_st.calls++;
                if (s_bat_on == 2) {
                    bat_verify_member(st, b, 0, list, nbytes);
                }
                return 1;
            }
        }
    }
    int r = call_slow(st, list, nbytes);
    if (r == CALL_HIT_FORMABLE) {
        form_add(list, nbytes);
    } else {
        form_end();
        gx_batch_pending = s_act != NULL;
        if (r == CALL_BATCH && s_bat_on == 2) {
            bat_verify_member(st, s_act, 0, list, nbytes);
        }
    }
    return r != CALL_MISS;
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
        lru_skip_batched();
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

static void bat_log(u32 frames) {
    u32 f = frames ? frames : 1;
    const typeof(s_bsum)* a = &s_bsum;
    u32 live = 0;
    for (u32 i = 0; i < BAT_SLOTS; i++) {
        live += s_bat[i].live;
    }
    u32 free_ix = s_ix_cap - s_ix_bump;
    for (u32 i = 0; i < s_ix_nruns; i++) {
        free_ix += s_ix_runs[i].len;
    }
    PLOG_I("gxbatch frame %lu: per frame %lu opens drawing %lu lists (%lu ended early) | in these frames: formed %lu "
           "refreshed %lu killed %lu (evicted %lu) not formed %lu; opens refused: entries %lu trust %lu vertex state "
           "%lu decision %lu state/group %lu | verify failures %lu | %lu batches, indices %lu of %lu KB (%lu frees "
           "pending)",
           (unsigned long)gx_perf_frame_no(), (unsigned long)(a->opens / f), (unsigned long)(a->members / f),
           (unsigned long)(a->partial / f), (unsigned long)a->formed, (unsigned long)a->refreshed,
           (unsigned long)a->killed, (unsigned long)a->evicted, (unsigned long)a->form_fail,
           (unsigned long)a->open_fail[OF_EPOCH], (unsigned long)a->open_fail[OF_TRUST],
           (unsigned long)a->open_fail[OF_STATE], (unsigned long)a->open_fail[OF_DECISION],
           (unsigned long)a->open_fail[OF_BEGIN], (unsigned long)a->bad, (unsigned long)live,
           (unsigned long)((s_ix_cap - free_ix) * sizeof(u16) / 1024), (unsigned long)(s_ix_cap * sizeof(u16) / 1024),
           (unsigned long)s_ix_npending);
    memset(&s_bsum, 0, sizeof(s_bsum));
}

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
    if (s_bat_on) {
        u32* bd = (u32*)&s_bsum;
        const u32* bs = (const u32*)&s_bst;
        for (size_t i = 0; i < sizeof(s_bst) / 4; i++) {
            bd[i] += bs[i];
        }
        memset(&s_bst, 0, sizeof(s_bst));
        if (s_bat_log_every != 0 && ++s_bat_log_frames >= s_bat_log_every) {
            bat_log(s_bat_log_frames);
            s_bat_log_frames = 0;
        }
    }
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
           "dynamic %lu too-big %lu odd %lu evicted %lu full %lu alloc-fail %lu verified %lu mismatched %lu relinks/frame %lu deferred %lu | "
           "%lu entries %lu KB of %lu KB",
           (unsigned long)(a->calls / f), (unsigned long)(a->hits / f), (unsigned long)pct,
           (unsigned long)(a->hit_verts / f), (unsigned long)(a->built / f), (unsigned long)(a->changed / f),
           (unsigned long)(a->variant / f), (unsigned long)(a->nocache / f), (unsigned long)(a->dynamic_calls / f),
           (unsigned long)(a->too_big / f), (unsigned long)(a->sparse / f), (unsigned long)a->evicted,
           (unsigned long)a->full,
           (unsigned long)a->alloc_fail, (unsigned long)a->verified, (unsigned long)a->mismatched,
           (unsigned long)(a->relinks / f), (unsigned long)(a->deferred / f),
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
