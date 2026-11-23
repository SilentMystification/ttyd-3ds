/* Test mode shared by the 3DS port (TARGET_PORT) and the GameCube debug DOL
 * (TARGET_GCDEBUG, configure.py --gcdebug): the switches determ, gwdump,
 * fx, battle and btlauto (docs/PORT_3DS_TEST_INFRA.md).
 *
 * test_mode.c, test_gallery.c and test_battle.c are compiled by both builds,
 * so they are written in C89 for MWCC. Each build supplies the tst_plat_*
 * functions (port3ds/game/test_plat_port.c, port3ds/gcdebug/gcdebug.c).
 */
#ifndef PORT3DS_TEST_MODE_H
#define PORT3DS_TEST_MODE_H

#include <dolphin/os.h>
#include <dolphin/types.h>

/* ---- supplied by each build ---- */
int tst_sw_has(const char* word);
int tst_sw_int(const char* key, int fallback);
int tst_sw_value(const char* key, char* out, int n);
void tst_log(const char* fmt, ...);
u32 tst_rand_state(void);
/* 1 if v is an address in this build's game memory (event GWs can hold
 * pointers, which differ between builds: gwdump writes them as P). */
int tst_is_ptr(u32 v);
/* Extra controller input for the next PADRead: GameCube PAD_BUTTON_* bits. */
void tst_set_keys(u32 buttons);
#ifndef TARGET_GCDEBUG
/* Port only: the picture finished at game frame `frame` (the next one
 * presented) goes to gallery/fNNNNN.ppm. Called one frame before. The
 * GameCube build reads its XFB at tst_shot_point instead (run_dolphin.py). */
void tst_plat_shot(u32 frame);
#endif

/* ---- the shared core (test_mode.c) ---- */
/* 1 when the switch determ is on: game time comes from the game frame count. */
int tst_determ(void);
/* One retrace in OS ticks: 1/60 s, or 1/50 s for a PAL TV format. */
OSTime tst_retrace_ticks(void);
/* The game's OSGetTime in determ mode: a fixed date plus one retrace per
 * virtual retrace, plus one tick per call within that retrace on the same
 * OS thread (so a busy-wait on the clock still ends). */
OSTime tst_game_time(void);
/* Determ mode: the one retrace of this game frame; returns the retrace count
 * to give the game's retrace callbacks. */
u32 tst_vretrace(void);
/* 1 while the DVD manager (dvdmgr.c) still has a read queued: determ mode
 * waits for it at the frame boundary, so a load always ends in the frame it
 * was queued, on the GameCube and on the port alike. */
int tst_dvd_busy(void);
/* Once per game frame, at the start of VIWaitForRetrace. */
void tst_frame(u32 frame);
/* rngtrace= hook, called by rand() with its caller address, the next two callers up (best effort) and the result */
u32 tst_cur_frame(void);
int tst_rngtrace_wanted(void); /* the window is open */
void tst_rngtrace(unsigned long caller, unsigned long caller2, unsigned long caller3, u32 val);
/* Game frames since the first map change (the switch warp), 0 before. */
u32 tst_warp_frames(void);

/* ---- test_gallery.c: fx=, battle= and the gallery shots ---- */
void tst_gallery_frame(u32 frame);
/* Map sweep of the stg_* battle stage maps: the host map (and story) that has an enemy, or
 * NULL if name is no stage; start a battle there on that stage; poll it each frame (0 waiting,
 * 1 stage up after settle frames, 2 back in the field, -1 failed). */
const char* tst_stage_host(const char* name, int* story);
int tst_stage_start(const char* name);
int tst_stage_poll(int settle);
/* The shot frame: called at the frame hook of game frame N when a shot of N
 * is due (shotat=, fxshots=, btlshots=), after the frame's picture is done.
 * The GameCube build's breakpoint for run_dolphin.py --shots. */
void tst_shot_point(u32 frame);
extern volatile u32 tst_shot_frame;
/* ---- test_battle.c: btlauto= ---- */
void tst_battle_frame(u32 frame);
/* ---- test_font.c (port only): fonttest=sjis ---- */
void tst_font_frame(u32 frame);

#endif
