/* THP decode timing on the 3DS. Writes sdmc:/3ds/ttyd/thp_bench.txt and ends with "BENCH DONE".
 * Compares: all on one core; the picture stage on a second core (a minimal worker standing in for the game's
 * plat_job queue, same submit/join_all shape). */
#include <3ds.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "frame.h"
#include "thp_decode.h"

#define TICKS_PER_MS (SYSCLOCK_ARM11 / 1000.0)
#define SLOTS 64

enum { FREE, QUEUED, RUNNING, DONE };
typedef struct {
    void (*fn)(void*);
    void* arg;
    _Atomic int state;
} Slot;
static Slot s_slots[SLOTS];
static unsigned s_next;
static volatile int s_quit;

static void run_slot(Slot* s) {
    s->fn(s->arg);
    atomic_store_explicit(&s->state, DONE, memory_order_release);
}

static void worker(void* arg) {
    (void)arg;
    while (!s_quit) {
        int ran = 0;
        for (unsigned i = 0; i < SLOTS; i++) {
            Slot* s = &s_slots[i];
            int exp = QUEUED;
            if (atomic_compare_exchange_strong(&s->state, &exp, RUNNING)) {
                run_slot(s);
                ran = 1;
            }
        }
        if (!ran) {
            svcSleepThread(20000);
        }
    }
}

static uint32_t submit(void (*fn)(void*), void* arg, void* user) {
    (void)user;
    unsigned idx = s_next++ % SLOTS;
    Slot* s = &s_slots[idx];
    s->fn = fn;
    s->arg = arg;
    atomic_store_explicit(&s->state, QUEUED, memory_order_release);
    return idx;
}

static void join_all(const uint32_t* h, uint32_t n, void* user) {
    (void)user;
    for (uint32_t k = n; k > 0; k--) { /* steal what the worker has not started, from the back */
        Slot* s = &s_slots[h[k - 1]];
        int exp = QUEUED;
        if (atomic_compare_exchange_strong(&s->state, &exp, RUNNING)) {
            run_slot(s);
        }
    }
    for (uint32_t k = 0; k < n; k++) {
        Slot* s = &s_slots[h[k]];
        while (atomic_load_explicit(&s->state, memory_order_acquire) != DONE) {
            svcSleepThread(10000);
        }
        atomic_store(&s->state, FREE);
    }
}

static uint32_t fnv(const uint8_t* p, size_t n, uint32_t h) {
    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

int main(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ttyd", 0777);
    FILE* log = fopen("sdmc:/3ds/ttyd/thp_bench.txt", "w");
    if (!log) {
        return 1;
    }
    unsigned w, h;
    if (thp_video_info(frame_data, &w, &h) != 0) {
        fprintf(log, "bad frame\n");
        fclose(log);
        return 1;
    }
    size_t ysz = (w + 7) / 8 * ((h + 3) / 4) * 32, csz = ((w + 1) / 2 + 7) / 8 * (((h + 1) / 2 + 3) / 4) * 32;
    uint8_t* ty = malloc(ysz);
    uint8_t* tu = malloc(csz);
    uint8_t* tv = malloc(csz);
    size_t need = thp_video_scratch_size(w, h);
    void* scratch = malloc(need);
    fprintf(log, "frame %ux%u, %u bytes, scratch %u KB, ARM11 %.0f MHz\n", w, h, (unsigned)sizeof(frame_data),
            (unsigned)(need / 1024), SYSCLOCK_ARM11 / 1e6);
    fflush(log);

    const int N = 60;
    uint64_t t0, worst;
    /* warm up */
    thp_video_decode(frame_data, ty, tu, tv);

    fprintf(log, "tile planes FNV-1a %08x (one core)\n",
            (unsigned)fnv(tv, csz, fnv(tu, csz, fnv(ty, ysz, 2166136261u))));
    t0 = svcGetSystemTick();
    worst = 0;
    for (int i = 0; i < N; i++) {
        uint64_t a = svcGetSystemTick();
        thp_video_decode(frame_data, ty, tu, tv);
        uint64_t d = svcGetSystemTick() - a;
        if (d > worst) {
            worst = d;
        }
    }
    double single = (svcGetSystemTick() - t0) / TICKS_PER_MS / N;
    fprintf(log, "one core:          %.2f ms/frame (worst %.2f ms)\n", single, worst / TICKS_PER_MS);
    fflush(log);

    s32 prio;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread th = threadCreate(worker, NULL, 32 * 1024, prio, 2, false);
    if (!th) {
        th = threadCreate(worker, NULL, 32 * 1024, prio, 1, false);
        fprintf(log, "no thread on core 2; worker on core 1\n");
    }
    ThpExec exec = {submit, join_all, NULL, 2};
    memset(ty, 0, ysz);
    memset(tu, 0, csz);
    memset(tv, 0, csz);
    thp_video_decode_ex(frame_data, ty, tu, tv, scratch, need, &exec);
    fprintf(log, "tile planes FNV-1a %08x (second core)\n",
            (unsigned)fnv(tv, csz, fnv(tu, csz, fnv(ty, ysz, 2166136261u))));
    t0 = svcGetSystemTick();
    worst = 0;
    for (int i = 0; i < N; i++) {
        uint64_t a = svcGetSystemTick();
        thp_video_decode_ex(frame_data, ty, tu, tv, scratch, need, &exec);
        uint64_t d = svcGetSystemTick() - a;
        if (d > worst) {
            worst = d;
        }
    }
    double two = (svcGetSystemTick() - t0) / TICKS_PER_MS / N;
    fprintf(log, "picture stage on a second core: %.2f ms/frame (worst %.2f ms), %.2fx\n", two, worst / TICKS_PER_MS,
            single / two);
    fprintf(log, "BENCH DONE\n");
    fclose(log);
    s_quit = 1;
    if (th) {
        threadJoin(th, U64_MAX);
    }
    return 0;
}
