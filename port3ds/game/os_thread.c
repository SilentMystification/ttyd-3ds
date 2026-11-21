/* Dolphin OS threads on the 3DS: the GameCube's single-core scheduler,
 * reproduced with one host thread per OSThread and a baton.
 *
 * The game relies on GameCube scheduling: one CPU, threads switch only when
 * the running thread yields (OSYieldThread), sleeps (OSSleepThread, a wait
 * for the retrace or a DVD read) or exits, or when an interrupt wakes a
 * thread of higher priority. TTYD's threads (the main thread, DvdCheckThread
 * and the DVD manager, all priority 16) yield to each other while the main
 * thread waits for the retrace.
 *
 * Each OSThread gets its own host thread on core 0. Only the holder of the
 * baton runs; the others wait on their own event. A switch signals the next
 * thread's event and waits on our own. Code standing in for interrupts (on
 * other host threads) may wake threads; when no game thread holds the baton
 * it hands the baton over itself (port_thread_dispatch_if_idle).
 *
 * A higher-priority thread woken by an interrupt takes over at the running
 * thread's next scheduler call rather than at once; TTYD runs every thread
 * at the same priority, where the GameCube does not preempt either.
 */
#include <dolphin/os.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "plat/plat.h"
#include "plat/thread.h"
#include "port3ds/os_internal.h"

#define MAX_THREADS 8
#define HOST_STACK_BYTES (64 * 1024)
#define HOST_PRIORITY 0x30
#define GAME_CORE 0

typedef struct ThreadInfo {
    OSThread* os;
    PlatThread* host;
    PlatEvent* baton;
    void* (*func)(void*);
    void* param;
    int started;
    int cancelled;
    jmp_buf exit_jmp;
} ThreadInfo;

static ThreadInfo s_info[MAX_THREADS];
static OSThread s_main_thread;
static PlatMutex* s_lock;          /* scheduler state */
static OSThread* s_running;        /* baton holder, NULL when idle */
static OSThreadQueue s_ready[32];  /* by priority, 0 highest */
static __thread OSThread* s_self;  /* the OSThread of this host thread */
static unsigned s_switches;

unsigned port_thread_switch_count(void) { return s_switches; }

static ThreadInfo* info_of(OSThread* t) { return (ThreadInfo*)t->context.gpr[0]; }

static void queue_push(OSThreadQueue* q, OSThread* t) {
    t->link.next = NULL;
    t->link.prev = q->tail;
    if (q->tail) {
        q->tail->link.next = t;
    } else {
        q->head = t;
    }
    q->tail = t;
}

static void queue_remove(OSThreadQueue* q, OSThread* t) {
    if (t->link.prev) {
        t->link.prev->link.next = t->link.next;
    } else if (q->head == t) {
        q->head = t->link.next;
    }
    if (t->link.next) {
        t->link.next->link.prev = t->link.prev;
    } else if (q->tail == t) {
        q->tail = t->link.prev;
    }
    t->link.next = t->link.prev = NULL;
}

static OSThread* queue_pop(OSThreadQueue* q) {
    OSThread* t = q->head;
    if (t != NULL) {
        queue_remove(q, t);
    }
    return t;
}

static void make_ready(OSThread* t) {
    t->state = OS_THREAD_STATE_READY;
    t->queue = &s_ready[t->priority];
    queue_push(&s_ready[t->priority], t);
}

/* Best ready thread with priority <= limit (numerically), removed from its
 * queue; NULL if none. Caller holds s_lock. */
static OSThread* take_ready(int limit) {
    for (int p = 0; p <= limit && p < 32; p++) {
        if (s_ready[p].head != NULL) {
            OSThread* t = queue_pop(&s_ready[p]);
            t->queue = NULL;
            return t;
        }
    }
    return NULL;
}

/* Hands the baton to `next` (or leaves the CPU idle when NULL) and waits
 * until this thread gets it back. Called with s_lock held; returns with it
 * released. */
