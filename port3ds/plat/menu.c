/* Bottom screen: HUD, the scrolling log and the menus.
 *
 * One 40x30 libctru console (layout at the LOG_FIRST define below): a HUD block on top,
 * the scrolling log in the middle, a build row at the bottom. The HUD refreshes twice a
 * second with trylock only (the game thread never waits); the HUD option (off, FPS only,
 * full) decides what it shows. A menu (Options, or the developer Select menu) or the
 * updater screen covers the screen; the log keeps collecting in a buffer and the close
 * repaints everything from the buffers, changed lines only, so there is no blank frame.
 * Touching the HUD rows (they show an [Options] label when there is room) opens Options.
 *
 * Two menus share the menu area:
 *  - the player Options menu (plat/options_menu.c), opened with SELECT or by
 *    touching the HUD rows. The game is paused while it is open.
 *  - this file's developer Select menu, with the switch "devmenu", the Options
 *    "Debug mode" setting (on, or auto in a development build), see
 *    dev_menu_enabled(): ZL+SELECT opens and closes it. L/R change
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
#include <malloc.h>

#include <3ds.h>

#include "plat/gpu.h"
#include "plat/hud.h"
#include "plat/input.h"
#include "plat_internal.h"
#include "port3ds_build.h"

extern u32 __ctru_heap_size, __ctru_linear_heap_size;
#define VRAM_TOTAL_KB 6144

enum { TAB_GENERAL, TAB_DEBUG, TAB_COUNT };

static int s_console_up;
static int s_open, s_tab, s_row;
static int s_exit_armed;
static volatile int s_reset_request;
static char s_warp_map[16], s_warp_bero[16];
static volatile int s_warp_pending;
static const char* const* s_maps;
static int s_map_count, s_map_index;
static PlatHudStats s_stats;
static uint32_t s_last_hud_ms, s_last_frames, s_last_presented;
static float s_fps;
static PrintConsole s_console;

/* Screen layout (1-based rows of the 40x30 console):
 *   1-5    HUD: frame rate and frame time, CPU and GPU times, RAM, VRAM, QTM
 *   6-8    blank, a line of '-', blank
 *   9-26   the log (18 rows, scrolling)
 *   27-29  blank, a line of '-', blank
 *   30     the build: version, build number and hash
 * HUD off blanks rows 1-5 (the touch label stays on row 1); the separators and the build row stay. */
#define COLS 40
#define LOG_ROWS 28 /* rows of log text kept; the last LOG_VIS are on the screen */
#define LOG_FIRST 9
#define LOG_LAST 26
#define LOG_VIS (LOG_LAST - LOG_FIRST + 1)
#define HUD_ROWS 5
#define FOOT_FIRST 27

/* The console writes the bottom frame buffer with the CPU (write-back cache). Normally the game's frames flush
 * it (C3D_FrameEnd flushes the whole linear heap), but while the game thread is paused (updater, menus) no frame
 * runs, so a draw stays in the cache and the screen shows the old picture. Flushes the bottom buffer only. */
static void present_bottom(void) {
    u16 w, h;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(GFX_BOTTOM);
    u32 bpp = fmt == GSP_RGBA8_OES ? 4 : fmt == GSP_BGR8_OES ? 3 : 2;
    if (fb != NULL) {
        GSPGPU_FlushDataCache(fb, (u32)w * h * bpp);
    }
}

static int s_menu_mode;
static int s_upd, s_upd_draw, s_upd_prev_menu; /* the updater screen is up; it is drawing; a menu was open under it */
static void draw_menu(void);

/* What the screen should show, kept as text so a menu can cover it and the close can
 * repaint it exactly: the HUD's lines and the last LOG_ROWS rows of the log
 * (wrapped at 40 columns; the last one is the row being written). */
static char s_hud_line[HUD_ROWS][COLS + 1];
static char s_log[LOG_ROWS][COLS + 1];
static int s_log_col; /* characters in the last row */
static char s_build_row[COLS + 1];
static const char k_dashes[] = "----------------------------------------";
/* What is on the screen for rows 1-30 as far as at() knows: a line is rewritten only when it differs. Row 0 of an entry is '\1' when unknown. */
static char s_shadow[30][COLS + 1];

