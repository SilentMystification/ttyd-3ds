/* Player options: a generic settings store and bottom-screen menu (plat/options.c,
 * plat/options_menu.c). The menu engine knows nothing about any game: the game
 * hands it a list of items (port3ds/game/options_ttyd.c for TTYD), and the
 * values live in sdmc:/3ds/ttyd/options.txt, never in the save file.
 *
 * The file is text, one "key=value" per line (integers; a choice stores its
 * index), written through a temp file and a rename. A missing, truncated or
 * corrupt file gives the defaults and a log line.
 *
 * Opening the menu: SELECT, or a touch on the "Options" button on the bottom
 * screen. The game is paused while it is open. */
#ifndef PLAT_OPTIONS_H
#define PLAT_OPTIONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Item list (the game's data) ---- */
enum {
    PLAT_OPT_CHOICE = 0, /* value is an index into names[] (0..count-1) */
    PLAT_OPT_RANGE = 1,  /* value min..max in steps of step, shown with suffix */
    PLAT_OPT_RESET = 2   /* action: every item on the same page returns to its default */
};

typedef struct {
    const char* key;          /* file key, <= 23 chars; NULL for PLAT_OPT_RESET */
    const char* label;        /* shown in the menu, <= 18 chars */
    int32_t kind;             /* PLAT_OPT_* */
    int32_t page;             /* index into the menu's page titles */
    int32_t def;              /* default value */
    int32_t min, max, step;   /* RANGE only (a CHOICE counts its names) */
    const char* const* names; /* CHOICE: the value names, NULL-terminated */
    const char* suffix;       /* RANGE: unit text, may be NULL */
} PlatOptItem;

typedef struct {
    const char* const* pages; /* page titles, NULL-terminated, <= 12 chars each */
    const PlatOptItem* items; /* in display order */
    int32_t item_count;
    /* Called with an item's key and value when it changes, and once per item
     * after the file is read, so the game applies its settings right away. */
    void (*on_change)(const char* key, int32_t value);
} PlatOptMenu;

/* The game registers its list once, early (before the first pad read).
 * Applies the stored values through on_change. */
void plat_options_register(const PlatOptMenu* menu);

/* ---- Generic access ---- */
/* The value of key: the stored one when valid, else the registered default,
 * else fallback. Reads the file on the first call. */
int plat_opt_get(const char* key, int fallback);
/* Sets a value (menu or code): applies it, marks the file for writing. */
void plat_opt_set(const char* key, int value);
/* Writes options.txt now if anything changed (the menu does so when it closes). */
void plat_options_save(void);

/* ---- Getters for everyone else ---- */
#define PLAT_OPT_HUD_OFF 0
#define PLAT_OPT_HUD_FPS 1
#define PLAT_OPT_HUD_FULL 2
#define PLAT_OPT_QUALITY_AUTO 0
#define PLAT_OPT_QUALITY_HIGH 1
#define PLAT_OPT_QUALITY_FAST 2
/* Texture filtering (key "texfilter"): GX's own filters as the game sets them, bilinear forced,
 * bilinear plus mip levels (GX's own, else generated at upload), or point sampling. */
#define PLAT_OPT_TEXFILTER_GAME 0
#define PLAT_OPT_TEXFILTER_LINEAR 1
#define PLAT_OPT_TEXFILTER_MIP 2
#define PLAT_OPT_TEXFILTER_NEAREST 3

/* Keys the platform itself acts on or that library users read. A game's list
 * should use them for these settings. */
#define PLAT_OPT_KEY_STEREO3D "stereo3d"
#define PLAT_OPT_KEY_HUD "hud"
#define PLAT_OPT_KEY_DEVUPDATE "devupdate" /* developer Select menu: update server on(1)/off(0); unset = the build default */
#define PLAT_OPT_KEY_VOLUME "volume"
#define PLAT_OPT_KEY_QUALITY "quality"
#define PLAT_OPT_KEY_LANGUAGE "language"
#define PLAT_OPT_KEY_TEXFILTER "texfilter"

/* 1 when the player wants stereoscopic 3D (default 1; the 3D slider still
 * decides the depth). */
int opt_get_stereo3d(void);
/* PLAT_OPT_HUD_* (default full). The bottom-screen status lines. */
int opt_get_hud_mode(void);
/* Master volume 0..100 (default 100). The platform applies it to NDSP itself. */
int opt_get_volume(void);
/* PLAT_OPT_QUALITY_* (default auto). Performance features read this. */
int opt_get_quality(void);
/* Language index, PAL only (default 0). The game side maps it to its languages. */
int opt_get_language(void);
/* PLAT_OPT_TEXFILTER_* (default game). The debug switch texfilter= overrides it in the GX texture code. */
int opt_get_texfilter(void);

#ifdef __cplusplus
}
#endif

#endif
