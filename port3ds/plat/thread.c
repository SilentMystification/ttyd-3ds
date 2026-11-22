/* Thread primitives over libctru (see include/plat/thread.h). */
#include <stdlib.h>
#include <string.h>

#include "plat/thread.h"
#include "plat_internal.h"

struct PlatThread {
    Thread thread;
    void (*entry)(void* arg);
    void* arg;
    char name[16];
};

struct PlatEvent {
    LightEvent ev;
};

struct PlatMutex {
    RecursiveLock lock;
};

/* Only for the refusal's Result (see plat_thread_create). */
static u8 probe_stack[512] __attribute__((aligned(8)));
static void probe_entry(void* arg) {
    (void)arg;
    svcExitThread();
}

static void thread_trampoline(void* p) {
    PlatThread* t = p;
    plat_crash_install_thread();
    t->entry(t->arg);
}

PlatThread* plat_thread_create(void (*entry)(void* arg), void* arg, uint32_t stack_bytes, int priority, int core,
                               const char* name) {
    PlatThread* t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->entry = entry;
    t->arg = arg;
    strncpy(t->name, name ? name : "?", sizeof(t->name) - 1);
    if (priority < 0x18) {
        priority = 0x18;
    } else if (priority > 0x3F) {
        priority = 0x3F;
    }
    t->thread = threadCreate(thread_trampoline, t, (stack_bytes + 7u) & ~7u, priority, core < 0 ? -2 : core, false);
    if (t->thread == NULL) {
        /* threadCreate does not return the Result: ask the kernel again with
         * a thread that exits at once, to get the code for the log. */
        Handle h = 0;
        Result pr = svcCreateThread(&h, probe_entry, 0, (u32*)(probe_stack + sizeof(probe_stack)), priority,
                                    core < 0 ? -2 : core);
        if (R_SUCCEEDED(pr)) {
            svcCloseHandle(h);
        }
        PLOG_E("thread %s: refused (core %d, priority %#x, result %08lx)", t->name, core, priority,
               (unsigned long)pr);
        free(t);
        return NULL;
    }
    PLOG_D("thread %s: core %d priority %#x stack %lu", t->name, core, priority, (unsigned long)stack_bytes);
    return t;
}

void plat_thread_join(PlatThread* t) {
    if (t == NULL) {
        return;
    }
    threadJoin(t->thread, U64_MAX);
    threadFree(t->thread);
    free(t);
}

void plat_thread_yield(void) { svcSleepThread(0); }
void plat_thread_sleep_ns(int64_t ns) { svcSleepThread(ns); }
int plat_thread_core(void) { return svcGetProcessorID(); }

PlatEvent* plat_event_create(void) {
    PlatEvent* e = calloc(1, sizeof(*e));
    if (e != NULL) {
        LightEvent_Init(&e->ev, RESET_ONESHOT);
    }
    return e;
}

void plat_event_signal(PlatEvent* e) { LightEvent_Signal(&e->ev); }
void plat_event_wait(PlatEvent* e) { LightEvent_Wait(&e->ev); }
int plat_event_wait_timeout(PlatEvent* e, int64_t ns) { return LightEvent_WaitTimeout(&e->ev, ns) == 0; }

PlatMutex* plat_mutex_create(void) {
    PlatMutex* m = calloc(1, sizeof(*m));
    if (m != NULL) {
        RecursiveLock_Init(&m->lock);
    }
    return m;
}

void plat_mutex_lock(PlatMutex* m) { RecursiveLock_Lock(&m->lock); }
int plat_mutex_trylock(PlatMutex* m) { return RecursiveLock_TryLock(&m->lock) == 0; }
void plat_mutex_unlock(PlatMutex* m) { RecursiveLock_Unlock(&m->lock); }
