/* Dolphin OS for the 3DS port: arena, time, interrupts, system state.
 *
 * Game-side file (int-sized enums, SDK headers). Threads are in os_thread.c,
 * module linking in os_link.c, the ROM font in os_font.c. The heap allocator
 * itself is the decomp's own libs/dolsdk2004/src/os/OSAlloc.c, compiled
 * unchanged, so heap layout and fragmentation behave as on the GameCube.
 */
#include <dolphin/os.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "plat/plat.h"
#include "plat/thread.h"
#include "port3ds/os_internal.h"
#include "port3ds/rel_static.h"

/* The arena the game sees at main() on the GameCube (G8ME01, measured in
 * Dolphin: 0x80439920..0x817EE1A0). The game's own DEMOInit and memInit
 * carve it, so every heap has its GameCube size. */
#define GAME_ARENA_BYTES 0x013B4880u

static void* s_arena_lo;
static void* s_arena_hi;
static u8* s_arena_block;
static int s_os_ready;

/* Paired-single quantization registers (port_fastcast.h). */
unsigned int port_gqr[8];

/* ---- Arena ---- */

void* OSGetArenaLo(void) { return s_arena_lo; }
void* OSGetArenaHi(void) { return s_arena_hi; }
void OSSetArenaLo(void* lo) { s_arena_lo = lo; }
void OSSetArenaHi(void* hi) { s_arena_hi = hi; }

void* OSAllocFromArenaLo(u32 size, u32 align) {
    u8* p = (u8*)(((u32)s_arena_lo + align - 1) & ~(align - 1));
    u8* end = (u8*)(((u32)p + size + align - 1) & ~(align - 1));
    if (end > (u8*)s_arena_hi) {
        plat_fatal("OSAllocFromArenaLo: %lu bytes do not fit", (unsigned long)size);
    }
    s_arena_lo = end;
    return p;
}

void* OSAllocFromArenaHi(u32 size, u32 align) {
    u8* hi = (u8*)((u32)s_arena_hi & ~(align - 1));
    u8* p = (u8*)(((u32)hi - size) & ~(align - 1));
    if (p < (u8*)s_arena_lo) {
        plat_fatal("OSAllocFromArenaHi: %lu bytes do not fit", (unsigned long)size);
    }
    s_arena_hi = p;
    return p;
}

/* ---- Time ----
 * The GameCube time base runs at OS_TIMER_CLOCK (40.5 MHz) and counts from
 * 2000-01-01 00:00. OSGetTime is the 3DS's RTC at boot (ms since 1900)
 * plus the system tick since then, converted to 40.5 MHz without overflow. */
#define GC_TIMER_HZ (OS_TIMER_CLOCK)
#define MS_1900_TO_2000 3155673600000ull /* 36524 days */

static u64 s_boot_ticks;
static s64 s_time_base;

static void time_init(void) {
    s_boot_ticks = plat_ticks();
    u64 ms = plat_rtc_ms_since_1900();
    s_time_base = ms >= MS_1900_TO_2000 ? (s64)((ms - MS_1900_TO_2000) * (GC_TIMER_HZ / 1000u)) : 0;
}

OSTime port_os_real_time(void) {
    u64 d = plat_ticks() - s_boot_ticks;
    u64 whole = d / PLAT_TICKS_PER_SEC;
    u64 part = d % PLAT_TICKS_PER_SEC;
    return s_time_base + (s64)(whole * GC_TIMER_HZ + part * GC_TIMER_HZ / PLAT_TICKS_PER_SEC);
}

/* A test mode can replace the game's clock (port_os_set_time_source). */
static OSTime (*volatile s_time_source)(void);

void port_os_set_time_source(OSTime (*source)(void)) { s_time_source = source; }

OSTime OSGetTime(void) {
    OSTime (*source)(void) = s_time_source;
    return source != NULL ? source() : port_os_real_time();
}

OSTick OSGetTick(void) { return (OSTick)OSGetTime(); }

