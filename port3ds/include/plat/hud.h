/* Bottom-screen HUD and debug menu data from the game side (plat/menu.c). */
#ifndef PLAT_HUD_H
#define PLAT_HUD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frames;        /* game frames since boot */
    uint32_t frame_us;      /* last frame's game thread time (update + draw record) */
    uint32_t render_us;     /* last frame's GPU submission time (render thread) */
    uint32_t gpu_us;        /* last frame's GPU drawing time */
    uint32_t draws;         /* draw calls last frame */
    uint32_t vertices;      /* vertices last frame */
    uint32_t tex_uploads;   /* textures converted last frame */
    uint32_t heap_free_kb;  /* game smart heap free */
    char map[16];           /* current map name */
} PlatHudStats;

/* Once per game frame from the game thread. */
void plat_hud_frame(const PlatHudStats* stats);

/* Warp requests from the debug menu: returns 1 and the map name (and the
 * entrance, may be empty) when the menu asked for a warp since last time. */
int plat_take_warp_request(char* map, uint32_t map_size, char* bero, uint32_t bero_size);
/* The game side's list of maps on the disc, for the warp menu ("gor_01"...). */
void plat_set_map_list(const char* const* maps, int count);

/* Bottom screen console, once gfx is up (GPU back end). */
void plat_console_init(void);

#ifdef __cplusplus
}
#endif

#endif
