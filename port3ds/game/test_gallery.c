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
 * fx= also takes ranges of indices: fx=0-80,n64:0-102.
 * Shots for the galleries (tools/port3ds/gallery.py): shotat=F,F,... (game
 *   frames), fxshots=O,O,... (frames after each fx= spawn) and btlshots=O,...
 *   (frames after the battle start). A shot of frame F is the picture finished
 *   at F's frame hook: the port saves it to gallery/fFFFFF.ppm (requested at
 *   F-1, tst_plat_shot), the GameCube build calls tst_shot_point(F), where
 *   run_dolphin.py --shots reads the XFB. Both log "shot: f=F".
 */
#include <stdio.h>
#include <string.h>

#include "battle/battle.h"
#include "battle/battle_information.h"
#include "data/mapdata.h"
#include "driver/effdrv.h"
#include "driver/npcdrv.h"
#include "driver/seqdrv.h"
#include "eff/eff_n64.h"
#include "evt/evt_cmd.h"
#include "evt/evt_eff.h"
#include "manager/evtmgr.h"
#include "mario/mario.h"
#include "mario/mario_party.h"
#include "mario/mario_pouch.h"
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

/* ---- shots: shotat=, fxshots=, btlshots= ---- */

#define SHOT_MAX 64
#define SHOT_OFFS 8

volatile u32 tst_shot_frame;
static int s_shot_state = -1;
static u32 s_shot_due[SHOT_MAX];
static int s_shot_n;
static int s_fxshot[SHOT_OFFS], s_fxshot_n;
static int s_btlshot[SHOT_OFFS], s_btlshot_n;

static int parse_int(const char* p);

