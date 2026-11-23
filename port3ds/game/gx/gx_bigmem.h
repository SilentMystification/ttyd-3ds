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
    return p;
}

#endif
