/* Portable THP movie decoder: a baseline JPEG decoder for THP video frames
 * and the THP ADPCM audio decoder. Pure computation: no SDK, game or platform
 * dependency.
 *
 * Derived from aurora's lib/dolphin/thp/THPDec.cpp and THPAudio.cpp
 * (https://github.com/encounter/aurora), Copyright (c) 2022 Luke Street,
 * MIT licence (see LICENSE in this directory), converted to C with a table
 * driven Huffman reader.
 */
#ifndef THP_DECODE_H
#define THP_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes one THP video frame (a JPEG without byte stuffing: 4:2:0, baseline,
 * 8 bit) into three GX I8 texture planes, each in 8x4 texel tiles (32 bytes,
 * four rows of eight) laid out left to right, top to bottom; the tile row
 * stride is ceil(width / 8) tiles. Y is width x height, U and V are
 * ceil(w/2) x ceil(h/2). Returns 0, or the SDK's error numbers. */
int32_t thp_video_decode(const void* frame, void* tile_y, void* tile_u, void* tile_v);

/* The same with the picture stage (inverse DCT and tiling) of finished MCU
 * rows handed to an executor while the serial entropy decoding goes on. The
 * caller supplies the executor, so this file knows no threads:
 *   submit(fn, arg, user) runs or queues fn(arg) and returns a handle;
 *   join_all(handles, n, user) returns when all n are done.
 * Jobs read the coefficient scratch and write disjoint tiles, so they may
 * run in any order. scratch needs thp_video_scratch_size() bytes (else the
 * decode runs on the calling thread). rows_per_job: at least this many MCU
 * rows (16 pixel rows each) per job (0: a default). */
typedef struct ThpExec {
    uint32_t (*submit)(void (*fn)(void*), void* arg, void* user);
    void (*join_all)(const uint32_t* handles, uint32_t n, void* user);
    void* user;
    unsigned rows_per_job;
} ThpExec;

int32_t thp_video_info(const void* frame, unsigned* width, unsigned* height);
size_t thp_video_scratch_size(unsigned width, unsigned height);
int32_t thp_video_decode_ex(const void* frame, void* tile_y, void* tile_u, void* tile_v, void* scratch,
                            size_t scratch_size, const ThpExec* exec);

/* Decodes one THP audio frame (4 bit ADPCM, one or two channels) to s16
 * and returns the samples per channel. flag 1: the right channel's samples
 * then the left's; otherwise interleaved (right first, then left). A mono
 * frame is copied to both channels. pal_rounding selects the PAL release's
 * rounding (add 0x8000); otherwise the rounding is to nearest, ties to even,
 * as every other release does. */
uint32_t thp_audio_decode(int16_t* out, const uint8_t* frame, int32_t flag, int pal_rounding);

#ifdef __cplusplus
}
#endif
#endif
