/* Dolphin PAD on 3DS input: one standard GameCube controller on port 1.
 *
 * Mapping (docs/PORT_3DS_PLAN.md 5.6): A, B, X, Y, Start and the D-pad map
 * directly; L and R are the GameCube's L and R, fully pressed (the 3DS
 * shoulders are digital, so the analog value is 255 with the click bit);
 * ZR is Z; the circle pad is the control stick and the New 3DS C-stick the
 * C-stick. The player's options (game/options_ttyd.c) can remap A, B, X, Y,
 * Z, L, R and Start to other 3DS buttons and make the D-pad the C-stick;
 * what is described here is the default. Stick values use the GameCube's raw range (about +-100 at full
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

/* The GameCube buttons the player can remap, and the 3DS keys (PLAT_KEY_* bits,
 * 0 = none) that press each one: the defaults above, replaced from the player's
 * options (game/options_ttyd.c, applied at PADInit and on every change). The
 * order is the one of the key list there. */
enum { GC_A, GC_B, GC_X, GC_Y, GC_Z, GC_L, GC_R, GC_START, GC_COUNT };
static u32 s_src[GC_COUNT] = {PLAT_KEY_A, PLAT_KEY_B, PLAT_KEY_X,  PLAT_KEY_Y,
                              PLAT_KEY_ZR, PLAT_KEY_L, PLAT_KEY_R, PLAT_KEY_START};
static int s_cstick_dpad; /* the D-pad is the C-stick (and sends no D-pad buttons) */
static int s_padlog = -1;
static u16 s_last_log;

void port_options_init(void);

void port_pad_set_button_source(int gc_button, u32 keys) {
    if (gc_button >= 0 && gc_button < GC_COUNT) {
        s_src[gc_button] = keys;
    }
}

void port_pad_set_cstick_dpad(int on) { s_cstick_dpad = on; }

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
    port_options_init();
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
    static const u16 gc_bits[GC_COUNT] = {PAD_BUTTON_A,  PAD_BUTTON_B,  PAD_BUTTON_X,   PAD_BUTTON_Y,
                                          PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_BUTTON_START};
    for (int i = 0; i < GC_COUNT; i++) {
        if (in.held & s_src[i]) b |= gc_bits[i];
    }
    if (b & PAD_TRIGGER_L) p->triggerLeft = 0xFF;
    if (b & PAD_TRIGGER_R) p->triggerRight = 0xFF;
    int cx = in.cstick_x, cy = in.cstick_y;
    if (s_cstick_dpad) {
        cx = (in.held & PLAT_KEY_DRIGHT) ? CIRCLE_FULL : (in.held & PLAT_KEY_DLEFT) ? -CIRCLE_FULL : 0;
        cy = (in.held & PLAT_KEY_DUP) ? CIRCLE_FULL : (in.held & PLAT_KEY_DDOWN) ? -CIRCLE_FULL : 0;
    } else {
        if (in.held & PLAT_KEY_DUP) b |= PAD_BUTTON_UP;
        if (in.held & PLAT_KEY_DDOWN) b |= PAD_BUTTON_DOWN;
        if (in.held & PLAT_KEY_DLEFT) b |= PAD_BUTTON_LEFT;
        if (in.held & PLAT_KEY_DRIGHT) b |= PAD_BUTTON_RIGHT;
    }
    if (s_padlog < 0) {
        s_padlog = plat_switch_has("padlog");
    }
    if (s_padlog && b != s_last_log) {
        /* test hook (switch padlog): the 3DS keys that arrived and the GameCube buttons the game gets */
        plat_log(PLAT_LOG_INFO, "padlog: 3ds=%05lx gc=%04x", (unsigned long)in.held, (unsigned)b);
        s_last_log = b;
    }
    /* recorded inputs (padrec=, padfile=): what the game sees goes through here */
    PlatPadState ps = {b, scale_stick(in.circle_x), scale_stick(in.circle_y), scale_stick(cx), scale_stick(cy),
                       p->triggerLeft, p->triggerRight};
    plat_pad_state(&ps);
    b = ps.buttons;
    p->button = b;
    p->stickX = ps.stick_x;
    p->stickY = ps.stick_y;
    p->substickX = ps.substick_x;
    p->substickY = ps.substick_y;
    p->triggerLeft = ps.trigger_l;
    p->triggerRight = ps.trigger_r;
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
