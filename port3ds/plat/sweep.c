/* Map sweep entry hook registry (see include/plat/sweep.h). Game-agnostic. */
#include "plat/sweep.h"

#include <stddef.h>

static SweepEntryHook s_entry_hook;

void plat_sweep_register_entry_hook(SweepEntryHook hook) {
    s_entry_hook = hook;
}

const char* plat_sweep_entry_host(const char* map, int* story) {
    if (s_entry_hook == NULL) {
        return NULL;
    }
    return s_entry_hook(map, story);
}