static void log_window(void) {
    consoleSetWindow(&s_console, 1, LOG_FIRST, COLS, LOG_VIS); /* libctru window and cursor coordinates are 1-based */
    s_console.cursorX = s_log_col + 1; /* the next text continues the last row, at the bottom */
    s_console.cursorY = LOG_VIS;
}

/* Writes one padded line of the 30-row screen without disturbing the log's
 * window or cursor, unless it is already there. Caller holds the console lock. row is 1-based. */
static void at(int row, const char* text) {
    if (s_upd && !s_upd_draw) {
        return; /* the updater screen owns the rows */
    }
    char buf[COLS + 1];
    snprintf(buf, sizeof(buf), "%-40.40s", text);
    if (strcmp(buf, s_shadow[row - 1]) == 0) {
        return;
    }
    memcpy(s_shadow[row - 1], buf, sizeof(buf));
    int wx = s_console.windowX, wy = s_console.windowY, ww = s_console.windowWidth, wh = s_console.windowHeight;
    int cx = s_console.cursorX, cy = s_console.cursorY;
    consoleSetWindow(&s_console, 1, 1, COLS, 30);
    s_console.cursorX = 1;
    s_console.cursorY = row;
    fputs(buf, stdout);
    consoleSetWindow(&s_console, wx, wy, ww, wh);
    s_console.cursorX = cx;
    s_console.cursorY = cy;
}

/* The whole normal screen from the buffers, changed lines only; the log window is set again.
 * Caller holds the console lock. */
static void repaint(int force) {
    if (force) {
        memset(s_shadow, 1, sizeof(s_shadow)); /* every row is written, whatever at() believes is there */
    }
    for (int r = 0; r < HUD_ROWS; r++) {
        at(1 + r, s_hud_line[r]);
    }
    at(HUD_ROWS + 1, "");
    at(HUD_ROWS + 2, k_dashes);
    at(HUD_ROWS + 3, "");
    for (int r = LOG_FIRST; r <= LOG_LAST; r++) {
        at(r, s_log[LOG_ROWS - 1 - (LOG_LAST - r)]);
    }
    at(FOOT_FIRST, "");
    at(FOOT_FIRST + 1, k_dashes);
    at(FOOT_FIRST + 2, "");
    at(FOOT_FIRST + 3, s_build_row);
    log_window();
    for (int r = LOG_FIRST - 1; r < LOG_LAST; r++) {
        s_shadow[r][0] = 1; /* the log scrolls again */
    }
}

void plat_console_init(void) {
    if (s_console_up) {
        return;
    }
    consoleInit(GFX_BOTTOM, &s_console);
    snprintf(s_build_row, sizeof(s_build_row), "v%s %s", PORT3DS_VERSION, plat_build_string());
    memset(s_shadow, 1, sizeof(s_shadow));
    s_console_up = 1;
    s_log_col = 0;
    repaint(0);
    s_console.cursorY = 1; /* boot lines fill the window from its top */
    plat_log_console_ready();
}

/* The console writer (plat/log.c) hands every log text here, under the console lock.
 * It always goes into the row buffer; it reaches the screen only when no menu covers it. */
void plat_console_log(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || s_log_col >= COLS) {
            memmove(s_log[0], s_log[1], sizeof(s_log) - sizeof(s_log[0]));
            s_log[LOG_ROWS - 1][0] = '\0';
            s_log_col = 0;
            if (c == '\n') {
                continue;
            }
        }
        s_log[LOG_ROWS - 1][s_log_col++] = (c >= 32 && c < 127) ? c : '?';
        s_log[LOG_ROWS - 1][s_log_col] = '\0';
    }
    if (!s_menu_mode) {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
        for (int r = LOG_FIRST - 1; r < LOG_LAST; r++) {
            s_shadow[r][0] = 1; /* the console scrolled: the screen text is no longer known */
        }
        if (s_upd) {
            present_bottom(); /* no game frame flushes it while the updater has the game paused */
        }
    }
}

