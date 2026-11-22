/* Host test tool: decodes a THP file with the port's decoder
 * (port3ds/third_party/aurora_thp) and writes the planar YUV 4:2:0 frames and
 * the stereo s16 audio (interleaved the way the SDK's flag 0 writes it) as raw
 * files. compare.py checks them against ffmpeg.
 *
 *   thp_dump IN.thp OUT_PREFIX [max_frames] [pal] [sum]
 *   -> OUT_PREFIX.yuv, OUT_PREFIX.pcm, and a line per frame with the decode time. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "thp_decode.h"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t fnv(const uint8_t* p, size_t n, uint32_t h) {
    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

static void detile(const uint8_t* t, unsigned w, unsigned h, uint8_t* out) {
    unsigned tpr = (w + 7) / 8;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            out[y * w + x] = t[((size_t)(y / 4) * tpr + x / 8) * 32 + (y & 3) * 8 + (x & 7)];
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: thp_dump IN.thp OUT_PREFIX [max_frames] [pal] [sum]\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(sz + 64);
    memset(d + sz, 0, 64);
    if (fread(d, 1, sz, f) != (size_t)sz) {
        return 1;
    }
    fclose(f);
    unsigned maxf = argc > 3 ? (unsigned)atoi(argv[3]) : ~0u;
    int pal = argc > 4 && !strcmp(argv[4], "pal");
    int hash = argc > 4 && !strcmp(argv[argc - 1], "sum"); /* also print a hash of each frame's tiled planes */

    uint32_t nframes = be32(d + 0x14), comp_off = be32(d + 0x20), first = be32(d + 0x28);
    uint32_t ncomp = be32(d + comp_off);
    unsigned w = 0, h = 0, has_audio = 0;
    uint32_t off = comp_off + 4 + 16;
    for (uint32_t i = 0; i < ncomp; i++) {
        uint8_t type = d[comp_off + 4 + i];
        if (type == 0) {
            w = be32(d + off);
            h = be32(d + off + 4);
            off += 12;
        } else {
            has_audio = 1;
            off += 16;
        }
    }
    unsigned cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t ysz = (w + 7) / 8 * ((h + 3) / 4) * 32, csz = (cw + 7) / 8 * ((ch + 3) / 4) * 32;
    uint8_t* ty = calloc(1, ysz + 64);
    uint8_t* tu = calloc(1, csz + 64);
    uint8_t* tv = calloc(1, csz + 64);
    uint8_t* py = malloc(w * h);
    uint8_t* pu = malloc(cw * ch);
    uint8_t* pv = malloc(cw * ch);
    int16_t* pcm = malloc(65536 * 4);

    char name[512];
    snprintf(name, sizeof name, "%s.yuv", argv[2]);
    FILE* fy = fopen(name, "wb");
    snprintf(name, sizeof name, "%s.pcm", argv[2]);
    FILE* fa = fopen(name, "wb");

    uint32_t pos = first, cur = be32(d + 0x18);
    double total = 0, worst = 0;
    unsigned done = 0;
    for (uint32_t fr = 0; fr < nframes && fr < maxf; fr++) {
        const uint8_t* p = d + pos;
        uint32_t next = be32(p);
        const uint8_t* data = p + 8 + ncomp * 4;
        for (uint32_t i = 0; i < ncomp; i++) {
            uint32_t csize = be32(p + 8 + i * 4);
            if (d[comp_off + 4 + i] == 0) {
                struct timespec a, b;
                clock_gettime(CLOCK_MONOTONIC, &a);
                int r = thp_video_decode(data, ty, tu, tv);
                clock_gettime(CLOCK_MONOTONIC, &b);
                double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
                total += ms;
                if (ms > worst) {
                    worst = ms;
                }
                if (r) {
                    fprintf(stderr, "frame %u: decode error %d\n", fr, r);
                    return 1;
                }
                if (hash) {
                    printf("frame %u tile planes FNV-1a %08x\n", fr,
                           fnv(tv, csz, fnv(tu, csz, fnv(ty, ysz, 2166136261u))));
                }
                detile(ty, w, h, py);
                detile(tu, cw, ch, pu);
                detile(tv, cw, ch, pv);
                fwrite(py, 1, w * h, fy);
                fwrite(pu, 1, cw * ch, fy);
                fwrite(pv, 1, cw * ch, fy);
            } else if (has_audio) {
                uint32_t n = thp_audio_decode(pcm, data, 0, pal);
                fwrite(pcm, 4, n, fa);
            }
            data += csize;
        }
        pos += cur;
        cur = next;
        done++;
    }
    fprintf(stderr, "%ux%u, %u frames: decode %.3f ms/frame avg, %.3f ms worst\n", w, h, done, total / done, worst);
    fclose(fy);
    fclose(fa);
    return 0;
}