static void switch_away(OSThread* self, OSThread* next) {
    ThreadInfo* me = info_of(self);
    int irq = port_irq_release_all();
    s_running = next;
    if (next != NULL) {
        next->state = OS_THREAD_STATE_RUNNING;
        s_switches++;
        plat_event_signal(info_of(next)->baton);
    }
    plat_mutex_unlock(s_lock);
    plat_event_wait(me->baton);
    if (me->cancelled) {
        longjmp(me->exit_jmp, 1);
    }
    port_irq_reacquire(irq);
}

static void host_entry(void* arg) {
    ThreadInfo* ti = arg;
    s_self = ti->os;
    plat_event_wait(ti->baton); /* first run */
    if (!ti->cancelled && setjmp(ti->exit_jmp) == 0) {
        void* ret = ti->func(ti->param);
        ti->os->val = ret;
    }
    /* Thread end: pass the baton on for good (a thread cancelled while
     * another one ran does not hold it). */
    plat_mutex_lock(s_lock);
    ti->os->state = OS_THREAD_STATE_MORIBUND;
    if (s_running == ti->os) {
        OSThread* next = take_ready(31);
        s_running = next;
        if (next != NULL) {
            next->state = OS_THREAD_STATE_RUNNING;
            s_switches++;
            plat_event_signal(info_of(next)->baton);
        }
    }
    plat_mutex_unlock(s_lock);
}

void port_thread_init(void) {
    s_lock = plat_mutex_create();
    memset(&s_main_thread, 0, sizeof(s_main_thread));
    ThreadInfo* ti = &s_info[0];
    ti->os = &s_main_thread;
    ti->baton = plat_event_create();
    ti->started = 1;
    s_main_thread.context.gpr[0] = (u32)ti;
    s_main_thread.state = OS_THREAD_STATE_RUNNING;
    s_main_thread.priority = s_main_thread.base = 16;
    s_self = &s_main_thread;
    s_running = &s_main_thread;
}

OSThread* OSGetCurrentThread(void) { return s_self != NULL ? s_self : s_running; }

int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize,
                   OSPriority priority, u16 attr) {
    (void)stack;
    (void)stackSize;
    if (priority < 0 || priority > 31) {
        return FALSE;
    }
    ThreadInfo* ti = NULL;
    for (int i = 1; i < MAX_THREADS; i++) {
        if (s_info[i].os == NULL || s_info[i].os->state == OS_THREAD_STATE_MORIBUND) {
            ti = &s_info[i];
            break;
        }
    }
    if (ti == NULL) {
        plat_fatal("OSCreateThread: more than %d threads", MAX_THREADS);
    }
    if (ti->host != NULL) {
        plat_thread_join(ti->host);
    }
    memset(thread, 0, sizeof(*thread));
    memset(ti, 0, sizeof(*ti));
    ti->os = thread;
    ti->func = func;
    ti->param = param;
    ti->baton = plat_event_create();
    thread->context.gpr[0] = (u32)ti;
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->suspend = 1;
    thread->priority = thread->base = priority;
    ti->host = plat_thread_create(host_entry, ti, HOST_STACK_BYTES, HOST_PRIORITY, GAME_CORE, "game");
    if (ti->host == NULL) {
        plat_fatal("OSCreateThread: no host thread");
    }
    PLOG_D("OSCreateThread %p priority %ld", (void*)thread, (long)priority);
    return TRUE;
}

s32 OSResumeThread(OSThread* thread) {
    plat_mutex_lock(s_lock);
    s32 prev = thread->suspend;
    if (thread->suspend > 0 && --thread->suspend == 0 && thread->state == OS_THREAD_STATE_READY) {
        make_ready(thread);
        OSThread* self = OSGetCurrentThread();
        if (s_self != NULL && thread->priority < self->priority) {
            /* A more important thread runs at once, as on the GameCube. */
            OSThread* next = take_ready(thread->priority);
            make_ready(self);
            switch_away(self, next);
            return prev;
        }
        if (s_running == NULL) {
            OSThread* next = take_ready(31);
            s_running = next;
            next->state = OS_THREAD_STATE_RUNNING;
            plat_event_signal(info_of(next)->baton);
        }
    }
    plat_mutex_unlock(s_lock);
    return prev;
}

