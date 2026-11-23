/* Bottom screen: HUD, the scrolling log and the menus.
 *
 * One 40x30 libctru console, laid out like the PM64 port's:
 *   rows 1-2    HUD: two status lines, refreshed twice a second, trylock only
 *               (the game thread never waits). The HUD option (off, FPS only,
 *               full) decides what they show.
 *   rows 3-30   the log (plat/log.c prints there)
 * When a menu opens (Options, or the developer Select menu) the screen changes
 * mode: the menu takes rows 3-14 and the log shrinks to rows 16-30, where it
 * keeps scrolling. On close the menu rows are blanked and the log gets rows
 * 3-30 back. Lines are rewritten padded, never cleared, so there is no blank frame.
 * Touching the top two rows (the HUD, which shows an [Options] label when it has room)
 * opens the Options menu.
 *
 * Two menus share the menu area:
 *  - the player Options menu (plat/options_menu.c), opened with SELECT or by
 *    touching the HUD rows. The game is paused while it is open.
 *  - this file's developer Select menu, only with the switch "devmenu" (or a
 *    build with PORT3DS_DEV_BUILD): ZL+SELECT opens and closes it. L/R change
 *    tab, Up/Down move, Left/Right/A change a value or run an action, B
 *    closes. While it is open the game sees no input but keeps running.
 *
 * Test aid: switch shotbottom=N saves the bottom screen as shots/bNNNNN.ppm
 * every N ticks (game frames, and menu polls while the Options menu is open).
 */
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

#include "options_internal.h"
#include "plat/hud.h"
#include "plat/input.h"
#include "plat_internal.h"

enum { TAB_GENERAL, TAB_DEBUG, TAB_COUNT };

static int s_console_up;
static int s_open, s_tab, s_row;
static int s_hud_drawn = -1; /* HUD mode whose rows are on screen */
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

#define LOG_TOP 2     /* log window top (0-based row) with no menu: rows 3-30 */
#define LOG_TOP_MENU 15 /* with a menu: rows 16-30 */
#define MENU_FIRST 3  /* 1-based, first and last row the menu owns */
#define MENU_LAST 14

static int s_menu_mode;

static void log_window(int menu) {
    int top = menu ? LOG_TOP_MENU : LOG_TOP;
    consoleSetWindow(&s_console, 1, top + 1, 40, 30 - top); /* libctru window and cursor coordinates are 1-based */
    s_console.cursorX = 1; /* the log's next line comes at the bottom */
    s_console.cursorY = 30 - top;
}

void plat_console_init(void) {
    if (s_console_up) {
        return;
    }
    consoleInit(GFX_BOTTOM, &s_console);
    log_window(0);
    s_console.cursorY = 1; /* boot lines fill the window from its top */
    s_console_up = 1;
    plat_log_console_ready();
}

/* Writes one padded line of the 30-row screen without disturbing the log's
 * window or cursor. Caller holds the console lock. row is 1-based. */
static void at(int row, const char* text) {
    int wx = s_console.windowX, wy = s_console.windowY, ww = s_console.windowWidth, wh = s_console.windowHeight;
    int cx = s_console.cursorX, cy = s_console.cursorY;
    consoleSetWindow(&s_console, 1, 1, 40, 30);
    s_console.cursorX = 1;
    s_console.cursorY = row;
    printf("%-40.40s", text); /* all 40 columns, so no stale log text stays in the last one */
    consoleSetWindow(&s_console, wx, wy, ww, wh);
    s_console.cursorX = cx;
    s_console.cursorY = cy;
}

/* A menu is opening (on) or closed (off): the log window shrinks or grows.
 * Caller holds the console lock. */
void plat_console_menu_mode(int on) {
    if (!s_console_up || s_menu_mode == on) {
        return;
    }
    s_menu_mode = on;
    if (!on) {
        for (int r = MENU_FIRST; r <= MENU_LAST + 1; r++) {
            at(r, "");
        }
    }
    log_window(on);
}

void plat_console_row(int row, const char* text) { at(row, text); }
int plat_console_ready(void) { return s_console_up; }

