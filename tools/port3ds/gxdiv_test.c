/* Host test: port3ds/game/gx/gx_div.h against plain division.
 * gcc -O2 -Iport3ds/game/gx tools/port3ds/gxdiv_test.c -o gxdiv_test && ./gxdiv_test */
#include <stdio.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
#include "gx_div.h"

int main(void) {
    long bad = 0, n_alpha = 0, n_cols = 0;
    for (u32 sw = 1; sw <= 1020; sw++) {
        for (u32 n = 0; n < (1u << 18); n++) {
            if (port_div_alpha(n, sw) != n / sw) {
                if (bad++ < 10) printf("alpha %u / %u\n", n, sw);
            }
            n_alpha++;
        }
    }
    static u16 cols[GX_COLS_MAX];
    for (u32 w = 1; w <= 2048; w++) {
        for (u32 pw = 1; pw <= GX_COLS_MAX; pw++) {
            if (!gx_build_cols(cols, w, pw)) { bad++; continue; }
            for (u32 i = 0; i < pw; i++) {
                if (cols[i] != i * w / pw) { if (bad++ < 10) printf("col %u %u %u\n", w, pw, i); }
                n_cols++;
            }
        }
    }
    printf("%ld alpha pairs, %ld column entries, %ld mismatches\n", n_alpha, n_cols, bad);
    return bad != 0;
}
