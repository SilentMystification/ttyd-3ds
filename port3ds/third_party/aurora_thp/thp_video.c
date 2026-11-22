/* THP video (baseline JPEG) decoder. Derived from aurora's
 * lib/dolphin/thp/THPDec.cpp (https://github.com/encounter/aurora),
 * Copyright (c) 2022 Luke Street, MIT licence: see LICENSE in this directory.
 * Converted to C: the Huffman reader is table driven and the output is
 * written a tile row at a time. The inverse DCT is aurora's AAN transform. */
#include <string.h>

#include "thp_decode.h"

/* Reproduce the SDK decoder's DC+AC1-only row transform (_quarterIDCT in
 * libs/dolsdk2004/src/thp/THPDec.c), which stores the sum of the odd
 * term in column 3 and the difference in column 4 where a correct IDCT does
 * the opposite. Aurora leaves it out; the port keeps the original's picture
 * (set to 0 for a mathematically exact decode). */
#ifndef THP_SDK_QUARTER_IDCT_BUG
#define THP_SDK_QUARTER_IDCT_BUG 1
#endif

enum {
    BAD_SYNTAX = 3,
    BAD_PRECISION = 10,
    UNSUPPORTED_MARKER = 11,
    BAD_COMPONENT_COUNT = 12,
    MISSING_HUFFMAN_TABLE = 15,
    BAD_SAMPLING = 19,
    NO_INPUT = 25,
    NO_OUTPUT = 27,
};

#define LOOKUP_BITS 9

