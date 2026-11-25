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

void plat_gpudump_init(void) {
    s_target_frame = plat_switch_int("gpudump", -1);
    s_test_hang = plat_switch_int("gpuhangtest", -1);
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
