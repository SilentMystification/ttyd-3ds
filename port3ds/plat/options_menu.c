/* Player options menu: the generic list UI (see include/plat/options.h).
 *
 * Bottom console rows (1-based, 40 columns; the menu covers the whole screen
 * while it is open, and the HUD and log come back on close, see plat/menu.c):
 *   1        title, page number, [Close] button
 *   3-24     the page's items (scrolls when there are more than 22)
 *   30       key help
 * Keys: L/R page, Up/Down move, Left/Right change a value, A changes it too
 * (or runs a reset), B or SELECT closes. Touch: tap an item to select and
 * change it, tap the title to turn the page, tap [Close] to close.
 *
 * The menu runs as a loop inside the frame hook, so the game thread stands
 * still and game time does not advance; the loop keeps the system alive
 * (HOME, sleep) through plat_frame_poll and holds the watchdog.
 */
#include <stdio.h>
#include <string.h>

#include "options_internal.h"
#include "plat/input.h"
#include "plat_internal.h"

#define FIRST_ROW 3
#define VISIBLE 22
#define MAX_PAGE_ITEMS 24

static int s_open;
static int s_page, s_sel, s_scroll;
static int s_redraw;
static int s_idx[MAX_PAGE_ITEMS]; /* menu item indexes of the current page */
static int s_idx_n;
static char s_note[24];

static int page_count(const PlatOptMenu* m) {
    int n = 0;
    while (m->pages != NULL && m->pages[n] != NULL) {
        n++;
    }
    return n;
}

static void build_page(const PlatOptMenu* m) {
    s_idx_n = 0;
    for (int i = 0; i < m->item_count && s_idx_n < MAX_PAGE_ITEMS; i++) {
        if (m->items[i].page == s_page) {
            s_idx[s_idx_n++] = i;
        }
    }
    if (s_sel >= s_idx_n) {
        s_sel = s_idx_n ? s_idx_n - 1 : 0;
    }
}

static int count_names(const PlatOptItem* it) {
    int n = 0;
    while (it->names != NULL && it->names[n] != NULL) {
        n++;
    }
    return n;
}

void plat_options_menu_open(void) {
    const PlatOptMenu* m = plat_options_menu();
    if (m == NULL) {
        PLOG_W("options: no option list registered");
        return;
    }
    s_open = 1;
    s_redraw = 1;
    s_note[0] = '\0';
    build_page(m);
}

int plat_options_menu_is_open(void) { return s_open; }

static void put(int row, const char* text) {
    plat_console_row(row, text);
    if (text[0] != '\0') {
        PLOG_D("options: screen row %2d |%s|", row, text); /* loglevel=3: the menu as drawn, for tests without a capture */
    }
}

static void draw(const PlatOptMenu* m) {
    if (!plat_console_ready() || !plat_console_trylock()) {
        return; /* try again next loop */
    }
    s_redraw = 0;
    plat_console_menu_mode(1);
    char line[48], buf[48];
    int pages = page_count(m);
    memset(line, ' ', 40);
    line[40] = '\0';
    snprintf(buf, sizeof(buf), " OPTIONS: %s (%d/%d)", m->pages[s_page], s_page + 1, pages);
    memcpy(line, buf, strlen(buf) < 30 ? strlen(buf) : 30);
    memcpy(line + 31, "[ Close ]", 9);
    put(1, line);
    put(2, "");
    for (int r = 0; r < VISIBLE; r++) {
        int k = s_scroll + r;
        if (k >= s_idx_n) {
            put(FIRST_ROW + r, "");
            continue;
        }
        const PlatOptItem* it = &m->items[s_idx[k]];
        char val[24] = "";
        if (it->kind == PLAT_OPT_CHOICE) {
            int v = plat_opt_get(it->key, it->def);
            snprintf(val, sizeof(val), "< %s >", it->names[v]);
        } else if (it->kind == PLAT_OPT_RANGE) {
            snprintf(val, sizeof(val), "< %d%s >", plat_opt_get(it->key, it->def), it->suffix ? it->suffix : "");
        } else {
            snprintf(val, sizeof(val), "%s", s_note);
        }
        snprintf(line, sizeof(line), "%c %-18.18s %-19.19s", k == s_sel ? '>' : ' ', it->label, val);
        put(FIRST_ROW + r, line);
    }
    for (int r = FIRST_ROW + VISIBLE; r < 30; r++) {
        put(r, "");
    }
    put(30, "L/R page Up/Dn move </> change B close");
    plat_console_unlock();
}

