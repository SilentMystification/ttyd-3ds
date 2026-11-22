/* The GameCube debug DOL's test layer (configure.py --gcdebug, TARGET_GCDEBUG).
 *
 * The game code of that build is compiled with OSGetTime, OSGetTick,
 * VIWaitForRetrace, VISet*RetraceCallback, PADRead, rand and srand renamed
 * to the gcdbg_* functions here (configure.py GCDEBUG_RENAMES); this file is
 * compiled without the renames and calls the real SDK. Together with the
 * shared test mode (port3ds/game/test_*.c) it gives the GameCube build the
 * 3DS port's test switches, with the same behaviour:
 *
 *   map=<map>[,<bero>] story=N party=N  warp at the title (port3ds/game/vi.c)
 *   fastboot   A every 6 frames (held 2) until the title or the warp
 *   padscript= the port's scripted input (port3ds/plat/input.c), as the
 *              port's pad.c turns it into a PADStatus
 *   determ, gwdump=N, fx=, battle=, btlauto=   test_mode.h
 *   stopframe=N  calls gcdbg_stop at game frame N (a breakpoint target)
 *
 * The switches are the text in gcdbg_switches, which tools/port3ds/
 * run_dolphin.py writes through Dolphin's GDB stub before the game runs.
 * Log lines (tst_log) go to the gcdbg_log buffer, which the same tool reads.
 * C89, for MWCC.
 */
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "mario/mario_party.h"
#include "mario/mariost.h"
#include "party.h"
#include "pmario_sound.h"
#include "test_mode.h" /* port3ds/game */

void stg0_00_init(void);

/* ---- switches and log, shared with run_dolphin.py ---- */

char gcdbg_switches[1024] = "gcdbg-no-switches";
char gcdbg_log[0x18000];
u32 gcdbg_log_len;
u32 gcdbg_log_lost;
volatile u32 gcdbg_stop_frame;

static int sw_find(const char* word, const char** where) {
    int n = (int)strlen(word);
    const char* p = gcdbg_switches;
    while ((p = strstr(p, word)) != NULL) {
        int edge0 = p == gcdbg_switches || (unsigned char)p[-1] <= ' ';
        char after = p[n];
        if (edge0 && ((unsigned char)after <= ' ' || after == '=')) {
            if (where != NULL) {
                *where = p;
            }
            return 1;
        }
        p += n;
    }
    return 0;
}

int tst_sw_has(const char* word) { return sw_find(word, NULL); }

int tst_sw_value(const char* key, char* out, int n) {
    const char* p;
    int i = 0;
    if (!sw_find(key, &p)) {
        return 0;
    }
    p += strlen(key);
    if (*p != '=') {
        return 0;
    }
    p++;
    while (p[i] != '\0' && (unsigned char)p[i] > ' ' && i + 1 < n) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}

int tst_sw_int(const char* key, int fallback) {
    char v[24];
    const char* p = v;
    int neg = 0, base = 10, any = 0;
    long r = 0;
    if (!tst_sw_value(key, v, sizeof(v))) {
        return fallback;
    }
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (base == 16 && *p >= 'a' && *p <= 'f') {
            d = *p - 'a' + 10;
        } else if (base == 16 && *p >= 'A' && *p <= 'F') {
            d = *p - 'A' + 10;
        } else {
            break;
        }
        r = r * base + d;
        any = 1;
    }
    return any ? (int)(neg ? -r : r) : fallback;
}

void tst_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    u32 n;
    va_start(ap, fmt);
    vsprintf(line, fmt, ap);
    va_end(ap);
    n = strlen(line);
    if (gcdbg_log_len + n + 1 >= sizeof(gcdbg_log)) {
        gcdbg_log_lost++;
        return;
    }
    memcpy(gcdbg_log + gcdbg_log_len, line, n);
    gcdbg_log[gcdbg_log_len + n] = '\n';
    gcdbg_log_len += n + 1;
}

/* ---- rand: MSL's generator, with its state readable ---- */

static u32 s_rand_next = 1;

int gcdbg_rand(void) {
    s_rand_next = s_rand_next * 1103515245u + 12345u;
    return (int)((s_rand_next >> 16) & 0x7FFF);
}

void gcdbg_srand(unsigned int seed) { s_rand_next = seed; }

u32 tst_rand_state(void) { return s_rand_next; }

/* ---- clock ---- */

OSTime gcdbg_OSGetTime(void) { return tst_determ() ? tst_game_time() : OSGetTime(); }

OSTick gcdbg_OSGetTick(void) { return tst_determ() ? (OSTick)tst_game_time() : OSGetTick(); }

