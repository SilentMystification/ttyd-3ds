/* Systems test hooks (port only; tools/port3ds/systems_test.py, docs/PORT_3DS_TESTING.md "Systems tests").
 *
 * storyjump  with map= (and story=N): the warp's new game runs the setup of every story point up to N, as the
 *   E3 scenario select does (event.c's tables: items, mail, partners, flags), instead of setting GSW 0 only.
 * marioat=x,y,z  once the map the game is on has been up marioatwait=N frames (default 30), puts Mario there
 *   through evt_mario_set_pos, the way event scripts move him. Once per run.
 * hp=N  at the same point, sets Mario's HP to N (pouchSetHP). Once per run.
 * syslog  logs, when they change, the sequence ("sys: seq A -> B"), the pause menu's state (open, tab, the
 *   tab's own state and mode) and Mario's HP, each with the game frame. Read by systems_test.py.
 *
 * Side content (tools/port3ds/side_test.py, docs "Side content tests"), at the marioat point too, once:
 * items=ID[,ID...]  gives each item (pouchGetItem; ids in hex with 0x, else decimal).
 * coins=N  sets the coins.  piantas=N  sets the Pianta Parlor's tokens, held (taken out, as inside the parlor).
 * gsw=I:V[,I:V...]  sets GSW bytes (swByteSet).  gswf=I[,I...]  sets GSWF flags (swSet). These two are set
 *   earlier, while the warp's first map loads, so its set-up script sees them.
 * marioat=hit:<name>[,dy[,dx,dz]]  puts Mario at the named hit object's centre (hitObjGetPos) plus that offset;
 *   hit:npc<i> is NPC slot i's position instead. hitpos=name,...  logs hit objects' centres; npcpos logs every
 *   NPC's slot, name (hex) and position (both at the marioat point).
 * marioat2=<as marioat>@F (also marioat3, marioat4)  more placements at game frame F (in the field), e.g. after a
 *   door.
 * syslog also logs on change: coins, the held items, Pianta tokens, GSW 0, Mario's motion id (in the field),
 *   each message or choice window an event opens ("sys: msg <tag>"; a choice also "sys: select <tag> -> N" when
 *   it closes), and the GSW bytes / GSWF flags listed in swwatch=I,fI,... ("sys: gsw I = V", "sys: gswf I = V").
 */
#include <dolphin/pad.h>
#include <stdio.h>
#include <string.h>

#include "driver/hitdrv.h"
#include "driver/msgdrv.h"
#include "driver/npcdrv.h"
#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "driver/windowdrv.h"
#include "event.h"
#include "evt/evt_cmd.h"
#include "evt/evt_mario.h"
#include "evt/evt_story.h"
#include "evt/evt_yuugijou.h"
#include "manager/evtmgr.h"
#include "mario/mario.h"
#include "mario/mario_pouch.h"
#include "mario/mariost.h"
#include "test_mode.h"
#include "win/win_main.h"

/* storyjump: the setups of every story point with an id up to story, in table order; GSW 0 = story. */
void tst_story_jump(int story) {
    s32 st, i, n = 0;
    for (st = 0; st < eventStgNum(); st++) {
        EventStage* stage = eventStgDtPtr(st);
        for (i = 0; i < stage->count; i++) {
            EventData* d = &stage->data[i];
            if ((int)d->id > story) {
                goto done;
            }
            if (d->initEvent != NULL) {
                d->initEvent();
                n++;
            }
        }
    }
done:
    swByteSet(GSW_STORY_PROGRESS, story);
    tst_log("sys: story jump to %d: %ld setups run, mail %ld", story, (long)n, (long)pouchReceiveMailCount());
}

static int s_state = -1; /* -1 unread, 0 nothing to do, 1 waiting for the map */
static int s_log;
static int s_at, s_hp;
static int s_wait;
static char s_map[17];
static u32 s_map_frames;

/* side content set-up */
#define SETUP_MAX 16
static int s_items[SETUP_MAX], s_items_n;
static int s_gsw[SETUP_MAX][2], s_gsw_n;
static int s_gswf[SETUP_MAX], s_gswf_n;
static int s_coins = -1, s_piantas = -1;
static int s_watch[SETUP_MAX], s_watch_flag[SETUP_MAX], s_watch_n;

static void read_autopilot(void);