OSTime __OSGetSystemTime(void) { return OSGetTime(); }

/* Same algorithm as the SDK's OSTicksToCalendarTime (OSTime.c). */
static const int s_year_days[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
static const int s_leap_year_days[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_before_year(int year) {
    int y = year - 1;
    return year > 0 ? year * 365 + y / 4 - y / 100 + y / 400 + 1 : 0;
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    int days, secs;
    OSTime d = ticks % OSSecondsToTicks(1);
    if (d < 0) {
        d += OSSecondsToTicks(1);
    }
    td->usec = (int)(OSTicksToMicroseconds(d) % 1000);
    td->msec = (int)(OSTicksToMilliseconds(d) % 1000);
    ticks -= d;
    days = (int)(OSTicksToSeconds(ticks) / 86400 + 730119); /* days from 0000-01-01 to 2000-01-01 */
    secs = (int)(OSTicksToSeconds(ticks) % 86400);
    if (secs < 0) {
        days -= 1;
        secs += 86400;
    }
    td->wday = (days + 6) % 7;
    int year = days / 365;
    while (days < days_before_year(year)) {
        year--;
    }
    days -= days_before_year(year);
    td->year = year;
    td->yday = days;
    const int* table = is_leap(year) ? s_leap_year_days : s_year_days;
    int month = 12;
    while (days < table[--month]) {
    }
    td->mon = month;
    td->mday = days - table[month] + 1;
    td->hour = secs / 3600;
    td->min = (secs / 60) % 60;
    td->sec = secs % 60;
}

/* ---- Interrupts ----
 * On the GameCube, OSDisableInterrupts keeps interrupt handlers (audio DMA,
 * DVD, VI) away from the code that follows. On the port those handlers run
 * on other host threads (audio, file, display), and they take one lock
 * before they run (port_irq_enter). "Interrupts disabled" on a host thread
 * means that thread holds the lock.
 *
 * As MSR[EE] on the GameCube, the state is a flag, not a count: disabling
 * twice and restoring the first saved state (FALSE) leaves interrupts
 * disabled, and one OSEnableInterrupts or OSRestoreInterrupts(TRUE)
 * enables them. A handler's port_irq_leave puts back the state its
 * port_irq_enter found (as rfi restores MSR). */
static PlatMutex* s_irq_lock;
static __thread int s_irq_off;
static __thread unsigned s_irq_nest;
static __thread u32 s_irq_saved; /* bit n: interrupts were off at port_irq_enter level n */

static void irq_off(void) {
    if (!s_irq_off && s_irq_lock != NULL) {
        plat_mutex_lock(s_irq_lock);
        s_irq_off = 1;
    }
}

static void irq_on(void) {
    if (s_irq_off) {
        s_irq_off = 0;
        plat_mutex_unlock(s_irq_lock);
    }
}

BOOL OSDisableInterrupts(void) {
    BOOL was_enabled = !s_irq_off;
    irq_off();
    return was_enabled;
}

BOOL OSEnableInterrupts(void) {
    BOOL was_enabled = !s_irq_off;
    irq_on();
    return was_enabled;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL was_enabled = !s_irq_off;
    if (level) {
        irq_on();
    } else {
        irq_off();
    }
    return was_enabled;
}

void port_irq_enter(void) {
    if (s_irq_nest >= 32) {
        plat_fatal("interrupts: handlers nested %u deep", s_irq_nest);
    }
    if (s_irq_off) {
        s_irq_saved |= 1u << s_irq_nest;
    } else {
        s_irq_saved &= ~(1u << s_irq_nest);
    }
    s_irq_nest++;
    irq_off();
}

void port_irq_leave(void) {
    if (s_irq_nest == 0) {
        plat_fatal("interrupts: port_irq_leave without port_irq_enter");
    }
    s_irq_nest--;
    if (s_irq_saved & (1u << s_irq_nest)) {
        irq_off();
    } else {
        irq_on();
    }
}

/* 1 while this host thread has interrupts disabled. */
int port_irq_depth(void) { return s_irq_off; }

/* Releases the interrupt lock around a blocking wait (the SDK's
 * OSSleepThread re-enables interrupts while the thread sleeps); returns
 * the state to give port_irq_reacquire. */
int port_irq_release_all(void) {
    int was_off = s_irq_off;
    irq_on();
    return was_off;
}

void port_irq_reacquire(int was_off) {
    if (was_off) {
        irq_off();
    }
}

/* ---- Reports and panics ---- */

void OSReport(const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);
    plat_vlog(PLAT_LOG_INFO, msg, ap);
    va_end(ap);
}

void OSVReport(const char* msg, va_list list) { plat_vlog(PLAT_LOG_INFO, msg, list); }

void OSPanic(const char* file, int line, const char* msg, ...) {
    char text[160];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(text, sizeof(text), msg, ap);
    va_end(ap);
    plat_fatal("OSPanic %s:%d: %s", file, line, text);
}

/* ---- Error handlers ----
 * The game registers handlers for DSI, ISI and program errors
 * (error_handler.c), which draw the error screen. On the port a CPU
 * exception goes to the platform crash handler, which writes the report
 * and exits to the HOME Menu, so the table is only kept. */
OSErrorHandler __OSErrorTable[17];

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    if ((u32)error >= 17) {
        return NULL;
    }
    OSErrorHandler old = __OSErrorTable[error];
    __OSErrorTable[error] = handler;
    return old;
}

