/* THP movie decoding and the SDK calls around it, for the JP and PAL
 * releases' digest map (rels/dig: demo.thp) and the JP demo's movie sequence
 * (seq_demo.c's THPSimple player). G8ME01 has no movie and never calls these.
 *
 * The SDK's THP decoder (libs/dolsdk2004/src/thp/THPDec.c) is paired-single
 * assembly working in the locked cache, so it is not compiled for the port.
 * The decoding itself is the portable decoder in third_party/aurora_thp (a
 * baseline JPEG decoder for the frames and the THP ADPCM decoder, from
 * aurora, MIT). This file is the thin adapter: the SDK's entry points on top
 * of it, with the inverse-DCT stage of the video split across cores by the
 * job queue.
 */
#include <dolphin/dvd.h>
#include <dolphin/exi.h>
#include <dolphin/os.h>
#include <dolphin/thp.h>
#include <malloc.h>
#include <string.h>

#include "plat/job.h"
#include "thp_decode.h"
#include "version.h"

/* The GameCube's locked cache is a 16 KB window of the data cache that the
 * SDK's decoder (and the THP player's scratch use) maps over the first cache
 * lines of memory. The 3DS has none and this decoder keeps no state in one:
 * the player's work buffers are plain memory. Enabling and disabling it
 * therefore has nothing to do, and the calls are correct as no-ops. */
void LCEnable(void) {}
void LCDisable(void) {}

/* The EXI bus: the port has no EXI devices, in particular no IS-Viewer, so
 * the digest map and the credits play the movie as on a retail console.
 * The SDK's EXIGetType returns 0 and leaves *type alone when no device
 * answers; its callers (rels/dig, tik, end) do not check the result and read
 * `type` uninitialised, so on a console it is whatever the stack held. The
 * port gives it a defined "no device" value, never an IS-Viewer's. */
s32 EXIGetType(s32 chan, u32 dev, u32* type) {
    (void)chan;
    (void)dev;
    *type = 0;
    return 0;
}

BOOL THPInit(void) { return TRUE; }

/* ---- Video ---- */

/* The executor the decoder hands its picture stage to: the job queue. */
static uint32_t exec_submit(void (*fn)(void*), void* arg, void* user) {
    (void)user;
    return plat_job_submit(fn, arg);
}

static void exec_join_all(const uint32_t* handles, uint32_t n, void* user) {
    (void)user;
    plat_job_join_all(handles, n);
}

static const ThpExec s_exec = {exec_submit, exec_join_all, NULL, 2};

/* Coefficients of one frame, kept between frames (one movie plays at a time;
 * the decoder runs on one thread). */
static void* s_scratch;
static size_t s_scratch_size;

s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    (void)work; /* the SDK's 4 KB scratch for the locked cache: unused */
    unsigned w = 0, h = 0;
    int32_t r = thp_video_info(file, &w, &h);
    if (r != 0) {
        return r;
    }
    const ThpExec* exec = NULL;
    if (plat_job_enabled()) {
        size_t need = thp_video_scratch_size(w, h);
        if (need > s_scratch_size) {
            free(s_scratch);
            s_scratch = memalign(32, need);
            s_scratch_size = s_scratch ? need : 0;
        }
        if (s_scratch != NULL) {
            exec = &s_exec;
        }
    }
    return thp_video_decode_ex(file, tileY, tileU, tileV, s_scratch, s_scratch_size, exec);
}

/* ---- Audio ---- */

u32 THPAudioDecode(s16* audioBuffer, u8* audioFrame, s32 flag) {
#ifdef VERSION_G8MP01
    /* the PAL release's copy of the SDK function rounds differently */
    return thp_audio_decode(audioBuffer, audioFrame, flag, 1);
#else
    return thp_audio_decode(audioBuffer, audioFrame, flag, 0);
#endif
}
