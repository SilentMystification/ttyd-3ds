/* Large port-side tables live on the heap, not in .bss: the executable image must end below 0x01000000
 * (port_addr_impl.h, PORT_IS_ADDR), so every MiB of .bss counts against that limit. Allocated once at
 * module init, zeroed like .bss was; failure is fatal. */
#ifndef GX_BIGMEM_H
#define GX_BIGMEM_H

#include <malloc.h>
#include <string.h>

#include "plat/plat.h"

static inline void* gx_big_alloc(const char* what, unsigned long bytes) {
    void* p = memalign(32, bytes);
    if (p == NULL) {
        plat_fatal("out of memory for %s (%lu bytes)", what, bytes);
    }
    memset(p, 0, bytes);
    plat_heap_note(what, bytes);
    return p;
}

/* As gx_big_alloc, as a plat/memplace placement named `what` for a CPU-only hot table: the heap as before, QTM
 * only by place=<what>:qtm (docs/PORT_3DS_QTM.md). Zeroed. */
static inline void* gx_big_place(const char* what, unsigned long bytes) {
    int h = plat_place_register(what, bytes, 64, PLAT_PREFS(PLAT_TIER_QTM, PLAT_TIER_HEAP), PLAT_PF_QTM_ASK);
    void* p = h >= 0 ? plat_place_alloc(h, bytes, 64) : NULL;
    if (p == NULL) {
        return gx_big_alloc(what, bytes);
    }
    memset(p, 0, bytes);
    const char* tier = NULL;
    plat_place_info(h, NULL, NULL, NULL, &tier);
    if (tier != NULL && tier[0] == 'H') {
        plat_heap_note(what, bytes);
    }
    return p;
}

#endif
