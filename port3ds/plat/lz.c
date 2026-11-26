/* LZ77 codec (include/plat/lz.h). A sequence is: a token byte (high nibble: literal count, low nibble: match
 * length - 4; 15 means more follow as bytes, each 255 adds and the first byte below 255 ends), the literals, and
 * unless the output is complete, a 2-byte little-endian offset (1..65535) back into the output. Matches of 4 or more
 * bytes are found greedily through a hash of the next 4 bytes. */
#include "plat/lz.h"

#include <string.h>

#define HASH_BITS 12
#define MIN_MATCH 4

static inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint8_t* put_len(uint8_t* o, uint32_t n) {
    while (n >= 255) {
        *o++ = 255;
        n -= 255;
    }
    *o++ = (uint8_t)n;
    return o;
}

uint32_t plat_lz_pack(const void* src, uint32_t n, void* dst) {
    const uint8_t* in = src;
    uint8_t* o = dst;
    uint32_t table[1u << HASH_BITS];
    memset(table, 0xFF, sizeof(table));
    uint32_t i = 0, lit = 0;
    while (i + MIN_MATCH <= n) {
        uint32_t h = rd32(in + i) * 2654435761u >> (32 - HASH_BITS);
        uint32_t c = table[h];
        table[h] = i;
        if (c == 0xFFFFFFFFu || i - c > 65535u || rd32(in + c) != rd32(in + i)) {
            i++;
            continue;
        }
        uint32_t len = MIN_MATCH;
        while (i + len < n && in[c + len] == in[i + len]) {
            len++;
        }
        uint32_t nlit = i - lit, m = len - MIN_MATCH;
        *o++ = (uint8_t)((nlit < 15 ? nlit : 15) << 4 | (m < 15 ? m : 15));
        if (nlit >= 15) {
            o = put_len(o, nlit - 15);
        }
        memcpy(o, in + lit, nlit);
        o += nlit;
        uint32_t off = i - c;
        *o++ = (uint8_t)off;
        *o++ = (uint8_t)(off >> 8);
        if (m >= 15) {
            o = put_len(o, m - 15);
        }
        i += len;
        lit = i;
    }
    /* the last literals, with no match after them */
    uint32_t nlit = n - lit;
    *o++ = (uint8_t)((nlit < 15 ? nlit : 15) << 4);
    if (nlit >= 15) {
        o = put_len(o, nlit - 15);
    }
    memcpy(o, in + lit, nlit);
    o += nlit;
    return (uint32_t)(o - (uint8_t*)dst);
}

static int get_len(const uint8_t** p, const uint8_t* end, uint32_t* n) {
    for (;;) {
        if (*p >= end) {
            return 0;
        }
        uint8_t b = *(*p)++;
        *n += b;
        if (b != 255) {
            return 1;
        }
    }
}

int plat_lz_unpack(const void* src, uint32_t packed, void* dst, uint32_t n) {
    const uint8_t* p = src;
    const uint8_t* end = p + packed;
    uint8_t* o = dst;
    uint8_t* oend = o + n;
    while (p < end) {
        uint8_t tok = *p++;
        uint32_t nlit = tok >> 4;
        if (nlit == 15 && !get_len(&p, end, &nlit)) {
            return 0;
        }
        if (nlit > (uint32_t)(end - p) || nlit > (uint32_t)(oend - o)) {
            return 0;
        }
        memcpy(o, p, nlit);
        o += nlit;
        p += nlit;
        if (o == oend) {
            return p == end;
        }
        if (end - p < 2) {
            return 0;
        }
        uint32_t off = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        p += 2;
        uint32_t len = tok & 15;
        if (len == 15 && !get_len(&p, end, &len)) {
            return 0;
        }
        len += MIN_MATCH;
        if (off == 0 || off > (uint32_t)(o - (uint8_t*)dst) || len > (uint32_t)(oend - o)) {
            return 0;
        }
        const uint8_t* m = o - off;
        for (uint32_t k = 0; k < len; k++) { /* may overlap its own output */
            o[k] = m[k];
        }
        o += len;
    }
    return o == oend;
}