s32 OSSuspendThread(OSThread* thread) {
    plat_mutex_lock(s_lock);
    s32 prev = thread->suspend++;
    if (prev == 0 && thread->queue == &s_ready[thread->priority]) {
        queue_remove(thread->queue, thread);
        thread->queue = NULL;
    }
    plat_mutex_unlock(s_lock);
    if (prev == 0 && thread == s_self) {
        plat_mutex_lock(s_lock);
        switch_away(thread, take_ready(31));
    }
    return prev;
}

void OSYieldThread(void) {
    OSThread* self = s_self;
    if (self == NULL) {
        return;
    }
    plat_mutex_lock(s_lock);
    OSThread* next = take_ready(self->priority);
    if (next == NULL) {
        plat_mutex_unlock(s_lock);
        return;
    }
    make_ready(self);
    switch_away(self, next);
}

void OSInitThreadQueue(OSThreadQueue* queue) { queue->head = queue->tail = NULL; }

void OSSleepThread(OSThreadQueue* queue) {
    OSThread* self = s_self;
    if (self == NULL) {
        plat_fatal("OSSleepThread outside a game thread");
    }
    plat_mutex_lock(s_lock);
    self->state = OS_THREAD_STATE_WAITING;
    self->queue = queue;
    queue_push(queue, self);
    switch_away(self, take_ready(31));
}

void OSWakeupThread(OSThreadQueue* queue) {
    plat_mutex_lock(s_lock);
    OSThread* t;
    int best = 32;
    while ((t = queue_pop(queue)) != NULL) {
        make_ready(t);
        if (t->priority < best) {
            best = t->priority;
        }
    }
    OSThread* self = s_self;
    if (self != NULL && s_running == self && best < self->priority) {
        OSThread* next = take_ready(best);
        make_ready(self);
        switch_away(self, next);
        return;
    }
    if (s_running == NULL) {
        OSThread* next = take_ready(31);
        if (next != NULL) {
            s_running = next;
            next->state = OS_THREAD_STATE_RUNNING;
            s_switches++;
            plat_event_signal(info_of(next)->baton);
        }
    }
    plat_mutex_unlock(s_lock);
}

void port_thread_dispatch_if_idle(void) {
    plat_mutex_lock(s_lock);
    if (s_running == NULL) {
        OSThread* next = take_ready(31);
        if (next != NULL) {
            s_running = next;
            next->state = OS_THREAD_STATE_RUNNING;
            s_switches++;
            plat_event_signal(info_of(next)->baton);
        }
    }
    plat_mutex_unlock(s_lock);
}

void OSCancelThread(OSThread* thread) {
    ThreadInfo* ti = info_of(thread);
    plat_mutex_lock(s_lock);
    if (thread->queue != NULL) {
        queue_remove(thread->queue, thread);
        thread->queue = NULL;
    }
    ti->cancelled = 1;
    thread->state = OS_THREAD_STATE_MORIBUND;
    if (thread == s_self) {
        plat_mutex_unlock(s_lock);
        longjmp(ti->exit_jmp, 1);
    }
    plat_mutex_unlock(s_lock);
    /* Wake its host thread so it leaves through its exit path. */
    plat_event_signal(ti->baton);
}

void OSExitThread(void* val) {
    ThreadInfo* ti = info_of(s_self);
    s_self->val = val;
    longjmp(ti->exit_jmp, 1);
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return thread->state == OS_THREAD_STATE_MORIBUND || thread->state == 0;
}

s32 OSGetThreadPriority(OSThread* thread) { return thread->priority; }
