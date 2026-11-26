/* GPU command list dumps (debug aid, no effect on rendering).
 *
 * The render thread calls plat_gpudump_record() for every record just before C3D_FrameEnd: the raw u32
 * words that citro3d/GPUCMD wrote for that record (the command buffer is one buffer, so only the latest
 * list is still in memory) and the address ranges of the vertex and index arenas the record draws from
 * are remembered. Two users:
 *   - gpu_fence_wait (plat/gpu.c), on the first 2 s timeout: plat_gpudump_hang() writes the list of the
 *     record the GPU has not finished (done seq + 1) to sdmc:/3ds/ttyd/gpuhang/. The render thread is
 *     then still inside C3D_FrameBegin waiting for the GPU, so the command buffer still holds that list.
 *   - switch gpudump=N (Azahar or console): every record of game frame N is written the same way, so the
 *     same frame can be compared with tools/port3ds/gpu_cmd_diff.py.
 * Files, in sdmc:/3ds/ttyd/gpuhang/: <tag>_f<frame>_r<seq>.bin (the raw words, little-endian) and
 * <tag>_f<frame>_r<seq>.txt (key=value lines: seq, frame, words, command buffer address, vertex and index
 * arena ranges, linear heap and VRAM ranges, the render phase). tag is "hang" or "dump".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "plat_internal.h"

typedef struct {
    u32 seq, frame, part; /* part: 0 for the first list of a record, then 1, 2 (a copy pass that needed a new frame) */
    const u32* addr;
    u32 words;
    uintptr_t verts, idx;
    u32 vert_bytes, idx_bytes;
    int valid;
} ListNote;

static ListNote s_last;
static int s_target_frame = -1; /* switch gpudump=N */
static int s_hang_done;
static int s_test_hang = -1; /* switch gpuhangtest=N: the render thread stops before C3D_FrameBegin of game frame N */

extern u32 __ctru_linear_heap, __ctru_linear_heap_size;

/* ---- the memory the list reads (<stem>.mem) ----
 * gpu_cmd_diff.py checks index ranges, attribute buffer extents and textures offline, so it needs the bytes the
 * GPU reads. The list is walked here with a small register tracker (attribute buffer base/offsets, index buffer,
 * texture units) and, at each draw trigger, the buffers the draw addresses are added to a region table: the vertex
 * and index arenas of the record, every attribute buffer base (from its start to the highest vertex a draw
 * fetches; alloc is the allocation size when linearGetSize knows it) and the first MEM_TEXSAMPLE bytes of each enabled texture. File layout, all
 * little-endian u32: "MEM1", n, then n entries {phys, virt, len, alloc, kind} (kind 0 vertex arena, 1 index arena,
 * 2 attribute buffer, 3 texture sample; alloc is 0 when unknown), then the n data blocks in order. */
#define MEM_MAX_REGIONS 1024
#define MEM_CAP (16u << 20)
#define MEM_TEXSAMPLE 64u
typedef struct {
    u32 phys, virt, len, alloc, kind;
} DumpRegion;
static DumpRegion s_reg[MEM_MAX_REGIONS];
static u32 s_nreg, s_mem_total;

static int lin_ok(u32 virt) { return virt >= __ctru_linear_heap && virt < __ctru_linear_heap + __ctru_linear_heap_size; }

/* The linear heap is one contiguous physical block: virtual = heap + (phys - heap's physical start). 0 outside it. */
static u32 phys_to_virt(u32 phys) {
    u32 p0 = (u32)osConvertVirtToPhys((void*)(uintptr_t)__ctru_linear_heap);
    return phys >= p0 && phys - p0 < __ctru_linear_heap_size ? __ctru_linear_heap + (phys - p0) : 0;
}

/* Adds (or widens) the region starting at virt. */
static void mem_add(u32 virt, u32 len, u32 alloc, u32 kind) {
    if (!lin_ok(virt) || len == 0) {
        return;
    }
    u32 room = __ctru_linear_heap + __ctru_linear_heap_size - virt;
    if (len > room) {
        len = room;
    }
    for (u32 i = 0; i < s_nreg; i++) {
        if (s_reg[i].virt == virt) {
            if (len > s_reg[i].len && s_mem_total + (len - s_reg[i].len) <= MEM_CAP) {
                s_mem_total += len - s_reg[i].len;
                s_reg[i].len = len;
            }
            return;
        }
    }
    if (s_nreg == MEM_MAX_REGIONS || s_mem_total + len > MEM_CAP) {
        return;
    }
    s_reg[s_nreg++] = (DumpRegion){(u32)osConvertVirtToPhys((void*)(uintptr_t)virt), virt, len, alloc, kind};
    s_mem_total += len;
}

