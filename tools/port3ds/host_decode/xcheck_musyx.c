/* Cross-check helper: runs paper-cut's MusyX converters (port/src/common/musyx_swap.c, MIT) on a disc file, so
 * tools/port3ds/test_decode.py can compare their output with the port's.
 *
 *   xcheck_musyx proj|pool|slib|stm IN OUT   converted bytes, as the game would read the file
 *   xcheck_musyx sdir IN OUT                 stdout: one line per SDIR entry (paper-cut's native entries);
 *                                            OUT: its copy of the file with the ADPCM records converted
 *
 * Built by test_decode.py with paper-cut's own source; it cannot be linked with the port's converters (same
 * function names). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musyx/synthdata.h>
#include "port/musyx_swap_pc.h"

static uint8_t* slurp(const char* path, uint32_t* n, uint32_t pad, uint32_t max) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t want = (uint32_t)size;
    if (max && want > max) {
        want = max;
    }
    uint32_t len = pad ? (want + pad - 1) / pad * pad : want;
    uint8_t* buf = calloc(1, (size_t)len + 64);
    if (fread(buf, 1, want, f) != want) {
        exit(2);
    }
    fclose(f);
    *n = len;
    return buf;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: xcheck_musyx proj|pool|sdir|slib|stm IN OUT\n");
        return 2;
    }
    const char* kind = argv[1];
    uint32_t n;
    FILE* out = fopen(argv[3], "wb");
    if (!strcmp(kind, "sdir")) {
        uint8_t* in = slurp(argv[2], &n, 32, 0);
        uint32_t size = portMusyxSdirNativeSize(in, n);
        uint8_t* dst = calloc(1, size + 64);
        portMusyxSdirConvert(dst, in, n);
        uint32_t table = size - n;
        SDIR_DATA* e = (SDIR_DATA*)dst;
        for (uint32_t i = 0; e[i].id != 0xFFFF; i++) {
            printf("%u %u %u %u %u %u %u %u %u\n", e[i].id, e[i].ref_cnt, e[i].offset, (unsigned)(uintptr_t)e[i].addr,
                   e[i].header.info, e[i].header.length, e[i].header.loopOffset, e[i].header.loopLength,
                   e[i].extraData ? e[i].extraData - table : 0);
        }
        fwrite(dst + table, 1, n, out);
    } else {
        uint8_t* buf = slurp(argv[2], &n, !strcmp(kind, "stm") ? 0 : 32, !strcmp(kind, "stm") ? 0x100 : 0);
        if (!strcmp(kind, "proj")) {
            portMusyxSwapProj(buf, n);
        } else if (!strcmp(kind, "pool")) {
            portMusyxSwapPool(buf, n);
        } else if (!strcmp(kind, "slib")) {
            portMusyxSwapSlib(buf, n);
        } else if (!strcmp(kind, "stm")) {
            portMusyxSwapStreamHeader(buf);
        } else {
            return 2;
        }
        fwrite(buf, 1, n, out);
    }
    fclose(out);
    return 0;
}
