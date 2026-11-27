/* Host test of the PICA register-file model (port3ds/plat/pica_model.c) and the list end (pica_enc_list_end), the two
 * pieces of switch gxchunk that need no console.
 *
 * Without arguments: unit cases. With gpudump/gpuhang command lists (.bin, raw words): each list goes through the
 * model (no bad command), and again with every command written twice (a redundant write, as a batch chunk makes
 * them): at every draw both models must be equal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../port3ds/plat/pica_enc.h"
#include "../../../port3ds/plat/pica_model.h"

static int s_fail;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
            s_fail++;                         \
        }                                     \
    } while (0)

#define HDR(inc, mask, reg, extra) ((uint32_t)(inc) << 31 | (uint32_t)(extra) << 20 | (uint32_t)(mask) << 16 | (reg))

static void test_list_end(void) {
    uint32_t w[16];
    for (uint32_t n = 0; n < 16; n += 2) {
        for (int fb = 0; fb < 2; fb++) {
            uint32_t k = pica_enc_list_end(w, n, fb);
            CHECK(k <= PICA_LIST_END_MAX, "end of %u words: %u words", n, k);
            CHECK(((n + k) & 3u) == 0, "end of %u words (fb %d): list of %u words", n, fb, n + k);
            uint32_t at = 0;
            if (fb) {
                CHECK(w[0] == 1 && w[1] == 0x000F0111u && w[2] == 1 && w[3] == 0x000F0110u && w[4] == 1 &&
                          w[5] == 0x000F0063u,
                      "framebuffer flush words");
                at = 6;
            }
            CHECK(w[at] == 0x12345678u && w[at + 1] == 0x000F0010u, "FINALIZE");
            /* GPUCMD_Split: a second FINALIZE only when the offset after the first is not a multiple of 4 */
            uint32_t want = at + 2 + (((n + at + 2) & 3u) ? 2u : 0u);
            CHECK(k == want, "end of %u words (fb %d): %u words, GPUCMD_Split %u", n, fb, k, want);
        }
    }
}

static void test_model(void) {
    static PicaModel a, b;
    PicaModelDiff d;
    pica_model_reset(&a);
    pica_model_reset(&b);
    /* uniform rows 5 and 6 in f32 mode: config, then 8 data words through one non-incremental run */
    uint32_t u[] = {0x80000005u, HDR(0, 0xF, 0x2C0, 0), 1, HDR(0, 0xF, 0x2C1, 7), 2, 3, 4, 5, 6, 7, 8, 0};
    pica_model_feed(&a, u, sizeof(u) / 4);
    CHECK(a.unif_known[5] && a.unif_known[6] && !a.unif_known[7], "rows 5, 6 known");
    CHECK(a.unif[5][0] == 1 && a.unif[5][3] == 4 && a.unif[6][3] == 8, "row values");
    pica_model_feed(&b, u, sizeof(u) / 4);
    pica_model_cmp(&a, &b, &d);
    CHECK(d.differ == 0 && d.one_sided == 0, "same rows: %u differ", d.differ);
    uint32_t u2[] = {0x80000006u, HDR(0, 0xF, 0x2C0, 0), 9, HDR(0, 0xF, 0x2C1, 3), 10, 11, 12, 0};
    pica_model_feed(&b, u2, sizeof(u2) / 4);
    pica_model_cmp(&a, &b, &d);
    CHECK(d.differ == 1 && d.first_reg == 0x1006u, "row 6 differs: %u, first %04x", d.differ, d.first_reg);
    /* byte masks, incremental runs, a register known to one model only */
    pica_model_reset(&a);
    pica_model_reset(&b);
    uint32_t r1[] = {0x11223344u, HDR(0, 0x3, 0x041, 0)};
    pica_model_feed(&a, r1, 2);
    CHECK(a.known[0x41] == 3 && a.reg[0x41] == 0x3344u, "masked write");
    uint32_t r2[] = {7, HDR(1, 0xF, 0x04D, 1), 8, 0};
    pica_model_feed(&a, r2, 4);
    CHECK(a.reg[0x4D] == 7 && a.reg[0x4E] == 8, "incremental run");
    pica_model_feed(&b, r2, 4);
    pica_model_cmp(&a, &b, &d);
    CHECK(d.differ == 0 && d.one_sided == 1, "one-sided register: %u differ, %u one-sided", d.differ, d.one_sided);
    /* the texture-cache clear bit is an event; draws and FINALIZE are counted, not compared */
    uint32_t t1[] = {0x00011007u, HDR(0, 0xF, 0x080, 0), 1, HDR(0, 0xF, 0x22F, 0), 0x12345678u, HDR(0, 0xF, 0x010, 0)};
    uint32_t t2[] = {0x00001007u, HDR(0, 0xF, 0x080, 0)};
    pica_model_reset(&a);
    pica_model_reset(&b);
    pica_model_feed(&a, t1, 6);
    pica_model_feed(&b, t2, 2);
    pica_model_cmp(&a, &b, &d);
    CHECK(d.differ == 0 && a.texclears == 1 && a.draws == 1 && a.finals == 1, "events");
    /* fixed attribute and fog LUT */
    uint32_t f1[] = {3, HDR(1, 0xF, 0x232, 3), 0xA, 0xB, 0xC, 0, 0, HDR(0, 0xF, 0x0E6, 0), 5, HDR(0, 0xF, 0x0E8, 1), 6, 0};
    pica_model_reset(&a);
    pica_model_feed(&a, f1, sizeof(f1) / 4);
    CHECK(a.fixed_known[3] && a.fixed[3][0] == 0xA && a.fixed[3][2] == 0xC, "fixed attribute");
    CHECK(a.fog_known[0] && a.fog_known[1] && a.fog[0] == 5 && a.fog[1] == 6 && !a.fog_known[2], "fog LUT");
    /* a header past the end */
    pica_model_reset(&a);
    uint32_t bad[] = {1, HDR(0, 0xF, 0x2C1, 5), 2};
    pica_model_feed(&a, bad, 3);
    CHECK(a.bad == 1, "short command");
    /* feed_draw stops after the draw trigger (t1: texture config, then the draw, then FINALIZE) */
    pica_model_reset(&a);
    CHECK(pica_model_feed_draw(&a, t1, 6) == 4 && a.draws == 1 && a.finals == 0, "feed_draw stops at the draw");
    CHECK(pica_model_feed_draw(&a, t1 + 4, 2) == 2 && a.finals == 1, "feed_draw: the rest");
    pica_model_reset(&b);
    pica_model_feed(&b, t2, 2);
    CHECK(pica_model_digest(&a) == pica_model_digest(&b), "digest: events and the clear bit are not state");
    uint32_t t3[] = {0x00001006u, HDR(0, 0xF, 0x080, 0)};
    pica_model_feed(&b, t3, 2);
    CHECK(pica_model_digest(&a) != pica_model_digest(&b), "digest: a state change");
}

