/* Gallery switches, shared by the 3DS port and the GameCube debug DOL (C89).
 *
 * fx=<name|index>[,<name|index>...]  spawns effects through evt_eff (or
 *   evt_eff64 for n64:<name|index>), the way event scripts start them, once
 *   the map the warp went to has been up for fxwait=N frames (default 60).
 *   fxargs=a,b,c,... replaces the 13 script arguments after the effect name
 *   (default: 0, Mario's x, y + 30, z, then 1.0; each is passed as a script
 *   float, which the effect reads as an int where it needs one). Several
 *   effects are spawned fxgap=N frames apart (default 120). Each spawn logs
 *   "fx: <set> ..." and, fxgap frames later or at the next spawn, whether the
 *   effect is still alive.
 * battle=<id|name>  starts the battle of the current area's battle setup
 *   table entry <id> (as npcSetBattleInfo numbers them) or of that name,
 *   once the map has been up battlewait=N frames (default 60), the way
 *   evt_npc_battle_start does, with the first NPC on the map as the enemy in
 *   the field. Logs "battle: start ..." and "battle: end result R ...".
 */
#include <string.h>

#include "battle/battle.h"
#include "battle/battle_information.h"
#include "data/mapdata.h"
#include "battle/battle_stage.h"
#include "driver/effdrv.h"
#include "driver/mapdrv.h"
#include "driver/npcdrv.h"
#include "driver/seqdrv.h"
#include "eff/eff_n64.h"
#include "evt/evt_cmd.h"
#include "evt/evt_eff.h"
#include "manager/evtmgr.h"
#include "mario/mario.h"
#include "mario/mario_party.h"
#include "mario/mariost.h"
#include "party.h"
#include "pmario_sound.h"
#include "test_mode.h"

extern EffectSet eff_set_table[];
extern eff_set eff_set_n64[];

/* Frames the current map has been up in SEQ_GAME (after the first map change). */
static u32 s_map_frames;
static char s_map_seen[17];

static void track_map(void) {
    if (gp == NULL || tst_warp_frames() == 0 || seqGetSeq() != SEQ_GAME) {
        s_map_frames = 0;
        return;
    }
    if (strncmp(s_map_seen, gp->currentMapName, 16) != 0) {
        memcpy(s_map_seen, gp->currentMapName, 16);
        s_map_seen[16] = '\0';
        s_map_frames = 0;
    }
    s_map_frames++;
}

/* ---- fx= ---- */

#define FX_MAX 16
#define FX_ARGS 13

static int s_fx_state = -1; /* -1 unread, 0 off/done, 1 running */
static char s_fx_list[FX_MAX][24];
static int s_fx_n, s_fx_i;
static int s_fx_wait, s_fx_gap;
static u32 s_fx_next, s_fx_spawned;
static int s_fx_have_args;
static f32 s_fx_args[FX_ARGS];
static char s_fx_name[24];
static int s_fx_alive_check;

static EvtWord s_fx_script[2 + 15 + 2];

static f32 parse_f32(const char* p);

static int parse_int(const char* p) {
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
    }
    return v;
}