/* an integer, "0x" hex or decimal, optionally negative; *end is where it stopped */
static long parse_int(const char* p, const char** end) {
    long v = 0;
    int neg = 0, base = 10, digits = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (;; p++, digits++) {
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
        v = v * base + d;
    }
    *end = p;
    if (digits == 0) {
        return 0;
    }
    return neg ? -v : v;
}

static int atoi_simple(const char* p) {
    const char* end;
    return (int)parse_int(p, &end);
}

/* a list of integers ("0x" hex or decimal) separated by ',' (and ':' within a pair) */
static int parse_ints(const char* p, int* out, int max) {
    int n = 0;
    while (*p != '\0' && n < max) {
        const char* end;
        long v = parse_int(p, &end);
        if (end == p) {
            break;
        }
        out[n++] = (int)v;
        p = end;
        if (*p == ',' || *p == ':') {
            p++;
        }
    }
    return n;
}

static f32 parse_f(const char** pp) {
    const char* p = *pp;
    f32 v = 0.0f, scale = 1.0f;
    int neg = 0, frac = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    for (; (*p >= '0' && *p <= '9') || (*p == '.' && !frac); p++) {
        if (*p == '.') {
            frac = 1;
        } else if (frac) {
            scale *= 0.1f;
            v += (f32)(*p - '0') * scale;
        } else {
            v = v * 10.0f + (f32)(*p - '0');
        }
    }
    if (*p == ',') {
        p++;
    }
    *pp = p;
    return neg ? -v : v;
}

/* marioat / marioat2: x,y,z, or hit:<name>[,dy,dx,dz] (a hit object's centre, or NPC slot i for npc<i>) */
typedef struct {
    int used;
    char hit[32];
    f32 pos[3], d[3];
    u32 frame;
    EvtWord script[8];
} Place;
#define PLACES 4
static Place s_place[PLACES];

static void parse_place(Place* pl, char* v) {
    const char* p = v;
    pl->used = 1;
    if (strncmp(v, "hit:", 4) == 0) {
        char* comma = strchr(v + 4, ',');
        if (comma != NULL) {
            *comma = '\0';
            p = comma + 1;
            pl->d[1] = parse_f(&p);
            pl->d[0] = parse_f(&p);
            pl->d[2] = parse_f(&p);
        }
        memcpy(pl->hit, v + 4, strlen(v + 4) < sizeof(pl->hit) ? strlen(v + 4) + 1 : sizeof(pl->hit) - 1);
    } else {
        pl->pos[0] = parse_f(&p);
        pl->pos[1] = parse_f(&p);
        pl->pos[2] = parse_f(&p);
    }
}

static void place(Place* pl, u32 frame) {
    f32 pos[3];
    if (strncmp(pl->hit, "npc", 3) == 0 && pl->hit[3] >= '0' && pl->hit[3] <= '9' && npcGetWorkPtr() != NULL) {
        NpcEntry* e = &npcGetWorkPtr()->entries[atoi_simple(pl->hit + 3)];
        tst_log("sys: npc slot %s at %ld,%ld,%ld", pl->hit + 3, (long)e->position.x, (long)e->position.y,
                (long)e->position.z);
        pos[0] = e->position.x;
        pos[1] = e->position.y;
        pos[2] = e->position.z;
    } else if (pl->hit[0] != '\0') {
        Vec v;
        hitObjGetPos(pl->hit, &v);
        tst_log("sys: hit %s at %ld,%ld,%ld%s", pl->hit, (long)v.x, (long)v.y, (long)v.z,
                hitNameToPtr(pl->hit) == NULL ? " (not found)" : "");
        pos[0] = v.x;
        pos[1] = v.y;
        pos[2] = v.z;
    } else {
        pos[0] = pl->pos[0];
        pos[1] = pl->pos[1];
        pos[2] = pl->pos[2];
    }
    pos[0] += pl->d[0];
    pos[1] += pl->d[1];
    pos[2] += pl->d[2];
    pl->script[0] = EVT_CMD(4, OPCODE_USER_FUNC);
    pl->script[1] = (EvtWord)evt_mario_set_pos;
    pl->script[2] = FLOAT(pos[0]);
    pl->script[3] = FLOAT(pos[1]);
    pl->script[4] = FLOAT(pos[2]);
    pl->script[5] = EVT_CMD(0, OPCODE_END_EVENT);
    pl->script[6] = OPCODE_END_SCRIPT;
    evtEntry(pl->script, 0, 0);
    pl->used = 0;
    tst_log("sys: marioat %ld,%ld,%ld on %s at frame %lu", (long)pos[0], (long)pos[1], (long)pos[2],
            gp->currentMapName, (unsigned long)frame);
}

