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
 * Lines go to sdmc:/3ds/ttyd/log.txt, the live log (TCP 17492, with switch
 * livelog or when netloaded) and the bottom screen console. Producers only
 * format into a lock-free queue; a writer thread on core 1 does the I/O.
 * Every line starts with #<sequence>, so a gap shows dropped lines. */
enum { PLAT_LOG_ERROR = 0, PLAT_LOG_WARN = 1, PLAT_LOG_INFO = 2, PLAT_LOG_DEBUG = 3 };
void plat_log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void plat_vlog(int level, const char* fmt, va_list ap);
#define PLOG_E(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)
#define PLOG_W(...) plat_log(PLAT_LOG_WARN, __VA_ARGS__)
#define PLOG_I(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define PLOG_D(...) plat_log(PLAT_LOG_DEBUG, __VA_ARGS__)
int plat_log_level(void);

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
uint32_t plat_uptime_ms(void);
uint64_t plat_rtc_ms_since_1900(void); /* the console's clock */

/* ---- Crash handling and exit (plat/crash.c) ----
 * Every thread the port creates installs the exception handler with
 * plat_crash_install_thread(). A crash, plat_fatal() or a watchdog timeout
 * writes [CRASH] lines (registers, stack scan, last phase, current map,
 * build) to log.txt, crash.txt and the live log, then ends the process,
 * which returns to the HOME Menu or the Homebrew Launcher. */
void plat_crash_install_thread(void);
void plat_fatal(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void plat_exit_process(void) __attribute__((noreturn));
void plat_crash_line(const char* s, int n);
/* Context shown in crash reports: a short name of what the game loop is
 * doing and the current map (set by the game side). */
void plat_set_phase(const char* phase);
void plat_set_map_name(const char* map);
/* Watchdog: the game loop calls this once per frame. No call for wdog=N
 * seconds (default 10) while the app is in the foreground is a hang. */
void plat_wd_progress(void);
/* Holds the watchdog while a long legitimate wait runs (saving, loading). */
void plat_wd_hold(int on);

/* ---- Frame and exit requests (plat/boot.c) ----
 * Called once per frame by the game side. Runs the APT loop (HOME menu,
 * sleep, power). Returns 0 when the system asked the app to close; the
 * caller then calls plat_shutdown(). */
int plat_frame_poll(void);
void plat_shutdown(void) __attribute__((noreturn));

/* ---- System information ---- */
int plat_is_new3ds(void);
const char* plat_build_string(void); /* "#<commits> <hash>" */

/* ---- Memory placement (plat/memplace.c) ----
 * Buffers that several cores touch every frame live in the New 3DS extra RAM
 * (QTM) when the process has it mapped (CIA), else in FCRAM (3dsx), see
 * docs/PORT_3DS_PLAN.md section 5.7. Every placement has an id from the
 * table in memplace.c with a size budget. Returns NULL if the id is unknown
 * or its budget is exceeded (the caller treats that as fatal). */
enum {
    PLAT_PLACE_GX_RECORD = 0,  /* GX record buffers, core 0 writes, core 2 reads */
    PLAT_PLACE_GX_STATE,       /* GX state tracker and caches */
    PLAT_PLACE_AUDIO_STATE,    /* MusyX voice state and aux working buffers */
    PLAT_PLACE_LOG,            /* log queue and profiler counters */
    PLAT_PLACE_COUNT
};
void* plat_place_alloc(int id, size_t size, size_t align);
int plat_qtm_available(void);

#ifdef __cplusplus
}
#endif

#endif
