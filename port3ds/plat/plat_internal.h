/* Declarations shared inside port3ds/plat only (libctru ABI). */
#ifndef PLAT_INTERNAL_H
#define PLAT_INTERNAL_H

#include <3ds.h>

#include "plat/plat.h"

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
void plat_console_lock(void);
int plat_console_trylock(void);
void plat_console_unlock(void);

/* crash.c */
void plat_crash_init(void);          /* handler stack, main thread handler, watchdog thread */
void plat_stack_register(int slot, const char* name, unsigned size);
extern volatile uint32_t plat_wd_counter;

/* memplace.c */
void plat_memplace_init(void);
void plat_memplace_report(void);

/* boot.c */
int plat_started_by_netload(void);
void plat_cpu_limit_init(void);      /* core 1 time limit and the APT hook that lowers it on HOME/sleep */

#endif
