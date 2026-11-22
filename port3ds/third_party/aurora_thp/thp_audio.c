/* THP ADPCM audio decoder. Derived from aurora's lib/dolphin/thp/THPAudio.cpp
 * (https://github.com/encounter/aurora), Copyright (c) 2022 Luke Street,
 * MIT licence: see LICENSE in this directory. Converted to C. */
#include "thp_decode.h"

#define HEADER_SIZE 80

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int16_t be16s(const uint8_t* p) { return (int16_t)(uint16_t)((uint16_t)p[0] << 8 | p[1]); }

/* One channel: 8 byte frames of one header byte (predictor, scale) and 14
 * samples, as nibbles. coef points at 8 predictors of two s16 each. */
static void decode_channel(int16_t* out, int stride, const uint8_t* enc, uint32_t count, const int16_t coef[16],
                           int16_t yn1, int16_t yn2, int pal_rounding) {
    uint32_t cursor = 1;
    uint32_t nibble_offset = 2;
    uint32_t predictor = (enc[0] >> 4) & 7;
    uint32_t scale = enc[0] & 15;
    for (uint32_t i = 0; i < count; i++) {
        if ((nibble_offset & 15) == 0) {
            uint8_t h = enc[cursor++];
            predictor = (h >> 4) & 7;
            scale = h & 15;
            nibble_offset += 2;
        }
        int32_t nib;
        if (nibble_offset & 1) {
            nib = enc[cursor++] & 15;
        } else {
            nib = enc[cursor] >> 4;
        }
        nibble_offset++;
        if (nib >= 8) {
            nib -= 16;
        }
        int64_t acc = (int64_t)coef[predictor * 2 + 1] * yn2 + (int64_t)coef[predictor * 2] * yn1 +
                      (int64_t)nib * ((int64_t)1 << scale) * 2048;
        acc *= 32;
        if (pal_rounding) {
            acc += 0x8000;
        } else {
            uint32_t frac = (uint32_t)(acc & 0xffff);
            if (frac > 0x8000 || (frac == 0x8000 && (acc & 0x10000))) {
                acc += 0x10000;
            }
        }
        if (acc > 2147483647LL) {
            acc = 2147483647LL;
        }
        if (acc < -2147483648LL) {
            acc = -2147483648LL;
        }
        int16_t s = (int16_t)(acc >> 16);
        out[(size_t)i * stride] = s;
        yn2 = yn1;
        yn1 = s;
    }
}

uint32_t thp_audio_decode(int16_t* out, const uint8_t* frame, int32_t flag, int pal_rounding) {
    if (out == NULL || frame == NULL) {
        return 0;
    }
    uint32_t next = be32(frame);
    uint32_t count = be32(frame + 4);
    int16_t lcoef[16], rcoef[16];
    for (int i = 0; i < 16; i++) {
        lcoef[i] = be16s(frame + 8 + i * 2);
        rcoef[i] = be16s(frame + 40 + i * 2);
    }
    int16_t lyn1 = be16s(frame + 72), lyn2 = be16s(frame + 74);
    int16_t ryn1 = be16s(frame + 76), ryn2 = be16s(frame + 78);

    int stride = flag == 1 ? 1 : 2;
    int16_t* right = out;
    int16_t* left = flag == 1 ? out + count : out + 1;
    const uint8_t* ldata = frame + HEADER_SIZE;

    decode_channel(left, stride, ldata, count, lcoef, lyn1, lyn2, pal_rounding);
    if (next == 0) {
        for (uint32_t i = 0; i < count; i++) {
            right[(size_t)i * stride] = left[(size_t)i * stride];
        }
    } else {
        decode_channel(right, stride, ldata + next, count, rcoef, ryn1, ryn2, pal_rounding);
    }
    return count;
}