/* The developer menu is for dev builds and the "devmenu" switch; players get Options. */
static int dev_menu_enabled(void) {
#ifdef PORT3DS_DEV_BUILD
    return 1;
#else
    return plat_switch_has("devmenu");
#endif
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
    plat_console_menu_mode(1);
    at(MENU_FIRST, line);
    for (int r = 0; r < 9; r++) {
        line[0] = '\0';
        const char* cur = r == s_row ? ">" : " ";
        if (s_tab == TAB_GENERAL) {
            switch (r) {
            case 0:
                snprintf(line, sizeof(line), "%s HUD: %s", cur,
                         (const char* const[]){"off", "fps", "full"}[opt_get_hud_mode()]);
                break;
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
        at(MENU_FIRST + 2 + r, line);
    }
    plat_console_unlock();
}

static void clear_menu(void) {
    if (!s_console_up) {
        return;
    }
    plat_console_lock(); /* the rows must be blanked: wait for the log writer */
    plat_console_menu_mode(0);
    plat_console_unlock();
}

static void activate(int dir) {
    if (s_tab == TAB_GENERAL) {
        switch (s_row) {
        case 0: plat_opt_set(PLAT_OPT_KEY_HUD, (opt_get_hud_mode() + (dir < 0 ? 2 : 1)) % 3); break;
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
                    plat_quit(0);
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
    if (plat_options_menu_is_open()) {
        return 1; /* opened this frame; the frame hook runs it */
    }
    int touch_button = 0;
    if (down & PLAT_KEY_TOUCH) {
        touchPosition tp;
        hidTouchRead(&tp);
        touch_button = tp.py < 16; /* the HUD rows */
    }
    if (down & PLAT_KEY_SELECT) {
        if (s_open) {
            s_open = 0;
            clear_menu();
        } else if (dev_menu_enabled() && (held & PLAT_KEY_ZL)) {
            s_open = 1;
            s_exit_armed = 0;
        } else {
            touch_button = 1;
        }
    }
    if (touch_button && !s_open) {
        plat_options_menu_open();
        return plat_options_menu_is_open();
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
    if (!s_console_up || !plat_console_trylock()) {
        return;
    }
    int mode = opt_get_hud_mode();
    char line[48];
    if (mode == PLAT_OPT_HUD_FULL) {
        snprintf(line, sizeof(line), "%s %-8s %4.1ffps %s", plat_build_string(), s_stats.map, (double)s_fps,
                 plat_qtm_available() ? "QTM" : "QTM:off");
        at(1, line);
        snprintf(line, sizeof(line), "cpu%5.1f gpu%5.1fms d%4lu v%5lu h%luK", (double)s_stats.frame_us / 1000.0,
                 (double)s_stats.gpu_us / 1000.0, (unsigned long)s_stats.draws, (unsigned long)s_stats.vertices,
                 (unsigned long)s_stats.heap_free_kb);
        at(2, line);
    } else if (mode == PLAT_OPT_HUD_FPS) {
        snprintf(line, sizeof(line), "%4.1f fps", (double)s_fps);
        at(1, line);
        if (s_hud_drawn != mode) {
            at(2, "");
        }
    } else if (s_hud_drawn != mode) {
        at(1, "");
        at(2, "");
    }
    if (mode != PLAT_OPT_HUD_FULL && !s_open) {
        at(mode == PLAT_OPT_HUD_FPS ? 2 : 1, "                               [Options]");
    }
    s_hud_drawn = mode;
    plat_console_unlock();
}

/* From the frame hook (plat/input.c): the player Options menu runs here, so the
 * game thread (and game time) stands still while it is open. */
void plat_menu_frame(void) {
    if (!plat_options_menu_is_open()) {
        return;
    }
    plat_options_menu_run();
    clear_menu();
    s_hud_drawn = -1; /* the HUD rows are rewritten at the next refresh */
    s_last_hud_ms = 0;
}

/* shotbottom=N: the bottom framebuffer as a PPM, every N ticks, at most 60. */
void plat_menu_shot_tick(void) {
    static int every = -1, ticks, taken;
    if (every < 0) {
        every = plat_switch_int("shotbottom", 0);
    }
    if (every <= 0 || !s_console_up || ++ticks % every != 0 || taken >= 60) {
        return;
    }
    u16 w, h;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h); /* column major, bottom row first */
    int bpp = gfxGetScreenFormat(GFX_BOTTOM) == GSP_BGR8_OES ? 3 : 2;
    char path[48];
    snprintf(path, sizeof(path), "shots/b%05d.ppm", ticks);
    mkdir("shots", 0777);
    FILE* f = fb ? fopen(path, "wb") : NULL;
    if (f == NULL) {
        return;
    }
    plat_console_lock();
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)h, (unsigned)w);
    for (unsigned y = 0; y < w; y++) {
        for (unsigned x = 0; x < h; x++) {
            const u8* px = fb + ((size_t)x * w + (w - 1 - y)) * (size_t)bpp;
            unsigned r, g, b;
            if (bpp == 3) {
                b = px[0]; g = px[1]; r = px[2];
            } else {
                unsigned c = px[0] | (px[1] << 8);
                r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31;
            }
            fputc((int)r, f); fputc((int)g, f); fputc((int)b, f);
        }
    }
    plat_console_unlock();
    fclose(f);
    taken++;
    PLOG_I("shotbottom: %s menu=%d", path, s_menu_mode);
}
