/* Memory map for the data-address profiler (tools/port3ds/pc_sample.py --data
 * --memmap). Switch memmap: logs "[MEMMAP] name 0xSTART 0xEND" lines once
 * after the game's setup (frame 30) and again 120 frames after each map
 * change; the tool keeps the latest line of each name.
 *
 * Covers: the game's heaps and the smart heap, the OS arena, the libctru
 * heap and linear heap, every static REL's data and bss, and every sizeable
 * mapped block (svcQueryMemory), which names QTM, thread stacks, the GX
 * buffers and other allocations by the block they live in. Finer owners need
 * the plat layer to publish them (memplace.c's placement table is private). */
#include <3ds/svc.h>
#include <3ds/types.h>
#include <stdio.h>
#include <string.h>

#include "mario/mariost.h"
#include "memory.h"
#include "plat/plat.h"
#include "port3ds/rel_static.h"
#include "version.h"

#if VERSION >= VERSION_US
#define MEMMAP_HEAPS 5
#else
#define MEMMAP_HEAPS 6
#endif

extern void* heapStart[];
extern void* heapEnd[];
extern SmartWork smartWork;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;

static void line(const char* name, u32 lo, u32 hi) {
    if (hi > lo) {
        PLOG_I("[MEMMAP] %s 0x%08lx 0x%08lx", name, (unsigned long)lo, (unsigned long)hi);
    }
}

static void memmap_dump(const char* why) {
    char name[40];
    PLOG_I("[MEMMAP] begin %s", why);
    line("arena", (u32)OSGetArenaLo(), (u32)OSGetArenaHi());
    for (int i = 0; i < MEMMAP_HEAPS; i++) {
        snprintf(name, sizeof name, "gameheap%d", i);
        line(name, (u32)heapStart[i], (u32)heapEnd[i]);
    }
    if (smartWork.ptr != NULL) {
        line("smartheap", (u32)smartWork.ptr, (u32)heapEnd[MEMMAP_HEAPS - 1]);
    }
    line("ctru_heap", __ctru_heap, __ctru_heap + __ctru_heap_size);
    line("ctru_linear", __ctru_linear_heap, __ctru_linear_heap + __ctru_linear_heap_size);
    for (int i = 0; i < port_rel_count; i++) {
        const PortRelStatic* r = &port_rel_table[i];
        snprintf(name, sizeof name, "rel_%s_data", r->name);
        line(name, (u32)r->data_start, (u32)r->data_end);
        snprintf(name, sizeof name, "rel_%s_bss", r->name);
        line(name, (u32)r->bss_start, (u32)r->bss_end);
    }
    u32 addr = 0x00100000;
    int n = 0;
    while (addr < 0x40000000 && n < 80) {
        MemInfo mi;
        PageInfo pi;
        if ((s32)svcQueryMemory(&mi, &pi, addr) < 0 || mi.size == 0) {
            break;
        }
        if (mi.state != MEMSTATE_FREE && mi.size >= 0x10000) {
            snprintf(name, sizeof name, "vm_s%lx_p%lx", (unsigned long)mi.state, (unsigned long)mi.perm);
            line(name, mi.base_addr, mi.base_addr + mi.size);
            n++;
        }
        if (mi.base_addr + mi.size <= addr) {
            break;
        }
        addr = mi.base_addr + mi.size;
    }
    PLOG_I("[MEMMAP] end");
}

void port_memmap_frame(u32 frame) {
    static int on = -1;
    static char map[0x10];
    static u32 map_frame;
    if (on < 0) {
        on = plat_switch_has("memmap");
    }
    if (!on) {
        return;
    }
    if (frame == 30) {
        memmap_dump("boot");
    }
    if (gp != NULL && strncmp(map, gp->currentMapName, sizeof map) != 0) {
        memcpy(map, gp->currentMapName, sizeof map);
        map_frame = frame + 120;
    }
    if (map_frame != 0 && frame == map_frame) {
        map_frame = 0;
        memmap_dump(map);
    }
}
