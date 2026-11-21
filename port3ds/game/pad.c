/* Dolphin PAD on 3DS input: one standard GameCube controller on port 1.
 *
 * Mapping (docs/PORT_3DS_PLAN.md 5.6): A, B, X, Y, Start and the D-pad map
 * directly; L and R are the GameCube's L and R, fully pressed (the 3DS
 * shoulders are digital, so the analog value is 255 with the click bit);
 * ZR is Z; the circle pad is the control stick and the New 3DS C-stick the
 * C-stick. Stick values use the GameCube's raw range (about +-100 at full
 * tilt, before PADClamp). Rumble has no 3DS equivalent, so PADControlMotor
 * only records the state.
 *
 * PADClamp is the SDK's own (libs/dolsdk2004/src/pad/Padclamp.c).
 */
#include <dolphin/pad.h>
#include <string.h>

#include "plat/input.h"
#include "plat/plat.h"
#include "port3ds/os_internal.h"

#define STICK_FULL 100 /* GameCube raw value at full tilt */
#define CIRCLE_FULL 150 /* 3DS circle pad value at full tilt */

static u32 s_motor[PAD_MAX_CONTROLLERS];
static BOOL s_reset_button;

static s8 scale_stick(int v) {
    int s = v * STICK_FULL / CIRCLE_FULL;
    if (s > 127) {
        s = 127;
    } else if (s < -128) {
        s = -128;
    }
    return (s8)s;
}

BOOL PADInit(void) {
    memset(s_motor, 0, sizeof(s_motor));
    return TRUE;
}

BOOL PADReset(u32 mask) {
    (void)mask;
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    PlatInput in;
    plat_input_scan(&in);
    s_reset_button = plat_reset_requested() ? TRUE : FALSE;

    memset(status, 0, sizeof(PADStatus) * PAD_MAX_CONTROLLERS);
    PADStatus* p = &status[0];
    u16 b = 0;
    if (in.held & PLAT_KEY_A) b |= PAD_BUTTON_A;
    if (in.held & PLAT_KEY_B) b |= PAD_BUTTON_B;
    if (in.held & PLAT_KEY_X) b |= PAD_BUTTON_X;
    if (in.held & PLAT_KEY_Y) b |= PAD_BUTTON_Y;
    if (in.held & PLAT_KEY_START) b |= PAD_BUTTON_START;
    if (in.held & PLAT_KEY_DUP) b |= PAD_BUTTON_UP;
    if (in.held & PLAT_KEY_DDOWN) b |= PAD_BUTTON_DOWN;
    if (in.held & PLAT_KEY_DLEFT) b |= PAD_BUTTON_LEFT;
    if (in.held & PLAT_KEY_DRIGHT) b |= PAD_BUTTON_RIGHT;
    if (in.held & PLAT_KEY_ZR) b |= PAD_TRIGGER_Z;
    if (in.held & PLAT_KEY_L) {
        b |= PAD_TRIGGER_L;
        p->triggerLeft = 0xFF;
    }
    if (in.held & PLAT_KEY_R) {
        b |= PAD_TRIGGER_R;
        p->triggerRight = 0xFF;
    }
    p->button = b;
    p->stickX = scale_stick(in.circle_x);
    p->stickY = scale_stick(in.circle_y);
    p->substickX = scale_stick(in.cstick_x);
    p->substickY = scale_stick(in.cstick_y);
    p->analogA = (b & PAD_BUTTON_A) ? 0xFF : 0;
    p->analogB = (b & PAD_BUTTON_B) ? 0xFF : 0;
    p->err = PAD_ERR_NONE;
    for (int i = 1; i < PAD_MAX_CONTROLLERS; i++) {
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return 0;
}

void PADControlMotor(s32 chan, u32 command) {
    if (chan >= 0 && chan < PAD_MAX_CONTROLLERS) {
        s_motor[chan] = command;
    }
}

BOOL port_reset_button_state(void) { return s_reset_button; }
