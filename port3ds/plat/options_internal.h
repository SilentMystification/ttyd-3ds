/* Shared inside port3ds/plat only: the options menu, the console and the input. */
#ifndef OPTIONS_INTERNAL_H
#define OPTIONS_INTERNAL_H

#include <stdint.h>

#include "plat/options.h"

/* options.c */
const PlatOptMenu* plat_options_menu(void);
void plat_options_tick(void);        /* once per frame: volume to NDSP */
void plat_options_menu_redraw(void); /* paint the open menu again (the screen was used by something else) */

/* options_menu.c: the list UI on the bottom console (rows 3-16). */
void plat_options_menu_open(void);
int plat_options_menu_is_open(void);
/* The modal loop: runs until the menu closes, polling input itself. Called
 * from the frame hook, so the game thread (and with it game time) stands still. */
void plat_options_menu_run(void);

/* menu.c */
void plat_menu_frame(void); /* from the frame hook: runs the options menu while it is open */
void plat_console_row(int row, const char* text); /* 1-based row of the 40x30 bottom console, whole line (caller holds the console lock) */
int plat_console_ready(void);                     /* the bottom console exists */

/* input.c: key source for the modal menu (scripted or real), edge-tracked. */
uint32_t plat_input_modal_poll(void);    /* scans, returns held; scripted steps advance one per call */
void plat_input_menu_closed(void);       /* keys still held at close stay hidden from the game until released */

#endif