static void read_switches(void) {
    char v[64];
    s_state = 0;
    int pairs[SETUP_MAX * 2], i, n;
    s_log = tst_sw_has("syslog");
    if (tst_sw_value("marioat", v, sizeof(v)) && v[0] != '\0') {
        parse_place(&s_place[0], v);
        s_at = 1;
    }
    for (i = 1; i < PLACES; i++) {
        char key[12];
        sprintf(key, "marioat%d", i + 1);
        if (tst_sw_value(key, v, sizeof(v)) && v[0] != '\0') {
            char* at = strchr(v, '@');
            if (at != NULL) {
                *at = '\0';
                parse_place(&s_place[i], v);
                s_place[i].frame = (u32)atoi_simple(at + 1);
            }
        }
    }
    s_hp = tst_sw_int("hp", -1);
    s_wait = tst_sw_int("marioatwait", 30);
    if (tst_sw_value("items", v, sizeof(v))) {
        s_items_n = parse_ints(v, s_items, SETUP_MAX);
    }
    if (tst_sw_value("gsw", v, sizeof(v))) {
        n = parse_ints(v, pairs, SETUP_MAX * 2);
        for (i = 0; i + 1 < n; i += 2, s_gsw_n++) {
            s_gsw[s_gsw_n][0] = pairs[i];
            s_gsw[s_gsw_n][1] = pairs[i + 1];
        }
    }
    if (tst_sw_value("gswf", v, sizeof(v))) {
        s_gswf_n = parse_ints(v, s_gswf, SETUP_MAX);
    }
    s_coins = tst_sw_int("coins", -1);
    s_piantas = tst_sw_int("piantas", -1);
    if (tst_sw_value("swwatch", v, sizeof(v))) {
        const char* p = v;
        while (*p != '\0' && s_watch_n < SETUP_MAX) {
            const char* end;
            int flag = (*p == 'f');
            long id = parse_int(p + flag, &end);
            if (end == p + flag) {
                break;
            }
            s_watch[s_watch_n] = (int)id;
            s_watch_flag[s_watch_n++] = flag;
            p = *end == ',' ? end + 1 : end;
        }
    }
    read_autopilot();
    if (s_at || s_hp >= 0 || s_items_n || s_gsw_n || s_gswf_n || s_coins >= 0 || s_piantas >= 0 ||
        tst_sw_value("hitpos", v, sizeof(v)) || tst_sw_has("npcpos")) {
        s_state = 1;
    }
}

/* gsw= and gswf=: set while the warp's map loads (after its new game cleared the flags), so the map's own
 * set-up script sees them */
static void apply_flags(u32 frame) {
    int i;
    for (i = 0; i < s_gsw_n; i++) {
        swByteSet(s_gsw[i][0], s_gsw[i][1]);
        tst_log("sys: set gsw %d = %d at frame %lu", s_gsw[i][0], s_gsw[i][1], (unsigned long)frame);
    }
    for (i = 0; i < s_gswf_n; i++) {
        swSet(s_gswf[i]);
        tst_log("sys: set gswf %d at frame %lu", s_gswf[i], (unsigned long)frame);
    }
}

