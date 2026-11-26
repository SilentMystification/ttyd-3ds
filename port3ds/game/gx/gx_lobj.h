/* ListObj index (switch gxlobj, default 0): one table keyed by a display list's (address, size).
 *
 * docs/PORT_3DS_GX_NATIVE.md step 1. A display list is known by the same key in several places; with gxlobj
 * one open-addressed index answers for all of them on its thread, and the per-module tables become what the
 * index points to (or are not probed when the index has the answer):
 *   - translator side (gx_dlcache.c): the index value is the first cache entry of the key; the entries of one
 *     key are chained through DlcEntry.next. This replaces the bucket array. The entry carries the other
 *     per-list fields: the pass-group record hint (gx_vsh.c rec_for), the gxsdl slot hint, the bounding-box
 *     record and the gxbatch chain of the batches that start with it, so a call needs one index probe.
 *   - game side (gx_rec.c): the value is a small object with the state-list answer of is_state_list and the
 *     gxrecfuse slot hint of fl_get.
 * The two sides are two instances (one per owning thread; there is no lock), both of this layout.
 *
 * Hot index: 8 bytes per slot (address, size / 32, value), a power-of-two slot count, linear probing with
 * masks, at most half full (a probe always ends at an empty slot), deletion by backward shift (no tombstones).
 * A key with a size that is not a multiple of 32 or above 2 MiB is not indexed (gx_lobj_key_ok): callers use
 * their old table for it. Values are only what callers store; the index never changes what a lookup finds,
 * only how: the picture cannot change.
 */
#ifndef GX_LOBJ_H
#define GX_LOBJ_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t list;
    uint16_t nb32; /* nbytes >> 5 */
    uint16_t val;  /* 0: empty slot */
} GxLobjHot;

typedef struct {
    GxLobjHot* hot;
    uint32_t mask, bits;
    uint32_t live, cap; /* cap = slots / 2 */
    uint32_t inserts, deletes, clears; /* counters */
} GxLobjTab;

/* switch gxlobj, read once: 0 off, 1 on, 2 on and verify (gx_dlcache.c: every 64 frames every index slot and
 * every cache entry are checked against each other; gx_rec.c: every kept answer and hint is checked against
 * the tables' own lookup; mismatches counted in "gxlobj verify" lines, must be 0) */
int gx_lobj_switch(void);

static inline int gx_lobj_key_ok(const void* list, uint32_t nbytes) {
    return list != NULL && nbytes != 0 && (nbytes & 31) == 0 && (nbytes >> 5) <= 0xFFFFu;
}

static inline uint32_t gx_lobj_home(const GxLobjTab* t, uint32_t list, uint32_t nb32) {
    return ((list >> 5) ^ (nb32 * 0x9E3779B1u)) * 0x9E3779B1u >> (32 - t->bits);
}

/* The slot of the key, or NULL. The pointer is valid until the next insert or delete on the table. */
static inline GxLobjHot* gx_lobj_find(GxLobjTab* t, const void* list, uint32_t nbytes) {
    uint32_t l = (uint32_t)(uintptr_t)list, nb = nbytes >> 5;
    for (uint32_t i = gx_lobj_home(t, l, nb);; i = (i + 1) & t->mask) {
        GxLobjHot* h = &t->hot[i];
        if (h->val == 0) {
            return NULL;
        }
        if (h->list == l && h->nb32 == nb) {
            return h;
        }
    }
}

/* Allocates an index of 2^bits slots (heap), all empty. */
void gx_lobj_init(GxLobjTab* t, const char* what, uint32_t bits);
/* Adds a key that is not in the index; NULL when the index holds cap keys. */
GxLobjHot* gx_lobj_insert(GxLobjTab* t, const void* list, uint32_t nbytes, uint16_t val);
/* Removes the key of slot h (from gx_lobj_find). */
void gx_lobj_delete(GxLobjTab* t, GxLobjHot* h);
/* Empties the index. */
void gx_lobj_clear(GxLobjTab* t);

#endif
