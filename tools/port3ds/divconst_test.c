/* Host test: port_divconst.h against plain division.
 * gcc -O2 -Iport3ds/include tools/port3ds/divconst_test.c -o divconst_test && ./divconst_test */
#include <stdio.h>
#include <stdint.h>
#include "port_divconst.h"

static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static long bad;

#define CHK(D) do { \
    unsigned long long u = (unsigned long long)n; long long sv = (long long)n; \
    if (port_udiv_c(u, D) != u / D) { bad++; printf("u %llu / %u\n", u, D); } \
    if (port_sdiv_c(sv, D) != sv / (long long)D) { bad++; printf("s %lld / %u\n", sv, D); } \
    if (PORT_DIV_C(sv * 8, D) != sv * 8 / D) { bad++; } \
} while (0)

static void one(uint64_t n) {
    CHK(40500000u); CHK(40500u); CHK(324u); CHK(8000u); CHK(268111856u);
}

int main(void) {
    uint64_t ds[] = {40500000u, 40500u, 324u, 8000u, 268111856u};
    long cnt = 0;
    for (int i = 0; i < 5; i++) {
        for (uint64_t k = 0; k < 3000; k++) {
            for (int e = -2; e <= 2; e++) {
                one(k * ds[i] + e); one(0 - (k * ds[i] + e)); cnt += 2;
            }
        }
        for (int b = 0; b < 64; b++) {
            for (int e = -3; e <= 3; e++) { one((1ull << b) + e); one(0 - ((1ull << b) + e)); cnt += 2; }
        }
        for (uint64_t k = 1; k < 200; k++) {
            uint64_t top = ~0ull / ds[i] * ds[i];
            for (int e = -2; e <= 2; e++) { one(top - k * ds[i] + e); one(0 - (top - k * ds[i] + e)); cnt += 2; }
        }
    }
    one(0x7fffffffffffffffull); one(0x8000000000000000ull); one(~0ull); one(0);
    for (long i = 0; i < 200000000; i++) {
        uint64_t n = rnd();
        one(n); one(n >> (rnd() & 63)); cnt += 2;
    }
    printf("%ld numerators, %ld mismatches\n", cnt, bad);
    return bad != 0;
}
