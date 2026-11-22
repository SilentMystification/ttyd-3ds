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
#include <stdio.h>
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

/* ---- recorded inputs (switches padrec=FILE, padfile=FILE) ----
 * The game-visible pad state of each game frame goes to / comes from a TPAD
 * file: 16-byte header (magic "TPAD", version u32 = 1, frame count u32, flags
 * u32, bit0 = recorded under determ), then 8 bytes per frame, little-endian:
 * buttons u16, stickX s8, stickY s8, substickX s8, substickY s8, triggerL u8,
 * triggerR u8 (the GameCube PADStatus fields). Paths are relative to the
 * working directory (sdmc:/3ds/ttyd) unless they start with "sdmc:" or "/". */
#define TPAD_MAGIC 0x44415054u /* "TPAD" little-endian */
#define TPAD_VERSION 1u
#define TPAD_FLAG_DETERM 1u
#define TPAD_REC_BYTES 8
#define TPAD_FLUSH_FRAMES 600

static FILE* s_rec_file;
static int s_rec_init;
static uint32_t s_rec_frames, s_rec_flushed;
static uint8_t* s_rec_buf; /* all records of the run (8 bytes a frame; a 3 h run is under 6 MB) */
static uint32_t s_rec_cap;

static uint8_t* s_play_buf;
static uint32_t s_play_frames;
static int s_play_on;

static uint32_t s_pad_frame; /* game frames so far: the index of the record the game sees now */
static PlatPadState s_cur;
static int s_cur_set;

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void rec_flush(void) {
    if (s_rec_file == NULL) {
        return;
    }
    uint8_t hdr[16];
    put32(hdr, TPAD_MAGIC);
    put32(hdr + 4, TPAD_VERSION);
    put32(hdr + 8, s_rec_frames);
    put32(hdr + 12, plat_switch_has("determ") ? TPAD_FLAG_DETERM : 0);
    fseek(s_rec_file, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), s_rec_file);
    fseek(s_rec_file, 16 + (long)s_rec_flushed * TPAD_REC_BYTES, SEEK_SET);
    fwrite(s_rec_buf + (size_t)s_rec_flushed * TPAD_REC_BYTES, TPAD_REC_BYTES, s_rec_frames - s_rec_flushed, s_rec_file);
    s_rec_flushed = s_rec_frames;
    fflush(s_rec_file);
}

static void rec_pad_init(void) {
    s_rec_init = 1;
    char path[256];
    if (plat_switch_value("padfile", path, sizeof(path)) && path[0] != '\0') {
        FILE* f = fopen(path, "rb");
        uint8_t hdr[16];
        if (f == NULL || fread(hdr, 1, 16, f) != 16 || get32(hdr) != TPAD_MAGIC || get32(hdr + 4) != TPAD_VERSION) {
            PLOG_E("[PAD] padfile %s: missing or not a TPAD v1 file", path);
        } else {
            uint32_t n = get32(hdr + 8);
            s_play_buf = malloc((size_t)n * TPAD_REC_BYTES + 1);
            if (s_play_buf != NULL) {
                s_play_frames = (uint32_t)fread(s_play_buf, TPAD_REC_BYTES, n, f);
                s_play_on = 1;
                PLOG_I("[PAD] padfile %s: %u frames (recorded %sunder determ)", path, (unsigned)s_play_frames,
                       (get32(hdr + 12) & TPAD_FLAG_DETERM) ? "" : "not ");
            }
        }
        if (f != NULL) {
            fclose(f);
        }
        return; /* a playback run records nothing */
    }
    if (plat_switch_value("padrec", path, sizeof(path)) && path[0] != '\0') {
        s_rec_file = fopen(path, "wb");
        if (s_rec_file == NULL) {
            PLOG_E("[PAD] padrec %s: cannot create", path);
            return;
        }
        s_rec_cap = 4096;
        s_rec_buf = malloc((size_t)s_rec_cap * TPAD_REC_BYTES);
        if (s_rec_buf == NULL) {
            fclose(s_rec_file);
            s_rec_file = NULL;
            return;
        }
        rec_flush(); /* a valid empty file from the start */
        atexit(rec_flush);
        PLOG_I("[PAD] padrec %s", path);
    }
}

/* The game's pad read (game side) calls this with the state it is about to give
 * the game: playback replaces it, recording notes it for this frame's record. */
void plat_pad_state(PlatPadState* s) {
    if (!s_rec_init) {
        rec_pad_init();
    }
    if (s_play_on) {
        memset(s, 0, sizeof(*s));
        if (s_pad_frame < s_play_frames) {
            const uint8_t* r = s_play_buf + (size_t)s_pad_frame * TPAD_REC_BYTES;
            s->buttons = (uint16_t)(r[0] | r[1] << 8);
            s->stick_x = (int8_t)r[2];
            s->stick_y = (int8_t)r[3];
            s->substick_x = (int8_t)r[4];
            s->substick_y = (int8_t)r[5];
            s->trigger_l = r[6];
            s->trigger_r = r[7];
        }
        return;
    }
    if (s_rec_file != NULL) {
        s_cur = *s;
        s_cur_set = 1;
    }
}

static void rec_pad_frame(void) {
    if (!s_rec_init) {
        rec_pad_init();
    }
    if (s_play_on) {
        if (++s_pad_frame == s_play_frames) {
            PLOG_I("[PAD] padfile end at frame %u", (unsigned)s_pad_frame);
        }
        return;
    }
    if (s_rec_file == NULL) {
        return;
    }
    if (s_rec_frames == s_rec_cap) {
        uint8_t* nb = realloc(s_rec_buf, (size_t)s_rec_cap * 2 * TPAD_REC_BYTES);
        if (nb == NULL) {
            return;
        }
        s_rec_buf = nb;
        s_rec_cap *= 2;
    }
    PlatPadState n;
    memset(&n, 0, sizeof(n));
    const PlatPadState* c = s_cur_set ? &s_cur : &n;
    uint8_t* r = s_rec_buf + (size_t)s_rec_frames * TPAD_REC_BYTES;
    r[0] = (uint8_t)c->buttons, r[1] = (uint8_t)(c->buttons >> 8);
    r[2] = (uint8_t)c->stick_x, r[3] = (uint8_t)c->stick_y;
    r[4] = (uint8_t)c->substick_x, r[5] = (uint8_t)c->substick_y;
    r[6] = c->trigger_l, r[7] = c->trigger_r;
    s_rec_frames++;
    s_pad_frame++;
    s_cur_set = 0;
    if (s_rec_frames % TPAD_FLUSH_FRAMES == 0) {
        rec_flush();
    }
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

static void rec_flush(void);

static void script_advance(void) {
    if (s_step < s_steps && ++s_step_frames >= s_script[s_step].frames) {
        s_step_frames = 0;
        if (++s_step == s_steps) {
            rec_flush(); /* a recorded run ends with its script: save what was recorded so far */
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
    rec_pad_frame();
    script_advance();
}