static const uint8_t k_natural_order[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const double k_aan_scale[8] = {1.0, 1.387039845, 1.306562965, 1.175875602,
                                      1.0, 0.785694958, 0.541196100, 0.275899379};

#define K_SQRT2 1.414213562f
#define K_C2 1.847759065f
#define K_C2_MINUS_C6 1.082392200f
#define K_C2_PLUS_C6 2.613125930f
#define K_C6 (K_C2 - K_C2_MINUS_C6)
#define K_OUTPUT_BIAS (128.0f * 8.0f)

typedef struct {
    float values[64];
    int valid;
} QuantTable;

typedef struct {
    uint8_t counts[17];
    uint16_t first_codes[17];
    uint16_t symbol_offsets[17];
    uint8_t symbols[256];
    uint16_t lookup[1 << LOOKUP_BITS]; /* (length << 8) | symbol; 0: longer than LOOKUP_BITS */
    int valid;
} HuffTable;

typedef struct {
    uint8_t quant_table, dc_table, ac_table;
    int32_t pred_dc;
} Component;

typedef struct {
    QuantTable quant[3];
    HuffTable huff[4]; /* index: id * 2 + class */
    Component comp[3];
    uint16_t width, height, restart_interval;
    size_t scan_offset;
} Context;

/* ---- headers ---------------------------------------------------------- */

static int read_segment(const uint8_t* file, size_t* pos, const uint8_t** data, size_t* size) {
    uint32_t len = (uint32_t)file[*pos] << 8 | file[*pos + 1];
    if (len < 2) {
        return 0;
    }
    *pos += 2;
    *data = file + *pos;
    *size = len - 2;
    *pos += len - 2;
    return 1;
}

static int parse_quant(const uint8_t* d, size_t size, Context* c) {
    size_t p = 0;
    while (p < size) {
        if (size - p < 65) {
            return BAD_SYNTAX;
        }
        uint8_t desc = d[p++];
        uint8_t id = desc & 15;
        if ((desc >> 4) != 0 || id >= 3) {
            return BAD_SYNTAX;
        }
        float natural[64];
        for (int i = 0; i < 64; i++) {
            natural[k_natural_order[i]] = (float)d[p++];
        }
        QuantTable* t = &c->quant[id];
        for (int r = 0; r < 8; r++) {
            for (int col = 0; col < 8; col++) {
                int i = r * 8 + col;
                t->values[i] = (float)((double)natural[i] * k_aan_scale[r] * k_aan_scale[col]);
            }
        }
        t->valid = 1;
    }
    return 0;
}

static int parse_frame(const uint8_t* d, size_t size, Context* c) {
    if (size < 6) {
        return BAD_SYNTAX;
    }
    if (d[0] != 8) {
        return BAD_PRECISION;
    }
    c->height = (uint16_t)(d[1] << 8 | d[2]);
    c->width = (uint16_t)(d[3] << 8 | d[4]);
    if (d[5] != 3) {
        return BAD_COMPONENT_COUNT;
    }
    if (size < 15 || c->width == 0 || c->height == 0) {
        return BAD_SYNTAX;
    }
    for (int i = 0; i < 3; i++) {
        uint8_t sampling = d[7 + i * 3];
        if ((i == 0 && sampling != 0x22) || (i != 0 && sampling != 0x11)) {
            return BAD_SAMPLING;
        }
        uint8_t table = d[8 + i * 3];
        if (table >= 3) {
            return BAD_SYNTAX;
        }
        c->comp[i].quant_table = table;
    }
    return 0;
}

static int parse_huffman(const uint8_t* d, size_t size, Context* c) {
    size_t p = 0;
    while (p < size) {
        if (size - p < 17) {
            return BAD_SYNTAX;
        }
        uint8_t desc = d[p++];
        uint8_t cls = desc >> 4;
        uint8_t id = desc & 15;
        if (cls > 1 || id > 1) {
            return BAD_SYNTAX;
        }
        HuffTable* t = &c->huff[id * 2 + cls];
        memset(t, 0, sizeof(*t));
        size_t nsym = 0;
        for (int len = 1; len <= 16; len++) {
            t->counts[len] = d[p++];
            nsym += t->counts[len];
        }
        if (nsym > 256 || nsym > size - p) {
            return BAD_SYNTAX;
        }
        memcpy(t->symbols, d + p, nsym);
        p += nsym;

        uint32_t code = 0;
        uint16_t sym_off = 0;
        for (int len = 1; len <= 16; len++) {
            uint32_t count = t->counts[len];
            if (code + count > (1u << len)) {
                return BAD_SYNTAX;
            }
            t->first_codes[len] = (uint16_t)code;
            t->symbol_offsets[len] = sym_off;
            if (len <= LOOKUP_BITS) {
                for (uint32_t k = 0; k < count; k++) {
                    uint32_t base = (code + k) << (LOOKUP_BITS - len);
                    uint16_t entry = (uint16_t)(len << 8 | t->symbols[sym_off + k]);
                    for (uint32_t f = 0; f < (1u << (LOOKUP_BITS - len)); f++) {
                        t->lookup[base + f] = entry;
                    }
                }
            }
            code = (code + count) << 1;
            sym_off = (uint16_t)(sym_off + count);
        }
        t->valid = 1;
    }
    return 0;
}

static int parse_scan(const uint8_t* d, size_t size, Context* c) {
    if (size < 1) {
        return BAD_SYNTAX;
    }
    if (d[0] != 3) {
        return BAD_COMPONENT_COUNT;
    }
    if (size < 10) {
        return BAD_SYNTAX;
    }
    for (int i = 0; i < 3; i++) {
        uint8_t sel = d[2 + i * 2];
        uint8_t dc = sel >> 4, ac = sel & 15;
        if (dc > 1 || ac > 1 || !c->huff[dc * 2].valid || !c->huff[ac * 2 + 1].valid) {
            return MISSING_HUFFMAN_TABLE;
        }
        c->comp[i].dc_table = dc;
        c->comp[i].ac_table = ac;
        c->comp[i].pred_dc = 0;
    }
    if (d[7] != 0 || d[8] != 63 || d[9] != 0) {
        return BAD_SYNTAX;
    }
    return 0;
}

static int parse_headers(const uint8_t* file, Context* c) {
    size_t pos = 0;
    for (;;) {
        if (file[pos++] != 0xFF) {
            return BAD_SYNTAX;
        }
        uint8_t marker;
        do {
            marker = file[pos++];
        } while (marker == 0xFF);
        if (marker == 0xD8) {
            continue;
        }
        const uint8_t* seg = NULL;
        size_t seg_size = 0;
        if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
            if (!read_segment(file, &pos, &seg, &seg_size)) {
                return BAD_SYNTAX;
            }
            continue;
        }
        switch (marker) {
        case 0xC0:
        case 0xC4:
        case 0xDA:
        case 0xDB:
        case 0xDD: {
            if (!read_segment(file, &pos, &seg, &seg_size)) {
                return BAD_SYNTAX;
            }
            int r = 0;
            if (marker == 0xC0) {
                r = parse_frame(seg, seg_size, c);
            } else if (marker == 0xC4) {
                r = parse_huffman(seg, seg_size, c);
            } else if (marker == 0xDB) {
                r = parse_quant(seg, seg_size, c);
            } else if (marker == 0xDD) {
                if (seg_size != 2) {
                    return BAD_SYNTAX;
                }
                c->restart_interval = (uint16_t)(seg[0] << 8 | seg[1]);
            } else {
                r = parse_scan(seg, seg_size, c);
                if (r == 0) {
                    c->scan_offset = pos;
                    return 0;
                }
            }
            if (r != 0) {
                return r;
            }
            break;
        }
        default:
            return UNSUPPORTED_MARKER;
        }
    }
}

