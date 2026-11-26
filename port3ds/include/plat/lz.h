/* A small LZ77 codec (LZ4-like block format, 64 KiB window) for data kept in memory and restored whole, such as
 * boot-time snapshots. Lossless; no allocation. */
#ifndef PLAT_LZ_H
#define PLAT_LZ_H

#include <stdint.h>

/* The most bytes plat_lz_pack writes for n input bytes. */
static inline uint32_t plat_lz_bound(uint32_t n) { return n + n / 255 + 16; }

/* Packs n bytes of src into dst (plat_lz_bound(n) bytes); returns the packed size. */
uint32_t plat_lz_pack(const void* src, uint32_t n, void* dst);

/* Unpacks into dst, which must take exactly n bytes; returns 1 when the packed data gave exactly n bytes. */
int plat_lz_unpack(const void* src, uint32_t packed, void* dst, uint32_t n);

#endif
