/* Bottom screen: HUD, Select menu and the scrolling log.
 *
 * Layout of the 40x30 libctru console on the bottom screen:
 *   rows 1-2   HUD (build, map, FPS, frame and GPU times, draws, vertices,
 *              memory, QTM, model), refreshed twice a second
 *   rows 4-15  the Select menu while it is open
 *   rows 17-30 the log (plat/log.c prints there)
 * Only changed lines are rewritten, so the screen does not flicker.
 *
 * Select opens and closes the menu. L/R change tab, Up/Down move, Left/
 * Right/A change a value or run an action, B closes. While it is open the
 * game sees no input.
 */
#include <stdio.h>
#include <string.h>

#include "plat/hud.h"
#include "plat_internal.h"

enum { TAB_GENERAL, TAB_DEBUG, TAB_COUNT };

static int s_console_up;
static int s_open, s_tab, s_row;
static int s_hud_on = 1;
static int s_exit_armed;
static volatile int s_reset_request;
static char s_warp_map[16], s_warp_bero[16];
static volatile int s_warp_pending;
static const char* const* s_maps;
static int s_map_count, s_map_index;
static PlatHudStats s_stats;
static uint32_t s_last_hud_ms, s_last_frames;
static float s_fps;
static PrintConsole s_console;

void plat_console_init(void) {
    if (s_console_up) {
        return;
    }
    consoleInit(GFX_BOTTOM, &s_console);
    consoleSetWindow(&s_console, 0, 16, 40, 14);
    s_console_up = 1;
    plat_log_console_ready();
}

static void at(int row, const char* text) {
    /* row is 1-based in the full 30-row screen; the window covers the log */
    printf("\x1b[s");
    consoleSetWindow(&s_console, 0, 0, 40, 30);
    printf("\x1b[%d;1H%-40.40s", row, text);
    consoleSetWindow(&s_console, 0, 16, 40, 14);
    printf("\x1b[u");
}

int plat_reset_requested(void) {
    if (s_reset_request) {
        s_reset_request--; /* held for a few frames, as a button press is */
        return 1;
    }
    return 0;
}

void plat_set_map_list(const char* const* maps, int count) {
    s_maps = maps;
    s_map_count = count;
}

int plat_take_warp_request(char* map, uint32_t map_size, char* bero, uint32_t bero_size) {
    if (!__atomic_exchange_n(&s_warp_pending, 0, __ATOMIC_ACQ_REL)) {
        return 0;
    }
    snprintf(map, map_size, "%s", s_warp_map);
    snprintf(bero, bero_size, "%s", s_warp_bero);
    return 1;
}

static const char* const s_level_names[] = {"error", "warn", "info", "debug"};

static int rows_in_tab(void) { return s_tab == TAB_GENERAL ? 3 : 3; }

static void draw_menu(void) {
    if (!s_console_up || !plat_console_trylock()) {
        return;
    }
    char line[48];
    snprintf(line, sizeof(line), " Options  %s %s  (L/R tab)", s_tab == TAB_GENERAL ? "[General]" : " General ",
             s_tab == TAB_DEBUG ? "[Debug]" : " Debug ");
    at(4, line);
    for (int r = 0; r < 10; r++) {
        line[0] = '\0';
        const char* cur = r == s_row ? ">" : " ";
        if (s_tab == TAB_GENERAL) {
            switch (r) {
            case 0: snprintf(line, sizeof(line), "%s HUD: %s", cur, s_hud_on ? "on" : "off"); break;
            case 1: snprintf(line, sizeof(line), "%s Return to title", cur); break;
            case 2:
                snprintf(line, sizeof(line), "%s Exit to HOME Menu%s", cur, s_exit_armed ? " (A again)" : "");
                break;
            default: break;
            }
        } else {
            switch (r) {
            case 0: snprintf(line, sizeof(line), "%s Log level: %s", cur, s_level_names[plat_log_level()]); break;
            case 1:
                snprintf(line, sizeof(line), "%s Warp to: %s", cur,
                         s_map_count ? s_maps[s_map_index] : "(no map list yet)");
                break;
            case 2: snprintf(line, sizeof(line), "%s Warp now", cur); break;
            default: break;
            }
        }
        at(6 + r, line);
    }
    plat_console_unlock();
}