static void apply(u32 frame) {
    int i;
    for (i = 0; i < s_items_n; i++) {
        BOOL ok = pouchGetItem((ItemType)s_items[i]);
        tst_log("sys: give item 0x%x %s", s_items[i], ok ? "ok" : "failed");
    }
    if (s_coins >= 0) {
        pouchSetCoin(s_coins);
        tst_log("sys: coins set to %d", s_coins);
    }
    if (s_piantas >= 0 && yuwp != NULL) {
        /* held, as after walking into the parlor: the counter counts (monteCountDisp) only with the tokens out */
        yuwp->tokens = s_piantas;
        yuwp->dispTokens = (u32)s_piantas;
        yuwp->flags |= PARLOR_FLAG_TOKENS_OUT | PARLOR_FLAG_COUNTER_SHOWN; /* the counter, which animates dispTokens, runs only when shown */
        tst_log("sys: piantas set to %d", s_piantas);
    }
    {
        /* hitpos=name,name...: log those hit objects' centres (to find where things are) */
        char names[200];
        if (tst_sw_value("hitpos", names, sizeof(names))) {
            char* p = names;
            while (*p != '\0') {
                char* c = strchr(p, ',');
                Vec pos;
                if (c != NULL) {
                    *c = '\0';
                }
                hitObjGetPos(p, &pos);
                tst_log("sys: hit %s at %ld,%ld,%ld%s", p, (long)pos.x, (long)pos.y, (long)pos.z,
                        hitNameToPtr(p) == NULL ? " (not found)" : "");
                if (c == NULL) {
                    break;
                }
                p = c + 1;
            }
        }
    }
    if (tst_sw_has("npcpos") && npcGetWorkPtr() != NULL) {
        /* npcpos: every active NPC's name (hex, the names are Shift-JIS) and position */
        NpcWork* nw = npcGetWorkPtr();
        for (i = 0; i < nw->total; i++) {
            NpcEntry* e = &nw->entries[i];
            char hex[40];
            int k;
            if (!(e->flags & 1)) {
                continue;
            }
            for (k = 0; k < 16 && e->description[k] != '\0'; k++) {
                sprintf(hex + 2 * k, "%02x", (unsigned)(unsigned char)e->description[k]);
            }
            hex[2 * k] = '\0';
            tst_log("sys: npc %d %s at %ld,%ld,%ld", i, hex, (long)e->position.x, (long)e->position.y,
                    (long)e->position.z);
        }
    }
    if (s_at) {
        place(&s_place[0], frame);
    }
    if (s_hp >= 0) {
        pouchSetHP(s_hp);
        tst_log("sys: hp set to %d on %s at frame %lu", s_hp, gp->currentMapName, (unsigned long)frame);
    }
}

static void log_changes(u32 frame) {
    static int last_seq = -99, last_hp = -99;
    static s32 last_win[8] = {-99};
    WindowWork* w;
    s32 cur[8];
    int seq = (int)seqGetSeq(), hp, i, changed = 0;

    if (seq != last_seq) {
        tst_log("sys: seq %d -> %d at frame %lu map %s", last_seq, seq, (unsigned long)frame, gp->currentMapName);
        last_seq = seq;
    }
    hp = (seq == SEQ_GAME || seq == SEQ_BATTLE || seq == SEQ_GAME_OVER) ? (int)pouchGetHP() : -1;
    if (hp != last_hp && hp >= 0) {
        tst_log("sys: hp %d at frame %lu", hp, (unsigned long)frame);
        last_hp = hp;
    }
    w = winGetPtr();
    memset(cur, 0, sizeof(cur));
    if (w != NULL && winCheck()) {
        cur[0] = 1;
        cur[1] = w->active;
        if (cur[1] > 20 && cur[1] < 100) {
            cur[1] = 20; /* the closing animation counts 20, 21, ... one per frame: log it once */
        }
        cur[2] = (w->tabSel >= 0 && w->tabSel < 5) ? w->tab[w->tabSel].id : -1;
        cur[3] = w->item.state;
        cur[4] = w->item.mode;
        cur[5] = w->badge.state;
        cur[6] = w->log.state;
        cur[7] = w->party.mode;
    }
    for (i = 0; i < 8; i++) {
        if (cur[i] != last_win[i]) {
            changed = 1;
        }
    }
    if (changed) {
        if (cur[0]) {
            tst_log("sys: win open active %ld tab %ld item %ld/%ld badge %ld log %ld party %ld mail %ld at frame %lu",
                    (long)cur[1], (long)cur[2], (long)cur[3], (long)cur[4], (long)cur[5], (long)cur[6], (long)cur[7],
                    (long)w->item.mailNum, (unsigned long)frame);
        } else if (last_win[0] != -99) {
            tst_log("sys: win closed at frame %lu", (unsigned long)frame);
        }
        memcpy(last_win, cur, sizeof(cur));
    }
}

/* message and choice windows opened by events: window id -> what was logged */
#define WIN_IDS 16
static struct {
    const char* tag;
    int select, sel, seen;
} s_msgwin[WIN_IDS];