/* ---- input: the port's padscript and pad mapping ---- */

#define SCRIPT_MAX 2048

typedef struct {
    u16 buttons;
    s8 sx, sy;
    u32 frames;
} ScriptStep;

static ScriptStep s_script[SCRIPT_MAX];
static int s_steps, s_step, s_script_loaded;
static u32 s_step_frames;
static u32 s_tst_keys;
static u32 s_frames;
static int s_fastboot = -1;

void tst_set_keys(u32 buttons) { s_tst_keys = buttons; }

/* the port's scale_stick (pad.c) of a circle pad value */
static s8 stick(int v) {
    int s = v * 100 / 150;
    return (s8)(s > 127 ? 127 : s < -128 ? -128 : s);
}

static u16 key_of(const char* k, s8* sx, s8* sy) {
    static const struct {
        const char* name;
        u16 bit;
    } keys[] = {
        {"a", PAD_BUTTON_A},       {"b", PAD_BUTTON_B},         {"x", PAD_BUTTON_X},
        {"y", PAD_BUTTON_Y},       {"l", PAD_TRIGGER_L},        {"r", PAD_TRIGGER_R},
        {"zl", 0},                 {"zr", PAD_TRIGGER_Z},       {"start", PAD_BUTTON_START},
        {"select", 0},             {"up", PAD_BUTTON_UP},       {"down", PAD_BUTTON_DOWN},
        {"left", PAD_BUTTON_LEFT}, {"right", PAD_BUTTON_RIGHT}, {"wait", 0},
    };
    u32 i;
    if (strcmp(k, "cl") == 0) {
        *sx = stick(-156);
        return 0;
    }
    if (strcmp(k, "cr") == 0) {
        *sx = stick(156);
        return 0;
    }
    if (strcmp(k, "cu") == 0) {
        *sy = stick(156);
        return 0;
    }
    if (strcmp(k, "cd") == 0) {
        *sy = stick(-156);
        return 0;
    }
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(k, keys[i].name) == 0) {
            return keys[i].bit;
        }
    }
    tst_log("padscript: unknown key \"%s\"", k);
    return 0;
}