/* ---- entropy decoding -------------------------------------------------- */

typedef struct {
    const uint8_t* p;
    uint32_t acc; /* the unread bits are the low nbits */
    int nbits;
} Bits;

static inline void refill(Bits* b) {
    while (b->nbits <= 24) {
        b->acc = (b->acc << 8) | *b->p++;
        b->nbits += 8;
    }
}

static inline uint32_t get_bits(Bits* b, int n) {
    if (n == 0) {
        return 0;
    }
    if (b->nbits < n) {
        refill(b);
    }
    b->nbits -= n;
    return (b->acc >> b->nbits) & ((1u << n) - 1);
}

static inline int decode_huff(Bits* b, const HuffTable* t) {
    if (b->nbits < 16) {
        refill(b);
    }
    uint32_t peek = (b->acc >> (b->nbits - LOOKUP_BITS)) & ((1u << LOOKUP_BITS) - 1);
    uint16_t e = t->lookup[peek];
    if (e) {
        b->nbits -= e >> 8;
        return e & 0xFF;
    }
    for (int len = LOOKUP_BITS + 1; len <= 16; len++) {
        uint32_t code = (b->acc >> (b->nbits - len)) & ((1u << len) - 1);
        uint32_t first = t->first_codes[len];
        if (code >= first && code - first < t->counts[len]) {
            b->nbits -= len;
            return t->symbols[t->symbol_offsets[len] + code - first];
        }
    }
    return -1;
}

static inline int32_t extend(uint32_t v, int n) {
    if (n != 0 && v < (1u << (n - 1))) {
        return (int32_t)v - (int32_t)(1u << n) + 1;
    }
    return (int32_t)v;
}

static int decode_block(Bits* b, Context* c, int ci, int16_t blk[64]) {
    memset(blk, 0, 64 * sizeof(int16_t));
    Component* comp = &c->comp[ci];
    const HuffTable* dct = &c->huff[comp->dc_table * 2];
    const HuffTable* act = &c->huff[comp->ac_table * 2 + 1];

    int n = decode_huff(b, dct);
    if (n < 0 || n > 16) {
        return 0;
    }
    comp->pred_dc += extend(get_bits(b, n), n);
    blk[0] = (int16_t)comp->pred_dc;

    int k = 1;
    while (k < 64) {
        int rs = decode_huff(b, act);
        if (rs < 0) {
            return 0;
        }
        int run = rs >> 4;
        n = rs & 15;
        if (n == 0) {
            if (run == 15) {
                k += 16;
                continue;
            }
            break;
        }
        k += run;
        if (k >= 64) {
            return 0;
        }
        blk[k_natural_order[k]] = (int16_t)extend(get_bits(b, n), n);
        k++;
    }
    return 1;
}

/* ---- inverse DCT (AAN) ------------------------------------------------- */

static inline void aan_odd(float z10, float z11, float z12, float z13, float* o7, float* o6, float* o5, float* o4) {
    *o7 = z11 + z13;
    float z5 = (z10 + z12) * K_C2;
    *o6 = (z5 - z10 * K_C2_PLUS_C6) - *o7;
    *o5 = (z11 - z13) * K_SQRT2 - *o6;
    *o4 = (z5 - z12 * K_C2_MINUS_C6) - *o5;
}

static inline void combine(float out[8], float e0, float e1, float e2, float e3, float o7, float o6, float o5,
                           float o4) {
    out[0] = e0 + o7;
    out[1] = e1 + o6;
    out[2] = e2 + o5;
    out[3] = e3 + o4;
    out[4] = e3 - o4;
    out[5] = e2 - o5;
    out[6] = e1 - o6;
    out[7] = e0 - o7;
}

