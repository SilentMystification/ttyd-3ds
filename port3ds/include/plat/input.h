/* 3DS input for the game side (plat/input.c). */
#ifndef PLAT_INPUT_H
#define PLAT_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Button bits, the same values as libctru's KEY_* (hid.h). */
#define PLAT_KEY_A (1u << 0)
#define PLAT_KEY_B (1u << 1)
#define PLAT_KEY_SELECT (1u << 2)
#define PLAT_KEY_START (1u << 3)
#define PLAT_KEY_DRIGHT (1u << 4)
#define PLAT_KEY_DLEFT (1u << 5)
#define PLAT_KEY_DUP (1u << 6)
#define PLAT_KEY_DDOWN (1u << 7)
#define PLAT_KEY_R (1u << 8)
#define PLAT_KEY_L (1u << 9)
#define PLAT_KEY_X (1u << 10)
#define PLAT_KEY_Y (1u << 11)
#define PLAT_KEY_ZL (1u << 14)
#define PLAT_KEY_ZR (1u << 15)
#define PLAT_KEY_TOUCH (1u << 20)

typedef struct {
    uint32_t held;      /* PLAT_KEY_* bits */
    int16_t circle_x;   /* about -156..156 */
    int16_t circle_y;
    int16_t cstick_x;   /* about -156..156 (New 3DS C-stick) */
    int16_t cstick_y;
    uint16_t touch_x;   /* valid while PLAT_KEY_TOUCH is held */
    uint16_t touch_y;
} PlatInput;

/* Scans the hardware (once per game frame, from the game thread) and
 * returns the state. While the Select menu is open the game gets an empty
 * state and the menu gets the input. Scripted input (switch padscript=)
 * replaces the hardware state. */
void plat_input_scan(PlatInput* out);

/* Registers a hook (game-specific code) whose PLAT_KEY_* bits are ORed into
 * every scan, on top of scripted or real input. NULL removes it. */
void plat_input_set_extra_keys(uint32_t (*fn)(void));

/* The pad state the game reads, in GameCube PADStatus terms (stick values
 * signed, 0 = centred). The game's pad read passes its state through
 * plat_pad_state() once per read: with switch padfile=FILE the call replaces
 * it with the file's record for the current game frame (neutral after the last
 * one); with padrec=FILE it keeps it as the frame's record, written to FILE
 * every 600 frames and at exit. See docs/PORT_3DS_TESTING.md, "Recorded inputs". */
typedef struct {
    uint16_t buttons;
    int8_t stick_x, stick_y;
    int8_t substick_x, substick_y;
    uint8_t trigger_l, trigger_r;
} PlatPadState;

void plat_pad_state(PlatPadState* s);

/* Called once per game frame: advances scripted input (switch padscript). */
void plat_input_frame(void);

#ifdef __cplusplus
}
#endif

#endif
