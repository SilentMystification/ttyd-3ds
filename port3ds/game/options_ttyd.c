/* TTYD's player options: the item list for the generic options menu
 * (plat/options_menu.c) and what each setting does in the game's pad layer.
 * The platform stores the values (sdmc:/3ds/ttyd/options.txt); this file is the
 * game data: the labels, the defaults, and the button mapping.
 *
 * Pages: Controls (GameCube button remap, C-stick source), Display (3D, bottom
 * screen HUD, texture filter), Audio (master volume), Performance (quality: stored only),
 * Language (PAL builds only: stored only), Debug (Debug mode: every debug and background
 * facility, the developer Select menu included, on, auto (development builds only) or off;
 * the platform acts on it, plat.h plat_debug).
 *
 * The settings that only other code reads (3D, quality, language) have their
 * getters in include/plat/options.h (opt_get_stereo3d and friends). */
#include <string.h>

#include "plat/input.h"
#include "plat/options.h"
#include "plat/plat.h"

void port_pad_set_button_source(int gc_button, uint32_t keys);
void port_pad_set_cstick_dpad(int on);
void port_options_init(void);

/* Index = the choice value stored in options.txt. */
static const char* const k_src_names[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "START", "none", NULL};
static const uint32_t k_src_keys[] = {PLAT_KEY_A, PLAT_KEY_B, PLAT_KEY_X,  PLAT_KEY_Y,     PLAT_KEY_L,
                                      PLAT_KEY_R, PLAT_KEY_ZL, PLAT_KEY_ZR, PLAT_KEY_START, 0};
enum { SRC_A, SRC_B, SRC_X, SRC_Y, SRC_L, SRC_R, SRC_ZL, SRC_ZR, SRC_START, SRC_NONE };

static const char* const k_cstick_names[] = {"C-stick nub", "D-pad", NULL};
static const char* const k_onoff_names[] = {"off", "on", NULL};
static const char* const k_hud_names[] = {"off", "FPS only", "full", NULL};
static const char* const k_texfilter_names[] = {"game", "linear", "mip", "nearest", NULL};
static const char* const k_quality_names[] = {"auto", "high", "fast", NULL};
static const char* const k_debug_names[] = {"auto", "on", "off", NULL}; /* PLAT_OPT_DEBUG_* order */
#ifdef VERSION_G8MP01
static const char* const k_lang_names[] = {"English", "Deutsch", "Francais", "Espanol", "Italiano", NULL};
#endif

static const char* const k_pages[] = {"Controls", "Display", "Audio", "Performance",
#ifdef VERSION_G8MP01
                                      "Language",
#endif
                                      "Debug", NULL};
enum {
    PAGE_CONTROLS,
    PAGE_DISPLAY,
    PAGE_AUDIO,
    PAGE_PERF,
#ifdef VERSION_G8MP01
    PAGE_LANG,
#endif
    PAGE_DEBUG, /* the last page */
};

/* Pad button keys, in the order of the pad layer's GC button enum (pad.c). */
static const char* const k_pad_keys[8] = {"pad_a", "pad_b", "pad_x", "pad_y", "pad_z", "pad_l", "pad_r", "pad_start"};

#define PAD_ITEM(k, label, def_src) \
    {k, label, PLAT_OPT_CHOICE, PAGE_CONTROLS, def_src, 0, 0, 0, k_src_names, NULL}

static const PlatOptItem k_items[] = {
    PAD_ITEM("pad_a", "GC A button", SRC_A),
    PAD_ITEM("pad_b", "GC B button", SRC_B),
    PAD_ITEM("pad_x", "GC X button", SRC_X),
    PAD_ITEM("pad_y", "GC Y button", SRC_Y),
    PAD_ITEM("pad_z", "GC Z button", SRC_ZR),
    PAD_ITEM("pad_l", "GC L trigger", SRC_L),
    PAD_ITEM("pad_r", "GC R trigger", SRC_R),
    PAD_ITEM("pad_start", "GC Start", SRC_START),
    {"cstick", "C-stick is", PLAT_OPT_CHOICE, PAGE_CONTROLS, 0, 0, 0, 0, k_cstick_names, NULL},
    {NULL, "Reset to defaults", PLAT_OPT_RESET, PAGE_CONTROLS, 0, 0, 0, 0, NULL, NULL},

    {PLAT_OPT_KEY_STEREO3D, "3D effect", PLAT_OPT_CHOICE, PAGE_DISPLAY, 1, 0, 0, 0, k_onoff_names, NULL},
    {PLAT_OPT_KEY_HUD, "Bottom screen HUD", PLAT_OPT_CHOICE, PAGE_DISPLAY, PLAT_OPT_HUD_FULL, 0, 0, 0, k_hud_names, NULL},

    {PLAT_OPT_KEY_TEXFILTER, "Texture filter", PLAT_OPT_CHOICE, PAGE_DISPLAY, PLAT_OPT_TEXFILTER_GAME, 0, 0, 0,
     k_texfilter_names, NULL},

    {PLAT_OPT_KEY_VOLUME, "Master volume", PLAT_OPT_RANGE, PAGE_AUDIO, 100, 0, 100, 10, NULL, "%"},

    {PLAT_OPT_KEY_QUALITY, "Quality", PLAT_OPT_CHOICE, PAGE_PERF, PLAT_OPT_QUALITY_AUTO, 0, 0, 0, k_quality_names, NULL},
#ifdef VERSION_G8MP01
    {PLAT_OPT_KEY_LANGUAGE, "Game language", PLAT_OPT_CHOICE, PAGE_LANG, 0, 0, 0, 0, k_lang_names, NULL},
#endif

    {PLAT_OPT_KEY_DEBUGMODE, "Debug mode", PLAT_OPT_CHOICE, PAGE_DEBUG, PLAT_OPT_DEBUG_AUTO, 0, 0, 0, k_debug_names, NULL},
};

static void on_change(const char* key, int32_t value) {
    static int32_t applied[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (int i = 0; i < 8; i++) {
        if (strcmp(key, k_pad_keys[i]) == 0) {
            port_pad_set_button_source(i, k_src_keys[value]);
            if (applied[i] != value) { /* PADInit may run more than once */
                plat_log(PLAT_LOG_INFO, "options: GC %s <- 3DS %s", k_pad_keys[i] + 4, k_src_names[value]);
                applied[i] = value;
            }
            return;
        }
    }
    if (strcmp(key, "cstick") == 0) {
        port_pad_set_cstick_dpad(value == 1);
    }
    /* volume: applied by the platform (plat_options_tick); 3D, HUD, quality and
     * language are read through their opt_get_* getters. */
}

static const PlatOptMenu k_menu = {k_pages, k_items, (int32_t)(sizeof(k_items) / sizeof(k_items[0])), on_change};

/* PADInit calls this: registers the list, which also applies the stored values. */
void port_options_init(void) { plat_options_register(&k_menu); }
