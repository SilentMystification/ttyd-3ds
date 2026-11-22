/* Debug dumps for tests (switch evtdump=N): every N frames, the running
 * event scripts with their current command and user function, so a script
 * that stops can be found (tools: addr2line / nm on ttyd.elf; the static
 * RELs are in it). */
#include "driver/msgdrv.h"
#include "driver/windowdrv.h"
#include "manager/evtmgr.h"
#include "memory.h"
#include "plat/plat.h"
#include "driver/npcdrv.h"
#include "mario/mario.h"
#include "mario/mariost.h"
#include "driver/seqdrv.h"
#include <stdio.h>
#include <string.h>

void port_debug_frame(u32 frame) {
    static int every = -1;
    if (every < 0) {
        every = plat_switch_int("evtdump", 0);
    }
    if (every <= 0 || frame % (u32)every != 0) {
        return;
    }
    EventWork* wk = evtGetWork();
    if (wk == NULL || wk->entries == NULL) {
        return;
    }
    PLOG_I("evtdump frame %lu: %ld entries, mapEventId %ld, seq %d, systemLevelFlags %lx, systemLevel %ld", (unsigned long)frame, (long)wk->count,
           gp != NULL ? (long)gp->mapEventId : -1L, (int)seqGetSeq(),
           gp != NULL ? (unsigned long)gp->systemLevelFlags : 0UL, (long)_mariostSystemLevel);
    for (s32 i = 0; i < wk->count; i++) {
        EventEntry* e = &wk->entries[i];
        if (!(e->flags & EVENT_ACTIVE)) {
            continue;
        }
        PLOG_I("  evt %ld id %ld flags %02x op %d cmd %p next %p func %p child %p name %s", (long)i, (long)e->eventId,
               e->flags, (int)e->opcode, e->currCommand, e->nextCommand, (void*)e->userfunc, (void*)e->waitingOnEvent,
               e->name ? e->name : "");
        if (e->printWindowId >= 0 && windowCheckID(e->printWindowId)) {
            WindowEntry* w = windowGetPointer(e->printWindowId);
            MsgWinData* d = w->userdata != NULL ? (MsgWinData*)w->userdata->address : NULL;
            if (d != NULL) {
                PLOG_I("    window %ld state %u flags %04x: msg flags %08lx page %u/%ld state %u next %ld play %ld "
                       "scroll %ld->%ld chars %ld",
                       (long)e->printWindowId, (unsigned)*(u16*)w->state, w->flags, (unsigned long)d->flags,
                       (unsigned)d->curPage, (long)d->pageCount, (unsigned)d->pageState, (long)d->nextEntry,
                       (long)d->playTime, (long)d->scrollPos, (long)d->scrollTarget, (long)d->charCount);
            }
        }
    }
}

/* Switch npctrace=<name> ("*" = any NPC, "hex:<bytes>" = the name's bytes in hex): one log line per frame while that NPC
 * moves under evt_npc_move_position (see npcMain's move and the call in
 * evt_npc.c), at most 600 lines. Also logs each distinct NPC name once when it
 * first enters evt_npc_move_position. */
u32 port_game_frame(void);

#define NPCTRACE_MAX 600
static char s_trace_name[64];
static int s_trace_init;
static int s_trace_lines;
static int s_trace_mario_logged;
static char s_seen[16][64];
static int s_seen_n;
static struct {
    int pending;
    NpcEntry* npc;
    u32 frame;
    Vec pos;
    f32 tx, tz, dist, step, dir, speed, dt;
} s_rec;

/* the name's bytes as hex, for names the switch file cannot hold (the Japanese ones) */
static const char* name_hex(const char* name) {
    static char buf[64];
    size_t i;
    for (i = 0; name[i] != '\0' && i < 31; i++) {
        snprintf(buf + 2 * i, 3, "%02x", (unsigned)(unsigned char)name[i]);
    }
    buf[2 * i] = '\0';
    return buf;
}

