/* Host study: newlib (devkitARM) sinf, transliterated from libm_a-sinf.o, against (float)fdlibm sin((double)x)
 * from libs/msl. Every non-NaN float. Build: see run.sh. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <omp.h>
#include "tables.h"

double msl_sin(double);

typedef struct { double sign[4], hpi_inv, hpi, c0, c1, c2, c3, c4, s1, s2, s3; } T;
static T tab[2];
static uint32_t inv[24];

static inline uint32_t asu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* The sinf of devkitARM newlib (ARM optimized-routines algorithm), operation order as in the object code
 * (vmla/vmls are unfused: product rounded, then the add). */
float nsinf(float y) {
    uint32_t u = asu(y), top = (u >> 20) & 0x7ff;
    double x = (double)y, x2;
    uint32_t n4 = 0; int n = 0;
    if (top < 0x3f4) {
        x2 = x * x;
        if (top < 0x398) return y;
        double x3 = x * x2;
        double s = x + x3 * tab[0].s1;
        double x5 = x2 * x3;
        double t = tab[0].s2 + x2 * tab[0].s3;
        return (float)(s + t * x5);
    } else if (top < 0x42f) {
        double r = x * tab[0].hpi_inv;
        n = (int)((int32_t)r + 0x800000) >> 24;
        n4 = (uint32_t)n;
        x = x - (double)n * tab[0].hpi;
        x2 = x * x;
    } else if (top <= 0x7f7) {
        uint32_t idx = (u >> 26) & 15;
        uint32_t a0 = inv[idx], a1 = inv[idx + 4], a2 = inv[idx + 8];
        uint32_t m = (u & 0x007fffffu) | 0x800000u;
        m <<= (u >> 23) & 7;
        uint32_t hi = a0 * m;
        uint32_t lo = (uint32_t)(((uint64_t)m * a2) >> 32);
        uint64_t res = ((uint64_t)hi << 32 | lo) + (uint64_t)m * a1;
        uint32_t h = (uint32_t)(res >> 32) + 0x20000000u;
        n4 = h >> 30;
        h &= 0xc0000000u;
        uint32_t hw = (uint32_t)(res >> 32) - h;
        int64_t sres = (int64_t)((uint64_t)hw << 32 | (uint32_t)res);
        double c;
        { uint64_t cb = 0x3c1921fb54442d18ull; memcpy(&c, &cb, 8); }
        x = (double)sres * c;
        n = (int)(n4 + (u >> 31));
        x2 = x * x;
    } else {
        float z = y - y;
        return z / z; /* NaN */
    }
    const T* p = (n & 2) ? &tab[1] : &tab[0];
    if ((n4 & 1) == 0) {
        double s = x * tab[0].sign[n & 3];
        double t = s * x2;
        double u2 = p->s2 + x2 * p->s3;
        double r = s + t * p->s1;
        double w = x2 * t;
        return (float)(r + u2 * w);
    } else {
        double r = p->c0 + x2 * p->c1;
        double x4 = x2 * x2;
        double x6 = x2 * x4;
        double c34 = p->c3 + x2 * p->c4;
        r = r + x4 * p->c2;
        return (float)(r + c34 * x6);
    }
}

static inline float msl_f(float x) { return (float)msl_sin((double)x); }

