/* Test mode core, shared by the 3DS port and the GameCube debug DOL (see
 * test_mode.h). C89, for MWCC.
 *
 * determ: game-visible time comes from the game frame count. Each game frame
 *   has exactly one retrace (1/60 s, 1/50 s for PAL): the build runs the
 *   game's retrace callbacks once per frame at a fixed point (after the wait
 *   in VIWaitForRetrace) with a virtual retrace count, and the game's
 *   OSGetTime is tst_game_time. DVD manager reads queued in a frame finish
 *   before the next frame starts (tst_dvd_busy). The platform keeps real time.
 * gwdump=N: every N game frames (gwdumpfrom=F to gwdumpto=T if given), log lines with the frame, the
 *   frames since the warp, the sequence, the map, the story flags (GSW, GSWF,
 *   LSW, LSWF as hashes), Mario's position, the RNG state, the clocks and a
 *   hash of all of GlobalWork. tools/port3ds/gw_diff.py compares the lines of
 *   two runs.
 */
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <stdio.h>
#include <string.h>

#include "battle/battle.h"
#include "driver/camdrv.h"
#include "cam_road.h"
#include "driver/seqdrv.h"
#include "driver/swdrv.h"
#include "event.h"
#include "mario/mario_motion.h"
#include "mario/mario_party.h"
#include "pmario_sound.h"
#include "manager/dvdmgr.h"
#include "manager/evtmgr.h"
#include "mario/mario.h"
#include "mario/mariost.h"
#include "test_mode.h"

extern DVDEntry* dvdq;

/* ---- switches ---- */

static int s_init;
static int s_determ;
static int s_gwdump;
static u32 s_gwdump_from, s_gwdump_to;

static void tst_init(void) {
    if (s_init) {
        return;
    }
    s_determ = tst_sw_has("determ");
    s_gwdump = tst_sw_int("gwdump", 0);
    s_gwdump_from = (u32)tst_sw_int("gwdumpfrom", 0);
    s_gwdump_to = (u32)tst_sw_int("gwdumpto", 0);
    s_init = 1;
    if (s_determ) {
        tst_log("determ: on, %lu ticks per retrace", (unsigned long)tst_retrace_ticks());
    }
}

int tst_determ(void) {
    tst_init();
    return s_determ;
}

/* ---- virtual time ---- */

/* 2004-07-22 12:00:00 in seconds since 2000-01-01 (OSGetTime's epoch). */
#define TST_EPOCH_SECONDS 143812800u
#define TST_THREADS 8

static u32 s_vretrace;
static struct {
    OSThread* thread;
    u32 vretrace;
    u32 calls;
} s_calls[TST_THREADS];

OSTime tst_retrace_ticks(void) {
    return (OSTime)OS_TIMER_CLOCK / (VIGetTvFormat() == VI_PAL ? 50 : 60);
}