/* The memory a draw reads from the buffer at phys: bytes from there on, at least need. The region covers what the
 * draw addresses (one stride past the highest vertex), so a vertex fetch past the end of the allocation is still
 * dumped; alloc (linearGetSize, 0 when the pointer is not an allocation start) says where the buffer really ends. */
static void mem_add_buffer(u32 phys, u32 need) {
    u32 virt = phys_to_virt(phys);
    if (!lin_ok(virt) || need == 0) {
        return;
    }
    for (u32 i = 0; i < s_nreg; i++) {
        if (s_reg[i].virt == virt) {
            mem_add(virt, need, s_reg[i].alloc, s_reg[i].kind); /* widen */
            return;
        }
        if (s_reg[i].kind <= 1 && virt > s_reg[i].virt && virt - s_reg[i].virt < s_reg[i].len) {
            return; /* inside an arena (the indices of a draw): already dumped with it */
        }
    }
    u32 alloc = (u32)linearGetSize((void*)(uintptr_t)virt);
    if (alloc == 0 || alloc > __ctru_linear_heap_size || !lin_ok(virt + alloc - 1)) {
        alloc = 0;
    }
    mem_add(virt, need, alloc, 2);
}

/* Register tracker over the list (same framing as gpu_cmd_diff.py's decode). */
static void mem_walk(const u32* w, u32 n) {
    static u32 st[0x300];
    memset(st, 0, sizeof(st));
    for (u32 i = 0; i + 1 < n;) {
        u32 p0 = w[i], hdr = w[i + 1];
        u32 reg = hdr & 0xFFFF, mask = (hdr >> 16) & 0xF, extra = (hdr >> 20) & 0xFF, consec = hdr >> 31;
        if (i + 2 + extra > n) {
            break;
        }
        for (u32 k = 0; k <= extra; k++) {
            u32 r = reg + (consec ? k : 0);
            u32 v = k == 0 ? p0 : w[i + 1 + k];
            if (r >= 0x300 || mask == 0) {
                continue;
            }
            u32 keep = 0;
            for (u32 b = 0; b < 4; b++) {
                if (!(mask >> b & 1)) {
                    keep |= 0xFFu << (8 * b);
                }
            }
            st[r] = (st[r] & keep) | (v & ~keep);
            if (r == 0x22E || r == 0x22F) {
                /* a draw trigger: the span of vertices it fetches from each attribute buffer */
                u32 base = st[0x200] << 3, nv = st[0x228], first = st[0x22A], hi = first + nv; /* vertices [first, hi) */
                if (r == 0x22F) {
                    u32 ib = phys_to_virt(base + (st[0x227] & 0x0FFFFFFF)), u16fmt = st[0x227] >> 31;
                    u32 isz = u16fmt ? 2 : 1, mx = 0, seen = 0;
                    if (lin_ok(ib) && lin_ok(ib + nv * isz - 1)) {
                        for (u32 j = 0; j < nv; j++) {
                            u32 x = u16fmt ? ((const u16*)(uintptr_t)ib)[j] : ((const u8*)(uintptr_t)ib)[j];
                            mx = x > mx ? x : mx;
                            seen = 1;
                        }
                        mem_add_buffer(base + (st[0x227] & 0x0FFFFFFF), nv * isz);
                    }
                    hi = seen ? first + mx + 1 : first;
                }
                for (u32 b = 0; b < 12; b++) {
                    u32 c2 = st[0x205 + 3 * b];
                    if ((c2 >> 28) != 0) {
                        mem_add_buffer(base + st[0x203 + 3 * b], hi * ((c2 >> 16) & 0xFF) + 16);
                    }
                }
                static const u16 ar[3] = {0x85, 0x95, 0x9D};
                for (u32 u = 0; u < 3; u++) {
                    if ((st[0x80] >> u & 1) && st[ar[u]] != 0) {
                        mem_add(phys_to_virt(st[ar[u]] << 3), MEM_TEXSAMPLE, 0, 3);
                    }
                }
            }
        }
        i += 2 + extra + ((extra + 2) & 1);
    }
}