static void clear_menu(void) {
    if (!s_console_up || !plat_console_trylock()) {
        return;
    }
    for (int r = 4; r <= 15; r++) {
        at(r, "");
    }
    plat_console_unlock();
}

static void activate(int dir) {
    if (s_tab == TAB_GENERAL) {
        switch (s_row) {
        case 0: s_hud_on = !s_hud_on; break;
        case 1:
            if (dir == 0) {
                s_reset_request = 4;
                s_open = 0;
                PLOG_I("menu: return to title");
            }
            break;
        case 2:
            if (dir == 0) {
                if (s_exit_armed) {
                    PLOG_I("menu: exit");
                    plat_shutdown();
                }
                s_exit_armed = 1;
            }
            break;
        default: break;
        }
    } else {
        switch (s_row) {
        case 0: plat_log_set_level(plat_log_level() + (dir < 0 ? -1 : 1)); break;
        case 1:
            if (s_map_count) {
                s_map_index = (s_map_index + (dir < 0 ? s_map_count - 1 : 1)) % s_map_count;
            }
            break;
        case 2:
            if (dir == 0 && s_map_count) {
                snprintf(s_warp_map, sizeof(s_warp_map), "%s", s_maps[s_map_index]);
                s_warp_bero[0] = '\0';
                __atomic_store_n(&s_warp_pending, 1, __ATOMIC_RELEASE);
                s_open = 0;
                PLOG_I("menu: warp to %s", s_warp_map);
            }
            break;
        default: break;
        }
    }
}

int plat_menu_input(uint32_t held, uint32_t down) {
    (void)held;
    if (down & KEY_SELECT) {
        s_open = !s_open;
        s_exit_armed = 0;
        if (!s_open) {
            clear_menu();
        }
    }
    if (!s_open) {
        return 0;
    }
    if (down & KEY_B) {
        s_open = 0;
        clear_menu();
        return 1;
    }
    if (down & (KEY_L | KEY_R)) {
        s_tab = (s_tab + 1) % TAB_COUNT;
        s_row = 0;
        s_exit_armed = 0;
    }
    if (down & KEY_DUP) {
        s_row = (s_row + rows_in_tab() - 1) % rows_in_tab();
        s_exit_armed = 0;
    }
    if (down & KEY_DDOWN) {
        s_row = (s_row + 1) % rows_in_tab();
        s_exit_armed = 0;
    }
    if (down & KEY_DLEFT) {
        activate(-1);
    }
    if (down & KEY_DRIGHT) {
        activate(1);
    }
    if (down & KEY_A) {
        activate(0);
    }
    if (s_open) {
        draw_menu();
    } else {
        clear_menu();
    }
    return 1;
}

void plat_hud_frame(const PlatHudStats* stats) {
    s_stats = *stats;
    plat_set_map_name(stats->map);
    uint32_t now = plat_uptime_ms();
    if (now - s_last_hud_ms < 500) {
        return;
    }
    uint32_t dt = now - s_last_hud_ms;
    s_fps = dt ? (float)(stats->frames - s_last_frames) * 1000.0f / (float)dt : 0.0f;
    s_last_hud_ms = now;
    s_last_frames = stats->frames;
    if (!s_console_up || !s_hud_on || !plat_console_trylock()) {
        return;
    }
    char line[48];
    snprintf(line, sizeof(line), "%s %-8s %4.1ffps %s", plat_build_string(), s_stats.map, (double)s_fps,
             plat_qtm_available() ? "QTM" : "QTM:off");
    at(1, line);
    snprintf(line, sizeof(line), "cpu%5.1f gpu%5.1fms d%4lu v%5lu h%luK", (double)s_stats.frame_us / 1000.0,
             (double)s_stats.gpu_us / 1000.0, (unsigned long)s_stats.draws, (unsigned long)s_stats.vertices,
             (unsigned long)s_stats.heap_free_kb);
    at(2, line);
    plat_console_unlock();
}
