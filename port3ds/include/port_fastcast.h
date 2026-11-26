/* Paired-single quantized loads and stores (psq_l / psq_st) for the 3DS port.
 *
 * On the GameCube, OSFastCast.h converts between floats and small integers
 * through the graphics quantization registers GQR2-GQR5, and game code
 * changes GQR5's scale directly (hwaccess.h HW_SET_GQR5: map collision in
 * hitdrv.c, effects in effdrv.c). The register is CPU-wide state, so a scale
 * set in one place applies to every later conversion through GQR5 until it is
 * set again. A port keeps the same state in port_gqr[] and applies it
 * exactly as the hardware does (Dolphin's Interpreter_LoadStorePaired.cpp):
 *   load:  value * 2^-scale, scale = signed 6-bit LD_SCALE (bits 24-29)
 *   store: value * 2^scale, ST_SCALE (bits 8-13), saturated to the type's
 *          range and truncated toward zero
 * Types (LD_TYPE bits 16-18, ST_TYPE bits 0-2): 0 f32, 4 u8, 5 u16, 6 s8, 7 s16.
 */
#ifndef PORT_FASTCAST_H
#define PORT_FASTCAST_H

extern unsigned int port_gqr[8];

static inline float port_gqr_pow2(unsigned int scale6, int negate) {
    int s = (int)(scale6 & 0x3F);
    if (s >= 32) {
        s -= 64;
    }
    if (negate) {
        s = -s;
    }
    /* s is -32..32 here: 2^s is a normal float, so its bits are (127+s)<<23.
     * (ldexpf with a variable exponent costs a libm call chain on the ARM11.) */
    union { unsigned int u; float f; } c;
    c.u = (unsigned int)(127 + s) << 23;
    return c.f;
}

static inline float port_psq_load(unsigned int gqr, const void* in) {
    unsigned int type = (gqr >> 16) & 7;
    float v;
    switch (type) {
    case 4: v = (float)*(const unsigned char*)in; break;
    case 5: v = (float)*(const unsigned short*)in; break;
    case 6: v = (float)*(const signed char*)in; break;
    case 7: v = (float)*(const short*)in; break;
    default: return *(const float*)in;
    }
    return v * port_gqr_pow2(gqr >> 24, 1);
}

static inline void port_psq_store(unsigned int gqr, float value, void* out) {
    unsigned int type = gqr & 7;
    if (type < 4) {
        *(float*)out = value;
        return;
    }
    float v = value * port_gqr_pow2(gqr >> 8, 0);
    float lo, hi;
    switch (type) {
    case 4: lo = 0.0f; hi = 255.0f; break;
    case 5: lo = 0.0f; hi = 65535.0f; break;
    case 6: lo = -128.0f; hi = 127.0f; break;
    default: lo = -32768.0f; hi = 32767.0f; break;
    }
    if (v < lo) {
        v = lo;
    } else if (v > hi) {
        v = hi;
    } else if (v != v) {
        v = 0.0f;
    }
    int i = (int)v; /* truncates toward zero */
    switch (type) {
    case 4: *(unsigned char*)out = (unsigned char)i; break;
    case 5: *(unsigned short*)out = (unsigned short)i; break;
    case 6: *(signed char*)out = (signed char)i; break;
    default: *(short*)out = (short)i; break;
    }
}

#endif