/* A menu opens (on): it covers the whole screen and the log goes to the buffer only.
 * Closed (off): the screen is repainted from the buffers, changed lines only.
 * Caller holds the console lock. */
void plat_console_menu_mode(int on) {
    if (!s_console_up || s_menu_mode == on) {
        return;
    }
    if (s_upd) {
        s_upd_prev_menu = on; /* the updater screen stays; the menu state returns with the old screen */
        return;
    }
    s_menu_mode = on;
    if (!on) {
        repaint(0);
    }
}

/* A HUD line: remembered, and drawn unless a menu covers the screen. */
static void hud_line(int row, const char* text) {
    snprintf(s_hud_line[row - 1], sizeof(s_hud_line[0]), "%-40.40s", text);
    if (!s_menu_mode && !s_upd) {
        at(row, text);
    }
}

/* The updater screen (devupdate): rows 1-7 are its title, build line, status line and progress bar, the log
 * keeps scrolling in the rows below. Off puts back exactly what was on the screen before. Called from the
 * updater thread while the game thread is paused. */
void plat_updater_screen(int on) {
    if (!s_console_up) {
        return;
    }
    plat_console_lock();
    if (on && !s_upd) {
        s_upd_prev_menu = s_menu_mode;
        s_menu_mode = 0;
        repaint(1); /* the log and footer back first (a menu may have covered them) */
        s_upd = 1;
        s_upd_draw = 1;
        for (int r = 1; r < LOG_FIRST; r++) {
            at(r, "");
        }
        at(1, "Updater");
        at(LOG_FIRST - 1, k_dashes);
        s_upd_draw = 0;
    } else if (!on && s_upd) {
        s_upd = 0;
        s_menu_mode = s_upd_prev_menu;
        if (!s_menu_mode) {
            repaint(1);
        } else {
            for (int r = 0; r < 30; r++) {
                s_shadow[r][0] = 1; /* the menu draws itself again */
            }
        }
    }
    present_bottom();
    plat_console_unlock();
    if (!on && s_menu_mode) {
        plat_options_menu_redraw();
        if (s_open) {
            draw_menu();
        }
    }
}

void plat_updater_draw(const char* title, const char* build, const char* line, int percent) {
    if (!s_console_up || !s_upd) {
        return;
    }
    char bar[COLS + 1];
    int pc = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    int fill = pc * 30 / 100;
    int i = 0;
    bar[i++] = '[';
    for (int k = 0; k < 30; k++) {
        bar[i++] = k < fill ? '#' : '-';
    }
    snprintf(bar + i, sizeof(bar) - (size_t)i, "] %3d%%", pc);
    char head[COLS + 1];
    snprintf(head, sizeof(head), "%s", title);
    plat_console_lock();
    s_upd_draw = 1;
    at(1, head);
    at(2, build);
    at(4, line);
    at(6, bar);
    s_upd_draw = 0;
    present_bottom();
    plat_console_unlock();
    plat_menu_shot_tick(); /* test aid: shotbottom=N also counts the updater's redraws */
}

void plat_console_row(int row, const char* text) { at(row, text); }
int plat_console_ready(void) { return s_console_up; }

/* The developer menu: the "devmenu" switch, or the Options "Debug mode" setting
 * (on always, off never, auto = on in development builds only). Players get Options. */