static void step_value(const PlatOptMenu* m, const PlatOptItem* it, int dir) {
    if (it->kind == PLAT_OPT_RESET) {
        if (dir >= 0) {
            for (int i = 0; i < m->item_count; i++) {
                const PlatOptItem* o = &m->items[i];
                if (o->page == it->page && o->key != NULL) {
                    plat_opt_set(o->key, o->def);
                }
            }
            snprintf(s_note, sizeof(s_note), "done");
        }
        return;
    }
    s_note[0] = '\0';
    int v = plat_opt_get(it->key, it->def);
    if (it->kind == PLAT_OPT_CHOICE) {
        int n = count_names(it);
        v = (v + (dir < 0 ? n - 1 : 1)) % n;
    } else {
        int st = it->step > 0 ? it->step : 1;
        if (dir == 0) { /* A: forward, wrapping */
            v = v + st > it->max ? it->min : v + st;
        } else {
            v += dir * st;
            v = v < it->min ? it->min : v > it->max ? it->max : v;
        }
    }
    plat_opt_set(it->key, v);
}

static void select_row(int row) {
    if (row < 0 || row >= s_idx_n) {
        return;
    }
    s_sel = row;
    s_note[0] = '\0';
    if (s_sel < s_scroll) {
        s_scroll = s_sel;
    }
    if (s_sel >= s_scroll + VISIBLE) {
        s_scroll = s_sel - VISIBLE + 1;
    }
}

static void turn_page(const PlatOptMenu* m, int dir) {
    int n = page_count(m);
    s_page = (s_page + n + dir) % n;
    s_sel = 0;
    s_scroll = 0;
    s_note[0] = '\0';
    build_page(m);
}

/* One frame of menu input. down: PLAT_KEY_* pressed this frame (with repeats). */
static void handle(const PlatOptMenu* m, uint32_t down) {
    if (down & (PLAT_KEY_B | PLAT_KEY_SELECT)) {
        s_open = 0;
        return;
    }
    if (down & PLAT_KEY_L) {
        turn_page(m, -1);
    }
    if (down & PLAT_KEY_R) {
        turn_page(m, 1);
    }
    if (down & PLAT_KEY_DUP) {
        select_row(s_sel > 0 ? s_sel - 1 : s_idx_n - 1);
    }
    if (down & PLAT_KEY_DDOWN) {
        select_row(s_sel + 1 < s_idx_n ? s_sel + 1 : 0);
    }
    if (s_idx_n > 0) {
        const PlatOptItem* it = &m->items[s_idx[s_sel]];
        if (down & PLAT_KEY_DLEFT) {
            step_value(m, it, -1);
        }
        if (down & PLAT_KEY_DRIGHT) {
            step_value(m, it, 1);
        }
        if (down & PLAT_KEY_A) {
            step_value(m, it, 0);
        }
    }
    s_redraw = 1;
}

static void handle_touch(const PlatOptMenu* m) {
    touchPosition tp;
    hidTouchRead(&tp);
    int row = tp.py / 8 + 1; /* console row, 1-based */
    if (row <= 2) { /* the title row */
        if (tp.px >= 240) {
            s_open = 0;
        } else {
            turn_page(m, 1);
            s_redraw = 1;
        }
    } else if (row >= FIRST_ROW && row < FIRST_ROW + VISIBLE) {
        int k = s_scroll + (row - FIRST_ROW);
        if (k < s_idx_n) {
            select_row(k);
            step_value(m, &m->items[s_idx[k]], 0);
            s_redraw = 1;
        }
    }
}

void plat_options_menu_run(void) {
    const PlatOptMenu* m = plat_options_menu();
    if (m == NULL || !s_open) {
        s_open = 0;
        return;
    }
    PLOG_I("options: menu open (game paused)");
    plat_set_phase("options menu");
    plat_wd_hold(1);
    uint32_t prev = plat_input_modal_poll(); /* the opening key may still be down */
    int rep[4] = {0, 0, 0, 0};
    static const uint32_t rep_keys[4] = {PLAT_KEY_DUP, PLAT_KEY_DDOWN, PLAT_KEY_DLEFT, PLAT_KEY_DRIGHT};
    while (s_open) {
        if (s_redraw) {
            draw(m);
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
            handle(m, down);
        }
        if (hidKeysDown() & KEY_TOUCH) {
            handle_touch(m);
        }
        plat_options_tick();
    }
    plat_options_save();
    plat_input_menu_closed();
    plat_wd_hold(0);
    PLOG_I("options: menu closed");
}
