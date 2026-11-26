/* ListObj index (switch gxlobj): see gx_lobj.h. */
#include "gx_lobj.h"

#include "gx_bigmem.h"
#include "plat/plat.h"

int gx_lobj_switch(void) {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = plat_switch_int("gxlobj", 0);
        if (s_on < 0 || s_on > 2) {
            s_on = 0;
        }
    }
    return s_on;
}

void gx_lobj_init(GxLobjTab* t, const char* what, uint32_t bits) {
    t->hot = gx_big_alloc(what, sizeof(GxLobjHot) << bits);
    t->bits = bits;
    t->mask = (1u << bits) - 1;
    t->cap = 1u << (bits - 1);
    t->live = t->inserts = t->deletes = t->clears = 0;
}

GxLobjHot* gx_lobj_insert(GxLobjTab* t, const void* list, uint32_t nbytes, uint16_t val) {
    if (t->live >= t->cap || val == 0) {
        return NULL;
    }
    uint32_t l = (uint32_t)(uintptr_t)list, nb = nbytes >> 5;
    uint32_t i = gx_lobj_home(t, l, nb);
    while (t->hot[i].val != 0) {
        i = (i + 1) & t->mask;
    }
    GxLobjHot* h = &t->hot[i];
    h->list = l;
    h->nb32 = (uint16_t)nb;
    h->val = val;
    t->live++;
    t->inserts++;
    return h;
}

/* Backward-shift deletion: every key after the hole up to the next empty slot that may move into the hole
 * (its home is not cyclically inside (hole, its slot]) moves there, and its old slot becomes the hole. */
void gx_lobj_delete(GxLobjTab* t, GxLobjHot* h) {
    uint32_t i = (uint32_t)(h - t->hot), m = t->mask;
    for (;;) {
        t->hot[i].val = 0;
        uint32_t j = i;
        for (;;) {
            j = (j + 1) & m;
            const GxLobjHot* c = &t->hot[j];
            if (c->val == 0) {
                t->live--;
                t->deletes++;
                return;
            }
            uint32_t k = gx_lobj_home(t, c->list, c->nb32);
            int stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
            if (!stays) {
                t->hot[i] = *c;
                i = j;
                break;
            }
        }
    }
}

void gx_lobj_clear(GxLobjTab* t) {
    memset(t->hot, 0, sizeof(GxLobjHot) << t->bits);
    t->live = 0;
    t->clears++;
}