/* ---- dialogue autopilot (field only; battles have btlauto) ----
 * mash=N  while a message window waits for a key or Mario's controls are off (an event runs), presses A every
 *   N frames (held 2).
 * msgkeys=<tag>:<k>.<k>...,<tag>:...  when a message or choice window with that tag opens, presses those keys
 *   (a b x y l r z start up down left right; w = no key) one every msgstep=N frames (default 12, held 2) from
 *   msgdelay=N frames after it opened (default 24); mash waits meanwhile. Each entry is used once, in order, so a
 *   tag that opens three times can have three entries. The D-pad moves the cursor of a choice window.
 * nomash=F1-F2,...  frame ranges without mash, for menus the padscript drives. */
#define RULES_MAX 24
#define RULE_KEYS 8
static struct {
    char tag[24];
    u16 keys[RULE_KEYS];
    int n, used;
} s_rules[RULES_MAX];
static int s_rules_n, s_mash, s_msgstep = 12, s_msgdelay = 24;
static int s_run = -1; /* the rule whose keys are being pressed */
static u32 s_run_frame;

static u16 key_bit(const char* k, int n) {
    static const struct {
        const char* name;
        u16 bit;
    } keys[] = {
        {"a", PAD_BUTTON_A},         {"b", PAD_BUTTON_B},       {"x", PAD_BUTTON_X},       {"y", PAD_BUTTON_Y},
        {"l", PAD_TRIGGER_L},        {"r", PAD_TRIGGER_R},      {"z", PAD_TRIGGER_Z},      {"start", PAD_BUTTON_START},
        {"up", PAD_BUTTON_UP},       {"down", PAD_BUTTON_DOWN}, {"left", PAD_BUTTON_LEFT}, {"right", PAD_BUTTON_RIGHT},
        {"w", 0},
    };
    u32 i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if ((int)strlen(keys[i].name) == n && strncmp(keys[i].name, k, n) == 0) {
            return keys[i].bit;
        }
    }
    tst_log("msgkeys: unknown key %.*s", n, k);
    return 0;
}

static void read_autopilot(void) {
    char text[400];
    const char* p;
    s_mash = tst_sw_int("mash", 0);
    s_msgstep = tst_sw_int("msgstep", 12);
    s_msgdelay = tst_sw_int("msgdelay", 24);
    if (!tst_sw_value("msgkeys", text, sizeof(text))) {
        return;
    }
    p = text;
    while (*p != '\0' && s_rules_n < RULES_MAX) {
        const char* colon = strchr(p, ':');
        int len;
        if (colon == NULL) {
            break;
        }
        len = (int)(colon - p) < 23 ? (int)(colon - p) : 23;
        memcpy(s_rules[s_rules_n].tag, p, len);
        s_rules[s_rules_n].tag[len] = '\0';
        p = colon + 1;
        while (*p != '\0' && *p != ',') {
            const char* k = p;
            while (*p != '\0' && *p != ',' && *p != '.') {
                p++;
            }
            if (p > k && s_rules[s_rules_n].n < RULE_KEYS) {
                s_rules[s_rules_n].keys[s_rules[s_rules_n].n++] = key_bit(k, (int)(p - k));
            }
            if (*p == '.') {
                p++;
            }
        }
        if (*p == ',') {
            p++;
        }
        s_rules_n++;
    }
    tst_log("msgkeys: %d rules, mash %d", s_rules_n, s_mash);
}

/* a window with this tag just opened: start its next unused rule */
static void rule_start(const char* tag, u32 frame) {
    int i;
    if (s_run >= 0) {
        return;
    }
    for (i = 0; i < s_rules_n; i++) {
        if (!s_rules[i].used && strcmp(s_rules[i].tag, tag) == 0) {
            s_rules[i].used = 1;
            s_run = i;
            s_run_frame = frame;
            tst_log("msgkeys: rule %d (%s) at frame %lu", i, tag, (unsigned long)frame);
            return;
        }
    }
}

static int msg_waiting(void) {
    int i;
    for (i = 0; i < 7; i++) {
        WindowEntry* w = windowGetPointer(i);
        u16 state = *(u16*)w->state;
        if ((w->flags & WINDOW_FLAG_USED) && (state == 3 || (state == 2 && !(w->flags & WINDOW_FLAG_PRINTING)))) {
            return 1;
        }
    }
    return 0;
}

