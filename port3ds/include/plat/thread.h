/* Thread primitives for the game side (implemented in plat/thread.c over
 * libctru). Handles are opaque pointers; no libctru types cross this line. */
#ifndef PLAT_THREAD_H
#define PLAT_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlatThread PlatThread;
typedef struct PlatEvent PlatEvent;   /* one-shot event: a signal wakes one waiter */
typedef struct PlatMutex PlatMutex;   /* recursive mutex */

/* priority: 0x18 (highest a game thread uses) .. 0x3F (lowest).
 * core: 0, 1, 2 (New 3DS) or -1 for any. stack_bytes is rounded up to 8.
 * The thread installs the crash handler before calling entry.
 * Returns NULL when the system refuses the thread. */
PlatThread* plat_thread_create(void (*entry)(void* arg), void* arg, uint32_t stack_bytes, int priority, int core,
                               const char* name);
void plat_thread_join(PlatThread* t);
void plat_thread_yield(void);
void plat_thread_sleep_ns(int64_t ns);
int plat_thread_core(void); /* the core the caller runs on (unreliable in Azahar) */

PlatEvent* plat_event_create(void);
void plat_event_signal(PlatEvent* e);
void plat_event_wait(PlatEvent* e);
int plat_event_wait_timeout(PlatEvent* e, int64_t ns); /* 1 signalled, 0 timed out */

PlatMutex* plat_mutex_create(void);
void plat_mutex_lock(PlatMutex* m);
int plat_mutex_trylock(PlatMutex* m); /* 1 when taken */
void plat_mutex_unlock(PlatMutex* m);

#ifdef __cplusplus
}
#endif

#endif
