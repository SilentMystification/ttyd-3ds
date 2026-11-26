/* Host test of the port's PICA encoders (port3ds/plat/pica_enc.c, switch gxone) against command lists citro3d wrote.
 *
 * Input: one or more raw command lists (gpudump / gpuhang .bin files: the words citro3d and GPUCMD wrote for a
 * record). The test splits each list into commands, finds every texture-environment, texture and pixel block in
 * the shape C3Di_UpdateContext writes them, takes their register values back as encoder inputs, encodes them
 * again and compares word for word, padding words included. The fixed values (PicaEncFixed) are taken from the
 * first block of each class and must be the same in every other block.
 *
 * Also checks pica_f24/pica_f31 on the viewport words: the integer width and height are found from the words and
 * every word of the group must come out equal.
 *
 * Build: tools/port3ds/pica_enc_test/build.sh; run: build/pica_enc_test/pica_enc_test LIST.bin [...]
 * Exit status 0 when every block found is equal and at least one block of each class was found. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../port3ds/plat/pica_enc.h"

typedef struct {
    uint32_t at;  /* word offset of the command */
    uint32_t len; /* words, padding included */
    uint32_t reg, mask, inc, n;
    const uint32_t* p; /* p[0] is the first parameter; the rest follow the header */
    uint32_t pv[256];
} Cmd;

static uint32_t* s_w;
static uint32_t s_nw;
static Cmd* s_c;
static uint32_t s_nc;

static void split(void) {
    s_nc = 0;
    s_c = calloc(s_nw / 2 + 1, sizeof(Cmd));
    uint32_t i = 0;
    while (i + 1 < s_nw) {
        Cmd* c = &s_c[s_nc];
        uint32_t h = s_w[i + 1];
        c->at = i;
        c->reg = h & 0x3FF;
        c->mask = h >> 16 & 0xF;
        c->inc = h >> 31;
        c->n = (h >> 20 & 0xFF) + 1;
        if (i + 1 + c->n > s_nw) {
            break;
        }
        c->pv[0] = s_w[i];
        for (uint32_t k = 1; k < c->n; k++) {
            c->pv[k] = s_w[i + 1 + k];
        }
        uint32_t len = c->n + 1;
        len += len & 1;
        c->len = len;
        i += len;
        s_nc++;
    }
}

static int is(const Cmd* c, uint32_t reg, uint32_t mask, uint32_t inc, uint32_t n) {
    return c->reg == reg && c->mask == mask && c->inc == inc && c->n == n;
}

static int s_have[3];
static PicaEncFixed s_fx[3];
static uint32_t s_found[3], s_bad[3], s_fixbad[3], s_logged;

static int cmp_words(int cls, const char* what, uint32_t at, const uint32_t* enc, uint32_t n) {
    if (at + n > s_nw || memcmp(s_w + at, enc, n * 4) != 0) {
        s_bad[cls]++;
        if (s_logged++ < 12) {
            for (uint32_t k = 0; k < n && at + k < s_nw; k++) {
                if (s_w[at + k] != enc[k]) {
                    printf("  %s block at word %u: word %u differs: list %08x, encoder %08x\n", what, at, k, s_w[at + k],
                           enc[k]);
                    break;
                }
            }
        }
        return 0;
    }
    return 1;
}

static void fix_check(int cls, const PicaEncFixed* f) {
    if (!s_have[cls]) {
        s_fx[cls] = *f;
        s_have[cls] = 1;
    } else if (memcmp(&s_fx[cls], f, sizeof(*f)) != 0) {
        s_fixbad[cls]++;
    }
}

/* texture environment: 0xE0 (mask 7), 0xFD, 0xE1, six stage runs */
static void try_tev(uint32_t i) {
    static const uint32_t k_reg[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};
    if (i + 9 > s_nc || !is(&s_c[i], 0xE0, 7, 0, 1) || !is(&s_c[i + 1], 0xFD, 0xF, 0, 1) ||
        !is(&s_c[i + 2], 0xE1, 0xF, 0, 1)) {
        return;
    }
    PicaTevStage st[6];
    for (int s = 0; s < 6; s++) {
        const Cmd* c = &s_c[i + 3 + s];
        if (!is(c, k_reg[s], 0xF, 1, 5)) {
            return;
        }
        memcpy(st[s].w, c->pv, 20);
    }
    PicaEncFixed f;
    memset(&f, 0, sizeof(f));
    f.texenv_buf_hi = s_c[i].pv[0] & 0xFFFE0000u;
    fix_check(0, &f);
    uint32_t enc[64];
    uint32_t n = pica_enc_tev(enc, st, s_c[i].pv[0] & 0x1FFFFu, s_c[i + 1].pv[0], s_c[i + 2].pv[0], &s_fx[0]);
    s_found[0]++;
    cmp_words(0, "tev", s_c[i].at, enc, n);
}

