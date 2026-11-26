/* Exact 64-bit division by the SDK's constant divisors, for the 3DS port.
 *
 * The ARM11 has no hardware divide: a 64-bit '/' calls __aeabi_(u)ldivmod, a
 * bit loop of 100+ cycles. For a divisor known at compile time the quotient
 * comes from one 64x64->high-64 multiply (Granlund-Montgomery, "Division by
 * invariant integers using multiplication", figure 4.1), exact for every
 * 64-bit numerator. Signed division is sign-magnitude: it truncates toward
 * zero like C, and -2^63 works because its magnitude fits in 64 bits.
 *
 * Each constant divisor needs its multiplier m = floor(2^64 (2^l - d) / d) + 1
 * and l = ceil(log2 d). tools/port3ds/divconst_test.c checks every entry
 * against plain division. Any other divisor falls back to '/'.
 */
#ifndef PORT_DIVCONST_H
#define PORT_DIVCONST_H

#define PORT_DC_INLINE static inline __attribute__((always_inline))

PORT_DC_INLINE unsigned long long port_mulhi64(unsigned long long a, unsigned long long b) {
    unsigned long long a0 = (unsigned int)a, a1 = a >> 32;
    unsigned long long b0 = (unsigned int)b, b1 = b >> 32;
    unsigned long long p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    unsigned long long mid = (p00 >> 32) + (unsigned int)p01 + (unsigned int)p10;
    return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

PORT_DC_INLINE unsigned long long port_udiv_magic(unsigned long long n, unsigned long long m, int l) {
    unsigned long long q = port_mulhi64(m, n);
    unsigned long long t = ((n - q) >> 1) + q;
    return t >> (l - 1);
}

/* d: a compile-time constant after inlining. */
PORT_DC_INLINE unsigned long long port_udiv_c(unsigned long long n, unsigned int d) {
    switch (d) {
    case 40500000u: return port_udiv_magic(n, 0xa831bdb32743ace9ull, 26);
    case 40500u: return port_udiv_magic(n, 0x9e409340f45816dcull, 16);
    case 324u: return port_udiv_magic(n, 0x948b0fcd6e9e0653ull, 9);
    case 8000u: return port_udiv_magic(n, 0x0624dd2f1a9fbe77ull, 13);
    case 268111856u: return port_udiv_magic(n, 0x004f1969268c4e73ull, 28);
    default: return n / d;
    }
}

PORT_DC_INLINE long long port_sdiv_c(long long n, unsigned int d) {
    unsigned long long u = n < 0 ? 0ull - (unsigned long long)n : (unsigned long long)n;
    unsigned long long q = port_udiv_c(u, d);
    return n < 0 ? (long long)(0ull - q) : (long long)q;
}

/* 'x / d' with the C type rules of the original expression: only a 64-bit x is
 * rewritten (d is unsigned in the SDK macros, so a signed 64-bit x divides as
 * signed 64-bit). Anything else keeps the plain division. */
#define PORT_DIV_C(x, d) _Generic((x), \
    long long: port_sdiv_c((x), (d)), \
    unsigned long long: port_udiv_c((x), (d)), \
    default: ((x) / (d)))

#endif
