// Hardware probe for the TTYD 3DS port.
//
// Reports what the running process was granted (New 3DS mode, clock, L2,
// core 2, memory, the QTM mapping) and measures a few costs the port design
// depends on. Results go to the top screen and to sdmc:/3ds/ttyd/probe.txt.
//
// In Azahar the timings only show that the code paths work; Azahar does not
// model CPU or memory timing. Real numbers come from the first hardware run
// (plan phase 8).
//
// Exits on START, or by itself when sdmc:/3ds/ttyd/debug3ds.txt contains the
// word "autoexit" (used by tools/port3ds/run_azahar.py).

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define QTM_VADDR 0x1E800000u
#define QTM_TEST_SIZE (1u << 20)
#define BW_BUF_SIZE (1u << 20)
#define BW_REPEATS 16
#define TICKS_PER_US (SYSCLOCK_ARM11 / 1000000.0)

static FILE *s_log;

static void out(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    if (s_log != NULL) {
        fputs(line, s_log);
    }
}

static bool debug_has(const char *word) {
    FILE *f = fopen("sdmc:/3ds/ttyd/debug3ds.txt", "r");
    if (f == NULL) {
        return false;
    }
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, word) != NULL;
}

// Copies BW_BUF_SIZE bytes from src to dst BW_REPEATS times and returns MB/s.
static double bandwidth(void *dst, const void *src) {
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < BW_REPEATS; i++) {
        memcpy(dst, src, BW_BUF_SIZE);
    }
    u64 dt = svcGetSystemTick() - t0;
    double us = dt / TICKS_PER_US;
    return us > 0.0 ? (double)BW_BUF_SIZE * BW_REPEATS / us : 0.0;
}

static void probe_system(void) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    out("model        : %s\n", n3ds ? "New 3DS" : "Old 3DS");

    if (n3ds) {
        // A CIA gets 804 MHz and L2 from its ExHeader; a 3dsx asks here.
        osSetSpeedupEnable(true);
    }
    out("speedup      : %s\n", n3ds ? "requested" : "n/a");

    s64 appmem = 0;
    svcGetSystemInfo(&appmem, 0, 1);
    out("APPLICATION  : %lld KiB used of %lu KiB region\n", appmem / 1024,
        (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024));
    out("linear free  : %lu KiB\n", (unsigned long)(linearSpaceFree() / 1024));

    u32 cpu_limit = 0;
    APT_GetAppCpuTimeLimit(&cpu_limit);
    out("core1 limit  : %lu%%\n", (unsigned long)cpu_limit);
}

static void probe_ticks(void) {
    const int n = 100000;
    u64 t0 = svcGetSystemTick();
    volatile u64 sink = 0;
    for (int i = 0; i < n; i++) {
        sink += svcGetSystemTick();
    }
    u64 dt = svcGetSystemTick() - t0;
    (void)sink;
    out("GetSystemTick: %.3f us per call\n", dt / TICKS_PER_US / n);
}

static void probe_bandwidth(void *qtm) {
    void *heap_a = malloc(BW_BUF_SIZE);
    void *heap_b = malloc(BW_BUF_SIZE);
    void *lin = linearAlloc(BW_BUF_SIZE);
    if (heap_a == NULL || heap_b == NULL || lin == NULL) {
        out("bandwidth    : allocation failed\n");
        goto done;
    }
    memset(heap_a, 0x5A, BW_BUF_SIZE);
    out("memcpy heap->heap  : %.1f MB/s\n", bandwidth(heap_b, heap_a));
    out("memcpy heap->linear: %.1f MB/s\n", bandwidth(lin, heap_a));
    if (qtm != NULL) {
        out("memcpy heap->QTM   : %.1f MB/s\n", bandwidth(qtm, heap_a));
        out("memcpy QTM->heap   : %.1f MB/s\n", bandwidth(heap_b, qtm));
    }
done:
    free(heap_a);
    free(heap_b);
    if (lin != NULL) {
        linearFree(lin);
    }
}

// Returns the QTM base if the process has it mapped read/write, else NULL.
static void *probe_qtm(void) {
    MemInfo mi;
    PageInfo pi;
    Result r = svcQueryMemory(&mi, &pi, QTM_VADDR);
    if (R_FAILED(r)) {
        out("QTM          : svcQueryMemory failed 0x%08lX\n", (unsigned long)r);
        return NULL;
    }
    out("QTM query    : base 0x%08lX size 0x%lX perm %lu state %lu\n", (unsigned long)mi.base_addr,
        (unsigned long)mi.size, (unsigned long)mi.perm, (unsigned long)mi.state);
    bool mapped = mi.base_addr <= QTM_VADDR && mi.base_addr + mi.size >= QTM_VADDR + QTM_TEST_SIZE &&
                  (mi.perm & (MEMPERM_READ | MEMPERM_WRITE)) == (MEMPERM_READ | MEMPERM_WRITE) &&
                  mi.state != MEMSTATE_FREE;
    if (!mapped) {
        out("QTM          : not mapped\n");
        return NULL;
    }

    u32 *q = (u32 *)QTM_VADDR;
    u32 words = QTM_TEST_SIZE / 4;
    for (u32 i = 0; i < words; i++) {
        q[i] = i * 2654435761u;
    }
    u32 bad = 0;
    for (u32 i = 0; i < words; i++) {
        if (q[i] != i * 2654435761u) {
            bad++;
        }
    }
    out("QTM CPU test : %s (%lu bad words in %u KiB)\n", bad == 0 ? "ok" : "FAILED", (unsigned long)bad,
        QTM_TEST_SIZE / 1024);
    return bad == 0 ? (void *)QTM_VADDR : NULL;
}