static void write_mem(const char* stem, const ListNote* n) {
    s_nreg = 0;
    s_mem_total = 0;
    mem_add((u32)(uintptr_t)n->verts, n->vert_bytes, n->vert_bytes, 0);
    mem_add((u32)(uintptr_t)n->idx, n->idx_bytes, n->idx_bytes, 1);
    mem_walk(n->addr, n->words);
    char path[64];
    snprintf(path, sizeof(path), "gpuhang/%s.mem", stem);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    u32 head[2] = {0x314D454Du /* "MEM1" */, s_nreg};
    fwrite(head, 4, 2, f);
    fwrite(s_reg, sizeof(DumpRegion), s_nreg, f);
    for (u32 i = 0; i < s_nreg; i++) {
        fwrite((const void*)(uintptr_t)s_reg[i].virt, 1, s_reg[i].len, f);
    }
    fclose(f);
    PLOG_I("gpudump: %lu memory regions, %lu bytes -> gpuhang/%s.mem", (unsigned long)s_nreg, (unsigned long)s_mem_total, stem);
}

/* Bisect aid (switches gpudrawcut=K, gpudrawskip=A-B, gpudrawrec=S): render_record asks before every game draw call
 * (C3D_DrawArrays / C3D_DrawElements) whether to make it. idx counts those calls in the record from 0, in the order
 * of the draw triggers in the dumped list (tools/port3ds/gpu_cmd_diff.py prints the same number). The filter applies
 * to the records of game frame gpudump=N when gpudump is given, else to every record; gpudrawrec=S narrows it to the
 * record with submit number S. cut: only draws 0..K-1 are made. skip: draws A..B (inclusive) are not made. */
static int s_cut = -1, s_skip_a = -1, s_skip_b = -1, s_rec_only = -1, s_filter;

int plat_gpudump_draw_filter(u32 frame, u32 seq, u32 idx) {
    if (!s_filter || (s_target_frame >= 0 && (int)frame != s_target_frame) || (s_rec_only >= 0 && (int)seq != s_rec_only)) {
        return 1;
    }
    if (s_cut >= 0 && (int)idx >= s_cut) {
        return 0;
    }
    return !(s_skip_a >= 0 && (int)idx >= s_skip_a && (int)idx <= s_skip_b);
}

void plat_gpudump_init(void) {
    s_target_frame = plat_switch_int("gpudump", -1);
    s_test_hang = plat_switch_int("gpuhangtest", -1);
    s_cut = plat_switch_int("gpudrawcut", -1);
    s_rec_only = plat_switch_int("gpudrawrec", -1);
    char v[24];
    if (plat_switch_value("gpudrawskip", v, sizeof(v))) {
        char* end;
        long a = strtol(v, &end, 0);
        long b = *end == '-' ? strtol(end + 1, NULL, 0) : a;
        if (end != v && b >= a) {
            s_skip_a = (int)a;
            s_skip_b = (int)b;
        }
    }
    s_filter = s_cut >= 0 || s_skip_a >= 0;
    if (s_filter) {
        PLOG_W("gpudrawcut/gpudrawskip: draws are dropped (cut %d, skip %d-%d, %s)", s_cut, s_skip_a, s_skip_b,
               s_target_frame >= 0 ? "game frame of gpudump only" : "every frame");
    }
    if (s_target_frame >= 0) {
        PLOG_I("gpudump: command lists of game frame %d go to gpuhang/dump_f%d_r*", s_target_frame, s_target_frame);
    }
}

