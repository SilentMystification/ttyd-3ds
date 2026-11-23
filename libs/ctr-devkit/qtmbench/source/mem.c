/* qtmbench: the memory regions, what the kernel says about their mappings,
 * and the page-table attributes (svcBackdoor, console only).
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

Region g_mem[MEM_COUNT] = {
    {"heap", NULL, 0, false, false, false},
    {"linear", NULL, 0, false, false, false},
    {"qtm", NULL, 0, false, false, false},
    {"vram", NULL, 0, false, false, false},
};
u8* g_aggr[2];
u8* g_lin_aux;

#define HEAP_BYTES (8u * QB_MIB)
#define LIN_BYTES (8u * QB_MIB)
#define VRAM_BYTES (2u * QB_MIB)

static const char* state_name(u32 s) {
    switch (s) {
    case MEMSTATE_FREE: return "free";
    case MEMSTATE_RESERVED: return "reserved";
    case MEMSTATE_IO: return "io";
    case MEMSTATE_STATIC: return "static";
    case MEMSTATE_CODE: return "code";
    case MEMSTATE_PRIVATE: return "private";
    case MEMSTATE_SHARED: return "shared";
    case MEMSTATE_CONTINUOUS: return "continuous";
    case MEMSTATE_ALIASED: return "aliased";
    case MEMSTATE_ALIAS: return "alias";
    case MEMSTATE_ALIASCODE: return "aliascode";
    case MEMSTATE_LOCKED: return "locked";
    default: return "?";
    }
}

static bool query(const void* p, MemInfo* mi) {
    PageInfo pi;
    return R_SUCCEEDED(svcQueryMemory(mi, &pi, (u32)p));
}

bool mem_setup(void) {
    g_mem[MEM_HEAP].base = memalign(4096, HEAP_BYTES);
    g_mem[MEM_HEAP].size = HEAP_BYTES;
    g_aggr[0] = memalign(4096, 4 * QB_MIB);
    g_aggr[1] = memalign(4096, 4 * QB_MIB);
    g_mem[MEM_LINEAR].base = linearMemAlign(LIN_BYTES, 4096);
    g_mem[MEM_LINEAR].size = LIN_BYTES;
    g_lin_aux = linearMemAlign(4 * QB_MIB, 4096);
    g_mem[MEM_VRAM].base = vramMemAlign(VRAM_BYTES, 4096);
    g_mem[MEM_VRAM].size = VRAM_BYTES;

    for (int m = 0; m < MEM_COUNT; m++) {
        Region* r = &g_mem[m];
        if (m == MEM_QTM) {
            MemInfo mi;
            r->base = (u8*)QTM_VADDR;
            r->size = QTM_APP_SIZE;
            r->present = query(r->base, &mi) && mi.state != MEMSTATE_FREE && mi.base_addr <= QTM_VADDR &&
                         mi.base_addr + mi.size >= QTM_VADDR + QTM_APP_SIZE && (mi.perm & MEMPERM_READ);
            r->cpu_write = r->present && (mi.perm & MEMPERM_WRITE);
        } else {
            MemInfo mi;
            r->present = r->base != NULL && query(r->base, &mi);
            r->cpu_write = r->present && (mi.perm & MEMPERM_WRITE);
        }
        r->gpu = r->present && m != MEM_HEAP && osConvertVirtToPhys(r->base) != 0;
        logf_("region %-6s base %08lX size %4lu KiB present %d cpu_write %d gpu %d pa %08lX", r->name,
              (unsigned long)r->base, (unsigned long)(r->size >> 10), r->present, r->cpu_write, r->gpu,
              (unsigned long)(r->present ? osConvertVirtToPhys(r->base) : 0));
    }
    if (g_aggr[0] == NULL || g_aggr[1] == NULL || g_lin_aux == NULL || !g_mem[MEM_HEAP].present ||
        !g_mem[MEM_LINEAR].present) {
        return false;
    }
    /* Touch everything once so no first-touch page mapping is timed. */
    for (int m = 0; m < MEM_COUNT; m++) {
        if (g_mem[m].present && g_mem[m].cpu_write) {
            memset(g_mem[m].base, 0, g_mem[m].size);
            cache_flush(g_mem[m].base, g_mem[m].size);
        }
    }
    memset(g_aggr[0], 1, 4 * QB_MIB);
    memset(g_aggr[1], 2, 4 * QB_MIB);
    memset(g_lin_aux, 0, 4 * QB_MIB);
    cache_flush(g_lin_aux, 4 * QB_MIB);
    return true;
}

void cache_flush(const void* p, u32 n) { GSPGPU_FlushDataCache(p, n); }
void cache_inval(const void* p, u32 n) { GSPGPU_InvalidateDataCache(p, n); }

void fill_pattern(u32* p, u32 words, u32 seed) {
    for (u32 i = 0; i < words; i++) {
        p[i] = (i * 2654435761u) ^ seed;
    }
}