static int dev_menu_enabled(void) {
    if (plat_switch_has("devmenu")) {
        return 1;
    }
    int mode = plat_opt_get(PLAT_OPT_KEY_DEBUGMODE, PLAT_OPT_DEBUG_AUTO);
    if (mode == PLAT_OPT_DEBUG_ON) {
        return 1;
    }
    if (mode == PLAT_OPT_DEBUG_OFF) {
        return 0;
    }
    return plat_is_dev_build();
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

static int rows_in_tab(void) { return s_tab == TAB_GENERAL ? 5 : 3; }

static void draw_menu(void) {
    if (!s_console_up || !plat_console_trylock()) {
        return;
    }
    char line[48];
    snprintf(line, sizeof(line), " Options  %s %s  (L/R tab)", s_tab == TAB_GENERAL ? "[General]" : " General ",
             s_tab == TAB_DEBUG ? "[Debug]" : " Debug ");
    plat_console_menu_mode(1);
    at(1, line);
    at(2, "");
    for (int r = 0; r < 28; r++) {
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
            case 3: {
                char addr[24];
                plat_devupdate_address(addr, sizeof(addr));
                snprintf(line, sizeof(line), "%s Update server: %s %s", cur, plat_devupdate_wanted() ? "on" : "off",
                         plat_devupdate_wanted() != plat_devupdate_enabled() ? "(after relaunch)" : addr);
                break;
            }
            case 4: snprintf(line, sizeof(line), "%s Relaunch (A)", cur); break;
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
        at(3 + r, line);
    }
    plat_console_unlock();
}

static void clear_menu(void) {
    if (!s_console_up) {
        return;
    }
    plat_console_lock(); /* the repaint must happen: wait for the log writer */
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
        case 3: plat_opt_set(PLAT_OPT_KEY_DEVUPDATE, !plat_devupdate_wanted()); break;
        case 4:
            if (dir == 0) {
                plat_relaunch();
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
        touch_button = tp.py < 8 * HUD_ROWS; /* the HUD rows */
    }
    if (down & PLAT_KEY_SELECT) {
        if (dev_menu_enabled() && (held & PLAT_KEY_ZL)) {
            s_open = 1;
            s_exit_armed = 0;
        } else {
            touch_button = 1;
        }
    }
    if (s_open) {
        return 1; /* opened this frame; the frame hook runs it (the game is paused, like the Options menu) */
    }
    if (touch_button) {
        plat_options_menu_open();
        return plat_options_menu_is_open();
    }
    return 0;
}

/* One key press of the developer menu. */
static void dev_menu_keys(uint32_t down) {
    if (down & (PLAT_KEY_B | PLAT_KEY_SELECT)) {
        s_open = 0;
        return;
    }
    if (down & (PLAT_KEY_L | PLAT_KEY_R)) {
        s_tab = (s_tab + 1) % TAB_COUNT;
        s_row = 0;
        s_exit_armed = 0;
    }
    if (down & PLAT_KEY_DUP) {
        s_row = (s_row + rows_in_tab() - 1) % rows_in_tab();
        s_exit_armed = 0;
    }
    if (down & PLAT_KEY_DDOWN) {
        s_row = (s_row + 1) % rows_in_tab();
        s_exit_armed = 0;
    }
    if (down & PLAT_KEY_DLEFT) {
        activate(-1);
    }
    if (down & PLAT_KEY_DRIGHT) {
        activate(1);
    }
    if (down & PLAT_KEY_A) {
        activate(0);
    }
}

/* The developer menu runs as a loop in the frame hook like the Options menu: the game thread is paused
 * (watchdog held, top screen greyed, sound muted) until it closes. */
static void dev_menu_run(void) {
    PLOG_I("menu: developer menu open (game paused)");
    plat_set_phase("developer menu");
    plat_pause_begin();
    uint32_t prev = plat_input_modal_poll(); /* the opening keys may still be down */
    int rep[4] = {0, 0, 0, 0};
    static const uint32_t rep_keys[4] = {PLAT_KEY_DUP, PLAT_KEY_DDOWN, PLAT_KEY_DLEFT, PLAT_KEY_DRIGHT};
    int dirty = 1;
    while (s_open) {
        if (dirty) {
            draw_menu(); /* trylock: when the log writer holds the console it is tried again next loop */
        }
        svcSleepThread(16666667);
        if (!plat_frame_poll()) {
            plat_options_save();
            plat_shutdown();
        }
        uint32_t held = plat_input_modal_poll();
        uint32_t down = held & ~prev;
        prev = held;
        for (int i = 0; i < 4; i++) {
            if (held & rep_keys[i]) {
                rep[i]++;
                if (rep[i] > 20 && rep[i] % 4 == 0) {
                    down |= rep_keys[i];
                }
            } else {
                rep[i] = 0;
            }
        }
        if (down) {
            dev_menu_keys(down);
            dirty = 1;
        }
        plat_options_tick();
    }
    clear_menu();
    plat_input_menu_closed();
    plat_pause_end();
    PLOG_I("menu: developer menu closed");
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
    uint32_t presented = plat_gpu_present_count();
    float drawn_fps = dt ? (float)(presented - s_last_presented) * 1000.0f / (float)dt : 0.0f;
    s_last_presented = presented;
    s_last_hud_ms = now;
    s_last_frames = stats->frames;
    if (!s_console_up || !plat_console_trylock()) {
        return;
    }
    int mode = opt_get_hud_mode();
    char l[HUD_ROWS][48] = {"", "", "", "", ""};
    if (mode != PLAT_OPT_HUD_OFF) {
        snprintf(l[0], sizeof(l[0]), "%4.1f fps  %4.1f ms", (double)s_fps, s_fps > 0.0f ? 1000.0 / (double)s_fps : 0.0);
        if (mode == PLAT_OPT_HUD_FULL) {
            size_t n = strlen(l[0]);
            snprintf(l[0] + n, sizeof(l[0]) - n, " drawn %4.1f", (double)drawn_fps);
        }
    }
    if (mode == PLAT_OPT_HUD_FULL) {
        snprintf(l[1], sizeof(l[1]), "cpu %5.1fms gpu %5.1fms rnd %4.1fms", (double)s_stats.frame_us / 1000.0,
                 (double)s_stats.gpu_us / 1000.0, (double)s_stats.render_us / 1000.0);
        struct mallinfo mi = mallinfo();
        unsigned long heap_t = __ctru_heap_size >> 20, heap_u = (unsigned long)mi.uordblks >> 20;
        unsigned long lin_t = __ctru_linear_heap_size >> 20, lin_u = (__ctru_linear_heap_size - linearSpaceFree()) >> 20;
        snprintf(l[2], sizeof(l[2]), "heap%3lu/%luM%3lu%% lin%3lu/%luM%3lu%%", heap_u, heap_t,
                 heap_t ? heap_u * 100 / heap_t : 0, lin_u, lin_t, lin_t ? lin_u * 100 / lin_t : 0);
        uint32_t qused, qtotal;
        plat_qtm_usage(&qused, &qtotal);
        if (qtotal) {
            snprintf(l[4], sizeof(l[4]), "QTM %lu/%lu KiB %lu%%", (unsigned long)(qused >> 10), (unsigned long)(qtotal >> 10),
                     (unsigned long)((uint64_t)qused * 100 / qtotal));
        } else {
            snprintf(l[4], sizeof(l[4]), "QTM: off");
        }
        unsigned long vfree = vramSpaceFree() / 1024, vused = VRAM_TOTAL_KB - vfree;
        static unsigned long s_big;
        static uint32_t s_big_ms;
        if (now - s_big_ms >= 4000) {
            s_big = plat_gpu_vram_largest_kb(); /* probes with vramAlloc: not every refresh */
            s_big_ms = now;
        }
        snprintf(l[3], sizeof(l[3]), "vram%5lu/%luK%3lu%% big %luK", vused, (unsigned long)VRAM_TOTAL_KB,
                 vused * 100 / VRAM_TOTAL_KB, s_big);
    }
    /* the touch hint on row 1, where the line leaves room */
    if (strlen(l[0]) <= 30) {
        snprintf(l[0], sizeof(l[0]), "%-31s[Options]", l[0]);
    }
    for (int r = 0; r < HUD_ROWS; r++) {
        hud_line(1 + r, l[r]);
    }
    plat_console_unlock();
}

/* From the frame hook (plat/input.c): the player Options menu runs here, so the
 * game thread (and game time) stands still while it is open. */
void plat_menu_frame(void) {
    if (!plat_options_menu_is_open()) {
        if (s_open) {
            dev_menu_run();
            s_last_hud_ms = 0;
        }
        return;
    }
    plat_options_menu_run();
    clear_menu();
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
    plat_pause_dump(ticks);
    PLOG_I("shotbottom: %s menu=%d", path, s_menu_mode);
}
