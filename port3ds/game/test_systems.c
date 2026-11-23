/* Systems test hooks (port only; tools/port3ds/systems_test.py, docs/PORT_3DS_TESTING.md "Systems tests").
 *
 * storyjump  with map= (and story=N): the warp's new game runs the setup of every story point up to N, as the
 *   E3 scenario select does (event.c's tables: items, mail, partners, flags), instead of setting GSW 0 only.
 * marioat=x,y,z  once the map the game is on has been up marioatwait=N frames (default 30), puts Mario there
 *   through evt_mario_set_pos, the way event scripts move him. Once per run.
 * hp=N  at the same point, sets Mario's HP to N (pouchSetHP). Once per run.
 * syslog  logs, when they change, the sequence ("sys: seq A -> B"), the pause menu's state (open, tab, the
 *   tab's own state and mode) and Mario's HP, each with the game frame. Read by systems_test.py.
 */
#include <stdio.h>
#include <string.h>

#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "event.h"
#include "evt/evt_cmd.h"
#include "evt/evt_mario.h"
#include "evt/evt_story.h"
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
static f32 s_at_pos[3];
static int s_wait;
static char s_map[17];
static u32 s_map_frames;
static EvtWord s_script[8];

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

static void read_switches(void) {
    char v[64];
    s_state = 0;
    s_log = tst_sw_has("syslog");
    if (tst_sw_value("marioat", v, sizeof(v)) && v[0] != '\0') {
        const char* p = v;
        s_at_pos[0] = parse_f(&p);
        s_at_pos[1] = parse_f(&p);
        s_at_pos[2] = parse_f(&p);
        s_at = 1;
    }
    s_hp = tst_sw_int("hp", -1);
    s_wait = tst_sw_int("marioatwait", 30);
    if (s_at || s_hp >= 0) {
        s_state = 1;
    }
}

static void apply(u32 frame) {
    if (s_at) {
        s_script[0] = EVT_CMD(4, OPCODE_USER_FUNC);
        s_script[1] = (EvtWord)evt_mario_set_pos;
        s_script[2] = FLOAT(s_at_pos[0]);
        s_script[3] = FLOAT(s_at_pos[1]);
        s_script[4] = FLOAT(s_at_pos[2]);
        s_script[5] = EVT_CMD(0, OPCODE_END_EVENT);
        s_script[6] = OPCODE_END_SCRIPT;
        evtEntry(s_script, 0, 0);
        tst_log("sys: marioat %ld,%ld,%ld on %s at frame %lu", (long)s_at_pos[0], (long)s_at_pos[1],
                (long)s_at_pos[2], gp->currentMapName, (unsigned long)frame);
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

void tst_systems_frame(u32 frame) {
    if (s_state < 0) {
        read_switches();
    }
    if (gp == NULL) {
        return;
    }
    if (s_log) {
        log_changes(frame);
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