/* the list through two models, B with every command written twice (the copy first), compared at every draw */
static void test_list(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("FAIL: cannot open %s\n", path);
        s_fail++;
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint32_t* w = malloc((size_t)sz + 16);
    uint32_t n = (uint32_t)(fread(w, 1, (size_t)sz, fp) / 4);
    fclose(fp);
    static PicaModel a, b, c;
    pica_model_reset(&a);
    pica_model_reset(&b);
    pica_model_reset(&c);
    uint32_t i = 0, draws = 0, cmps = 0, differ = 0, cdiffer = 0, cmds = 0, ddiffer = 0, dseen = 0;
    while (i + 1 < n) {
        uint32_t hdr = w[i + 1], extra = hdr >> 20 & 0xFFu, reg = hdr & 0xFFFFu;
        uint32_t len = 2 + extra;
        len += len & 1u;
        if (i + len > n) {
            break;
        }
        int trig = reg == 0x22E || reg == 0x22F || (extra > 0 && (hdr >> 31) && reg <= 0x22F && reg + extra >= 0x22E);
        int port = (reg >= 0x2C1 && reg <= 0x2C8) || reg == 0x2C0 || (reg >= 0x0E6 && reg <= 0x0EF) ||
                   (reg >= 0x232 && reg <= 0x235) || (reg >= 0x1C5 && reg <= 0x1CF);
        if (!trig && !port && reg != 0x010) {
            pica_model_feed(&b, w + i, len); /* a redundant write of the same values */
        }
        pica_model_feed(&a, w + i, len);
        pica_model_feed(&b, w + i, len);
        /* C: every face-culling value changed (the model must see it) */
        uint32_t cw[512];
        if (len <= 512) {
            memcpy(cw, w + i, len * 4);
            if (reg == 0x040 && extra == 0) {
                cw[0] ^= 1u;
            }
            pica_model_feed(&c, cw, len);
        }
        cmds++;
        if (trig) {
            PicaModelDiff d;
            pica_model_cmp(&a, &b, &d);
            cmps++;
            differ += d.differ != 0;
            pica_model_cmp(&a, &c, &d);
            cdiffer += d.differ != 0 && d.first_reg == 0x040;
            /* gxfldig: equal states give equal digests; a changed cull value changes it */
            ddiffer += pica_model_digest(&a) != pica_model_digest(&b);
            dseen += d.differ != 0 && pica_model_digest(&a) != pica_model_digest(&c);
        }
        draws = a.draws;
        i += len;
    }
    CHECK(a.bad == 0 && b.bad == 0, "%s: %u bad commands", path, a.bad + b.bad);
    CHECK(differ == 0, "%s: %u of %u draws differ", path, differ, cmps);
    CHECK(cdiffer > 0, "%s: a changed face-culling value was not seen", path);
    CHECK(ddiffer == 0 && dseen == cdiffer, "%s: digests: %u equal states differ, %u of %u changed states seen", path,
          ddiffer, dseen, cdiffer);
    /* pica_model_feed_draw: the same list in runs that end at each draw gives the same draws and state */
    static PicaModel e;
    pica_model_reset(&e);
    uint32_t left = i, runs = 0;
    const uint32_t* q = w;
    while (left != 0) {
        uint32_t k = pica_model_feed_draw(&e, q, left);
        q += k;
        left -= k;
        runs++;
    }
    CHECK(e.draws == a.draws && runs >= e.draws && pica_model_digest(&e) == pica_model_digest(&a),
          "%s: feed_draw: %u draws in %u runs, %u fed whole", path, e.draws, runs, a.draws);
    printf("%s: %u words, %u commands, %u draws, %u compared, %u differ (with every cull value changed: %u), "
           "%u texture-cache clears, %u LUT words\n",
           path, n, cmds, draws, cmps, differ, cdiffer, a.texclears, a.luts);
    free(w);
}

int main(int argc, char** argv) {
    test_list_end();
    test_model();
    for (int i = 1; i < argc; i++) {
        test_list(argv[i]);
    }
    printf("%s (%d failures)\n", s_fail ? "FAILED" : "ok", s_fail);
    return s_fail != 0;
}