// GPU TextureCopy of `size` bytes from src to dst (raw copy); returns the
// number of mismatching words.
static u32 gpu_copy_check(const char *label, u32 *src, u32 *dst, u32 size) {
    memset(dst, 0, size);
    for (u32 i = 0; i < size / 4; i++) {
        src[i] = 0xC0DE0000u ^ i;
    }
    Result rf1 = GSPGPU_FlushDataCache(src, size);
    Result rf2 = GSPGPU_FlushDataCache(dst, size);
    Result r = GX_TextureCopy(src, GX_BUFFER_DIM(size, 0), dst, GX_BUFFER_DIM(size, 0), size, 8);
    if (R_SUCCEEDED(r)) {
        gspWaitForPPF();
    }
    GSPGPU_InvalidateDataCache(dst, size);
    u32 bad = 0;
    for (u32 i = 0; i < size / 4; i++) {
        if (dst[i] != (0xC0DE0000u ^ i)) {
            bad++;
        }
    }
    out("%s: %s (%lu bad words; flush 0x%08lX/0x%08lX copy 0x%08lX)\n", label, bad == 0 ? "ok" : "FAILED",
        (unsigned long)bad, (unsigned long)rf1, (unsigned long)rf2, (unsigned long)r);
    return bad;
}

// Checks whether the GPU can read QTM, with a linear-to-linear copy as the
// control for the copy call itself.
static void probe_qtm_gpu(void *qtm) {
    const u32 size = 64 * 1024;
    u32 *src = linearAlloc(size);
    u32 *dst = linearAlloc(size);
    if (src == NULL || dst == NULL) {
        out("QTM GPU read : linearAlloc failed\n");
    } else {
        out("QTM phys     : 0x%08lX (osConvertVirtToPhys)\n", (unsigned long)osConvertVirtToPhys(qtm));
        gpu_copy_check("GPU lin->lin ", src, dst, size);
        gpu_copy_check("GPU QTM->lin ", (u32 *)qtm, dst, size);
        gpu_copy_check("GPU lin->QTM ", src, (u32 *)qtm, size);
    }
    if (src != NULL) {
        linearFree(src);
    }
    if (dst != NULL) {
        linearFree(dst);
    }
}

typedef struct {
    int core;
    volatile u64 ticks;
    volatile int ran_on;
} CoreJob;

static void core_job(void *arg) {
    CoreJob *job = arg;
    job->ran_on = svcGetProcessorID();
    u64 t0 = svcGetSystemTick();
    volatile u32 x = 1;
    for (u32 i = 0; i < 10000000u; i++) {
        x = x * 1664525u + 1013904223u;
    }
    job->ticks = svcGetSystemTick() - t0;
}

static void probe_cores(void) {
    Result r = APT_SetAppCpuTimeLimit(80);
    u32 limit = 0;
    APT_GetAppCpuTimeLimit(&limit);
    out("core1 limit  : set 80%% -> 0x%08lX, now %lu%%\n", (unsigned long)r, (unsigned long)limit);
    for (int core = 0; core <= 2; core++) {
        CoreJob job = {core, 0, -1};
        Thread t = threadCreate(core_job, &job, 16 * 1024, 0x30, core, false);
        if (t == NULL) {
            out("core %d       : threadCreate refused\n", core);
            continue;
        }
        threadJoin(t, U64_MAX);
        threadFree(t);
        out("core %d       : ran on %d, 10M LCG steps in %.1f ms\n", core, job.ran_on, job.ticks / TICKS_PER_US / 1000.0);
    }
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ttyd", 0777);
    s_log = fopen("sdmc:/3ds/ttyd/probe.txt", "w");

    out("TTYD 3DS hardware probe\n");
    probe_system();
    probe_ticks();
    void *qtm = probe_qtm();
    if (qtm != NULL) {
        probe_qtm_gpu(qtm);
    }
    probe_bandwidth(qtm);
    probe_cores();
    out("PROBE DONE\n");

    if (s_log != NULL) {
        fclose(s_log);
        s_log = NULL;
    }

    bool autoexit = debug_has("autoexit");
    if (!autoexit) {
        printf("\nPress START to exit.\n");
    }
    while (aptMainLoop()) {
        if (autoexit) {
            break;
        }
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