const void* chase_build(u32* base, u32 size, u32 addr_base, u32 seed) {
    u32 n = size / 32;
    u32* order = malloc(n * sizeof(u32));
    if (order == NULL) {
        return NULL;
    }
    for (u32 i = 0; i < n; i++) {
        order[i] = i;
    }
    u32 x = seed | 1;
    for (u32 i = n - 1; i > 0; i--) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        u32 j = x % (i + 1);
        u32 t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (u32 k = 0; k < n; k++) {
        u32 from = order[k], to = order[(k + 1) % n];
        base[from * 8] = addr_base + to * 32;
    }
    const void* start = (const void*)(addr_base + order[0] * 32);
    free(order);
    return start;
}

/* ---- mapping report ---- */

static void prot_test(const char* name, u32 va) {
    u32 out = 0;
    Result r1 = svcControlMemory(&out, va, 0, 0x1000, MEMOP_PROT, MEMPERM_READ);
    MemInfo mi = {0};
    query((void*)va, &mi);
    u32 perm_after = mi.perm;
    Result r2 = 0;
    if (R_SUCCEEDED(r1)) {
        r2 = svcControlMemory(&out, va, 0, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
    }
    char note[100];
    snprintf(note, sizeof(note), "rc=%08lX perm_after=%lu restore=%08lX", (unsigned long)r1, (unsigned long)perm_after,
             (unsigned long)r2);
    rec("map", "prot_to_r", name, -1, 0x1000, R_SUCCEEDED(r1) ? 1 : 0, "ok", NAN, note);
}

static void alias_test(const char* name, u32 va) {
    const u32 dst = 0x13F00000u; /* in the alias area MEMOP_MAP uses */
    u32 out = 0;
    Result r = svcControlMemory(&out, dst, va, 0x1000, MEMOP_MAP, MEMPERM_READ | MEMPERM_WRITE);
    char note[100];
    bool same = false;
    if (R_SUCCEEDED(r)) {
        same = memcmp((void*)dst, (void*)va, 64) == 0;
        Result u = svcControlMemory(&out, dst, va, 0x1000, MEMOP_UNMAP, 0);
        snprintf(note, sizeof(note), "rc=%08lX same=%d unmap=%08lX", (unsigned long)r, same, (unsigned long)u);
    } else {
        snprintf(note, sizeof(note), "rc=%08lX", (unsigned long)r);
    }
    rec("map", "alias_map", name, -1, 0x1000, R_SUCCEEDED(r) ? 1 : 0, "ok", NAN, note);
}

void mem_report(void) {
    for (int m = 0; m < MEM_COUNT; m++) {
        Region* r = &g_mem[m];
        if (r->base == NULL) {
            continue;
        }
        MemInfo mi = {0};
        bool ok = query(r->base, &mi);
        char note[100];
        snprintf(note, sizeof(note), "base=%08lX size=%lX state=%s pa=%08lX", (unsigned long)mi.base_addr,
                 (unsigned long)mi.size, state_name(mi.state), (unsigned long)osConvertVirtToPhys(r->base));
        rec("map", "perm", r->name, -1, mi.size, ok ? (double)mi.perm : -1.0, "perm", mi.state, note);
    }
    /* Beyond the application part of QTM: should be unmapped for us. */
    MemInfo mi = {0};
    if (query((void*)(QTM_VADDR + QTM_APP_SIZE), &mi)) {
        char note[100];
        snprintf(note, sizeof(note), "base=%08lX size=%lX state=%s", (unsigned long)mi.base_addr, (unsigned long)mi.size,
                 state_name(mi.state));
        rec("map", "perm", "qtm+2M", -1, mi.size, mi.perm, "perm", mi.state, note);
    }
    prot_test("heap", (u32)g_mem[MEM_HEAP].base);
    if (g_mem[MEM_QTM].present) {
        prot_test("qtm", QTM_VADDR);
        alias_test("qtm", QTM_VADDR);
    }
    alias_test("heap", (u32)g_mem[MEM_HEAP].base);
}

/* ---- page tables (kernel mode through svcBackdoor) ----
 * Reads TTBR0/TTBCR and the process's L1/L2 descriptors for one VA. Physical
 * table addresses are reached through the kernel's linear mappings of FCRAM
 * (0xE0000000) and AXI WRAM (0xDFF80000), kernel 8.0 and later; anything
 * outside those is not touched (err set). */

static volatile u32 s_k_va, s_k_ttbr0, s_k_ttbcr, s_k_l1, s_k_l2, s_k_err;

static u32 k_kva(u32 pa) {
    if (pa >= 0x20000000u && pa < 0x30000000u) {
        return pa - 0x20000000u + 0xE0000000u;
    }
    if (pa >= 0x1FF80000u && pa < 0x20000000u) {
        return pa - 0x1FF80000u + 0xDFF80000u;
    }
    return 0;
}

static s32 k_walk(void) {
    u32 ttbr0, ttbcr;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
    s_k_ttbr0 = ttbr0;
    s_k_ttbcr = ttbcr;
    s_k_l1 = s_k_l2 = 0;
    u32 va = s_k_va;
    u32 n = ttbcr & 7;
    if (n != 0 && va >= (1u << (32 - n))) {
        s_k_err = 1;
        return 0;
    }
    u32 kv = k_kva(ttbr0 & ~((1u << (14 - n)) - 1));
    if (kv == 0) {
        s_k_err = 2;
        return 0;
    }
    u32 l1 = ((volatile u32*)kv)[va >> 20];
    s_k_l1 = l1;
    if ((l1 & 3) == 1) {
        u32 k2 = k_kva(l1 & ~0x3FFu);
        if (k2 == 0) {
            s_k_err = 3;
            return 0;
        }
        s_k_l2 = ((volatile u32*)k2)[(va >> 12) & 0xFF];
    }
    s_k_err = 0;
    return 0;
}

static const char* memtype(u32 tex, u32 c, u32 b, char* buf, size_t n) {
    static const char* pol[4] = {"NC", "WB-WA", "WT", "WB"};
    u32 cb = (c << 1) | b;
    if (tex & 4) {
        snprintf(buf, n, "normal inner=%s outer=%s", pol[cb], pol[tex & 3]);
    } else if (tex == 0) {
        static const char* t0[4] = {"strongly-ordered", "shared-device", "normal WT", "normal WB"};
        snprintf(buf, n, "%s", t0[cb]);
    } else if (tex == 1 && cb == 0) {
        snprintf(buf, n, "normal non-cacheable");
    } else if (tex == 1 && cb == 3) {
        snprintf(buf, n, "normal WB-WA");
    } else if (tex == 2 && cb == 0) {
        snprintf(buf, n, "non-shared device");
    } else {
        snprintf(buf, n, "reserved tex=%lu cb=%lu", (unsigned long)tex, (unsigned long)cb);
    }
    return buf;
}

static void pte_one(const char* name, const void* p) {
    s_k_va = (u32)p;
    s_k_err = 0xFF;
    svcBackdoor(k_walk);
    char note[100], mt[40];
    u32 l1 = s_k_l1, l2 = s_k_l2;
    u32 tex, c, b, s, xn, ap;
    const char* kind;
    if (s_k_err != 0) {
        snprintf(note, sizeof(note), "walk failed err=%lu ttbr0=%08lX ttbcr=%lX", (unsigned long)s_k_err,
                 (unsigned long)s_k_ttbr0, (unsigned long)s_k_ttbcr);
        rec("map", "pte", name, -1, 0, -1, "raw", NAN, note);
        return;
    }
    if ((l1 & 3) == 2) {
        kind = (l1 & (1u << 18)) ? "supersection" : "section";
        b = (l1 >> 2) & 1;
        c = (l1 >> 3) & 1;
        xn = (l1 >> 4) & 1;
        ap = ((l1 >> 10) & 3) | (((l1 >> 15) & 1) << 2);
        tex = (l1 >> 12) & 7;
        s = (l1 >> 16) & 1;
    } else if ((l1 & 3) == 1 && (l2 & 3) == 1) {
        kind = "64K page";
        b = (l2 >> 2) & 1;
        c = (l2 >> 3) & 1;
        ap = ((l2 >> 4) & 3) | (((l2 >> 9) & 1) << 2);
        s = (l2 >> 10) & 1;
        tex = (l2 >> 12) & 7;
        xn = (l2 >> 15) & 1;
    } else if ((l1 & 3) == 1 && (l2 & 2)) {
        kind = "4K page";
        xn = l2 & 1;
        b = (l2 >> 2) & 1;
        c = (l2 >> 3) & 1;
        ap = ((l2 >> 4) & 3) | (((l2 >> 9) & 1) << 2);
        tex = (l2 >> 6) & 7;
        s = (l2 >> 10) & 1;
    } else {
        snprintf(note, sizeof(note), "unmapped l1=%08lX l2=%08lX", (unsigned long)l1, (unsigned long)l2);
        rec("map", "pte", name, -1, 0, -1, "raw", NAN, note);
        return;
    }
    snprintf(note, sizeof(note), "%s %s S=%lu XN=%lu AP=%lu", kind, memtype(tex, c, b, mt, sizeof(mt)),
             (unsigned long)s, (unsigned long)xn, (unsigned long)ap);
    rec("map", "pte", name, -1, 0, (l1 & 3) == 2 ? l1 : l2, "raw", l1, note);
    info(name[0] == 'q' ? "qtm_attrs" : name[0] == 'h' ? "heap_attrs" : name[0] == 'l' ? "linear_attrs" : "vram_attrs",
         "%s", note);
}

void mem_pte_walk(void) {
    if (g_opt.emulator) {
        rec("map", "pte", "all", -1, 0, -1, "raw", NAN, "skipped: emulator has no page tables");
        return;
    }
    for (int m = 0; m < MEM_COUNT; m++) {
        if (g_mem[m].present) {
            pte_one(g_mem[m].name, g_mem[m].base);
        }
    }
}
