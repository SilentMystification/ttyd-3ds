/* THP movie decoding and the SDK calls around it, for the JP and PAL
 * releases' digest map (rels/dig: demo.thp, played by seq_demo.c's
 * THPSimple player). G8ME01 has no movie and never calls these.
 *
 * The SDK's THP video decoder (libs/dolsdk2004/src/thp/THPDec.c) is
 * paired-single assembly working in the locked cache, so it is not compiled
 * for the port. Until a port decoder exists, each frame decodes to black
 * (Y 16, U and V 128) and the sound track to silence: the movie runs for its
 * length with the player's normal timing and the digest map then goes on as
 * on the GameCube. TODO(regions): a JPEG baseline decoder for THP frames and
 * the THP ADPCM audio (THPAudio.c reads its big-endian headers in place).
 */
#include <dolphin/dvd.h>
#include <dolphin/exi.h>
#include <dolphin/os.h>
#include <dolphin/thp.h>
#include <string.h>

#include "plat/plat.h"

/* No locked cache on the 3DS: the player's scratch buffers are plain memory. */
void LCEnable(void) {}
void LCDisable(void) {}

/* No EXI devices: in particular no IS-Viewer, so the digest map plays the
 * movie, as on a retail console. */
s32 EXIGetType(s32 chan, u32 dev, u32* type) {
    (void)chan;
    (void)dev;
    *type = 0;
    return 0;
}

/* The THP player's DVDMgr prefetch (seq_demo.c THPSimpleLoadStop). A read
 * the port has started cannot be stopped (dvd.c reads synchronously on the
 * DVD manager thread); it completes and the block ends as cancelled. */
s32 DVDCancel(volatile DVDCommandBlock* block) {
    if (block != NULL) {
        block->state = DVD_STATE_END;
    }
    return 0;
}

BOOL THPInit(void) {
    PLOG_W("THP: no video decoder on the port; movies play black and silent");
    return TRUE;
}

/* The frame's size from its JPEG start-of-frame marker (FFC0). */
static int frame_size(const u8* p, u32* w, u32* h) {
    for (u32 i = 0; i + 9 < 0x400; i++) {
        if (p[i] == 0xFF && p[i + 1] == 0xC0) {
            *h = (u32)p[i + 5] << 8 | p[i + 6];
            *w = (u32)p[i + 7] << 8 | p[i + 8];
            return *w != 0 && *h != 0 && *w <= 640 && *h <= 528;
        }
    }
    return 0;
}

s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    (void)work;
    u32 w, h;
    if (!frame_size(file, &w, &h)) {
        return 1; /* the player reports a failed frame and goes on */
    }
    memset(tileY, 16, w * h);
    memset(tileU, 128, (w / 2) * (h / 2));
    memset(tileV, 128, (w / 2) * (h / 2));
    return 0;
}

u32 THPAudioDecode(s16* audioBuffer, u8* audioFrame, s32 flag) {
    (void)audioBuffer;
    (void)audioFrame;
    (void)flag;
    return 0;
}