/* key=N,N,... into out (at most max values); returns the count. */
static int shot_list(const char* key, int* out, int max) {
    char text[400];
    const char* p = text;
    int n = 0;
    if (!tst_sw_value(key, text, sizeof(text))) {
        return 0;
    }
    while (*p >= '0' && *p <= '9' && n < max) {
        out[n++] = parse_int(p);
        while (*p >= '0' && *p <= '9') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return n;
}

static void shot_add(u32 frame) {
    if (s_shot_n < SHOT_MAX) {
        s_shot_due[s_shot_n++] = frame;
    }
}

/* Shots at offsets after a spawn or a battle start. */
static void shot_add_after(const int* offs, int n, u32 frame) {
    int i;
    for (i = 0; i < n; i++) {
        if (offs[i] > 0) {
            shot_add(frame + (u32)offs[i]);
        }
    }
}

#ifdef __MWERKS__
#pragma dont_inline on
#endif
void tst_shot_point(u32 frame) { tst_shot_frame = frame; }
#ifdef __MWERKS__
#pragma dont_inline reset
#endif

static void shot_frame(u32 frame) {
    int i;
    if (s_shot_state < 0) {
        int at[SHOT_MAX];
        int n = shot_list("shotat", at, SHOT_MAX);
        for (i = 0; i < n; i++) {
            shot_add((u32)at[i]);
        }
        s_fxshot_n = shot_list("fxshots", s_fxshot, SHOT_OFFS);
        s_btlshot_n = shot_list("btlshots", s_btlshot, SHOT_OFFS);
        s_shot_state = 1;
    }
    for (i = 0; i < s_shot_n; i++) {
#ifndef TARGET_GCDEBUG
        if (s_shot_due[i] == frame + 1) {
            tst_plat_shot(s_shot_due[i]);
        }
#endif
        if (s_shot_due[i] == frame) {
            tst_log("shot: f=%lu map=%s", (unsigned long)frame, gp != NULL ? gp->currentMapName : "-");
            tst_shot_point(frame);
            s_shot_due[i--] = s_shot_due[--s_shot_n];
        }
    }
}

/* ---- fx= ---- */

#define FX_MAX 128
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

static int is_number(const char* s);

/* An fx= word "A-B" or "n64:A-B" (indices) adds A..B; anything else is one entry. */
static void fx_add(const char* word) {
    const char* num = strncmp(word, "n64:", 4) == 0 ? word + 4 : word;
    const char* dash = strchr(num, '-');
    if (dash != NULL && dash > num && *num >= '0' && *num <= '9' && is_number(dash + 1)) {
        int a = parse_int(num), b = parse_int(dash + 1);
        for (; a <= b && s_fx_n < FX_MAX; a++) {
            sprintf(s_fx_list[s_fx_n++], "%s%d", num != word ? "n64:" : "", a);
        }
        return;
    }
    if (s_fx_n < FX_MAX) {
        strcpy(s_fx_list[s_fx_n++], word);
    }
}

static void fx_read(void) {
    char text[600];
    char* p;
    s_fx_state = 0;
    if (!tst_sw_value("fx", text, sizeof(text)) || text[0] == '\0') {
        return;
    }
    p = text;
    while (*p != '\0' && s_fx_n < FX_MAX) {
        char word[24];
        char* comma = strchr(p, ',');
        int len = comma != NULL ? (int)(comma - p) : (int)strlen(p);
        if (len > 23) {
            len = 23;
        }
        memcpy(word, p, len);
        word[len] = '\0';
        fx_add(word);
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
    shot_add_after(s_fxshot, s_fxshot_n, frame);
    s_fx_spawned = frame;
    s_fx_i++;
    s_fx_next = frame + (u32)s_fx_gap;
}

/* ---- battle= ---- */

static int s_btl_state = -1; /* -1 unread, 0 off/done, 1 waiting, 2 in battle */
static char s_btl_word[32];
static int s_btl_wait;
static u32 s_btl_start;

/* battle=A,B,C or battle=A-B: the battles one after another, each battlewait
 * frames after the field is back, with Mario's and the partners' HP and FP
 * refilled before each (so a lost battle with btlauto=fail does not end the
 * list in a game over). */
#define BTL_MAX 96
static char s_btl_list[BTL_MAX][24];
static int s_btl_n, s_btl_i;

static void btl_add(const char* word) {
    const char* dash = strchr(word, '-');
    if (dash != NULL && dash > word && *word >= '0' && *word <= '9' && is_number(dash + 1)) {
        int a = parse_int(word), b = parse_int(dash + 1);
        for (; a <= b && s_btl_n < BTL_MAX; a++) {
            sprintf(s_btl_list[s_btl_n++], "%d", a);
        }
    } else if (s_btl_n < BTL_MAX) {
        strcpy(s_btl_list[s_btl_n++], word);
    }
}

static void btl_read_list(const char* text) {
    const char* p = text;
    while (*p != '\0' && s_btl_n < BTL_MAX) {
        char word[24];
        const char* comma = strchr(p, ',');
        int len = comma != NULL ? (int)(comma - p) : (int)strlen(p);
        if (len > 23) {
            len = 23;
        }
        memcpy(word, p, len);
        word[len] = '\0';
        if (word[0] != '\0') {
            btl_add(word);
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
}

static void btl_heal(void) {
    PouchData* pouch = pouchGetPtr();
    int i;
    pouch->currentHP = pouch->maximumHP;
    pouch->currentFP = pouch->maximumFP;
    for (i = 0; i < 8; i++) {
        pouch->partyData[i].currentHP = pouch->partyData[i].maximumHP;
    }
}

/* The next battle of the list, or the end of it. */
static void btl_next(void) {
    s_btl_i++;
    if (s_btl_i < s_btl_n) {
        strcpy(s_btl_word, s_btl_list[s_btl_i]);
        s_btl_state = 1;
    } else {
        if (s_btl_n > 1) {
            tst_log("battle: done");
        }
        s_btl_state = 0;
    }
}

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
    shot_add_after(s_btlshot, s_btlshot_n, frame);
}

/* btllist: 30 frames after each map comes up, logs its NPCs (used, enemies)
 * and, the first time its area is seen, the area's battle setup table. With
 * sweep= it lists every area's table in one run (gallery.py --list-battles). */
static void battle_list(void) {
    static int on = -1;
    static char seen[64][4];
    static int n_seen;
    char area[4];
    AreaData* ad;
    NpcSetupBattleInfo* tbl;
    NpcWork* w;
    s32 i, n, used = 0, enemies = 0;
    if (on < 0) {
        on = tst_sw_has("btllist");
    }
    if (!on || s_map_frames != 30) {
        return;
    }
    w = npcGetWorkPtr();
    for (i = 0; i < w->total; i++) {
        if (w->entries[i].flags & NPC_FLAG_USED) {
            used++;
            if (w->entries[i].flags & NPC_FLAG_ENEMY) {
                enemies++;
            }
        }
    }
    tst_log("btllist: map %s npcs %ld enemies %ld", gp->currentMapName, (long)used, (long)enemies);
    memcpy(area, gp->currentMapName, 3);
    area[3] = '\0';
    for (i = 0; i < n_seen; i++) {
        if (strcmp(seen[i], area) == 0) {
            return;
        }
    }
    if (n_seen < 64) {
        strcpy(seen[n_seen++], area);
    }
    ad = areaDataPtr(area);
    tbl = ad != NULL ? (NpcSetupBattleInfo*)ad->btlSetupTbl : NULL;
    for (n = 0; tbl != NULL && tbl[n].name != NULL; n++) {
        tst_log("btllist: area %s %ld %s", area, (long)n, tbl[n].name);
    }
    tst_log("btllist: area %s has %ld setups", area, (long)n);
}

static void battle_frame(u32 frame) {
    battle_list();
    if (s_btl_state < 0) {
        char text[200];
        s_btl_state = 0;
        if (tst_sw_value("battle", text, sizeof(text)) && text[0] != '\0') {
            btl_read_list(text);
        }
        if (s_btl_n > 0) {
            strcpy(s_btl_word, s_btl_list[0]);
            s_btl_wait = tst_sw_int("battlewait", 60);
            s_btl_state = 1;
            tst_log("battle: %s after %d frames on the map (%d in the list)", s_btl_word, s_btl_wait, s_btl_n);
        }
    }
    if (s_btl_state == 1 && s_map_frames >= (u32)s_btl_wait && seqGetSeq() == SEQ_GAME) {
        if (s_btl_n > 1) {
            btl_heal();
        }
        battle_start(frame);
        if (s_btl_state == 0) {
            btl_next(); /* this one did not start */
        }
    } else if (s_btl_state == 2 && frame > s_btl_start + 2 && seqGetSeq() == SEQ_GAME) {
        FieldBattleData* fbat = fbatGetPointer();
        PartyEntry* party = partyGetPtr(marioGetPartyId());
        tst_log("battle: end result %ld after %lu frames", (long)BattleInformationGetResult(&fbat->info),
                (unsigned long)(frame - s_btl_start));
        marioCtrlOn();
        if (party != NULL) {
            partyCtrlOn();
        }
        btl_next();
    }
}

void tst_gallery_frame(u32 frame) {
    track_map();
    fx_frame(frame);
    battle_frame(frame);
    shot_frame(frame);
}