static void script_load(void) {
    static char text[4096];
    char* p;
    s_script_loaded = 1;
    if (!tst_sw_value("padscript", text, sizeof(text))) {
        return;
    }
    p = text;
    while (*p != '\0' && s_steps < SCRIPT_MAX) {
        ScriptStep st;
        char* end = strchr(p, ',');
        char* star;
        char* k;
        if (end != NULL) {
            *end = '\0';
        }
        st.buttons = 0;
        st.sx = st.sy = 0;
        st.frames = 1;
        star = strchr(p, '*');
        if (star != NULL) {
            *star = '\0';
            st.frames = 0;
            for (k = star + 1; *k >= '0' && *k <= '9'; k++) {
                st.frames = st.frames * 10 + (u32)(*k - '0');
            }
        }
        k = p;
        while (*k != '\0') {
            char name[16];
            int n = 0;
            while (*k != '\0' && *k != '+' && n < 15) {
                char c = *k++;
                name[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            name[n] = '\0';
            if (*k == '+') {
                k++;
            }
            st.buttons |= key_of(name, &st.sx, &st.sy);
        }
        s_script[s_steps++] = st;
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    tst_log("padscript: %d steps", s_steps);
}

u32 gcdbg_PADRead(PADStatus* status) {
    u32 r = PADRead(status);
    u16 b = 0;
    s8 sx = 0, sy = 0;
    int i;
    if (!s_script_loaded) {
        script_load();
    }
    /* as the port: the script, else no input (the port's Azahar runs have
     * none), plus fastboot and the test keys */
    if (s_step < s_steps) {
        b = s_script[s_step].buttons;
        sx = s_script[s_step].sx;
        sy = s_script[s_step].sy;
    }
    if (s_fastboot == 1 && (s_frames % 6) < 2) {
        b |= PAD_BUTTON_A;
    }
    b |= (u16)s_tst_keys;
    memset(status, 0, sizeof(PADStatus) * PAD_MAX_CONTROLLERS);
    status[0].button = b;
    status[0].stickX = sx;
    status[0].stickY = sy;
    status[0].triggerLeft = (b & PAD_TRIGGER_L) ? 0xFF : 0;
    status[0].triggerRight = (b & PAD_TRIGGER_R) ? 0xFF : 0;
    status[0].analogA = (b & PAD_BUTTON_A) ? 0xFF : 0;
    status[0].analogB = (b & PAD_BUTTON_B) ? 0xFF : 0;
    status[0].err = PAD_ERR_NONE;
    for (i = 1; i < PAD_MAX_CONTROLLERS; i++) {
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return r;
}

static void script_frame(void) {
    if (!s_script_loaded) {
        script_load();
    }
    if (s_step < s_steps && ++s_step_frames >= s_script[s_step].frames) {
        s_step_frames = 0;
        if (++s_step == s_steps) {
            tst_log("padscript: done");
        }
    }
}

/* ---- the port's map= warp and fastboot (port3ds/game/vi.c) ---- */

static int s_warped;

static void warp_new_game(int story, int party) {
    psndStopAllFadeOut();
    psndSFXAllOff();
    stg0_00_init();
    marioPartyKill();
    swInit();
    swByteSet(0, story);
    if (party > 0 && party < 8) {
        partyJoin((PartyId)party);
        marioPartyEntry((PartyId)party);
    }
}

static void warp_switch(void) {
    static int state = -1;
    static char map[16], bero[16];
    if (state < 0) {
        char v[40];
        state = 0;
        if (tst_sw_value("map", v, sizeof(v)) && v[0] != '\0') {
            char* comma = strchr(v, ',');
            bero[0] = '\0';
            if (comma != NULL) {
                *comma = '\0';
                strncpy(bero, comma + 1, 15);
                bero[15] = '\0';
            }
            strncpy(map, v, 15);
            map[15] = '\0';
            state = 1;
        }
    }
    if (state != 1 || gp == NULL) {
        return;
    }
    if (seqGetSeq() == SEQ_TITLE || (seqGetSeq() == SEQ_GAME && strcmp(gp->currentMapName, map) != 0)) {
        warp_new_game(tst_sw_int("story", 0), tst_sw_int("party", 1));
        tst_log("warp: %s %s (switch) at game frame %lu", map, bero, (unsigned long)s_frames);
        seqSetSeq(SEQ_MAP_CHANGE, map, bero[0] ? bero : NULL);
        state = 2;
        s_warped = 1;
    }
}

static void fastboot_frame(void) {
    int seq;
    if (s_fastboot < 0) {
        s_fastboot = tst_sw_has("fastboot") ? 1 : 0;
        if (s_fastboot) {
            tst_log("fastboot: on");
        }
    }
    if (s_fastboot != 1) {
        return;
    }
    seq = gp != NULL ? seqGetSeq() : SEQ_UNDEFINED;
    if (s_warped || (seq != SEQ_UNDEFINED && seq != SEQ_LOGO)) {
        s_fastboot = 0;
        tst_log("fastboot: off at frame %lu (sequence %d)", (unsigned long)s_frames, seq);
    }
}

/* ---- retrace ---- */

static VIRetraceCallback s_pre, s_post;

VIRetraceCallback gcdbg_VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old;
    if (!tst_determ()) {
        return VISetPreRetraceCallback(cb);
    }
    old = s_pre;
    s_pre = cb;
    return old;
}

VIRetraceCallback gcdbg_VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old;
    if (!tst_determ()) {
        return VISetPostRetraceCallback(cb);
    }
    old = s_post;
    s_post = cb;
    return old;
}

/* The breakpoint run_dolphin.py stops at (stopframe=N). */
#pragma dont_inline on
void gcdbg_stop(u32 frame) { gcdbg_stop_frame = frame; }
#pragma dont_inline reset

static void frame_hooks(void) {
    static int stop = -1;
    if (stop < 0) {
        stop = tst_sw_int("stopframe", 0);
        tst_log("gcdbg: switches \"%s\"", gcdbg_switches);
    }
    s_frames++;
    tst_frame(s_frames);
    script_frame();
    warp_switch();
    fastboot_frame();
    if (stop > 0 && s_frames == (u32)stop) {
        gcdbg_stop(s_frames);
    }
}

void gcdbg_VIWaitForRetrace(void) {
    frame_hooks();
    VIWaitForRetrace();
    if (tst_determ()) {
        int waits = 0;
        u32 count;
        BOOL level;
        while (tst_dvd_busy() && waits < 600) {
            VIWaitForRetrace();
            waits++;
        }
        if (waits == 600) {
            tst_log("determ: DVD manager still busy after 600 retraces");
        }
        count = tst_vretrace();
        level = OSDisableInterrupts();
        if (s_pre != NULL) {
            s_pre(count - 1);
        }
        if (s_post != NULL) {
            s_post(count);
        }
        OSRestoreInterrupts(level);
    }
}
