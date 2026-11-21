/* Debug dumps for tests (switch evtdump=N): every N frames, the running
 * event scripts with their current command and user function, so a script
 * that stops can be found (tools: addr2line / nm on ttyd.elf; the static
 * RELs are in it). */
#include "driver/msgdrv.h"
#include "driver/windowdrv.h"
#include "manager/evtmgr.h"
#include "memory.h"
#include "plat/plat.h"

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
    PLOG_I("evtdump frame %lu: %ld entries", (unsigned long)frame, (long)wk->count);
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
