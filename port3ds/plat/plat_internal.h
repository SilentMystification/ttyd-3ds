/* Declarations shared inside port3ds/plat only (libctru ABI). */
#ifndef PLAT_INTERNAL_H
#define PLAT_INTERNAL_H

#include <3ds.h>

#include "plat/plat.h"
#include "plat/sdio.h"

/* switches.c */
void plat_switches_load(int argc, char** argv);
const char* plat_switches_text(void);

/* log.c */
void plat_log_init(int live);        /* open latest.log (previous run kept as previous.log), live socket */
void plat_log_start_writer(void);    /* the writer thread on core 1; before it, lines are written directly */
void plat_log_stop(void);            /* normal exit: write everything, stop the writer */
void plat_log_crash_flush(void);     /* crash path: write the queue in the calling thread */
void plat_log_crash_close(void);     /* before svcExitProcess */
void plat_log_crash_sync(void);      /* one fsync of latest.log (the crash lines are written without one) */
void plat_log_console_ready(void);   /* the bottom console exists: show the lines kept until now */
void plat_log_poll(void);            /* accept a live-log client while the writer does not run */
unsigned plat_log_dropped(void);
void plat_log_set_level(int level);
int plat_retail_keeps(const char* name); /* retail mode: retailkeep= names it (or it is always read) */
void plat_log_retail_now(int on);    /* retail mode switched in the Select menu: lines stop (1) or come back (0) at once */
/* Every log line (no sequence number) goes to fn, one call at a time, on the thread that writes the log
 * (the core 1 writer once it runs). For the devupdate queue's `until` pattern. */
void plat_log_set_line_hook(void (*fn)(const char* line));
void plat_console_lock(void);
int plat_console_trylock(void);
void plat_console_unlock(void);

/* crash.c (glue to libs/ctr-devkit/crash) */
void plat_crash_setup(void);         /* config and the main thread's handler, as early as the switches allow */
void plat_crash_init(void);          /* watchdog thread, boot crash tests */
void plat_stack_register(int slot, const char* name, unsigned size);
void plat_thread_register(const char* name, unsigned stack_bytes); /* call at a thread's entry (installs the handler) */
Result plat_thread_refusal(int priority, int core); /* thread.c: the Result of a refused threadCreate */
void plat_set_audio_abort(void (*fn)(void)); /* DSP release for the crash exit (no waiting on the audio thread) */
void plat_wd_quiesce(void);
uint32_t plat_wd_frames(void);       /* plat_wd_progress calls so far */

const char* plat_get_phase(void);    /* the last plat_set_phase text (what the game thread says it is doing) */
const char* plat_get_map_name(void); /* the last plat_set_map_name text ("-" before a map) */
int plat_options_menu_is_open(void);

/* boot.c: 1 for a development build (PORT3DS_VERSION contains "-dev"); the one test for it. */
int plat_is_dev_build(void);

/* devupdate.c (glue to libs/ctr-devkit/devupdate; switch devupdate) */
typedef bool (*PlatRecoveryHook)(const char* report, void* user);
int plat_devupdate_enabled(void);
int plat_devupdate_wanted(void);     /* the option as set (or its default): what the next launch does */
void plat_devupdate_address(char* out, size_t n); /* "ip:port" while the server runs, else "" */
void plat_relaunch(void);            /* restart the title now with the current switches (does not return) */
void plat_devupdate_init(void);      /* starts the server when the switch is on */
void plat_devupdate_stop(void);
void plat_devupdate_frame(void);     /* game thread, each frame: pauses there during an update */
PlatRecoveryHook plat_devupdate_recovery_hook(void); /* for the crash handler; NULL when off */

/* gpuprobe_mode.c: dev tool inside the game CIA (switch gpuprobe; development builds only) */
int plat_gpuprobe_requested(void);   /* dev build and the switch is given */
int plat_gpuprobe_mode(void);        /* 1 once the probe mode runs (no game, no game GPU) */
void plat_gpuprobe_mode_run(void) __attribute__((noreturn));

/* calib_mode.c: dev tool inside the game CIA (switch calib; development builds only) */
int plat_calib_requested(void);      /* dev build and the switch is given */
int plat_calib_mode(void);           /* 1 once the benchmark mode runs (no game, no game GPU) */
void plat_calib_mode_run(void) __attribute__((noreturn));

/* menu.c: returns 1 while the Select menu is open (it consumed the input). */
int plat_menu_input(uint32_t held, uint32_t down);
void plat_qtm_usage(uint32_t* used, uint32_t* total); /* memplace.c: QTM RAM placed and available, bytes (0, 0 when not mapped) */
uint32_t plat_gpu_vram_largest_kb(void);              /* gpu.c: the largest block vramAlloc can still give (probes; not every frame) */
void plat_pause_grey(void);          /* pause look: the top screen's last frame, greyed and darker (both eyes); the game's next frame replaces it */
void plat_pause_release(void);
void plat_pause_begin_updater(int idle); /* the updater's pause, from its server thread: no GPU sync; greys the top screen only when idle (game thread parked) */
void plat_pause_begin(void);        /* a menu pauses the game thread: hold the watchdog, mute, grey the top screen */
void plat_pause_end(void);           /* ...and undo it, counting the time for plat_paused_ticks */
void plat_pause_add_ticks(uint64_t ticks);
void plat_pause_dump(int tick);      /* test aid: the frozen top screen as shots/pNNNNN.ppm */
void plat_updater_screen(int on);    /* the updater screen (devupdate) replaces the bottom screen (on) or the previous screen returns (off) */
void plat_updater_draw(const char* title, const char* build, const char* line, int percent); /* title row, build line, one status line, progress bar; the log below keeps scrolling */
void plat_console_menu_mode(int on); /* on: a menu covers the whole screen and the log goes to a buffer only; off: HUD and log are repainted from the buffers. Caller holds the console lock */
void plat_console_log(const char* s, size_t n); /* the console writer's text, under the console lock: buffered, and shown unless a menu covers the screen */
void plat_menu_shot_tick(void);      /* switch shotbottom=N: bottom screen to shots/bNNNNN.ppm */

/* memplace.c */
void plat_memplace_init(void);
void plat_memplace_report(void);

/* boot.c */
int plat_started_by_netload(void);
void plat_cpu_limit_home_pressed(void); /* vblank thread: HOME seen, lower the limit now */
uint64_t plat_boot_tick(void);       /* svcGetSystemTick at process start */
void plat_cpu_limit_transfer(int on); /* devupdate: 30% while a transfer runs, then the game's limit */
void plat_cpu_limit_init(void);      /* core 1 time limit and the APT hook that lowers it on HOME/sleep */
void plat_set_gpu_shutdown(void (*fn)(void)); /* the GPU back end's teardown, for message screens */
void plat_set_gpu_abort(void (*fn)(void));    /* teardown for the crash screen: must not wait for other threads */
void plat_crash_gpu_down(void);

#endif
