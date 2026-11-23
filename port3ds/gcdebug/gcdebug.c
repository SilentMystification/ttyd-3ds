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
 *   padfile=<disc path>  plays a TPAD file (port3ds/plat/input.c), one record
 *              per game frame, neutral after the last; the file must be in
 *              the extracted disc folder, e.g. padfile=/pad1.tpad for
 *              build/<disc>/files/pad1.tpad (read once, through DVDOpen)
 *   rngtrace=<from>,<to>  [FRM], [SEQ] and [CARD] lines (test_mode.c); the CARD*Async
 *              calls are wrapped below to log each job's post and completion
 *   determ, gwdump=N, fx=, battle=, btlauto=   test_mode.h
 *   stopframe=N  calls gcdbg_stop at game frame N (a breakpoint target)
 *
 * The switches are the text in gcdbg_switches, which tools/port3ds/
 * run_dolphin.py writes through Dolphin's GDB stub before the game runs.
 * Log lines (tst_log) go to the gcdbg_log buffer, which the same tool reads.
 * C89, for MWCC.
 */
#include <dolphin/card.h>
#include <dolphin/dvd.h>
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
char gcdbg_log[0x30000];
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
    register u32 lr;
    register u32 stk;
    u32 v;
    u32 c2 = 0, c3 = 0;
    asm { mflr lr } /* the caller's address: nothing before this has changed LR */
    asm { mr stk, r1 }
    s_rand_next = s_rand_next * 1103515245u + 12345u;
    v = (s_rand_next >> 16) & 0x7FFF;
    if (tst_rngtrace_wanted()) {
        /* the back chain: our frame -> irand's frame (its LR slot is lr) -> irand's caller's frame, which holds
         * the address irand returns to */
        u32 sp1 = *(u32*)stk;
        if (sp1 >= 0x80000000u && sp1 < 0x81800000u) {
            u32 sp2 = *(u32*)sp1;
            if (sp2 >= 0x80000000u && sp2 < 0x81800000u) {
                u32 sp3 = *(u32*)sp2;
                c2 = *(u32*)(sp2 + 4);
                if (sp3 >= 0x80000000u && sp3 < 0x81800000u) {
                    c3 = *(u32*)(sp3 + 4);
                }
            }
        }
    }
    tst_rngtrace(lr, c2, c3, v);
    return (int)v;
}

void gcdbg_srand(unsigned int seed) { s_rand_next = seed; }

u32 tst_rand_state(void) { return s_rand_next; }

/* main memory, cached or uncached (24 MB) */
int tst_is_ptr(u32 v) { return (v >= 0x80000000u && v < 0x81800000u) || (v >= 0xC0000000u && v < 0xC1800000u); }

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

/* padfile=: a TPAD file (16-byte header "TPAD", version 1, frame count, flags;
 * then 8 bytes a frame: buttons u16, stickX/Y, substickX/Y s8, triggerL/R u8,
 * little-endian) from the disc, at most PADFILE_MAX frames */
#define PADFILE_MAX 8192
static u8 s_pf_buf[(16 + 8 * PADFILE_MAX + 31) & ~31] ATTRIBUTE_ALIGN(32);
static u32 s_pf_frames, s_pf_frame;
static int s_pf_on, s_pf_loaded;

static void padfile_load(void) {
    char path[64];
    DVDFileInfo fi;
    s32 len, got;
    s_pf_loaded = 1;
    if (!tst_sw_value("padfile", path, sizeof(path))) {
        return;
    }
    if (!DVDOpen(path, &fi)) {
        tst_log("[PAD] padfile %s: not on the disc", path);
        return;
    }
    len = (s32)((fi.length + 31) & ~31);
    if (len > (s32)sizeof(s_pf_buf)) {
        len = (s32)sizeof(s_pf_buf);
    }
    got = DVDReadPrio(&fi, s_pf_buf, len, 0, 2);
    DVDClose(&fi);
    if (got < 16 || s_pf_buf[0] != 'T' || s_pf_buf[1] != 'P' || s_pf_buf[2] != 'A' || s_pf_buf[3] != 'D' ||
        s_pf_buf[4] != 1) {
        tst_log("[PAD] padfile %s: not a TPAD v1 file", path);
        return;
    }
    s_pf_frames = (u32)s_pf_buf[8] | (u32)s_pf_buf[9] << 8 | (u32)s_pf_buf[10] << 16 | (u32)s_pf_buf[11] << 24;
    if (s_pf_frames > (u32)(got - 16) / 8) {
        s_pf_frames = (u32)(got - 16) / 8;
    }
    s_pf_on = 1;
    tst_log("[PAD] padfile %s: %lu frames", path, (unsigned long)s_pf_frames);
}

/* ---- card jobs: [CARD] lines as the port's card.c logs them (kinds as its JobKind:
 * 0 mount, 1 check, 2 format, 3 create, 4 read, 5 write, 6 setstatus) ---- */

static CARDCallback s_card_cb[7];
static s32 s_card_len[7], s_card_off[7];
static OSTime s_hook_time; /* real time at the last frame hook: the ticks since it place an event in the frame */