/* texture units: 0x81 run of 5, 0x8E, 0x91 run, 0x96, 0x99 run, 0x9E, 0x80 (mask B), 0x80 (mask 4), 0x8B */
static void try_tex(uint32_t i) {
    static const uint32_t k_reg[3][2] = {{0x81, 0x8E}, {0x91, 0x96}, {0x99, 0x9E}};
    if (i + 9 > s_nc) {
        return;
    }
    PicaTexUnit u[3];
    for (int k = 0; k < 3; k++) {
        const Cmd* a = &s_c[i + 2 * k];
        const Cmd* b = &s_c[i + 2 * k + 1];
        if (!is(a, k_reg[k][0], 0xF, 1, 5) || !is(b, k_reg[k][1], 0xF, 0, 1)) {
            return;
        }
        u[k] = (PicaTexUnit){a->pv[0], a->pv[1], a->pv[2], a->pv[3], a->pv[4], b->pv[0]};
    }
    const Cmd* c0 = &s_c[i + 6];
    if (!is(c0, 0x80, 0xB, 0, 1) || !is(&s_c[i + 7], 0x80, 0x4, 0, 1) || !is(&s_c[i + 8], 0x8B, 0xF, 0, 1)) {
        return;
    }
    PicaEncFixed f;
    memset(&f, 0, sizeof(f));
    f.tex_cfg_hi = c0->pv[0] & ~0x10007u;
    f.tex_shadow = s_c[i + 8].pv[0];
    fix_check(1, &f);
    uint32_t enc[64];
    uint32_t n = pica_enc_tex(enc, u, &f);
    s_found[1]++;
    cmp_words(1, "tex", s_c[i].at, enc, n);
}

/* a float whose pica_f24 is `w` (the low mantissa bits it drops are 0) */
static float from_f24(uint32_t w) {
    uint32_t s = w >> 23 & 1, e = w >> 16 & 0x7F, m = w & 0xFFFF;
    if ((w & 0x7FFFFF) == 0) {
        return s ? -0.0f : 0.0f;
    }
    uint32_t b = s << 31 | (e + 64) << 23 | m << 7;
    float f;
    memcpy(&f, &b, 4);
    return f;
}

/* pixel: [0x41 run of 4, 0x68], 0x65 run of 3, then the effect group (0x6D ... 0x6A) */
static void try_pix(uint32_t i) {
    uint32_t j = i;
    PicaPixIn in;
    memset(&in, 0, sizeof(in));
    if (j + 2 <= s_nc && is(&s_c[j], 0x41, 0xF, 1, 4) && is(&s_c[j + 1], 0x68, 0xF, 0, 1)) {
        const uint32_t* v = s_c[j].pv;
        uint32_t vw = 0, vh = 0;
        for (uint32_t x = 1; x <= 1024 && (vw == 0 || vh == 0); x++) {
            if (vw == 0 && pica_f24((float)x * 0.5f) == v[0]) {
                vw = x;
            }
            if (vh == 0 && pica_f24((float)x * 0.5f) == v[2]) {
                vh = x;
            }
        }
        if (vw == 0 || vh == 0) {
            s_found[2]++;
            s_bad[2]++;
            if (s_logged++ < 12) {
                printf("  pix block at word %u: no integer viewport size gives %08x / %08x\n", s_c[j].at, v[0], v[2]);
            }
            return;
        }
        in.viewport = 1;
        in.vw = vw;
        in.vh = vh;
        in.vx = s_c[j + 1].pv[0] & 0xFFFF;
        in.vy = s_c[j + 1].pv[0] >> 16;
        j += 2;
    }
    static const uint32_t k_eff[][3] = {{0x6D, 0xF, 1}, {0x40, 0xF, 1}, {0x4D, 0xF, 2}, {0x104, 0xF, 4},
                                        {0x126, 0x8, 1}, {0x103, 0xF, 1}, {0x101, 0xF, 1}, {0x102, 0xF, 1},
                                        {0x100, 0x7, 1}, {0x130, 0xF, 1}, {0x62, 0x1, 1}, {0x118, 0xF, 1},
                                        {0x61, 0x1, 1}, {0x6A, 0x7, 1}};
    if (j + 15 > s_nc || !is(&s_c[j], 0x65, 0xF, 1, 3)) {
        return;
    }
    const Cmd* sc = &s_c[j];
    for (int k = 0; k < 14; k++) {
        if (!is(&s_c[j + 1 + k], k_eff[k][0], k_eff[k][1], k_eff[k][2] > 1, k_eff[k][2])) {
            return;
        }
    }
    const Cmd* e = &s_c[j + 1];
    if (e[0].pv[0] != 1 || e[1].pv[0] != 0) {
        return; /* not the renderer's shape (depth map on, culling off) */
    }
    in.sc_mode = sc->pv[0];
    in.sc_x0 = sc->pv[1] & 0xFFFF;
    in.sc_y0 = sc->pv[1] >> 16;
    in.sc_x1 = (sc->pv[2] & 0xFFFF) + 1;
    in.sc_y1 = (sc->pv[2] >> 16) + 1;
    in.zscale = from_f24(e[2].pv[0]);
    in.zoffset = from_f24(e[2].pv[1]);
    in.alpha_test = e[3].pv[0];
    in.depth_test = e[3].pv[3];
    in.alpha_blend = e[6].pv[0];
    in.logic_op = e[7].pv[0];
    in.blend_on = (e[8].pv[0] >> 8 & 0xFF) != 0;
    PicaEncFixed f;
    memset(&f, 0, sizeof(f));
    f.stencil_test = e[3].pv[1];
    f.stencil_op = e[3].pv[2];
    f.blend_color = e[5].pv[0];
    f.fragop_lo = e[8].pv[0] & ~0xFF00u;
    f.fragop_shadow = e[9].pv[0];
    f.early_depth = e[10].pv[0];
    f.early_func = e[12].pv[0];
    f.early_ref = e[13].pv[0];
    fix_check(2, &f);
    uint32_t enc[64];
    uint32_t n = pica_enc_pix(enc, &in, &f);
    s_found[2]++;
    cmp_words(2, "pix", s_c[i].at, enc, n);
}