void OSFillFPUContext(OSContext* context) {
    /* Floating-point registers of a crashed context: the port's crash report
     * has the ARM registers; the game's error screen never runs. */
    memset(context->fpr, 0, sizeof(context->fpr));
}

/* ---- System state ---- */

static u32 s_sound_mode = OS_SOUND_MODE_STEREO;
static u32 s_progressive;
static u32 s_reset_code;

u32 OSGetSoundMode(void) { return s_sound_mode; }
void OSSetSoundMode(u32 mode) { s_sound_mode = mode; }
/* The 3DS has no component video: the game's progressive-scan prompt never
 * shows, as on a console without the cable. */
u32 OSGetProgressiveMode(void) { return s_progressive; }
void OSSetProgressiveMode(u32 on) { s_progressive = on; }
u32 OSGetResetCode(void) { return s_reset_code; }
/* The reset button is the Select menu's "Return to title" on the port. */
BOOL OSGetResetButtonState(void) { return port_reset_button_state(); }

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)reset;
    (void)forceMenu;
    s_reset_code = resetCode;
    /* A GameCube reset restarts the program. The port does a clean exit
     * with a relaunch request (plat_quit), so GSP, the DSP and the audio
     * thread are released; the HOME Menu or Homebrew Launcher then takes over. */
    plat_log(PLAT_LOG_WARN, "OSResetSystem(reset %d, code %08lx, menu %d)", reset, (unsigned long)resetCode,
             forceMenu);
    plat_quit(1);
}

void PPCHalt(void) {
    plat_fatal("PPCHalt");
}

u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL1; }

/* ---- Init ---- */

void OSInit(void) {
    if (s_os_ready) {
        return;
    }
    s_os_ready = 1;
    time_init();
    s_irq_lock = plat_mutex_create();
    s_arena_block = memalign(32, GAME_ARENA_BYTES);
    if (s_arena_block == NULL) {
        plat_fatal("OSInit: no memory for the %lu-byte game arena", (unsigned long)GAME_ARENA_BYTES);
    }
    s_arena_lo = s_arena_block;
    s_arena_hi = s_arena_block + GAME_ARENA_BYTES;
    port_thread_init();
    port_rel_snapshot_all();
    PLOG_I("OSInit: arena %p..%p (%lu bytes)", s_arena_lo, s_arena_hi, (unsigned long)GAME_ARENA_BYTES);
}
