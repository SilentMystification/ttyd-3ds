/* Dolphin AR/ARQ: the GameCube's 16 MiB auxiliary RAM as a plain buffer.
 *
 * ARAM addresses are offsets into the buffer (the first 16 KiB are the OS's
 * on the GameCube, so the user base is 0x4000 as there). A transfer is a
 * copy done at once; its completion callback runs afterwards on the DMA
 * thread as an interrupt (under the interrupt lock), in posting order, as
 * the GameCube's ARQ interrupt does. The game posts first and links the
 * request into its lists afterwards (arammgr.c), and spins in loops until
 * the callback sets a flag (PORT_SPIN), so the callback must not run inside
 * ARQPostRequest.
 */
#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <malloc.h>
#include <string.h>

#include "plat/memstable.h"
#include "plat/plat.h"
#include "plat/thread.h"
#include "port3ds/os_internal.h"

#define ARAM_SIZE (16u << 20)
#define ARAM_BASE 0x4000u
#define PENDING_MAX 256

static u8* s_aram;
static u32 s_next = ARAM_BASE;
static int s_ar_ready, s_arq_ready;

typedef struct {
    ARQRequest* request;
    ARQCallback callback;
    u64 due; /* plat_ticks() when the GameCube's DMA would finish */
} Pending;

/* The GameCube's ARAM DMA moves about 80 MB/s, so its completion interrupt
 * comes well after ARQPostRequest returns. Game code relies on that: it
 * links the request's owner into a list after posting (aramMgrToAramAsync),
 * and the callback looks it up there. The copy is done at once here, but the
 * callback waits for the DMA's time. */
#define DMA_BYTES_PER_SEC (80u << 20)
#define DMA_MIN_US 20u

static u64 dma_ticks(u32 length) {
    u64 us = (u64)length * 1000000u / DMA_BYTES_PER_SEC;
    if (us < DMA_MIN_US) {
        us = DMA_MIN_US;
    }
    return us * (PLAT_TICKS_PER_SEC / 1000000u);
}

static Pending s_pending[PENDING_MAX];
static volatile u32 s_head, s_tail;
static PlatMutex* s_lock;
static PlatEvent* s_wake;
static PlatThread* s_thread;

u8* port_aram_ptr(u32 addr) { return s_aram + addr; }

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    (void)stack_index_addr;
    (void)num_entries;
    if (!s_ar_ready) {
        s_aram = memalign(32, ARAM_SIZE);
        if (s_aram == NULL) {
            plat_fatal("ARInit: no memory for the 16 MiB ARAM");
        }
        memset(s_aram, 0, ARAM_SIZE);
        s_ar_ready = 1;
    }
    return ARAM_BASE;
}

BOOL ARCheckInit(void) { return s_ar_ready; }
u32 ARGetBaseAddress(void) { return ARAM_BASE; }
u32 ARGetSize(void) { return ARAM_SIZE; }

u32 ARAlloc(u32 length) {
    u32 addr = s_next;
    if (s_next + length > ARAM_SIZE) {
        plat_fatal("ARAlloc: %lu bytes do not fit in ARAM", (unsigned long)length);
    }
    s_next += length;
    return addr;
}

static void dma_thread(void* arg) {
    (void)arg;
    for (;;) {
        plat_event_wait(s_wake);
        for (;;) {
            plat_mutex_lock(s_lock);
            if (s_tail == s_head) {
                plat_mutex_unlock(s_lock);
                break;
            }
            Pending p = s_pending[s_tail % PENDING_MAX];
            s_tail++;
            plat_mutex_unlock(s_lock);
            u64 now = plat_ticks();
            if (now < p.due) {
                plat_thread_sleep_ns((int64_t)((p.due - now) * 1000000000ull / PLAT_TICKS_PER_SEC));
            }
            port_irq_enter();
            p.callback((u32)p.request);
            port_irq_leave();
            port_thread_dispatch_if_idle();
        }
    }
}

void ARQInit(void) {
    if (s_arq_ready) {
        return;
    }
    s_arq_ready = 1;
    s_lock = plat_mutex_create();
    s_wake = plat_event_create();
    s_thread = plat_thread_create(dma_thread, NULL, 16 * 1024, 0x28, 1, "arq");
    if (s_thread == NULL) {
        plat_fatal("ARQInit: no DMA thread");
    }
}

BOOL ARQCheckInit(void) { return s_arq_ready; }

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback) {
    request->next = NULL;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        if (dest + length > ARAM_SIZE) {
            plat_fatal("ARQ: write of %lu bytes at ARAM %08lx", (unsigned long)length, (unsigned long)dest);
        }
        memcpy(s_aram + dest, (const void*)source, length);
    } else {
        if (source + length > ARAM_SIZE) {
            plat_fatal("ARQ: read of %lu bytes at ARAM %08lx", (unsigned long)length, (unsigned long)source);
        }
        memcpy((void*)dest, s_aram + source, length);
        plat_memstable_fill((const void*)dest, length); /* DMA-filled memory (include/plat/memstable.h) */
    }
    if (callback == NULL) {
        return;
    }
    plat_mutex_lock(s_lock);
    if (s_head - s_tail >= PENDING_MAX) {
        plat_mutex_unlock(s_lock);
        plat_fatal("ARQ: %d transfers pending", PENDING_MAX);
    }
    /* in order behind the transfers already queued, as the one DMA channel runs them */
    u64 start = plat_ticks();
    if (s_head != s_tail && s_pending[(s_head - 1) % PENDING_MAX].due > start) {
        start = s_pending[(s_head - 1) % PENDING_MAX].due;
    }
    s_pending[s_head % PENDING_MAX] = (Pending){request, callback, start + dma_ticks(length)};
    s_head++;
    plat_mutex_unlock(s_lock);
    plat_event_signal(s_wake);
}

/* Interrupts that the game waits for in an empty loop (port_spin.h). */
void port_spin(void) {
    __asm__ volatile("" ::: "memory");
    plat_thread_yield();
}
