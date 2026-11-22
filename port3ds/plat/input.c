/* 3DS input (see include/plat/input.h).
 *
 * The Select menu (plat/menu.c) takes the input while open. Scripted input
 * (switch padscript=...) drives the game for automated tests: a comma list
 * of steps, each "KEYS*frames" ("A*2,wait*30,right*60,A+B*1"); key names
 * a b x y l r zl zr start select up down left right, "wait" for nothing;
 * "cl/cr/cu/cd" push the circle pad fully left/right/up/down. After the
 * script, real input takes over again.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "options_internal.h"
#include "plat/input.h"
#include "plat_internal.h"

typedef struct {
    uint32_t keys;
    int16_t cx, cy;
    uint32_t frames;
} ScriptStep;

#define MAX_STEPS 2048

static ScriptStep s_script[MAX_STEPS];
static int s_steps, s_step;
static uint32_t s_step_frames;
static int s_script_loaded;

static uint32_t key_of(const char* name, int16_t* cx, int16_t* cy) {
    static const struct {
        const char* name;
        uint32_t key;
    } keys[] = {
        {"a", PLAT_KEY_A},         {"b", PLAT_KEY_B},       {"x", PLAT_KEY_X},       {"y", PLAT_KEY_Y},
        {"l", PLAT_KEY_L},         {"r", PLAT_KEY_R},       {"zl", PLAT_KEY_ZL},     {"zr", PLAT_KEY_ZR},
        {"start", PLAT_KEY_START}, {"select", PLAT_KEY_SELECT}, {"up", PLAT_KEY_DUP}, {"down", PLAT_KEY_DDOWN},
        {"left", PLAT_KEY_DLEFT},  {"right", PLAT_KEY_DRIGHT}, {"wait", 0},
    };
    if (!strcmp(name, "cl")) { *cx = -156; return 0; }
    if (!strcmp(name, "cr")) { *cx = 156; return 0; }
    if (!strcmp(name, "cu")) { *cy = 156; return 0; }
    if (!strcmp(name, "cd")) { *cy = -156; return 0; }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!strcmp(name, keys[i].name)) {
            return keys[i].key;
        }
    }
    PLOG_W("padscript: unknown key \"%s\"", name);
    return 0;
}

static void script_load(void) {
    s_script_loaded = 1;
    static char text[4096];
    if (!plat_switch_value("padscript", text, sizeof(text))) {
        return;
    }
    char* save1;
    for (char* step = strtok_r(text, ",", &save1); step && s_steps < MAX_STEPS; step = strtok_r(NULL, ",", &save1)) {
        ScriptStep st = {0, 0, 0, 1};
        char* star = strchr(step, '*');
        if (star != NULL) {
            *star = '\0';
            st.frames = (uint32_t)strtoul(star + 1, NULL, 10);
        }
        char* save2;
        for (char* k = strtok_r(step, "+", &save2); k; k = strtok_r(NULL, "+", &save2)) {
            for (char* c = k; *c; c++) {
                *c = (char)tolower((unsigned char)*c);
            }
            st.keys |= key_of(k, &st.cx, &st.cy);
        }
        s_script[s_steps++] = st;
    }
    PLOG_I("padscript: %d steps", s_steps);
}

static uint32_t (*s_extra_keys)(void);

void plat_input_set_extra_keys(uint32_t (*fn)(void)) {
    s_extra_keys = fn;
}

static void scan(PlatInput* out);

void plat_input_scan(PlatInput* out) {
    scan(out);
    if (s_extra_keys != NULL) {
        out->held |= s_extra_keys();
    }
}

static uint32_t s_prev_held; /* last effective key state, for press edges */
static uint32_t s_block;     /* keys held when a menu closed: hidden from the game until released */

static int script_active(void) { return s_step < s_steps; }

/* The effective key state: the script's while one runs, else the hardware's. */
static uint32_t effective_held(uint32_t hw) { return script_active() ? s_script[s_step].keys : hw; }

static void script_advance(void) {
    if (s_step < s_steps && ++s_step_frames >= s_script[s_step].frames) {
        s_step_frames = 0;
        if (++s_step == s_steps) {
            PLOG_I("padscript: done");
        }
    }
}

/* The options menu's loop (plat/options_menu.c) polls here, about every 16 ms:
 * scripted steps count one per call, as they count one per game frame outside. */
uint32_t plat_input_modal_poll(void) {
    hidScanInput();
    if (!s_script_loaded) {
        script_load();
    }
    uint32_t held = effective_held(hidKeysHeld());
    if (script_active()) {
        script_advance();
    }
    s_prev_held = held;
    return held;
}

void plat_input_menu_closed(void) { s_block = s_prev_held; }

static void scan(PlatInput* out) {
    memset(out, 0, sizeof(*out));
    hidScanInput();
    if (!s_script_loaded) {
        script_load();
    }
    uint32_t held = effective_held(hidKeysHeld());
    uint32_t down = held & ~s_prev_held;
    s_prev_held = held;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);

    if (plat_menu_input(held, down)) {
        return; /* a menu has the input */
    }
    s_block &= held;
    if (script_active()) {
        const ScriptStep* st = &s_script[s_step];
        out->held = st->keys & ~s_block;
        out->circle_x = st->cx;
        out->circle_y = st->cy;
        return;
    }
    out->held = held & ~s_block;
    out->circle_x = cp.dx;
    out->circle_y = cp.dy;
    out->cstick_x = cs.dx;
    out->cstick_y = cs.dy;
    if (held & KEY_TOUCH) {
        touchPosition tp;
        hidTouchRead(&tp);
        out->touch_x = tp.px;
        out->touch_y = tp.py;
    }
}

/* One game frame passed (the game's VIWaitForRetrace): scripted input steps
 * count game frames, so a script does the same at any frame rate. */
void plat_input_frame(void) {
    if (!s_script_loaded) {
        script_load();
    }
    plat_options_tick();
    plat_menu_frame(); /* runs the options menu while it is open: the game waits here */
    script_advance();
}