static inline void row_full(const int16_t* c, const float* q, float out[8]) {
    float x0 = (float)c[0] * q[0], x1 = (float)c[1] * q[1], x2 = (float)c[2] * q[2], x3 = (float)c[3] * q[3];
    float c4 = (float)c[4] * q[4], c5 = (float)c[5] * q[5], c6 = (float)c[6] * q[6], c7 = (float)c[7] * q[7];
    float sum04 = x0 + c4, dif04 = x0 - c4, sum26 = x2 + c6, dif26 = x2 - c6;
    float rot = dif26 * K_SQRT2 - sum26;
    float o7, o6, o5, o4;
    aan_odd(c5 - x3, x1 + c7, x1 - c7, c5 + x3, &o7, &o6, &o5, &o4);
    combine(out, sum04 + sum26, dif04 + rot, dif04 - rot, sum04 - sum26, o7, o6, o5, o4);
}

static inline void row_low4(const int16_t* c, const float* q, float out[8]) {
    float x0 = (float)c[0] * q[0], x1 = (float)c[1] * q[1], x2 = (float)c[2] * q[2], x3 = (float)c[3] * q[3];
    float sum02 = x0 + x2, dif02 = x0 - x2;
    float o7, o6, o5, o4;
    aan_odd(-x3, x1, x1, x3, &o7, &o6, &o5, &o4);
    combine(out, sum02, x2 * K_SQRT2 + dif02, sum02 - x2 * K_SQRT2, dif02, o7, o6, o5, o4);
}

static inline void row_dc_ac(float dc, float ac, float out[8]) {
    float a1 = ac * K_C2 - ac;
    float a2 = ac * K_SQRT2 - a1;
    float a3 = a2 - ac * K_C6;
    out[0] = dc + ac;
    out[1] = dc + a1;
    out[2] = dc + a2;
#if THP_SDK_QUARTER_IDCT_BUG
    out[3] = dc + a3;
    out[4] = dc - a3;
#else
    out[3] = dc - a3;
    out[4] = dc + a3;
#endif
    out[5] = dc - a2;
    out[6] = dc - a1;
    out[7] = dc - ac;
}

static inline uint8_t clamp_px(float v) {
    int iv = (int)(v * 0.125f); /* truncates toward zero, as the float compares did */
    if ((unsigned)iv > 255u) {
        iv = iv < 0 ? 0 : 255;
    }
    return (uint8_t)iv;
}

static void inverse_dct(const int16_t* coef, const QuantTable* qt, uint8_t out[64]) {
    float ws[64];
    int last_row = 0;
    int ac = 0;
    for (int i = 1; i < 64; i++) {
        ac |= coef[i];
    }
    if (ac == 0) { /* DC only: every output is the same value (exactly what the full transform gives) */
        memset(out, clamp_px((float)coef[0] * qt->values[0] + K_OUTPUT_BIAS), 64);
        return;
    }
    for (int r = 0; r < 8; r++) {
        const int16_t* rc = coef + r * 8;
        const float* rq = qt->values + r * 8;
        int ext = 8;
        while (ext > 0 && rc[ext - 1] == 0) {
            ext--;
        }
        float* w = ws + r * 8;
        if (ext != 0) {
            last_row = r;
        }
        if (ext <= 1) {
            float v = (float)rc[0] * rq[0];
            for (int i = 0; i < 8; i++) {
                w[i] = v;
            }
        } else if (ext == 2) {
            row_dc_ac((float)rc[0] * rq[0], (float)rc[1] * rq[1], w);
        } else if (ext <= 4) {
            row_low4(rc, rq, w);
        } else {
            row_full(rc, rq, w);
        }
    }
    if (last_row < 4) {
        /* Rows 4 to 7 are all zero (the common case): their terms drop out of the column transform exactly. */
        for (int col = 0; col < 8; col++) {
            float x0 = ws[col], x1 = ws[8 + col], x2 = ws[16 + col], x3 = ws[24 + col];
            float s0 = x0 + K_OUTPUT_BIAS;
            float rot = x2 * K_SQRT2 - x2;
            float o7, o6, o5, o4, res[8];
            aan_odd(-x3, x1, x1, x3, &o7, &o6, &o5, &o4);
            combine(res, s0 + x2, s0 + rot, s0 - rot, s0 - x2, o7, o6, o5, o4);
            for (int r = 0; r < 8; r++) {
                out[r * 8 + col] = clamp_px(res[r]);
            }
        }
        return;
    }
    for (int col = 0; col < 8; col++) {
        float x0 = ws[col], x1 = ws[8 + col], x2 = ws[16 + col], x3 = ws[24 + col];
        float x4 = ws[32 + col], x5 = ws[40 + col], x6 = ws[48 + col], x7 = ws[56 + col];
        float sum04 = (x0 + x4) + K_OUTPUT_BIAS, dif04 = (x0 - x4) + K_OUTPUT_BIAS;
        float sum26 = x2 + x6, dif26 = x2 - x6;
        float rot = dif26 * K_SQRT2 - sum26;
        float o7, o6, o5, o4, res[8];
        aan_odd(x5 - x3, x1 + x7, x1 - x7, x5 + x3, &o7, &o6, &o5, &o4);
        combine(res, sum04 + sum26, dif04 + rot, dif04 - rot, sum04 - sum26, o7, o6, o5, o4);
        for (int r = 0; r < 8; r++) {
            out[r * 8 + col] = clamp_px(res[r]);
        }
    }
}