OSTime tst_game_time(void) {
    OSThread* t = OSGetCurrentThread();
    u32 calls = 0;
    int i;
    int free_slot = -1;
    BOOL level = OSDisableInterrupts();
    for (i = 0; i < TST_THREADS; i++) {
        if (s_calls[i].thread == t) {
            break;
        }
        if (s_calls[i].thread == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (i == TST_THREADS) {
        i = free_slot >= 0 ? free_slot : TST_THREADS - 1;
        s_calls[i].thread = t;
        s_calls[i].vretrace = s_vretrace;
        s_calls[i].calls = 0;
    }
    if (s_calls[i].vretrace != s_vretrace) {
        s_calls[i].vretrace = s_vretrace;
        s_calls[i].calls = 0;
    }
    calls = s_calls[i].calls++;
    OSRestoreInterrupts(level);
    return (OSTime)TST_EPOCH_SECONDS * (OSTime)OS_TIMER_CLOCK + (OSTime)s_vretrace * tst_retrace_ticks() + calls;
}

u32 tst_vretrace(void) {
    return ++s_vretrace;
}

/* ---- DVD ---- */

int tst_dvd_busy(void) {
    int i;
    if (dvdq == NULL) {
        return 0;
    }
    for (i = 0; i < DVDEntryCount; i++) {
        u16 st = dvdq[i].status;
        if ((st & DVDMGR_INUSE) && (st & DVDMGR_READING) && !(st & DVDMGR_UNK_ERROR2)) {
            return 1;
        }
    }
    return 0;
}

/* ---- gwdump ---- */

static u32 s_warp_frame;

u32 tst_warp_frames(void) {
    return s_warp_frame;
}

/* FNV-1a over bytes */
static u32 hash_bytes(u32 h, const void* p, u32 n) {
    const u8* b = (const u8*)p;
    u32 i;
    for (i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* FNV-1a over 32-bit values, most significant byte first: the same on a
 * big-endian and a little-endian build. */
static u32 hash_words(u32 h, const u32* p, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        u32 v = p[i];
        h = (h ^ (v >> 24)) * 16777619u;
        h = (h ^ ((v >> 16) & 0xFF)) * 16777619u;
        h = (h ^ ((v >> 8) & 0xFF)) * 16777619u;
        h = (h ^ (v & 0xFF)) * 16777619u;
    }
    return h;
}

static u32 fbits(f32 f) {
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

#define FNV0 2166136261u

static void gwdump(u32 frame) {
    MarioWork* m;
    EventWork* ew;
    u32 h_gw, h_gswf, h_gsw, h_lswf, h_lsw, h_evtgw, h_evtgf;
    char gwv[220];
    int gwv_len = 0, i;
    char map[17];
    s32 evts = -1;
    s32 btl_turn = -1, btl_unit = -1;

    if (gp == NULL) {
        return;
    }
    memcpy(map, gp->currentMapName, 16);
    map[16] = '\0';
    if (map[0] == '\0') {
        map[0] = '-';
        map[1] = '\0';
    }
    h_gswf = hash_words(FNV0, gp->gswf, sizeof(gp->gswf) / 4);
    h_gsw = hash_bytes(FNV0, gp->gsw, sizeof(gp->gsw));
    h_lswf = hash_words(FNV0, gp->lswf, sizeof(gp->lswf) / 4);
    h_lsw = hash_bytes(FNV0, gp->lsw, sizeof(gp->lsw));
    /* the raw struct: pointers and byte order make this one build-specific */
    h_gw = hash_bytes(FNV0, gp, sizeof(GlobalWork));
    ew = evtGetWork();
    h_evtgw = h_evtgf = 0;
    gwv[0] = '-';
    gwv[1] = '\0';
    if (ew != NULL) {
        evts = ew->count;
        h_evtgw = FNV0;
        for (i = 0; i < (int)(sizeof(ew->gwData) / 4); i++) {
            u32 v = (u32)ew->gwData[i];
            v = tst_is_ptr(v) ? 0x50545221u : v; /* "PTR!" */
            h_evtgw = hash_words(h_evtgw, &v, 1);
        }
        h_evtgf = hash_words(FNV0, (const u32*)ew->gfData, sizeof(ew->gfData) / 4);
        /* the non-zero event GWs, pointers as P */
        for (i = 0; i < (int)(sizeof(ew->gwData) / 4) && gwv_len < (int)sizeof(gwv) - 16; i++) {
            if (ew->gwData[i] == 0) {
                continue;
            }
            if (tst_is_ptr((u32)ew->gwData[i])) {
                gwv_len += sprintf(gwv + gwv_len, "%s%d:P", gwv_len ? "," : "", i);
            } else {
                gwv_len += sprintf(gwv + gwv_len, "%s%d:%08lx", gwv_len ? "," : "", i, (unsigned long)ew->gwData[i]);
            }
        }
    }
    if (_battleWorkPointer != NULL && gp->inBattle) {
        btl_turn = _battleWorkPointer->turnCount;
        btl_unit = _battleWorkPointer->activeUnitId;
    }
    m = marioGetPtr();
    /* two lines (the port's log lines hold about 250 characters); gw_diff.py joins them by f */
    tst_log("gwdump f=%lu wf=%lu seq=%d map=%s gsw0=%ld gswf=%08lx gsw=%08lx lswf=%08lx lsw=%08lx evtgw=%08lx "
            "evtgf=%08lx evts=%ld",
            (unsigned long)frame, (unsigned long)s_warp_frame, (int)seqGetSeq(), map, (long)gp->gsw0,
            (unsigned long)h_gswf, (unsigned long)h_gsw, (unsigned long)h_lswf, (unsigned long)h_lsw,
            (unsigned long)h_evtgw, (unsigned long)h_evtgf, (long)evts);
    tst_log("gwdump f=%lu gwv=%s", (unsigned long)frame, gwv);
    tst_log("gwdump f=%lu mario=%08lx,%08lx,%08lx rng=%08lx rc=%lu lv=%lx inbtl=%ld btl=%ld,%ld rt=%08lx%08lx "
            "ft=%08lx%08lx pt=%08lx%08lx btn=%08lx stick=%d,%d gw=%08lx",
            (unsigned long)frame, (unsigned long)(m ? fbits(m->position.x) : 0),
            (unsigned long)(m ? fbits(m->position.y) : 0), (unsigned long)(m ? fbits(m->position.z) : 0),
            (unsigned long)tst_rand_state(), (unsigned long)gp->retraceCount,
            (unsigned long)(u32)gp->systemLevelFlags, (long)gp->inBattle, (long)btl_turn, (long)btl_unit,
            (unsigned long)(u32)((u64)gp->renderTime >> 32), (unsigned long)(u32)gp->renderTime,
            (unsigned long)(u32)((u64)gp->renderFieldTime >> 32), (unsigned long)(u32)gp->renderFieldTime,
            (unsigned long)(u32)((u64)gp->playTime >> 32), (unsigned long)(u32)gp->playTime,
            (unsigned long)gp->button[0], (int)gp->stickX[0], (int)gp->stickY[0], (unsigned long)h_gw);
    {
        CameraEntry* c = camGetPtr(CAMERA_3D);
        if (c != NULL) {
            tst_log("gwdump f=%lu cam=%08lx,%08lx,%08lx cat=%08lx,%08lx,%08lx cmode=%u,%u,%u,%04x cfov=%08lx cprog=%08lx,%u",
                    (unsigned long)frame, (unsigned long)fbits(c->cameraPos.x), (unsigned long)fbits(c->cameraPos.y),
                    (unsigned long)fbits(c->cameraPos.z), (unsigned long)fbits(c->cameraAt.x),
                    (unsigned long)fbits(c->cameraAt.y), (unsigned long)fbits(c->cameraAt.z), (unsigned)c->mode,
                    (unsigned)c->stateA, (unsigned)c->stateB, (unsigned)c->flags, (unsigned long)fbits(c->fovY),
                    (unsigned long)fbits(c->progressA), (unsigned)c->curveA);
            {
                CamRoadWork* w = camRoadGetWorkPtr();
                tst_log("gwdump f=%lu rd=%ld,%ld,%ld,%ld,%08lx bp=%08lx,%08lx,%08lx ba=%08lx,%08lx,%08lx np=%08lx,%08lx,%08lx",
                        (unsigned long)frame, (long)w->next.on, (long)w->next.onCurve, (long)w->hokanOn,
                        (long)w->state, (unsigned long)fbits(w->hokanT), (unsigned long)fbits(c->blendPos.x),
                        (unsigned long)fbits(c->blendPos.y), (unsigned long)fbits(c->blendPos.z),
                        (unsigned long)fbits(c->blendAt.x), (unsigned long)fbits(c->blendAt.y),
                        (unsigned long)fbits(c->blendAt.z), (unsigned long)fbits(w->next.pos.x),
                        (unsigned long)fbits(w->next.pos.y), (unsigned long)fbits(w->next.pos.z));
            }
        }
    }
}

/* switch rngtrace=<from>,<to>: every rand() call in that game frame window, and each sequence change */
static u32 s_cur_frame, s_rt_from, s_rt_to;
static int s_rt_verbose; /* rngtrace=from,to,v: one line per call; else one summary line per frame */
static int s_rt_state = -1; /* -1 unread, 0 off, 1 on */

static int rngtrace_on(void) {
    if (s_rt_state < 0) {
        char v[40];
        s_rt_state = 0;
        if (tst_sw_value("rngtrace", v, sizeof(v)) && v[0] != '\0') {
            char* c = v;
            s_rt_from = s_rt_to = 0;
            while (*c >= '0' && *c <= '9') s_rt_from = s_rt_from * 10 + (u32)(*c++ - '0');
            if (*c == ',') c++;
            while (*c >= '0' && *c <= '9') s_rt_to = s_rt_to * 10 + (u32)(*c++ - '0');
            if (*c == ',') {
                s_rt_verbose = c[1] == 'v';
            }
            s_rt_state = 1;
        }
    }
    return s_rt_state == 1 && s_cur_frame >= s_rt_from && s_cur_frame <= s_rt_to;
}

u32 tst_cur_frame(void) { return s_cur_frame; }

int tst_rngtrace_wanted(void) { return rngtrace_on(); }

static u32 s_rt_calls, s_rt_hash, s_rt_first;

void tst_rngtrace(unsigned long caller, unsigned long caller2, unsigned long caller3, u32 val) {
    if (rngtrace_on()) {
        if (s_rt_calls == 0) {
            s_rt_first = val;
        }
        s_rt_calls++;
        s_rt_hash = s_rt_hash * 31u + val;
        if (s_rt_verbose) {
            tst_log("[RNG] f=%lu caller=%08lx caller2=%08lx caller3=%08lx val=%lu", (unsigned long)s_cur_frame, caller,
                    caller2, caller3, (unsigned long)val);
        }
    }
}

void tst_frame(u32 frame) {
    static int last_seq = -99;
    tst_init();
    if (s_rt_state == 1 && frame > s_rt_from && frame - 1 <= s_rt_to) {
        tst_log("[FRM] f=%lu rand calls=%lu first=%lu hash=%08lx", (unsigned long)(frame - 1),
                (unsigned long)s_rt_calls, (unsigned long)s_rt_first, (unsigned long)s_rt_hash);
    }
    s_rt_calls = 0;
    s_rt_hash = 0;
    s_cur_frame = frame;
    if (rngtrace_on() && gp != NULL && (int)seqGetSeq() != last_seq) {
        last_seq = (int)seqGetSeq();
        tst_log("[SEQ] f=%lu seq=%d", (unsigned long)frame, last_seq);
    }
    if (s_warp_frame == 0 && gp != NULL && seqGetSeq() == SEQ_MAP_CHANGE) {
        s_warp_frame = 1;
        tst_log("tst: first map change at frame %lu", (unsigned long)frame);
    } else if (s_warp_frame != 0) {
        s_warp_frame++;
    }
    if (s_gwdump > 0 && frame % (u32)s_gwdump == 0 && frame >= s_gwdump_from &&
        (s_gwdump_to == 0 || frame <= s_gwdump_to)) {
        gwdump(frame);
    }
    tst_gallery_frame(frame);
    tst_battle_frame(frame);
#ifdef PORT3DS_GAME_SIDE
    tst_font_frame(frame);
#endif
}

/* ---- storyjump=: the game's own story points (src/event.c) ----
 *
 * storyjump=<textId>[.<n>] (the n-th row with that id, from 0; e.g. STG1_16,
 * STG9_00.2) or storyjump=<stage>.<row> (event_dt[stage].data[row]). Applied
 * at the warp instead of story=/party=, the way the E3 scenario select applies
 * a story point (seq_e3.c, state 102): the story flags cleared, GSW 0 set to
 * the previous row's id (so the row's own scene is the next to play), every
 * row's set-up function up to and including this row run in order (key items,
 * flags), the row's motion and its partner and follower. Unlike the E3 select
 * it keeps the items and badges the set-ups give. */
static int s_sj_state = -1; /* -1 unread, 0 none, 1 found */
static int s_sj_stage, s_sj_row;

static int sj_num(const char* s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s++ - '0');
    }
    return n;
}

static int sj_find(void) {
    char v[32];
    char* dot;
    int stage, row, nth = 0, want = 0;
    if (s_sj_state >= 0) {
        return s_sj_state;
    }
    s_sj_state = 0;
    if (!tst_sw_value("storyjump", v, sizeof(v)) || v[0] == '\0') {
        return 0;
    }
    dot = strchr(v, '.');
    if (dot != NULL) {
        *dot = '\0';
        want = sj_num(dot + 1);
    }
    if (v[0] >= '0' && v[0] <= '9') {
        stage = sj_num(v);
        if (stage < eventStgNum() && want < eventStgDtPtr(stage)->count) {
            s_sj_stage = stage;
            s_sj_row = want;
            s_sj_state = 1;
        }
    } else {
        for (stage = 0; stage < eventStgNum() && s_sj_state == 0; stage++) {
            EventStage* st = eventStgDtPtr(stage);
            for (row = 0; row < st->count; row++) {
                if (strcmp(st->data[row].textId, v) == 0 && nth++ == want) {
                    s_sj_stage = stage;
                    s_sj_row = row;
                    s_sj_state = 1;
                    break;
                }
            }
        }
    }
    if (s_sj_state == 0) {
        tst_log("storyjump: %s not found in the story table", v);
    }
    return s_sj_state;
}

int tst_storyjump_target(const char** map, const char** bero) {
    EventData* d;
    if (!sj_find()) {
        return 0;
    }
    d = &eventStgDtPtr(s_sj_stage)->data[s_sj_row];
    *map = d->mapName;
    *bero = d->beroName != NULL ? d->beroName : "";
    return 1;
}

int tst_storyjump_apply(void) {
    int stage, row;
    u32 prev = 0;
    EventData* d;
    if (!sj_find()) {
        return 0;
    }
    d = &eventStgDtPtr(s_sj_stage)->data[s_sj_row];
    if (s_sj_row > 0) {
        prev = d[-1].id;
    } else if (s_sj_stage > 0) {
        EventStage* st = eventStgDtPtr(s_sj_stage - 1);
        prev = st->data[st->count - 1].id;
    }
    psndStopAllFadeOut();
    psndSFXAllOff();
    marioPartyKill();
    swInit();
    swByteSet(0, prev);
    for (stage = 0; stage <= s_sj_stage; stage++) {
        EventStage* st = eventStgDtPtr(stage);
        for (row = 0; row < st->count && (stage < s_sj_stage || row <= s_sj_row); row++) {
            if (st->data[row].initEvent != NULL) {
                st->data[row].initEvent();
            }
        }
    }
    if (d->motionType == 1) {
        marioChgShipMotion();
    } else {
        marioChgStayMotion();
    }
    marioPartyKill();
    marioGetPtr()->prevPartyMemberId[0] = 0;
    marioGetPtr()->prevPartyMemberId[1] = 0;
    for (row = 0; row < 2; row++) {
        if (d->partners[row] != 0) {
            if (d->partners[row] < 8) {
                partyJoin((PartyId)d->partners[row]);
            }
            marioPartyEntry((PartyId)d->partners[row]);
        }
    }
    tst_log("storyjump: %s (%d.%d) gsw0=%lu map=%s bero=%s partners=%u,%u", d->textId, s_sj_stage, s_sj_row,
            (unsigned long)prev, d->mapName, d->beroName != NULL ? d->beroName : "-", (unsigned)d->partners[0],
            (unsigned)d->partners[1]);
    return 1;
}
