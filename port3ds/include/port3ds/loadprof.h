/* Map-change and battle-start load profile (switch loadprof; port3ds/game/loadprof.c).
 *
 * A window runs from the request (the sequence becomes SEQ_MAP_CHANGE or
 * SEQ_BATTLE) to the first frame of the new field (or the first battle
 * frame). The hooks below add to cumulative counters; the window logs the
 * difference as one [LOADPROF] line, a DVD read size histogram, and then the
 * game thread's busy time in the first frames after the map appears (first
 * use of textures and TEV configurations in the GX layer shows up there). */
#ifndef PORT3DS_LOADPROF_H
#define PORT3DS_LOADPROF_H

#include <dolphin/types.h>

extern int port_lp_state; /* -1 unread, 0 off, 1 on */
int port_lp_probe(void);
static inline int port_lp_active(void) { return port_lp_state > 0 || (port_lp_state < 0 && port_lp_probe()); }

/* A game read that reached the image: disc offset, bytes, ticks the caller waited. The ticks between port_lp_dvd_done and
 * this call (the caller waiting for the baton) are counted as the baton wake. */
void port_lp_dvd(u32 disc_offset, u32 size, u64 ticks);
/* One ARAM transfer: bytes, ticks the GameCube's DMA would take (the callback's delay), ticks the DMA thread slept. */
void port_lp_aram(u32 bytes, u64 sim_ticks, u64 slept_ticks);
/* Ticks the game thread spent in port_spin. */
void port_lp_spin(u64 ticks);
/* Ticks of one conversion (port_conv_end). */
void port_lp_conv(u64 ticks);
/* The file thread finished a game read (tick). */
void port_lp_dvd_done(u64 tick);
/* Ticks the game thread waited for the retrace. */
void port_lp_retrace(u64 ticks);
/* Once per game frame, from the game thread. busy: ticks the previous frame took on the game thread without
 * its retrace wait. ready: the field (or the battle) is up. */
void port_lp_frame(u32 frame, int seq, int ready, const char* map, u64 busy);

#endif