/* nomash=F1-F2[,F3-F4...]: game frame ranges where mash presses nothing (the padscript drives a menu) */
static int nomash(u32 frame) {
    static int n = -1;
    static int r[16];
    int i;
    if (n < 0) {
        char v[96], *p;
        n = 0;
        if (tst_sw_value("nomash", v, sizeof(v))) {
            for (p = v; *p != '\0'; p++) {
                if (*p == '-') {
                    *p = ',';
                }
            }
            n = parse_ints(v, r, 16) / 2;
        }
    }
    for (i = 0; i < n; i++) {
        if (frame >= (u32)r[2 * i] && frame <= (u32)r[2 * i + 1]) {
            return 1;
        }
    }
    return 0;
}

static void autopilot(u32 frame) {
    static int was_on;
    u32 keys = 0;
    if (s_mash <= 0 && s_rules_n == 0) {
        return;
    }
    if (seqGetSeq() != SEQ_GAME) {
        if (was_on) {
            tst_set_keys(0);
            was_on = 0;
        }
        return;
    }
    was_on = 1;
    if (s_run >= 0) {
        u32 since = frame - s_run_frame;
        if (since >= (u32)s_msgdelay) {
            u32 k = (since - (u32)s_msgdelay) / (u32)s_msgstep;
            if ((int)k >= s_rules[s_run].n) {
                s_run = -1;
            } else if ((since - (u32)s_msgdelay) % (u32)s_msgstep < 2) {
                keys = s_rules[s_run].keys[k];
            }
        }
    } else if (s_mash > 0 && frame % (u32)s_mash < 2 && !nomash(frame) && (msg_waiting() || marioKeyOffChk())) {
        keys = PAD_BUTTON_A;
    }
    tst_set_keys(keys);
}

static void log_window(s32 id, int select, u32 frame) {
    WindowEntry* w;
    MsgWinData* d;
    if (id < 0 || id >= WIN_IDS || !windowCheckID(id)) {
        return;
    }
    w = windowGetPointer(id);
    d = (w != NULL && w->userdata != NULL) ? (MsgWinData*)w->userdata->address : NULL;
    if (d == NULL || d->tag == NULL) {
        return;
    }
    select = (w->type == 5); /* msgdrv's choice window type (selectWindow_Main) */
    s_msgwin[id].seen = 1;
    if (s_msgwin[id].tag != d->tag) {
        s_msgwin[id].tag = d->tag;
        s_msgwin[id].select = select;
        if (s_log) {
            tst_log("sys: msg %.48s%s at frame %lu", d->tag, select ? " (choice)" : "", (unsigned long)frame);
        }
        rule_start(d->tag, frame);
    }
    if (select) {
        s_msgwin[id].sel = d->sel;
    }
}

