#ifndef _DOLPHIN_OSFASTCAST_H_
#define _DOLPHIN_OSFASTCAST_H_

#ifdef __cplusplus
extern "C" {
#endif

#define OS_GQR_F32 0x0000
#define OS_GQR_U8  0x0004
#define OS_GQR_U16 0x0005
#define OS_GQR_S8  0x0006
#define OS_GQR_S16 0x0007

#define OS_FASTCAST_U8  2
#define OS_FASTCAST_U16 3
#define OS_FASTCAST_S8  4
#define OS_FASTCAST_S16 5

#ifdef TARGET_PORT
// Ports: the same conversions through emulated GQRs (port_fastcast.h).
#include "port_fastcast.h"

static inline void OSInitFastCast(void) {
    port_gqr[OS_FASTCAST_U8] = (OS_GQR_U8 << 16) | OS_GQR_U8;
    port_gqr[OS_FASTCAST_U16] = (OS_GQR_U16 << 16) | OS_GQR_U16;
    port_gqr[OS_FASTCAST_S8] = (OS_GQR_S8 << 16) | OS_GQR_S8;
    port_gqr[OS_FASTCAST_S16] = (OS_GQR_S16 << 16) | OS_GQR_S16;
}

static inline s16 __OSf32tos16(f32 inF) {
    s16 out;
    port_psq_store(port_gqr[OS_FASTCAST_S16], inF, &out);
    return out;
}

static inline void OSf32tos16(f32* f, s16* out) {
    *out = __OSf32tos16(*f);
}

static inline f32 __OSs16tof32(const s16* in) {
    return port_psq_load(port_gqr[OS_FASTCAST_S16], in);
}

static inline void OSs16tof32(const s16* in, f32* out) {
    *out = __OSs16tof32(in);
}

static inline f32 __OSs8tof32(const s8* in) {
    return port_psq_load(port_gqr[OS_FASTCAST_S8], in);
}

static inline void OSs8tof32(const s8* in, f32* out) {
    *out = __OSs8tof32(in);
}

#else
// clang-format off
static inline void OSInitFastCast(void) {
#ifdef __MWERKS__
  asm
  {
        li      r3, OS_GQR_U8
        oris    r3, r3, OS_GQR_U8
        mtspr   GQR2, r3

        li      r3, OS_GQR_U16
        oris    r3, r3, OS_GQR_U16
        mtspr   GQR3, r3

        li      r3, OS_GQR_S8
        oris    r3, r3, OS_GQR_S8
        mtspr   GQR4, r3

        li      r3, OS_GQR_S16
        oris    r3, r3, OS_GQR_S16
        mtspr   GQR5, r3
  }
#else

#endif
}
// clang-format off

static inline s16 __OSf32tos16(register f32 inF) {
    u32 tmp;
    register u32* tmpPtr = &tmp;
    register s16 out;
    asm {
        psq_st inF, 0(tmpPtr), 0x1, OS_FASTCAST_S16
        lha out, 0(tmpPtr)
    }
    return out;
}

static inline void OSf32tos16(f32* f, s16* out) {
    *out = __OSf32tos16(*f);
}

static inline f32 __OSs16tof32(register const s16* in) {
    register f32 out;
    asm {
        psq_l out, 0(in), 1, OS_FASTCAST_S16
    }
    return out;
}

static inline void OSs16tof32(const s16* in, f32* out) {
    *out = __OSs16tof32(in);
}

static inline f32 __OSs8tof32(register const s8* in) {
    register f32 out;
    asm {
        psq_l out, 0(in), 1, OS_FASTCAST_S8
    }
    return out;
}

static inline void OSs8tof32(const s8* in, f32* out) {
    *out = __OSs8tof32(in);
}
#endif // TARGET_PORT

#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_OSFASTCAST