/* Writes an 8x8 block at (bx, by), clipped to w x h, into 8x4 texel tiles. */
static void write_block(uint8_t* out, unsigned w, unsigned h, unsigned bx, unsigned by, const uint8_t px[64]) {
    unsigned tiles_per_row = (w + 7) / 8;
    if (bx + 8 <= w && by + 8 <= h) {
        for (unsigned r = 0; r < 8; r++) {
            unsigned y = by + r;
            memcpy(out + ((size_t)(y / 4) * tiles_per_row + bx / 8) * 32 + (y & 3) * 8, px + r * 8, 8);
        }
        return;
    }
    for (unsigned r = 0; r < 8 && by + r < h; r++) {
        for (unsigned col = 0; col < 8 && bx + col < w; col++) {
            unsigned x = bx + col, y = by + r;
            out[((size_t)(y / 4) * tiles_per_row + x / 8) * 32 + (y & 3) * 8 + (x & 7)] = px[r * 8 + col];
        }
    }
}

/* The picture stage for one MCU: its six blocks (four luma, U, V) of
 * coefficients to pixels in the three planes. */
static void idct_mcu(const Context* c, const int16_t* mcu, unsigned mx, unsigned my, uint8_t* ty, uint8_t* tu,
                     uint8_t* tv) {
    unsigned cw = (c->width + 1u) / 2, ch = (c->height + 1u) / 2;
    unsigned lx = mx * 16, ly = my * 16;
    uint8_t px[64];
    for (int b = 0; b < 6; b++) {
        int ci = b < 4 ? 0 : b - 3;
        inverse_dct(mcu + b * 64, &c->quant[c->comp[ci].quant_table], px);
        switch (b) {
        case 0: write_block(ty, c->width, c->height, lx, ly, px); break;
        case 1: write_block(ty, c->width, c->height, lx + 8, ly, px); break;
        case 2: write_block(ty, c->width, c->height, lx, ly + 8, px); break;
        case 3: write_block(ty, c->width, c->height, lx + 8, ly + 8, px); break;
        case 4: write_block(tu, cw, ch, mx * 8, my * 8, px); break;
        default: write_block(tv, cw, ch, mx * 8, my * 8, px); break;
        }
    }
}

/* The entropy stage for one MCU: six blocks of coefficients into mcu[6][64]. */
static int decode_mcu(Bits* b, Context* c, int16_t* mcu) {
    for (int i = 0; i < 6; i++) {
        if (!decode_block(b, c, i < 4 ? 0 : i - 3, mcu + i * 64)) {
            return 0;
        }
    }
    return 1;
}

static void restart_check(Bits* b, Context* c, uint32_t* count) {
    if (c->restart_interval != 0 && ++*count == c->restart_interval) {
        b->nbits -= b->nbits & 7;
        *count = 0;
        for (int i = 0; i < 3; i++) {
            c->comp[i].pred_dc = 0;
        }
    }
}

static int begin(const void* file, void* tile_y, void* tile_u, void* tile_v, Context* c) {
    if (file == NULL) {
        return NO_INPUT;
    }
    if (tile_y == NULL || tile_u == NULL || tile_v == NULL) {
        return NO_OUTPUT;
    }
    memset(c, 0, sizeof(*c));
    return parse_headers((const uint8_t*)file, c);
}