static void log_side(u32 frame) {
    static int last_coins = -99, last_tokens = -99, last_gsw0 = -99, last_motion = -99;
    static u32 last_items = 0;
    static int last_watch[SETUP_MAX];
    static int watch_init;
    PouchData* pd = pouchGetPtr();
    int i, v;

    if (pd != NULL) {
        u32 h = 2166136261u;
        if (pd->coins != last_coins) {
            last_coins = pd->coins;
            tst_log("sys: coins %d at frame %lu", last_coins, (unsigned long)frame);
        }
        for (i = 0; i < MAX_HELD_ITEMS; i++) {
            h = (h ^ (u32)(u16)pd->heldItems[i]) * 16777619u;
        }
        if (h != last_items) {
            char buf[200];
            int n = 0;
            last_items = h;
            buf[0] = '\0';
            for (i = 0; i < MAX_HELD_ITEMS && n < (int)sizeof(buf) - 8; i++) {
                if (pd->heldItems[i] != 0) {
                    n += sprintf(buf + n, "%s%x", n ? "," : "", (unsigned)pd->heldItems[i]);
                }
            }
            tst_log("sys: items [%s] at frame %lu", buf, (unsigned long)frame);
        }
    }
    if (yuwp != NULL && yuwp->tokens != last_tokens) {
        last_tokens = yuwp->tokens;
        tst_log("sys: piantas %d at frame %lu", last_tokens, (unsigned long)frame);
    }
    v = (int)swByteGet(0);
    if (v != last_gsw0) {
        last_gsw0 = v;
        tst_log("sys: gsw0 %d at frame %lu", v, (unsigned long)frame);
    }
    for (i = 0; i < s_watch_n; i++) {
        v = s_watch_flag[i] ? (int)swGet(s_watch[i]) : (int)swByteGet(s_watch[i]);
        if (!watch_init || v != last_watch[i]) {
            last_watch[i] = v;
            tst_log("sys: gsw%s %d = %d at frame %lu", s_watch_flag[i] ? "f" : "", s_watch[i], v,
                    (unsigned long)frame);
        }
    }
    watch_init = 1;
    if (npcGetWorkPtr() != NULL && npcGetWorkPtr()->entries != NULL) {
        static NpcEntry* last_talk;
        NpcEntry* t = npcGetWorkPtr()->talkTarget;
        if (t != last_talk) {
            last_talk = t;
            if (t != NULL) {
                tst_log("sys: talk target npc %d at frame %lu", (int)(t - npcGetWorkPtr()->entries),
                        (unsigned long)frame);
            }
        }
    }
    if (seqGetSeq() == SEQ_GAME && marioGetPtr() != NULL && tst_sw_has("hitlog")) {
        /* hitlog: the hit objects in front of Mario (what A acts on) and under him, on change */
        static HitEntry *last_front, *last_fronta, *last_on;
        MarioWork* m = marioGetPtr();
        if (m->hitobjFront != last_front || m->hitobjFrontA != last_fronta || m->hitobjStandOn != last_on) {
            last_front = m->hitobjFront;
            last_fronta = m->hitobjFrontA;
            last_on = m->hitobjStandOn;
            tst_log("sys: hits front %s fronta %s on %s at frame %lu", last_front ? hitGetName(last_front) : "-",
                    last_fronta ? hitGetName(last_fronta) : "-", last_on ? hitGetName(last_on) : "-",
                    (unsigned long)frame);
        }
    }
    if (seqGetSeq() == SEQ_GAME && marioGetPtr() != NULL) {
        v = (int)marioGetPtr()->currMotionId;
        if (v != last_motion) {
            last_motion = v;
            tst_log("sys: motion 0x%x at frame %lu", v, (unsigned long)frame);
        }
    }
}

static void track_windows(u32 frame) {
    EventWork* ew = evtGetWork();
    int i;
    for (i = 0; i < WIN_IDS; i++) {
        s_msgwin[i].seen = 0;
    }
    if (ew != NULL && ew->entries != NULL) {
        for (i = 0; i < ew->count; i++) {
            EventEntry* e = &ew->entries[i];
            if (!(e->flags & EVENT_ACTIVE)) {
                continue;
            }
            /* the choice first: a stale printWindowId can name the same (reused) window */
            log_window(e->selectWindowId, 1, frame);
            log_window(e->printWindowId, 0, frame);
        }
    }
    for (i = 0; i < WIN_IDS; i++) {
        if (!s_msgwin[i].seen && s_msgwin[i].tag != NULL) {
            if (s_msgwin[i].select) {
                tst_log("sys: select %.48s -> %d at frame %lu", s_msgwin[i].tag, s_msgwin[i].sel,
                        (unsigned long)frame);
            }
            s_msgwin[i].tag = NULL;
        }
    }
}

void tst_systems_frame(u32 frame) {
    if (s_state < 0) {
        read_switches();
    }
    if (gp == NULL) {
        return;
    }
    if (s_log) {
        log_changes(frame);
        log_side(frame);
    }
    if (s_log || s_mash > 0 || s_rules_n > 0) {
        track_windows(frame);
    }
    autopilot(frame);
    {
        static int flags_done;
        int i;
        if (!flags_done && seqGetSeq() == SEQ_MAP_CHANGE && tst_warp_frames() > 0) {
            flags_done = 1;
            apply_flags(frame);
        }
        for (i = 1; i < PLACES; i++) {
            if (s_place[i].used && frame >= s_place[i].frame && seqGetSeq() == SEQ_GAME) {
                place(&s_place[i], frame);
            }
        }
    }
    if (s_state != 1) {
        return;
    }
    if (seqGetSeq() != SEQ_GAME || tst_warp_frames() == 0) {
        s_map_frames = 0;
        return;
    }
    if (strncmp(s_map, gp->currentMapName, 16) != 0) {
        memcpy(s_map, gp->currentMapName, 16);
        s_map[16] = '\0';
        s_map_frames = 0;
    }
    if (++s_map_frames >= (u32)s_wait) {
        apply(frame);
        s_state = 0;
    }
}