static void write_files(const char* tag, const ListNote* n, const char* extra) {
    mkdir("gpuhang", 0777);
    char path[64], stem[48];
    if (n->part) {
        snprintf(stem, sizeof(stem), "%s_f%lu_r%lu_p%lu", tag, (unsigned long)n->frame, (unsigned long)n->seq, (unsigned long)n->part);
    } else {
        snprintf(stem, sizeof(stem), "%s_f%lu_r%lu", tag, (unsigned long)n->frame, (unsigned long)n->seq);
    }
    snprintf(path, sizeof(path), "gpuhang/%s.bin", stem);
    FILE* f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(n->addr, 4, n->words, f);
        fclose(f);
    }
    write_mem(stem, n);
    snprintf(path, sizeof(path), "gpuhang/%s.txt", stem);
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "seq=%lu\npart=%lu\nframe=%lu\nwords=%lu\ncmdbuf=%08lx\n", (unsigned long)n->seq, (unsigned long)n->part,
                (unsigned long)n->frame, (unsigned long)n->words, (unsigned long)(uintptr_t)n->addr);
        fprintf(f, "verts=%08lx-%08lx\nidx=%08lx-%08lx\n", (unsigned long)n->verts,
                (unsigned long)(n->verts + n->vert_bytes), (unsigned long)n->idx, (unsigned long)(n->idx + n->idx_bytes));
        fprintf(f, "linear=%08lx-%08lx\nvram=18000000-18600000\n", (unsigned long)__ctru_linear_heap,
                (unsigned long)(__ctru_linear_heap + __ctru_linear_heap_size));
        /* the GPU sees physical addresses: the linear heap starts at physical 20000000 (FCRAM) */
        fprintf(f, "verts_phys=%08lx\nidx_phys=%08lx\nlinear_phys=%08lx\n", (unsigned long)osConvertVirtToPhys((void*)n->verts),
                (unsigned long)osConvertVirtToPhys((void*)n->idx), (unsigned long)osConvertVirtToPhys((void*)__ctru_linear_heap));
        if (extra != NULL) {
            fprintf(f, "%s\n", extra);
        }
        fclose(f);
    }
    PLOG_I("gpudump: %s record %lu (game frame %lu): %lu words at %08lx, vertex arena %08lx+%lu, index arena %08lx+%lu "
           "-> gpuhang/%s.bin", tag, (unsigned long)n->seq, (unsigned long)n->frame, (unsigned long)n->words,
           (unsigned long)(uintptr_t)n->addr, (unsigned long)n->verts, (unsigned long)n->vert_bytes,
           (unsigned long)n->idx, (unsigned long)n->idx_bytes, stem);
}

/* Render thread, right before C3D_FrameEnd of the record. */
void plat_gpudump_record(u32 seq, u32 frame, const void* verts, u32 vert_bytes, const void* idx, u32 idx_bytes) {
    u32* addr = NULL;
    u32 size = 0, off = 0;
    GPUCMD_GetBuffer(&addr, &size, &off);
    ListNote n = {seq, frame, s_last.valid && s_last.seq == seq ? s_last.part + 1 : 0, addr, off, (uintptr_t)verts,
                  (uintptr_t)idx, vert_bytes, idx_bytes, addr != NULL};
    s_last = n;
    if (s_target_frame >= 0 && (int)frame == s_target_frame && addr != NULL) {
        write_files("dump", &n, NULL);
    }
}

/* Game thread, on the first fence timeout. seq is the record the game thread waits for to be done; the GPU
 * is stuck on the one after the last done record. */
void plat_gpudump_hang(u32 seq, u32 done, u32 game_frame, const char* phase_line) {
    if (s_hang_done) {
        return;
    }
    s_hang_done = 1;
    ListNote n = s_last; /* a copy: the render thread may still change it */
    char extra[200];
    snprintf(extra, sizeof(extra), "waiting_for=%lu\ndone=%lu\ngame_frame=%lu\n%s", (unsigned long)seq, (unsigned long)done,
             (unsigned long)game_frame, phase_line != NULL ? phase_line : "");
    if (!n.valid || n.words == 0) {
        PLOG_W("gpudump: hang at seq %lu (done %lu): no command list was recorded", (unsigned long)seq,
               (unsigned long)done);
        return;
    }
    if (n.seq != done + 1) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "\nnote=last list is record %lu, the GPU is on %lu",
                 (unsigned long)n.seq, (unsigned long)(done + 1));
    }
    write_files("hang", &n, extra);
}

/* Test aid (switch gpuhangtest=N): true for the record of game frame N. The render thread stops where
 * C3D_FrameBegin would wait for a GPU that never finishes, the console's hang state, so the fence timeout dump, the
 * watchdog report and the recovery can be tried in Azahar. */
int plat_gpudump_test_hang(u32 frame) { return s_test_hang >= 0 && (int)frame == s_test_hang; }
