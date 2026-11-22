/* The 3DS port's side of the shared test mode (test_mode.h): switches from
 * plat/switches.c, lines in the game log, the MSL rand state (msl.c) and
 * extra keys through plat_input_set_extra_keys. */
#include <dolphin/pad.h>
#include <stdarg.h>

#include "plat/input.h"
#include "plat/plat.h"
#include "test_mode.h"

u32 ttyd_rand_state(void);

int tst_sw_has(const char* word) { return plat_switch_has(word); }

int tst_sw_int(const char* key, int fallback) { return plat_switch_int(key, fallback); }

int tst_sw_value(const char* key, char* out, int n) { return plat_switch_value(key, out, (size_t)n); }

void tst_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    plat_vlog(PLAT_LOG_INFO, fmt, ap);
    va_end(ap);
}

u32 tst_rand_state(void) { return ttyd_rand_state(); }

static volatile uint32_t s_keys;

static uint32_t extra_keys(void) { return s_keys; }

void tst_set_keys(u32 buttons) {
    static int installed;
    uint32_t k = 0;
    if (buttons & PAD_BUTTON_A) k |= PLAT_KEY_A;
    if (buttons & PAD_BUTTON_B) k |= PLAT_KEY_B;
    if (buttons & PAD_BUTTON_X) k |= PLAT_KEY_X;
    if (buttons & PAD_BUTTON_Y) k |= PLAT_KEY_Y;
    if (buttons & PAD_BUTTON_START) k |= PLAT_KEY_START;
    if (buttons & PAD_BUTTON_UP) k |= PLAT_KEY_DUP;
    if (buttons & PAD_BUTTON_DOWN) k |= PLAT_KEY_DDOWN;
    if (buttons & PAD_BUTTON_LEFT) k |= PLAT_KEY_DLEFT;
    if (buttons & PAD_BUTTON_RIGHT) k |= PLAT_KEY_DRIGHT;
    if (buttons & PAD_TRIGGER_Z) k |= PLAT_KEY_ZR;
    if (buttons & PAD_TRIGGER_L) k |= PLAT_KEY_L;
    if (buttons & PAD_TRIGGER_R) k |= PLAT_KEY_R;
    s_keys = k;
    /* fastboot holds the hook until the warp; battles come after it */
    if (k != 0 && !installed) {
        installed = 1;
        plat_input_set_extra_keys(extra_keys);
    }
}
