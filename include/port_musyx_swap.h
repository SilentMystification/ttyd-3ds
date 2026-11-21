// MusyX data from the disc converted to host byte order at load (port3ds/game/musyx_swap.c). sound.c calls these on
// ports right after reading each file. Sample data (.samp) is not converted: the mixer reads it big-endian.
#pragma once

#ifdef TARGET_PORT
void portMusyxSwapProj(void* data, u32 len);
void portMusyxSwapPool(void* data, u32 len);
void portMusyxSwapSdir(void* data, u32 len);
void portMusyxSwapSlib(void* data, u32 len);
void portMusyxSwapArr(void* arr, u32 len);
void portMusyxSwapStreamHeader(void* header);

static inline u16 portBE16(const void* p) {
    const u8* b = (const u8*)p;
    return (u16)((b[0] << 8) | b[1]);
}
#endif