int32_t thp_video_info(const void* frame, unsigned* width, unsigned* height) {
    Context ctx;
    int r = begin(frame, (void*)1, (void*)1, (void*)1, &ctx);
    if (r == 0) {
        *width = ctx.width;
        *height = ctx.height;
    }
    return r;
}

size_t thp_video_scratch_size(unsigned width, unsigned height) {
    return (size_t)((width + 15u) / 16) * ((height + 15u) / 16) * 6 * 64 * sizeof(int16_t);
}

/* One job: the picture stage of MCU rows [row0, row1). */
typedef struct {
    const Context* c;
    const int16_t* coef; /* the frame's coefficients */
    uint8_t *ty, *tu, *tv;
    unsigned mcu_cols, row0, row1;
} RowJob;

static void row_job(void* arg) {
    const RowJob* j = (const RowJob*)arg;
    for (unsigned my = j->row0; my < j->row1; my++) {
        for (unsigned mx = 0; mx < j->mcu_cols; mx++) {
            idct_mcu(j->c, j->coef + ((size_t)my * j->mcu_cols + mx) * 6 * 64, mx, my, j->ty, j->tu, j->tv);
        }
    }
}

#define MAX_ROW_JOBS 32

int32_t thp_video_decode_ex(const void* file, void* tile_y, void* tile_u, void* tile_v, void* scratch,
                            size_t scratch_size, const ThpExec* exec) {
    Context ctx;
    Context* c = &ctx;
    int r = begin(file, tile_y, tile_u, tile_v, c);
    if (r != 0) {
        return r;
    }
    Bits b = {(const uint8_t*)file + c->scan_offset, 0, 0};
    unsigned mcu_cols = (c->width + 15u) / 16, mcu_rows = (c->height + 15u) / 16;
    uint32_t restart_count = 0;
    int result = 0;

    if (exec == NULL || exec->submit == NULL || exec->join_all == NULL ||
        scratch_size < thp_video_scratch_size(c->width, c->height) || mcu_rows < 2) {
        int16_t mcu[6 * 64];
        for (unsigned my = 0; my < mcu_rows && result == 0; my++) {
            for (unsigned mx = 0; mx < mcu_cols; mx++) {
                if (!decode_mcu(&b, c, mcu)) {
                    result = BAD_SYNTAX;
                    break;
                }
                idct_mcu(c, mcu, mx, my, tile_y, tile_u, tile_v);
                restart_check(&b, c, &restart_count);
            }
        }
    } else {
        /* Entropy decoding is serial; the picture stage of finished rows runs
         * meanwhile on the executor, and the caller joins them all at the end. */
        RowJob jobs[MAX_ROW_JOBS];
        uint32_t handles[MAX_ROW_JOBS];
        unsigned njobs = 0;
        unsigned group = (mcu_rows + MAX_ROW_JOBS - 1) / MAX_ROW_JOBS;
        if (group < exec->rows_per_job) {
            group = exec->rows_per_job;
        }
        int16_t* coef = (int16_t*)scratch;
        unsigned done_row = 0;
        for (unsigned my = 0; my < mcu_rows && result == 0; my++) {
            for (unsigned mx = 0; mx < mcu_cols; mx++) {
                if (!decode_mcu(&b, c, coef + ((size_t)my * mcu_cols + mx) * 6 * 64)) {
                    result = BAD_SYNTAX;
                    break;
                }
                restart_check(&b, c, &restart_count);
            }
            if (result == 0 && (my + 1 - done_row >= group || my + 1 == mcu_rows)) {
                RowJob* j = &jobs[njobs];
                j->c = c;
                j->coef = coef;
                j->ty = tile_y;
                j->tu = tile_u;
                j->tv = tile_v;
                j->mcu_cols = mcu_cols;
                j->row0 = done_row;
                j->row1 = my + 1;
                handles[njobs] = exec->submit(row_job, j, exec->user);
                njobs++;
                done_row = my + 1;
            }
        }
        exec->join_all(handles, njobs, exec->user);
    }
    return result;
}

int32_t thp_video_decode(const void* file, void* tile_y, void* tile_u, void* tile_v) {
    return thp_video_decode_ex(file, tile_y, tile_u, tile_v, NULL, 0, NULL);
}