static int trace_on(const char* name) {
    if (!s_trace_init) {
        s_trace_init = 1;
        s_trace_name[0] = '\0';
        plat_switch_value("npctrace", s_trace_name, sizeof(s_trace_name));
    }
    if (s_trace_name[0] == '\0' || s_trace_lines >= NPCTRACE_MAX) {
        return 0;
    }
    if (strncmp(s_trace_name, "hex:", 4) == 0) {
        return strcmp(s_trace_name + 4, name_hex(name)) == 0;
    }
    return strcmp(s_trace_name, "*") == 0 || strcmp(s_trace_name, name) == 0;
}

void port_npctrace_evt(NpcEntry* npc, const char* name, const Vec* before, f32 tx, f32 tz, f32 dist, f32 step) {
    int i;
    if (!s_trace_init) {
        trace_on(name);
    }
    if (s_trace_name[0] == '\0') {
        return;
    }
    for (i = 0; i < s_seen_n; i++) {
        if (strcmp(s_seen[i], name) == 0) {
            break;
        }
    }
    if (i == s_seen_n && s_seen_n < 16) {
        strncpy(s_seen[s_seen_n], name, 63);
        s_seen[s_seen_n++][63] = '\0';
        PLOG_I("npctrace: npc \"%s\" first enters evt_npc_move_position (hex:%s), frame %lu", name,
               name_hex(name),
               (unsigned long)port_game_frame());
    }
    if (!trace_on(name)) {
        return;
    }
    if (!s_trace_mario_logged) {
        MarioWork* m = marioGetPtr();
        s_trace_mario_logged = 1;
        PLOG_I("npctrace: start frame %lu, mario pos %.3f %.3f %.3f", (unsigned long)port_game_frame(),
               (double)m->position.x, (double)m->position.y, (double)m->position.z);
    }
    if (s_rec.pending) {
        s_trace_lines++;
        PLOG_I("npctrace f%lu evtonly (no npcMain move) before %.3f %.3f tgt %.3f %.3f", (unsigned long)s_rec.frame,
               (double)s_rec.pos.x, (double)s_rec.pos.z, (double)s_rec.tx, (double)s_rec.tz);
    }
    s_rec.pending = 1;
    s_rec.npc = npc;
    s_rec.frame = port_game_frame();
    s_rec.pos = *before;
    s_rec.tx = tx;
    s_rec.tz = tz;
    s_rec.dist = dist;
    s_rec.step = step;
    s_rec.dir = npc->moveDir;
    s_rec.speed = npc->moveSpeed;
    s_rec.dt = npc->moveDt;
}

void port_npctrace_move(NpcEntry* npc, f32 mx, f32 mz, const Vec* before) {
    if (!s_rec.pending || s_rec.npc != npc || s_trace_lines >= NPCTRACE_MAX) {
        return;
    }
    s_rec.pending = 0;
    s_trace_lines++;
    if (s_trace_lines == NPCTRACE_MAX) {
        PLOG_I("npctrace: cap of %d lines reached", NPCTRACE_MAX);
    }
    PLOG_I("npctrace f%lu evt@f%lu before %.3f %.3f tgt %.3f %.3f dist %.4f step %.4f dir %.3f spd %.3f dt %.5f "
           "| npcMain dt %.5f dir %.3f off %.5f %.5f (expect %.3f %.3f) after %.3f %.3f wallHit %d noWallHit %d",
           (unsigned long)port_game_frame(), (unsigned long)s_rec.frame, (double)s_rec.pos.x, (double)s_rec.pos.z,
           (double)s_rec.tx, (double)s_rec.tz, (double)s_rec.dist, (double)s_rec.step, (double)s_rec.dir,
           (double)s_rec.speed, (double)s_rec.dt, (double)npc->moveDt, (double)npc->moveDir, (double)mx, (double)mz,
           (double)(before->x + mx), (double)(before->z + mz), (double)npc->position.x, (double)npc->position.z,
           npc->wallHit != NULL, (npc->flags & NPC_FLAG_NO_WALL_HIT) != 0);
}
