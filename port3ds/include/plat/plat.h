/* Platform interface: what the game side of the port (port3ds/game, built
 * with the game's int-sized enums) may call in the platform side
 * (port3ds/plat, built with libctru's short enums).
 *
 * Rule for every header in include/plat/: only fixed-size integers,
 * pointers, and structs of those. No enum-typed struct fields and no libctru
 * or game types, so both enum ABIs see the same layout. */
#ifndef PLAT_PLAT_H
#define PLAT_PLAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Logging (plat/log.c) ----
 * Lines go to sdmc:/3ds/ttyd/latest.log, the live log (TCP 17492, with switch
 * livelog or when netloaded) and the bottom screen console. Producers only
 * format into a lock-free queue; a writer thread on core 1 does the I/O.
 * Every line starts with #<sequence>, so a gap shows dropped lines. */
enum { PLAT_LOG_ERROR = 0, PLAT_LOG_WARN = 1, PLAT_LOG_INFO = 2, PLAT_LOG_DEBUG = 3 };
void plat_log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void plat_vlog(int level, const char* fmt, va_list ap);
/* The TTYD_RETAIL build (CMake -DTTYD_RETAIL=ON, build.sh --retail) compiles every PLOG_* out: the arguments are
 * type-checked and never evaluated. Other builds call plat_log, which returns at its first compare when the
 * level is off or retail mode is on. */
#ifdef TTYD_RETAIL
#define PLAT_RETAIL_BUILD 1
#define PLAT_LOG_AT(level, ...) (0 ? plat_log(level, __VA_ARGS__) : (void)0)
#else
#define PLAT_RETAIL_BUILD 0
#define PLAT_LOG_AT(level, ...) plat_log(level, __VA_ARGS__)
#endif
#define PLOG_E(...) PLAT_LOG_AT(PLAT_LOG_ERROR, __VA_ARGS__)
#define PLOG_W(...) PLAT_LOG_AT(PLAT_LOG_WARN, __VA_ARGS__)
#define PLOG_I(...) PLAT_LOG_AT(PLAT_LOG_INFO, __VA_ARGS__)
#define PLOG_D(...) PLAT_LOG_AT(PLAT_LOG_DEBUG, __VA_ARGS__)
int plat_log_level(void);
/* One line that is written in every mode and build (the bench switch): through the log when it runs, else
 * appended to latest.log by the SD worker. */
