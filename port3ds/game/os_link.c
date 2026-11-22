/* OSLink / OSUnlink for the static RELs.
 *
 * Every REL is linked into the executable (tools/port3ds/rel_link.py). The
 * game still loads "<root>/rel/<area>.rel", copies it to gp->mapAlloc, calls
 * OSLink and then the header's prolog. On the port that file is a small
 * module header made by the port's DVD layer (port_rel_make_header) whose
 * id names a static REL. OSLink then does what loading a fresh REL does on
 * the GameCube: the REL's data (and read-only data, writable RAM on the
 * GameCube) is restored from the snapshot taken at boot, its bss is zeroed,
 * and the header gets the addresses of its prolog, epilog and unresolved
 * functions.
 */
#include <dolphin/os.h>
#include <malloc.h>
#include <string.h>

#include "plat/memstable.h"
#include "plat/plat.h"
#include "port3ds/os_internal.h"
#include "port3ds/rel_static.h"

#define PORT_REL_ID_BASE 0x7D10000u /* module ids made by the port */

static u8** s_snapshots;
static OSModuleInfo* s_linked;

void port_rel_snapshot_all(void) {
    s_snapshots = calloc((size_t)port_rel_count, sizeof(u8*));
    u32 total = 0;
    for (int i = 0; i < port_rel_count; i++) {
        const PortRelStatic* r = &port_rel_table[i];
        u32 size = (u32)(r->data_end - r->data_start);
        s_snapshots[i] = malloc(size ? size : 1);
        if (s_snapshots[i] == NULL) {
            plat_fatal("REL %s: no memory for its %lu-byte snapshot", r->name, (unsigned long)size);
        }
        memcpy(s_snapshots[i], r->data_start, size);
        total += size;
    }
    PLOG_I("RELs: %d static, %lu bytes of data snapshots", port_rel_count, (unsigned long)total);
}

int port_rel_index(const char* area) {
    for (int i = 0; i < port_rel_count; i++) {
        if (strcmp(port_rel_table[i].name, area) == 0) {
            return i;
        }
    }
    return -1;
}

u32 port_rel_header_size(void) { return sizeof(OSModuleHeader); }

/* Fills the module header the DVD layer returns for "rel/<area>.rel". */
void port_rel_make_header(int index, void* out) {
    OSModuleHeader* h = out;
    memset(h, 0, sizeof(*h));
    h->info.id = PORT_REL_ID_BASE + (u32)index;
    h->info.version = OS_MODULE_VERSION;
}

static const PortRelStatic* rel_of(OSModuleInfo* m, int* index) {
    u32 i = m->id - PORT_REL_ID_BASE;
    if (m->id < PORT_REL_ID_BASE || i >= (u32)port_rel_count) {
        return NULL;
    }
    *index = (int)i;
    return &port_rel_table[i];
}

BOOL OSLink(OSModuleInfo* newModule, void* bss) {
    (void)bss;
    int i;
    const PortRelStatic* r = rel_of(newModule, &i);
    if (r == NULL) {
        plat_fatal("OSLink: module id %08lx is not a static REL", (unsigned long)newModule->id);
    }
    memcpy(r->data_start, s_snapshots[i], (size_t)(r->data_end - r->data_start));
    plat_memstable_fill(r->data_start, (u32)(r->data_end - r->data_start)); /* the REL file's data again */
    memset(r->bss_start, 0, (size_t)(r->bss_end - r->bss_start));
    OSModuleHeader* h = (OSModuleHeader*)newModule;
    h->prolog = (u32)r->prolog;
    h->epilog = (u32)r->epilog;
    h->unresolved = (u32)r->unresolved;
    newModule->link.next = s_linked;
    newModule->link.prev = NULL;
    s_linked = newModule;
    plat_set_map_name(r->name);
    PLOG_I("OSLink %s (data %lu, bss %lu)", r->name, (unsigned long)(r->data_end - r->data_start),
           (unsigned long)(r->bss_end - r->bss_start));
    return TRUE;
}

BOOL OSUnlink(OSModuleInfo* oldModule) {
    int i;
    const PortRelStatic* r = rel_of(oldModule, &i);
    if (r == NULL) {
        return FALSE;
    }
    OSModuleInfo** p = &s_linked;
    while (*p != NULL && *p != oldModule) {
        p = &(*p)->link.next;
    }
    if (*p == oldModule) {
        *p = oldModule->link.next;
    }
    plat_memstable_release(r->data_start, (u32)(r->data_end - r->data_start));
    PLOG_I("OSUnlink %s", r->name);
    return TRUE;
}
