/* Shared declarations of the port's game-side SDK (port3ds/game). */
#ifndef PORT3DS_OS_INTERNAL_H
#define PORT3DS_OS_INTERNAL_H

#include <dolphin/os.h>

/* os.c: interrupt emulation. Code that stands in for an interrupt handler
 * (audio DMA, DVD completion, retrace callbacks) runs between
 * port_irq_enter and port_irq_leave on its host thread. */
void port_irq_enter(void);
void port_irq_leave(void);
int port_irq_depth(void);
int port_irq_release_all(void);
void port_irq_reacquire(int depth);

/* os.c: a replacement source for OSGetTime (and OSGetTick), for a test mode
 * where game time comes from the frame count; NULL restores the clock.
 * port_os_real_time is the clock itself, for code that needs real time. */
void port_os_set_time_source(OSTime (*source)(void));
OSTime port_os_real_time(void);

/* ai.c: frame-paced audio. While on, the platform's audio thread only plays
 * silence, and port_ai_pump runs the DMA interrupt for n 5 ms quanta of the
 * game's own (the caller's) clock. */
void port_ai_set_frame_paced(int on);
void port_ai_pump(int quanta);

/* os_thread.c */
void port_thread_init(void);
/* From a host thread that is not a game thread (an "interrupt"): give the
 * CPU to the best ready game thread if none is running. */
void port_thread_dispatch_if_idle(void);
/* Number of times the cooperative scheduler switched threads (HUD). */
unsigned port_thread_switch_count(void);

/* pad.c: the Select menu's "Return to title" acts as the reset button. */
BOOL port_reset_button_state(void);

#endif