static void fx_read(void) {
    char text[200];
    char* p;
    s_fx_state = 0;
    if (!tst_sw_value("fx", text, sizeof(text)) || text[0] == '\0') {
        return;
    }
    p = text;
    while (*p != '\0' && s_fx_n < FX_MAX) {
        char* comma = strchr(p, ',');
        int len = comma != NULL ? (int)(comma - p) : (int)strlen(p);
        if (len > 23) {
            len = 23;
        }
        memcpy(s_fx_list[s_fx_n], p, len);
        s_fx_list[s_fx_n][len] = '\0';
        s_fx_n++;
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    s_fx_wait = tst_sw_int("fxwait", 60);
    s_fx_gap = tst_sw_int("fxgap", 120);
    if (tst_sw_value("fxargs", text, sizeof(text)) && text[0] != '\0') {
        int i = 0;
        p = text;
        while (i < FX_ARGS && *p != '\0') {
            s_fx_args[i++] = parse_f32(p);
            p = strchr(p, ',');
            if (p == NULL) {
                break;
            }
            p++;
        }
        s_fx_have_args = 1;
    }
    tst_log("fx: %d effects, first after %d frames on the map, %d frames apart", s_fx_n, s_fx_wait, s_fx_gap);
    s_fx_state = s_fx_n > 0 ? 1 : 0;
}

/* A decimal number ("-12", "3.5"); MSL has no atof in this build. */
static f32 parse_f32(const char* p) {
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
    return neg ? -v : v;
}

static int is_number(const char* s) {
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

/* The effect-set name for an fx= word, or NULL. */
static const char* fx_set_name(const char* word, int* n64) {
    int i;
    *n64 = strncmp(word, "n64:", 4) == 0;
    if (*n64) {
        word += 4;
        if (is_number(word)) {
            int want = parse_int(word);
            for (i = 0; eff_set_n64[i].id != -1; i++) {
                if (i == want) {
                    return eff_set_n64[i].name;
                }
            }
            return NULL;
        }
        return effGetSetN64(word) != NULL ? word : NULL;
    }
    if (is_number(word)) {
        int want = parse_int(word);
        for (i = 0; eff_set_table[i].id != -1; i++) {
            if (i == want) {
                return eff_set_table[i].name;
            }
        }
        return NULL;
    }
    return effGetSet(word) != NULL ? word : NULL;
}

static void fx_spawn(const char* word, u32 frame) {
    int n64, i;
    const char* set = fx_set_name(word, &n64);
    MarioWork* m = marioGetPtr();
    f32 args[FX_ARGS];
    EventEntry* evt;

    if (set == NULL) {
        tst_log("fx: %s is not an effect set name or index", word);
        return;
    }
    if (s_fx_have_args) {
        memcpy(args, s_fx_args, sizeof(args));
    } else {
        args[0] = 0.0f;
        args[1] = m->position.x;
        args[2] = m->position.y + 30.0f;
        args[3] = m->position.z;
        for (i = 4; i < FX_ARGS; i++) {
            args[i] = 1.0f;
        }
    }
    strcpy(s_fx_name, "tst_fx");
    i = 0;
    s_fx_script[i++] = EVT_CMD(16, OPCODE_USER_FUNC);
    s_fx_script[i++] = (EvtWord)(n64 ? evt_eff64 : evt_eff);
    s_fx_script[i++] = (EvtWord)s_fx_name;
    s_fx_script[i++] = (EvtWord)set;
    for (; i < 2 + 15; i++) {
        s_fx_script[i] = FLOAT(args[i - 4]);
    }
    s_fx_script[i++] = EVT_CMD(0, OPCODE_END_EVENT);
    s_fx_script[i++] = OPCODE_END_SCRIPT;
    evt = evtEntry(s_fx_script, 0, 0);
    tst_log("fx: %s%s at frame %lu (map %s, %lu frames up), args %ld %ld %ld %ld (x1024), event %ld", n64 ? "n64:" : "",
            set, (unsigned long)frame, gp->currentMapName, (unsigned long)s_map_frames, (long)(args[0] * 1024.0f),
            (long)(args[1] * 1024.0f), (long)(args[2] * 1024.0f), (long)(args[3] * 1024.0f),
            evt != NULL ? (long)evt->eventId : -1L);
    s_fx_alive_check = 1;
}

static void fx_frame(u32 frame) {
    if (s_fx_state < 0) {
        fx_read();
    }
    if (s_fx_state != 1) {
        return;
    }
    if (s_fx_i == 0 && s_fx_next == 0) {
        if (s_map_frames < (u32)s_fx_wait) {
            return;
        }
        s_fx_next = frame;
    }
    if (frame == s_fx_spawned + 2 && s_fx_alive_check) {
        EffectEntry* e = effNameToPtr(s_fx_name);
        tst_log("fx: %s 2 frames after the spawn: %s", s_fx_list[s_fx_i - 1], e != NULL ? "alive" : "not found");
    }
    if (frame != s_fx_next) {
        return;
    }
    if (s_fx_i > 0 && s_fx_alive_check) {
        EffectEntry* e = effNameToPtr(s_fx_name);
        tst_log("fx: %s after %d frames: %s", s_fx_list[s_fx_i - 1], s_fx_gap, e != NULL ? "alive" : "ended");
        if (e != NULL) {
            effDelete(e);
        }
        s_fx_alive_check = 0;
    }
    if (s_fx_i >= s_fx_n) {
        tst_log("fx: done");
        s_fx_state = 0;
        return;
    }
    if (seqGetSeq() != SEQ_GAME) {
        s_fx_next++;
        return;
    }
    fx_spawn(s_fx_list[s_fx_i], frame);
    s_fx_spawned = frame;
    s_fx_i++;
    s_fx_next = frame + (u32)s_fx_gap;
}

/* ---- battle= ---- */

static int s_btl_state = -1; /* -1 unread, 0 off/done, 1 waiting, 2 in battle */
static char s_btl_word[32];
static int s_btl_wait;
static u32 s_btl_start;

static NpcEntry* battle_npc(void) {
    NpcWork* w = npcGetWorkPtr();
    NpcEntry* any = NULL;
    s32 i;
    for (i = 0; i < w->total; i++) {
        NpcEntry* e = &w->entries[i];
        if (!(e->flags & NPC_FLAG_USED)) {
            continue;
        }
        if (e->flags & NPC_FLAG_ENEMY) {
            return e;
        }
        if (any == NULL) {
            any = e;
        }
    }
    return any;
}

static void battle_start(u32 frame) {
    char area[4];
    AreaData* ad;
    NpcSetupBattleInfo* tbl;
    s32 id = -1, n = 0, i;
    NpcEntry* npc;
    FieldBattleData* fbat;
    PartyEntry* party;

    area[0] = gp->currentMapName[0];
    area[1] = gp->currentMapName[1];
    area[2] = gp->currentMapName[2];
    area[3] = '\0';
    ad = areaDataPtr(area);
    tbl = ad != NULL ? (NpcSetupBattleInfo*)ad->btlSetupTbl : NULL;
    if (tbl == NULL) {
        tst_log("battle: area %s has no battle setup table", area);
        s_btl_state = 0;
        return;
    }
    for (n = 0; tbl[n].name != NULL; n++) {
        if (strcmp(tbl[n].name, s_btl_word) == 0) {
            id = n;
        }
    }
    if (id < 0 && is_number(s_btl_word)) {
        id = parse_int(s_btl_word);
    }
    if (id < 0 || id >= n) {
        tst_log("battle: %s is not in area %s's table (%ld setups):", s_btl_word, area, (long)n);
        for (i = 0; i < n; i++) {
            tst_log("battle:   %ld %s", (long)i, tbl[i].name);
        }
        s_btl_state = 0;
        return;
    }
    npc = battle_npc();
    if (npc == NULL) {
        tst_log("battle: no NPC on %s to stand in for the enemy", gp->currentMapName);
        s_btl_state = 0;
        return;
    }
    npcSetBattleInfo(npc, id);
    if (npc->battleInfo.groupSetup == NULL) {
        tst_log("battle: setup %ld (%s) chose no group", (long)id, tbl[id].name);
        s_btl_state = 0;
        return;
    }
    fbat = fbatGetPointer();
    fbat->target = npc;
    marioFBattlePrepare();
    marioCtrlOff();
    party = partyGetPtr(marioGetPartyId());
    if (party != NULL) {
        partyCtrlOff();
    }
    BattleInformationSetResult(&fbat->info, 0);
    psndSetFlag(PSND_FLAG_SFX_WITH_BGM);
    seqSetSeq(SEQ_BATTLE, NULL, NULL);
    tst_log("battle: start %ld (%s) on %s at frame %lu, npc %s", (long)id, tbl[id].name, gp->currentMapName,
            (unsigned long)frame, npc->description);
    s_btl_start = frame;
    s_btl_state = 2;
}

static void battle_frame(u32 frame) {
    if (s_btl_state < 0) {
        s_btl_state = 0;
        if (tst_sw_value("battle", s_btl_word, sizeof(s_btl_word)) && s_btl_word[0] != '\0') {
            s_btl_wait = tst_sw_int("battlewait", 60);
            s_btl_state = 1;
            tst_log("battle: %s after %d frames on the map", s_btl_word, s_btl_wait);
        }
    }
    if (s_btl_state == 1 && s_map_frames >= (u32)s_btl_wait && seqGetSeq() == SEQ_GAME) {
        battle_start(frame);
    } else if (s_btl_state == 2 && frame > s_btl_start + 2 && seqGetSeq() == SEQ_GAME) {
        FieldBattleData* fbat = fbatGetPointer();
        PartyEntry* party = partyGetPtr(marioGetPartyId());
        tst_log("battle: end result %ld after %lu frames", (long)BattleInformationGetResult(&fbat->info),
                (unsigned long)(frame - s_btl_start));
        marioCtrlOn();
        if (party != NULL) {
            partyCtrlOn();
        }
        s_btl_state = 0;
    }
}

/* ---- battle stage maps (stg_*) for the map sweep ----
 * The stg_* maps are the battle backgrounds: bmapLoad loads one next to the
 * bti_0x stage as a battle's specificStageDataDir, so no map change can enter
 * them. The sweep runs the game's own path instead: the host map's enemy starts
 * a battle (as battle= does) whose stage data names the stg map, with no stage
 * objects. */
static BattleStageData s_stg_data;
static char s_stg_name[16];
static int s_stg_state; /* 0 idle, 1 battle starting, 2 shot taken, battle ending */
static int s_stg_wait;
static int s_stg_up; /* frames the battle has been set up */

const char* tst_stage_host(const char* name, int* story) {
    if (strncmp(name, "stg", 3) != 0) {
        return NULL;
    }
    *story = 30;
    return "hei_01";
}

int tst_stage_start(const char* name) {
    NpcEntry* npc;
    FieldBattleData* fbat;
    PartyEntry* party;

    npc = battle_npc();
    if (npc == NULL || seqGetSeq() != SEQ_GAME) {
        tst_log("stage: %s: no host enemy on %s", name, gp->currentMapName);
        return 0;
    }
    npcSetBattleInfo(npc, 4);
    if (npc->battleInfo.groupSetup == NULL || npc->battleInfo.stageData == NULL) {
        tst_log("stage: %s: host setup 4 chose no group", name);
        return 0;
    }
    memset(&s_stg_data, 0, sizeof(s_stg_data));
    strncpy(s_stg_name, name, sizeof(s_stg_name) - 1);
    s_stg_name[sizeof(s_stg_name) - 1] = '\0';
    s_stg_data.globalStageDataDir = "bti_01";
    s_stg_data.specificStageDataDir = s_stg_name;
    npc->battleInfo.stageData = &s_stg_data;
    fbat = fbatGetPointer();
    fbat->target = npc;
    marioFBattlePrepare();
    marioCtrlOff();
    party = partyGetPtr(marioGetPartyId());
    if (party != NULL) {
        partyCtrlOff();
    }
    BattleInformationSetResult(&fbat->info, 0);
    psndSetFlag(PSND_FLAG_SFX_WITH_BGM);
    seqSetSeq(SEQ_BATTLE, NULL, NULL);
    s_stg_state = 1;
    s_stg_wait = 0;
    s_stg_up = 0;
    tst_log("stage: %s battle started", name);
    return 1;
}

/* Called every frame while a stage battle runs. 0 waiting; 1 the battle is up
 * with the stage loaded for settle frames (take the picture now; the battle is
 * then ended); 2 back in the field; -1 the stage never came up. */
int tst_stage_poll(int settle) {
    MapWork* mw;
    int seq = seqGetSeq();

    s_stg_wait++;
    if (s_stg_state == 1) {
        if (seq == SEQ_BATTLE && _battleWorkPointer != NULL && gp->inBattle) {
            s_stg_up++;
        }
        if (s_stg_up > settle) {
            mw = mapGetWork();
            tst_log("stage: %s up: %ld maps, stage map joints %ld, frame %d", s_stg_name,
                    (long)mw->entries[0].count, (long)mw->entries[1].numJoints, s_stg_wait);
            s_stg_state = 2;
            s_stg_wait = 0;
            _battleWorkPointer->flags |= BATTLE_SEQ_END;
            if (mw->entries[0].count != 2 || mw->entries[1].numJoints <= 0) {
                tst_log("stage: %s did not load", s_stg_name);
                return -1;
            }
            return 1;
        }
        if (s_stg_wait > 1800) {
            tst_log("stage: %s timeout (sequence %d)", s_stg_name, seq);
            return -1;
        }
        return 0;
    }
    if (s_stg_state == 2) {
        if (seq == SEQ_GAME) {
            s_stg_state = 0;
            return 2;
        }
        if (s_stg_wait > 900) {
            tst_log("stage: %s battle did not end (sequence %d)", s_stg_name, seq);
            return -1;
        }
    }
    return 0;
}

void tst_gallery_frame(u32 frame) {
    track_map();
    fx_frame(frame);
    battle_frame(frame);
}