int main(int argc, char** argv) {
    memcpy(tab, tab_bytes, sizeof tab);
    memcpy(inv, inv_bytes, sizeof inv);
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    if (mode == 2) { /* sanity: against the host's sinf */
        uint64_t same = 0, one = 0, more = 0; uint32_t st = 12345;
        for (int i = 0; i < 20000000; i++) {
            st = st * 1664525u + 1013904223u; uint32_t u = st ^ (st >> 7);
            float x; memcpy(&x, &u, 4); if (x != x || fabsf(x) > 1e30f) continue;
            float a = nsinf(x), b = sinf(x); int d = (int)asu(a) - (int)asu(b); if (d < 0) d = -d;
            if (!d) same++; else if (d == 1) one++; else more++;
        }
        printf("vs host sinf: same %llu, 1 ulp %llu, more %llu\n", (unsigned long long)same, (unsigned long long)one, (unsigned long long)more);
        return 0;
    }
    if (mode == 3) { /* who is correctly rounded: sample against long double sinl */
        uint64_t n = 0, na = 0, nb = 0, only_a = 0, only_b = 0, both_wrong = 0; uint32_t st = 777;
        for (int i = 0; i < 40000000; i++) {
            st = st * 1664525u + 1013904223u; uint32_t u = st ^ (st >> 9) ^ (uint32_t)i * 2654435761u;
            float x; memcpy(&x, &u, 4); if (x != x || fabsf(x) > 1e9f) continue;
            float c = (float)sinl((long double)x), a = nsinf(x), b = msl_f(x);
            n++; na += a == c; nb += b == c;
            if (a == c && b != c) only_b++; /* b wrong */
            if (b == c && a != c) only_a++; /* a wrong */
            if (a != c && b != c) both_wrong++;
        }
        printf("sample %llu (|x|<1e9): newlib correctly rounded %llu, fdlibm correctly rounded %llu; newlib wrong only %llu, fdlibm wrong only %llu, both wrong %llu\n",
               (unsigned long long)n, (unsigned long long)na, (unsigned long long)nb, (unsigned long long)only_a, (unsigned long long)only_b, (unsigned long long)both_wrong);
        return 0;
    }
    if (mode == 1) { /* timing */
        volatile float sink = 0;
        float acc = 0;
        struct timespec t0, t1;
        for (int rep = 0; rep < 2; rep++) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            float x = 0.3f;
            for (int i = 0; i < 20000000; i++) { acc += nsinf(x); x += 0.013f; if (x > 6.2831853f) x -= 6.2831853f; }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("newlib sinf (transliteration), 0..2pi: %.1f ns/call\n", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 2e7);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            x = 0.3f;
            for (int i = 0; i < 20000000; i++) { acc += msl_f(x); x += 0.013f; if (x > 6.2831853f) x -= 6.2831853f; }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("(float)fdlibm sin, 0..2pi: %.1f ns/call\n", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 2e7);
        }
        sink = acc;
        return 0;
    }
    /* exhaustive */
    uint64_t diff = 0, nan_diff = 0, total = 0;
    uint64_t hist[256] = {0}, ulp1 = 0, ulpbig = 0;
    uint32_t first[8], nfirst = 0, worst_u = 0; int64_t worst = 0;
    #pragma omp parallel
    {
        uint64_t ld = 0, ln = 0, lh[256] = {0}, l1 = 0, lb = 0;
        int64_t lw = 0; uint32_t lwu = 0;
        #pragma omp for schedule(static, 65536) nowait
        for (int64_t i = 0; i < (1ll << 32); i++) {
            uint32_t u = (uint32_t)i; float x; memcpy(&x, &u, 4);
            float a = nsinf(x), b = msl_f(x);
            uint32_t ua = asu(a), ub = asu(b);
            if (ua == ub) continue;
            if (a != a && b != b) continue;
            if (a != a || b != b) { ln++; continue; }
            ld++;
            lh[(u >> 23) & 0xff]++;
            int64_t d = (int64_t)(int32_t)ua - (int32_t)ub; /* ordering is fine for same-sign values; sign diff rare */
            if (d < 0) d = -d;
            if (d == 1) l1++; else lb++;
            if (d > lw) { lw = d; lwu = u; }
        }
        #pragma omp critical
        {
            diff += ld; nan_diff += ln; ulp1 += l1; ulpbig += lb;
            for (int k = 0; k < 256; k++) hist[k] += lh[k];
            if (lw > worst) { worst = lw; worst_u = lwu; }
        }
    }
    printf("differ: %llu of 4294967296 (%.4f%%), NaN-vs-number: %llu, 1-ulp: %llu, >1 ulp: %llu\n", (unsigned long long)diff,
           100.0 * diff / 4294967296.0, (unsigned long long)nan_diff, (unsigned long long)ulp1, (unsigned long long)ulpbig);
    printf("worst: %lld ulp at x=%a\n", (long long)worst, (double)*(float*)&worst_u);
    printf("by binary exponent (biased exp: count), nonzero only:\n");
    for (int k = 0; k < 256; k++) if (hist[k]) printf("  e=%3d (2^%d): %llu\n", k, k - 127, (unsigned long long)hist[k]);
    return 0;
}