static void self_test(void) {
    /* known values: libctru f32tof24(1.0) = 0x3F0000, f32tof24(0.5) = 0x3E0000, f32tof31(1.0) = 0x1F800000 */
    struct {
        float v;
        uint32_t f24, f31;
    } k[] = {{1.0f, 0x3F0000u, 0x1F800000u}, {0.5f, 0x3E0000u, 0x1F000000u}, {0.0f, 0, 0}, {-2.0f, 0xC00000u, 0x60000000u}};
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (pica_f24(k[i].v) != k[i].f24 || pica_f31(k[i].v) != k[i].f31) {
            printf("self-test: %g gives f24 %08x f31 %08x, expected %08x %08x\n", (double)k[i].v, pica_f24(k[i].v),
                   pica_f31(k[i].v), k[i].f24, k[i].f31);
            exit(1);
        }
    }
}

int main(int argc, char** argv) {
    self_test();
    if (argc < 2) {
        fprintf(stderr, "usage: %s LIST.bin [...]\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        FILE* fp = fopen(argv[a], "rb");
        if (fp == NULL) {
            perror(argv[a]);
            return 2;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        s_w = malloc((size_t)sz + 8);
        s_nw = (uint32_t)(fread(s_w, 1, (size_t)sz, fp) / 4);
        fclose(fp);
        split();
        uint32_t f0[3] = {s_found[0], s_found[1], s_found[2]};
        for (uint32_t i = 0; i < s_nc; i++) {
            try_tev(i);
            try_tex(i);
            try_pix(i);
        }
        printf("%s: %u words, %u commands; blocks tev %u tex %u pix %u\n", argv[a], s_nw, s_nc, s_found[0] - f0[0],
               s_found[1] - f0[1], s_found[2] - f0[2]);
        free(s_c);
        free(s_w);
    }
    static const char* const k_name[3] = {"tev", "tex", "pix"};
    int ok = 1;
    for (int c = 0; c < 3; c++) {
        printf("%s: %u blocks, %u differ, %u with other fixed values\n", k_name[c], s_found[c], s_bad[c], s_fixbad[c]);
        ok &= s_found[c] > 0 && s_bad[c] == 0 && s_fixbad[c] == 0;
    }
    if (s_have[2]) {
        printf("fixed (pix): stencil %08x %08x blend colour %08x fragop %08x shadow %08x early %u %u %u\n",
               s_fx[2].stencil_test, s_fx[2].stencil_op, s_fx[2].blend_color, s_fx[2].fragop_lo, s_fx[2].fragop_shadow,
               s_fx[2].early_depth, s_fx[2].early_func, s_fx[2].early_ref);
    }
    if (s_have[0]) {
        printf("fixed (tev): texenv buffer high bits %08x\n", s_fx[0].texenv_buf_hi);
    }
    if (s_have[1]) {
        printf("fixed (tex): config high bits %08x shadow %08x\n", s_fx[1].tex_cfg_hi, s_fx[1].tex_shadow);
    }
    printf("%s\n", ok ? "pica_enc_test: OK" : "pica_enc_test: FAILED");
    return ok ? 0 : 1;
}