static void card_posted(int kind, s32 len, s32 off) {
    s_card_len[kind] = len;
    s_card_off[kind] = off;
    if (tst_rngtrace_wanted()) {
        tst_log("[CARD] job %d posted at game frame %lu len=%d off=%d since_hook=%lu", kind,
                (unsigned long)tst_cur_frame(), (int)len, (int)off, (unsigned long)(OSGetTime() - s_hook_time));
    }
}

static void card_done(int kind, s32 chan, s32 result) {
    CARDCallback cb = s_card_cb[kind];
    if (tst_rngtrace_wanted()) {
        tst_log("[CARD] job %d done result %d at game frame %lu len=%d off=%d since_hook=%lu", kind, (int)result,
                (unsigned long)tst_cur_frame(), (int)s_card_len[kind], (int)s_card_off[kind],
                (unsigned long)(OSGetTime() - s_hook_time));
    }
    if (cb != NULL) {
        cb(chan, result);
    }
}

static void card_cb0(s32 chan, s32 result) { card_done(0, chan, result); }
static void card_cb1(s32 chan, s32 result) { card_done(1, chan, result); }
static void card_cb2(s32 chan, s32 result) { card_done(2, chan, result); }
static void card_cb3(s32 chan, s32 result) { card_done(3, chan, result); }
static void card_cb4(s32 chan, s32 result) { card_done(4, chan, result); }
static void card_cb5(s32 chan, s32 result) { card_done(5, chan, result); }
static void card_cb6(s32 chan, s32 result) { card_done(6, chan, result); }

s32 gcdbg_CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback) {
    card_posted(0, 0, 0);
    s_card_cb[0] = attachCallback;
    return CARDMountAsync(chan, workArea, detachCallback, card_cb0);
}

s32 gcdbg_CARDCheckAsync(s32 chan, CARDCallback callback) {
    card_posted(1, 0, 0);
    s_card_cb[1] = callback;
    return CARDCheckAsync(chan, card_cb1);
}

s32 gcdbg_CARDFormatAsync(s32 chan, CARDCallback callback) {
    card_posted(2, 0, 0);
    s_card_cb[2] = callback;
    return CARDFormatAsync(chan, card_cb2);
}

s32 gcdbg_CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback) {
    card_posted(3, (s32)size, 0);
    s_card_cb[3] = callback;
    return CARDCreateAsync(chan, fileName, size, fileInfo, card_cb3);
}

s32 gcdbg_CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback) {
    card_posted(4, length, offset);
    s_card_cb[4] = callback;
    return CARDReadAsync(fileInfo, buf, length, offset, card_cb4);
}

s32 gcdbg_CARDWriteAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback) {
    card_posted(5, length, offset);
    s_card_cb[5] = callback;
    return CARDWriteAsync(fileInfo, buf, length, offset, card_cb5);
}

s32 gcdbg_CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback) {
    card_posted(6, 0, 0);
    s_card_cb[6] = callback;
    return CARDSetStatusAsync(chan, fileNo, stat, card_cb6);
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
    if (!s_pf_loaded) {
        padfile_load();
    }
    if (s_pf_on) {
        /* replaces the script, fastboot and the test keys */
        const u8* r = s_pf_buf + 16 + 8 * (s_pf_frame < s_pf_frames ? s_pf_frame : 0);
        int live = s_pf_frame < s_pf_frames;
        static int first = 1;
        if (first) {
            first = 0;
            tst_log("[PAD] padfile first record at game frame %lu", (unsigned long)s_frames);
        }
        b = live ? (u16)(r[0] | r[1] << 8) : 0;
        status[0].button = b;
        status[0].stickX = live ? (s8)r[2] : 0;
        status[0].stickY = live ? (s8)r[3] : 0;
        status[0].substickX = live ? (s8)r[4] : 0;
        status[0].substickY = live ? (s8)r[5] : 0;
        status[0].triggerLeft = live ? r[6] : 0;
        status[0].triggerRight = live ? r[7] : 0;
        status[0].analogA = (b & PAD_BUTTON_A) ? 0xFF : 0;
        status[0].analogB = (b & PAD_BUTTON_B) ? 0xFF : 0;
    }
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
    /* counts every game frame from the first, as the port does, even those
     * before the first PADRead loads the file */
    s_pf_frame++;
    if (s_pf_on && s_pf_frame == s_pf_frames) {
        tst_log("[PAD] padfile end at frame %lu", (unsigned long)s_pf_frame);
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
    if (tst_storyjump_apply()) {
        return; /* storyjump=: the story point's own set-up (test_mode.c) */
    }
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
        const char* sj_map;
        const char* sj_bero;
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
        } else if (tst_storyjump_target(&sj_map, &sj_bero)) { /* storyjump= alone: the point's map */
            strncpy(map, sj_map, 15);
            map[15] = '\0';
            strncpy(bero, sj_bero, 15);
            bero[15] = '\0';
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
    s_hook_time = OSGetTime();
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
