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
void plat_log_init(int live);        /* open log.txt (previous run kept as log.old), live socket */
void plat_log_start_writer(void);    /* the writer thread on core 1; before it, lines are written directly */
void plat_log_stop(void);            /* normal exit: write everything, stop the writer */
void plat_log_crash_flush(void);     /* crash path: write the queue in the calling thread */
void plat_log_crash_close(void);     /* before svcExitProcess */
void plat_log_console_ready(void);   /* the bottom console exists: show the lines kept until now */
void plat_log_poll(void);            /* accept a live-log client while the writer does not run */
unsigned plat_log_dropped(void);
void plat_log_set_level(int level);
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
void plat_set_audio_abort(void (*fn)(void)); /* DSP release for the crash exit (no waiting on the audio thread) */
void plat_wd_quiesce(void);
uint32_t plat_wd_frames(void);       /* plat_wd_progress calls so far */

/* devupdate.c (glue to libs/ctr-devkit/devupdate; switch devupdate) */
typedef bool (*PlatRecoveryHook)(const char* report, void* user);
int plat_devupdate_enabled(void);
void plat_devupdate_init(void);      /* starts the server when the switch is on */
void plat_devupdate_stop(void);
void plat_devupdate_frame(void);     /* game thread, each frame: pauses there during an update */
PlatRecoveryHook plat_devupdate_recovery_hook(void); /* for the crash handler; NULL when off */

/* menu.c: returns 1 while the Select menu is open (it consumed the input). */
int plat_menu_input(uint32_t held, uint32_t down);
void plat_console_menu_mode(int on); /* a menu owns rows 3-14 and the log shrinks to rows 16-30 (on), or the log has rows 3-30 (off); caller holds the console lock for on */
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
