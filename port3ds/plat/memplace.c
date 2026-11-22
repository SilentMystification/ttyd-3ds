/* Memory placement: shared per-frame CPU state in the New 3DS extra RAM.
 *
 * The CIA's ExHeader maps the extra RAM ("QTM RAM") at 0x1E800000
 * (port3ds/rsf/ttyd.rsf; Azahar maps 4 MiB minus 128 KiB). It holds state
 * that several cores touch every frame, keeping that traffic off the FCRAM
 * bus the GPU and DSP use. The GPU does not use it (Azahar's GPU cannot reach
 * it; see docs/PORT_3DS_TESTING.md). When the mapping is absent (a 3dsx) the
 * same buffers come from the FCRAM heap: behaviour is the same, only speed
 * differs, and the fallback is reported in the log and on the HUD.
 *
 * Every placement is in s_table with a budget, so what lives in QTM is
 * visible in one place and the total is checked at boot.
 */
#include <malloc.h>
#include <string.h>

#include "plat_internal.h"

#define QTM_VADDR 0x1E800000u
#define QTM_BYTES 0x003E0000u

typedef struct {
    const char* name;
    u32 budget;
    u32 used;
    u8* base;
} Placement;

static Placement s_table[PLAT_PLACE_COUNT] = {
    [PLAT_PLACE_GX_RECORD] = {"gx record", 2u << 20, 0, NULL},
    [PLAT_PLACE_GX_STATE] = {"gx state", 512u << 10, 0, NULL},
    [PLAT_PLACE_AUDIO_STATE] = {"audio state", 512u << 10, 0, NULL},
    [PLAT_PLACE_LOG] = {"log", 320u << 10, 0, NULL},
};
static int s_qtm;

int plat_qtm_available(void) { return s_qtm; }

static int qtm_mapped(void) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, QTM_VADDR))) {
        return 0;
    }
    return mi.base_addr <= QTM_VADDR && mi.base_addr + mi.size >= QTM_VADDR + QTM_BYTES &&
           (mi.perm & (MEMPERM_READ | MEMPERM_WRITE)) == (MEMPERM_READ | MEMPERM_WRITE) && mi.state != MEMSTATE_FREE;
}

void plat_memplace_init(void) {
    u32 total = 0;
    for (int i = 0; i < PLAT_PLACE_COUNT; i++) {
        total += (s_table[i].budget + 63u) & ~63u;
    }
    if (total > QTM_BYTES) {
        plat_fatal("memplace: budgets total %lu bytes, QTM has %lu", (unsigned long)total, (unsigned long)QTM_BYTES);
    }
    /* QTM RAM is also used by the system qtm sysmodule. Writing it without
     * the qtm switch crashed the console at boot (Luma dump: qtm, NULL
     * read), so the buffers stay in FCRAM unless the switch asks for QTM. */
    int mapped = qtm_mapped();
    s_qtm = plat_switch_has("qtm") && mapped;
    u8* next = (u8*)QTM_VADDR;
    for (int i = 0; i < PLAT_PLACE_COUNT; i++) {
        Placement* p = &s_table[i];
        if (s_qtm) {
            p->base = next;
            next += (p->budget + 63u) & ~63u;
        } else {
            p->base = memalign(64, p->budget);
            if (p->base == NULL) {
                plat_fatal("memplace: no FCRAM for %s (%lu bytes)", p->name, (unsigned long)p->budget);
            }
        }
    }
    if (s_qtm) {
        PLOG_I("[MEM] QTM mapped at %08lx, %lu of %lu bytes placed", (unsigned long)QTM_VADDR, (unsigned long)total,
               (unsigned long)QTM_BYTES);
    } else if (mapped) {
        PLOG_W("[MEM] QTM mapped but not used (the system qtm module owns it; switch qtm to use it), using FCRAM (%lu bytes)",
               (unsigned long)total);
    } else {
        PLOG_W("[MEM] QTM unavailable, using FCRAM (%lu bytes)", (unsigned long)total);
    }
}

void* plat_place_alloc(int id, size_t size, size_t align) {
    if (id < 0 || id >= PLAT_PLACE_COUNT || align == 0 || (align & (align - 1)) != 0) {
        return NULL;
    }
    Placement* p = &s_table[id];
    if (p->base == NULL) {
        return NULL;
    }
    u32 start = (p->used + (u32)align - 1u) & ~((u32)align - 1u);
    if (start > p->budget || size > p->budget - start) {
        PLOG_E("[MEM] %s: %lu bytes do not fit (%lu of %lu used)", p->name, (unsigned long)size,
               (unsigned long)p->used, (unsigned long)p->budget);
        return NULL;
    }
    p->used = start + (u32)size;
    return p->base + start;
}

void plat_memplace_report(void) {
    for (int i = 0; i < PLAT_PLACE_COUNT; i++) {
        PLOG_I("[MEM] %-11s %7lu of %7lu bytes (%s)", s_table[i].name, (unsigned long)s_table[i].used,
               (unsigned long)s_table[i].budget, s_qtm ? "QTM" : "FCRAM");
    }
}
