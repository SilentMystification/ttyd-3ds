/* fmod with exact fast paths, installed by the linker (--wrap=fmod).
 *
 * Game code (reviseAngle: fmod(angle, 360.0)) almost always passes an angle
 * that is already near the range, and the library fmod is a bit loop. For
 * y > 0 these cases have the same result as fmod, bit for bit:
 *   0 <= x < y        -> x          (also x = -0.0 -> -0.0)
 *   -y < x < 0        -> x          (fmod keeps the sign of x)
 *   y <= x < 2y       -> x - y      (exact, Sterbenz: y/2 <= x <= 2y; x == y gives +0)
 * Everything else, NaN, infinity and y <= 0 included, goes to the library.
 * tools/port3ds/fmod_test.c checks this file against the host fmod.
 */
double __real_fmod(double x, double y);

double __wrap_fmod(double x, double y) {
    if (y > 0.0) {
        if (x >= 0.0) {
            if (x < y) {
                return x;
            }
            if (x < 2.0 * y) {
                return x - y;
            }
        } else if (x > -y) {
            return x;
        }
    }
    return __real_fmod(x, y);
}
