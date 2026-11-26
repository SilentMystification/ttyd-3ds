/* Host test: port3ds/plat/port_fmod.c against fmod.
 * gcc -O2 -D__real_fmod=fmod -D__wrap_fmod=port_fmod tools/port3ds/fmod_test.c port3ds/plat/port_fmod.c -lm -o fmod_test */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

double port_fmod(double x, double y);
static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static long bad, cnt;
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static void t(double x, double y) {
    double a = port_fmod(x, y), b = fmod(x, y);
    cnt++;
    if (bits(a) != bits(b) && !(a != a && b != b)) {
        if (bad++ < 10) printf("x=%a y=%a got %a want %a\n", x, y, a, b);
    }
}

int main(void) {
    double ys[] = {360.0, 90.0, 1.0, 0.1, 5e-324, 2.2250738585072014e-308, 1e300, 8.98846567431158e307,
                   1.7976931348623157e308, INFINITY, 0.0, -0.0, -360.0, NAN, 3.0, 1e-10};
    double xs[] = {0.0, -0.0, 5e-324, -5e-324, INFINITY, -INFINITY, NAN, 1.7976931348623157e308, -1.7976931348623157e308};
    int ny = sizeof ys / sizeof *ys, nx = sizeof xs / sizeof *xs;
    for (int i = 0; i < ny; i++) {
        double y = ys[i];
        for (int j = 0; j < nx; j++) { t(xs[j], y); }
        for (int k = -3; k <= 3; k++) {
            double c = k * y;
            for (int u = -100; u <= 100; u++) {
                t(dbl(bits(c) + u), y);
                t(dbl(bits(c) - u), y);
            }
        }
    }
    for (long i = 0; i < 300000000; i++) {
        uint64_t r = rnd();
        double y = (r & 1) ? 360.0 : dbl(rnd());
        double x;
        switch ((r >> 1) & 7) {
        case 0: x = dbl(rnd()); break;
        case 1: x = (double)(int64_t)rnd() / 4294967296.0; break;
        case 2: x = y * ((double)(rnd() >> 11) / 9007199254740992.0 * 4.0 - 2.0); break;
        case 3: x = (double)(float)((double)(int64_t)rnd() / 1e9); break;
        case 4: x = y * ((double)(rnd() >> 11) / 9007199254740992.0 * 8.0 - 4.0); break;
        default: x = ((double)(rnd() >> 11) / 9007199254740992.0 - 0.5) * 1000.0; break;
        }
        t(x, y);
    }
    printf("%ld cases, %ld mismatches\n", cnt, bad);
    return bad != 0;
}