void plat_log_always(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- Retail mode (plat/switches.c; docs/PORT_3DS_TESTING.md "Retail mode") ----
 * All debugging off: no log, no stats, counters, verify modes, dumps or test hooks, no update server or
 * supervisor heartbeat (unless the devupdate switch or setting asks), the HUD at most fps. Decided at boot:
 * switch retail=1/retail=0, else the Select menu setting, else off; always on in a TTYD_RETAIL build. */
extern int plat_retail_on;
static inline int plat_retail(void) { return PLAT_RETAIL_BUILD || plat_retail_on; }
int plat_retail_booted(void);  /* the mode this launch booted in */
int plat_retail_setting(void); /* the stored setting: what the next launch does */
void plat_retail_set(int on);  /* the Select menu; see switches.c for what changes at once */

/* ---- Debug switches (plat/switches.c) ----
 * Words from sdmc:/3ds/ttyd/debug3ds.txt and the 3dslink arguments, for
 * example "map=gor_01 shots wdog=20". plat_switch_has("shots") is true for a
 * whole word; plat_switch_value("map", buf, n) copies the text after "map=". */
int plat_switch_has(const char* word);
int plat_switch_value(const char* key, char* out, size_t n);
int plat_switch_int(const char* key, int fallback);

/* ---- Time (plat/boot.c) ---- */
#define PLAT_TICKS_PER_SEC 268111856u /* ARM11 system tick */
uint64_t plat_ticks(void);
/* System ticks the game thread has spent in intentional pauses (menus, an update transfer) since boot:
 * a frame's timing subtracts the change over the frame. */
uint64_t plat_paused_ticks(void);
uint32_t plat_uptime_ms(void);
uint64_t plat_rtc_ms_since_1900(void); /* the console's clock */

/* ---- Heap use without a walk (plat/boot.c) ----
 * arena: the heap memory malloc holds now (in use, free holes, at most 128 KiB of free top). peak_take: the
 * highest arena since the last call, then a new period starts. note: one log line for a large allocation. */
uint32_t plat_heap_size(void);
uint32_t plat_heap_arena(void);
uint32_t plat_heap_peak_take(void);
void plat_heap_note(const char* what, unsigned long bytes);

/* ---- Crash handling and exit (plat/crash.c) ----
 * Every thread the port creates installs the exception handler with
 * plat_crash_install_thread(). A crash, plat_fatal() or a watchdog timeout
 * writes [CRASH] lines (registers, stack scan, last phase, current map,
 * build) to latest.log, crash.txt and the live log, then ends the process,
 * which returns to the HOME Menu or the Homebrew Launcher. */
void plat_crash_install_thread(void);
void plat_fatal(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void plat_exit_process(void) __attribute__((noreturn));
void plat_crash_line(const char* s, int n);
/* A problem the user can fix (no disc image, wrong game, no DSP firmware):
 * shown on screen with the message until START, then the process ends.
 * Also written to the log. */
void plat_user_error(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
/* Context shown in crash reports: a short name of what the game loop is
 * doing and the current map (set by the game side). */
void plat_set_phase(const char* phase);
void plat_set_map_name(const char* map);
/* Watchdog: the game loop calls this once per frame. No call for wdog=N
 * seconds (default 10) while the app is in the foreground is a hang. */
void plat_wd_progress(void);
/* Holds the watchdog while a long legitimate wait runs (saving, loading). */
void plat_wd_hold(int on);
/* The card layer registers whether a card operation is queued or running,
 * so a devupdate relaunch waits for it (plat/devupdate.c). */
void plat_set_save_busy(int (*busy)(void));

/* ---- Frame and exit requests (plat/boot.c) ----
 * Called once per frame by the game side. Runs the APT loop (HOME menu,
 * sleep, power). Returns 0 when the system asked the app to close; the
 * caller then calls plat_shutdown(). */
int plat_frame_poll(void);
void plat_shutdown(void) __attribute__((noreturn));
/* Clean exit for the game side (reset, Select menu exit): runs exit(), so the
 * atexit handlers and APT/GSP teardown run. relaunch: ask APT to start the
 * app again. */
void plat_quit(int relaunch) __attribute__((noreturn));

/* ---- Vertical blank (plat/vblank.c) ----
 * Calls `handler` on a dedicated thread at every top-screen vertical blank
 * (59.83 Hz), standing in for the GameCube's retrace interrupt. */
void plat_vblank_start(void (*handler)(void));
uint32_t plat_vblank_count(void);

/* ---- Select menu (plat/menu.c) ---- */
/* The menu's "Return to title" holds the GameCube reset button until the
 * game has seen it (OSGetResetButtonState). */
int plat_reset_requested(void);

/* ---- System information ---- */
int plat_is_new3ds(void);
const char* plat_build_string(void); /* "#<commits> <hash>" */

/* ---- Memory placement (plat/memplace.c) ----
 * A placement is a named buffer region with a size, an ordered list of memory
 * tiers it may live in, and flags saying who touches it. The platform packs
 * all registrations by priority into the tiers at boot (plat_memplace_init),
 * falls down each placement's tier list, logs the result ("[MEM]") and shows
 * the QTM state on the HUD. See docs/PORT_3DS_PLAN.md section 5.7.
 *
 * How a game registers its own placements (the portable path): the game
 * supplies a manifest function and hands it over before boot packs:
 *
 *     static void game_manifest(void) {
 *         int h = plat_place_register("hitdrv", 256u << 10, 64,
 *                                     PLAT_PREFS(PLAT_TIER_QTM, PLAT_TIER_HEAP), PLAT_PF_CPU_SHARED);
 *         plat_place_priority(h, 10);  // lower packs first; default is registration order
 *     }
 *     plat_place_set_manifest(game_manifest);   // before plat_memplace_init
 *
 * and later takes memory with plat_place_alloc(h, bytes, align) (a bump
 * allocation inside the registered region; NULL when it does not fit or the
 * handle is unknown). Names may contain spaces; in the "place=" switch write
 * them as '_'.
 *
 * Tiers: QTM (2 MiB application part of the New 3DS extra RAM), LINEAR
 * (linear heap: CPU, GPU and DSP reach it), HEAP (ordinary cached FCRAM, CPU
 * only). VRAM is not a tier: the CPU cannot map it, it needs transfers.
 * Rules: a GPU_READ/GPU_WRITE placement is not put in QTM unless the switch
 * "qtmgpu" says the GPU check passed (unverified on hardware); a DSP
 * placement is never put in QTM. The switch
 * "place=<name>:<tier>[,<name>:<tier>...]" (tier qtm, linear or heap) forces a
 * placement's tier for A/B runs; the rules still apply. A bad name or tier is
 * logged and ignored. */
enum {
    PLAT_TIER_QTM = 0,
    PLAT_TIER_LINEAR,
    PLAT_TIER_HEAP,
    PLAT_TIER_COUNT,
    PLAT_TIER_NONE = 0xFF /* ends a tier list */
};
enum {
    PLAT_PF_CPU_SHARED = 1, /* several cores touch it every frame */
    PLAT_PF_GPU_READ = 2,
    PLAT_PF_GPU_WRITE = 4,
    PLAT_PF_DSP = 8
};
/* PLAT_PREFS(PLAT_TIER_QTM, PLAT_TIER_HEAP): an ordered tier list, best first. */
#define PLAT_PREFS(...) ((const uint8_t[]){__VA_ARGS__, PLAT_TIER_NONE})
/* Returns a handle (>= 0), or -1 if the table is full or the arguments are bad.
 * Before boot packs this only records the request; afterwards it places at once. */
int plat_place_register(const char* name, size_t bytes, size_t align, const uint8_t* prefs, unsigned flags);
void plat_place_priority(int handle, int priority); /* before packing; lower packs first */
void plat_place_set_manifest(void (*fn)(void));     /* called by plat_memplace_init before packing */
void* plat_place_alloc(int handle, size_t size, size_t align);
/* Compatibility ids: the platform's own placements, registered at init in
 * this order, so an id is also its handle. */
enum {
    PLAT_PLACE_GX_RECORD = 0,  /* GX record buffers, core 0 writes, core 2 reads */
    PLAT_PLACE_GX_STATE,       /* GX state tracker and caches */
    PLAT_PLACE_AUDIO_STATE,    /* MusyX voice state and aux working buffers */
    PLAT_PLACE_LOG,            /* log queue and profiler counters */
    PLAT_PLACE_GX_INDEX,       /* index arenas of the frame records: CPU writes, GPU reads (linear by default) */
    PLAT_PLACE_COUNT
};
int plat_qtm_available(void);
/* The i-th registered placement (0 <= i < the return value, which is the count): its name, base, budget and
 * tier name. For memory-map dumps and tools; any out pointer may be NULL. */
int plat_place_info(int i, const char** name, void** base, size_t* bytes, const char** tier);

#ifdef __cplusplus
}
#endif

#endif
