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
    uint32_t i = 0, draws = 0, cmps = 0, differ = 0, cdiffer = 0, cmds = 0;
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
        }
        draws = a.draws;
        i += len;
    }
    CHECK(a.bad == 0 && b.bad == 0, "%s: %u bad commands", path, a.bad + b.bad);
    CHECK(differ == 0, "%s: %u of %u draws differ", path, differ, cmps);
    CHECK(cdiffer > 0, "%s: a changed face-culling value was not seen", path);
    printf("%s: %u words, %u commands, %u draws, %u compared, %u differ (with every cull value changed: %u), "
           "%u texture-cache clears, %u LUT words\n",
           path, n, cmds, draws, cmps, differ, cdiffer, a.texclears, a.luts);
    free(w);
}

/* ---- gxcall: command-buffer calls (pica_enc_call / pica_enc_ret, as plat/gpu.c uses them) ----
 * A frame list in which some segments are called chunks must reach the command processor as the same commands as the
 * list with those segments copied. cp_exec is the command processor as the gpuprobe cases cb_call* describe it: it
 * fetches SIZEn * 8 bytes at physical ADDRn * 8 when CMDBUF_JUMPn is written; no return stack. It also checks the
 * port's own rules: every jump write ends a 16-byte block, every fetch is 16-byte aligned with a size a multiple of
 * 16. It returns the commands it executed less CMDBUF writes and byte-mask-0 pads. */
#define FL_PHYS 0x18200000u
#define CK_PHYS 0x18600000u
#define CP_WORDS (1u << 18)
static uint32_t s_fl[CP_WORDS], s_ckm[CP_WORDS], s_out_a[CP_WORDS], s_out_b[CP_WORDS];
static uint32_t s_cp_cases, s_cp_calls, s_last_na, s_last_nb, s_cp_finals;

static const uint32_t* cp_at(uint32_t phys, uint32_t nw) {
    if (phys >= FL_PHYS && phys + nw * 4 <= FL_PHYS + CP_WORDS * 4) {
        return s_fl + (phys - FL_PHYS) / 4;
    }
    if (phys >= CK_PHYS && phys + nw * 4 <= CK_PHYS + CP_WORDS * 4) {
        return s_ckm + (phys - CK_PHYS) / 4;
    }
    return NULL;
}

/* 0: done; < 0: a rule broken. *jumps: the jumps taken. */
static int cp_exec(uint32_t phys, uint32_t bytes, uint32_t* out, uint32_t* nout, uint32_t* jumps) {
    uint32_t size[2] = {0, 0}, addr[2] = {0, 0};
    *nout = 0;
    *jumps = 0;
    for (;;) {
        const uint32_t* w = cp_at(phys, bytes / 4);
        if (w == NULL || (phys & 15u) || (bytes & 15u)) {
            return -1;
        }
        uint32_t n = bytes / 4, i = 0;
        int jumped = 0;
        while (i + 2 <= n) {
            uint32_t h = w[i + 1], e = (h >> 20) & 0xFFu, reg = h & 0xFFFFu, mask = (h >> 16) & 0xFu;
            uint32_t len = 2 + e + (e & 1u);
            if (i + len > n) {
                return -2;
            }
            if (e == 0 && mask == 0xF && reg >= 0x238 && reg <= 0x23B) {
                uint32_t k = reg - 0x238;
                if (k < 2) {
                    size[k] = w[i];
                } else {
                    addr[k - 2] = w[i];
                }
            } else if (e == 0 && mask == 0xF && (reg == 0x23C || reg == 0x23D)) {
                if (((phys / 4 + i + 2) & 3u) != 0) {
                    return -3; /* not the last command of a 16-byte block */
                }
                uint32_t ch = reg - 0x23C;
                phys = addr[ch] << 3;
                bytes = size[ch] << 3;
                jumped = 1;
                if (++*jumps > 100000) {
                    return -4;
                }
                break;
            } else if (e == 0 && reg == 0x010) {
                s_cp_finals++; /* FINALIZE: once or twice, as the list's length needs (GPUCMD_Split) */
            } else if (!(e == 0 && mask == 0 && reg == 0x040)) {
                if (*nout + len > CP_WORDS) {
                    return -5;
                }
                memcpy(out + *nout, w + i, len * 4);
                *nout += len;
            }
            i += len;
        }
        if (!jumped) {
            return 0;
        }
    }
}

typedef struct {
    const uint32_t* w;
    uint32_t n;
    int call;
} CpSeg;

/* Builds the list both ways into s_fl (copy at 0, call at half) and the chunks into s_ckm, runs both, compares. */
static void cp_case(const char* what, const CpSeg* sg, int ns, uint32_t lead) {
    uint32_t half = CP_WORDS / 2, cu = 0, sz0[4096], ret[4096], nc = 0;
    uint32_t* a = s_fl;
    uint32_t* b = s_fl + half;
    uint32_t na = 0, nb = 0;
    for (uint32_t i = 0; i < lead; i += 2) { /* a state write, to move the calls' alignment */
        a[na++] = b[nb++] = 0x1234u + i;
        a[na++] = b[nb++] = HDR(0, 0xF, 0x041, 0);
    }
    for (int s = 0; s < ns; s++) {
        if (na + sg[s].n + 64 > half || cu + sg[s].n + 16 > CP_WORDS || nc >= 4096) {
            CHECK(0, "%s: buffers full", what);
            return;
        }
        memcpy(a + na, sg[s].w, sg[s].n * 4);
        na += sg[s].n;
        if (!sg[s].call) {
            memcpy(b + nb, sg[s].w, sg[s].n * 4);
            nb += sg[s].n;
            continue;
        }
        /* the chunk as plat_gpu_chunk_build stores it: the words, a byte-mask-0 pad to 16 bytes, the return */
        uint32_t c0 = cu;
        memcpy(s_ckm + cu, sg[s].w, sg[s].n * 4);
        cu += sg[s].n;
        if (cu & 3u) {
            s_ckm[cu++] = 0;
            s_ckm[cu++] = HDR(0, 0, 0x040, 0);
        }
        cu += pica_enc_ret(s_ckm + cu);
        uint32_t k;
        uint32_t len = pica_enc_call(b + nb, FL_PHYS + (half + nb) * 4, CK_PHYS + c0 * 4, (cu - c0) * 4, &k);
        PicaCall pc;
        CHECK(pica_call_parse(b + nb, len, &pc) == len && pc.set == 0xFu, "%s: call %u does not parse", what, nc);
        CHECK(pica_ret_check(s_ckm + c0 + sg[s].n, cu - c0 - sg[s].n), "%s: return %u", what, nc);
        CHECK(len <= PICA_CALL_WORDS_MAX, "%s: call of %u words", what, len);
        sz0[nc] = nb + k;
        nb += len;
        ret[nc++] = nb;
    }
    na += pica_enc_list_end(a + na, na, 1);
    nb += pica_enc_list_end(b + nb, nb, 1);
    for (uint32_t i = 0; i < nc; i++) {
        pica_enc_call_patch(b + sz0[i], (nb - ret[i]) * 4);
    }
    s_last_na = na;
    s_last_nb = nb;
    uint32_t oa, ob, ja, jb;
    s_cp_finals = 0;
    int ra = cp_exec(FL_PHYS, na * 4, s_out_a, &oa, &ja);
    uint32_t fa = s_cp_finals;
    s_cp_finals = 0;
    int rb = cp_exec(FL_PHYS + half * 4, nb * 4, s_out_b, &ob, &jb);
    CHECK(fa >= 1 && s_cp_finals >= 1, "%s: no FINALIZE", what);
    CHECK(ra == 0 && rb == 0, "%s: command processor %d (copy), %d (call)", what, ra, rb);
    CHECK(jb == 2 * nc && ja == 0, "%s: %u jumps for %u calls", what, jb, nc);
    CHECK(oa == ob && memcmp(s_out_a, s_out_b, oa * 4) == 0, "%s: %u and %u words differ", what, oa, ob);
    static PicaModel ma, mb;
    PicaModelDiff d;
    pica_model_reset(&ma);
    pica_model_reset(&mb);
    pica_model_feed(&ma, s_out_a, oa);
    pica_model_feed(&mb, s_out_b, ob);
    pica_model_cmp(&ma, &mb, &d);
    CHECK(d.differ == 0 && d.one_sided == 0 && ma.draws == mb.draws && ma.bad == 0, "%s: models differ", what);
    s_cp_cases++;
    s_cp_calls += nc;
}

static uint32_t s_rng = 12345u;
static uint32_t rnd(uint32_t n) {
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) % n;
}

/* n words of random commands (single writes with any byte mask, bursts, incremental runs), no CMDBUF register */
static uint32_t gen_cmds(uint32_t* w, uint32_t want) {
    static const uint32_t base[] = {0x041, 0x060, 0x0C0, 0x0E0, 0x100, 0x200, 0x22E};
    uint32_t n = 0;
    while (n + 12 <= want) {
        uint32_t reg = base[rnd(7)], e = rnd(4), inc = rnd(2), mask = 1 + rnd(15);
        if (reg != 0x22E) {
            reg += rnd(16);
        } else {
            e = 0; /* a draw trigger */
        }
        w[n] = rnd(0xFFFFFFFu);
        w[n + 1] = HDR(inc, mask, reg, e);
        for (uint32_t i = 0; i < e; i++) {
            w[n + 2 + i] = rnd(0xFFFFFFFu);
        }
        n += 2 + e;
        if (e & 1u) {
            w[n++] = 0;
        }
    }
    return n;
}

static void test_calls(void) {
    /* the encoders alone */
    uint32_t w[16], k;
    for (uint32_t at = 0; at < 4; at += 2) {
        uint32_t len = pica_enc_call(w, FL_PHYS + at * 4, CK_PHYS + 0x40, 0x60, &k);
        PicaCall pc;
        CHECK(len == (at == 0 ? 12u : 10u), "call at word %u: %u words", at, len);
        CHECK(((at + len) & 3u) == 0, "call at word %u ends a 16-byte block", at);
        CHECK(pica_call_parse(w, len, &pc) == len && pc.set == 0xFu && pc.size[1] == 0x60 >> 3 &&
                  pc.addr[1] == (CK_PHYS + 0x40) >> 3 && pc.addr[0] == (FL_PHYS + (at + len) * 4) >> 3 &&
                  pc.size[0] == 0,
              "call at word %u parses", at);
        CHECK(w[len - 1] == HDR(0, 0xF, 0x23D, 0) && w[len - 2] == 1, "JUMP1 last");
        CHECK(pica_call_parse(w, len - 2, &pc) == 0, "a call without JUMP1");
    }
    CHECK(pica_enc_ret(w) == PICA_RET_WORDS && pica_ret_check(w, PICA_RET_WORDS), "return");
    CHECK(w[3] == HDR(0, 0xF, 0x23C, 0) && w[1] == HDR(0, 0, 0x040, 0), "return words");
    w[3] = HDR(0, 0xF, 0x23D, 0);
    CHECK(!pica_ret_check(w, PICA_RET_WORDS), "JUMP1 is not a return");
    uint32_t j0[] = {1, HDR(0, 0xF, 0x23C, 0)};
    PicaCall pc;
    CHECK(pica_call_parse(j0, 2, &pc) == 0, "JUMP0 is not a call");

    /* random lists: segments copied or called, every alignment of the calls, calls back to back, first and last */
    static uint32_t pool[64][160];
    for (int t = 0; t < 400; t++) {
        CpSeg sg[64];
        int ns = 1 + (int)rnd(63);
        for (int s = 0; s < ns; s++) {
            sg[s].n = gen_cmds(pool[s], 14 + rnd(146));
            sg[s].w = pool[s];
            sg[s].call = (int)rnd(3) != 0 && sg[s].n > 0;
        }
        char what[32];
        snprintf(what, sizeof(what), "random list %d", t);
        cp_case(what, sg, ns, 2 * rnd(4));
    }

    /* the rule check sees a wrong continuation size */
    uint32_t body[64];
    uint32_t nbw = gen_cmds(body, 40);
    CpSeg one[2] = {{body, nbw, 1}, {body, nbw, 0}};
    uint32_t before = (uint32_t)s_fail;
    cp_case("one call", one, 2, 0);
    CHECK((uint32_t)s_fail == before, "one call");
    /* patch the built call list's SIZE0 short: the copied segment's last words are not reached */
    uint32_t half = CP_WORDS / 2;
    PicaCall p2;
    uint32_t len = pica_call_parse(s_fl + half, 16, &p2);
    if (len != 0) {
        s_fl[half + 4] -= 8; /* 64 bytes: past the list end into the copied segment */
        uint32_t ob, jb, oa, ja;
        int rb = cp_exec(FL_PHYS + half * 4, s_last_nb * 4, s_out_b, &ob, &jb);
        cp_exec(FL_PHYS, s_last_na * 4, s_out_a, &oa, &ja);
        CHECK(rb != 0 || ob != oa || memcmp(s_out_a, s_out_b, ob * 4) != 0, "a short continuation is not seen");
    } else {
        CHECK(0, "one call: no call at the list start");
    }
    printf("calls: %u lists, %u calls: the command processor (cb_call rules) reads the same commands as the copies\n",
           s_cp_cases, s_cp_calls);
}

/* a gpudump/gpuhang list cut at every draw, every second draw's commands called */
static void test_list_calls(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return;
    }
    static uint32_t w[CP_WORDS / 4];
    uint32_t n = (uint32_t)(fread(w, 4, CP_WORDS / 4, fp));
    fclose(fp);
    static CpSeg sg[4096];
    int ns = 0;
    uint32_t i = 0, s0 = 0;
    while (i + 1 < n && ns < 4096) {
        uint32_t hdr = w[i + 1], extra = hdr >> 20 & 0xFFu, reg = hdr & 0xFFFFu;
        uint32_t len = 2 + extra + (extra & 1u);
        if (i + len > n || (reg >= 0x238 && reg <= 0x23D) || reg == 0x010) {
            break; /* the list's own end or jumps */
        }
        i += len;
        if (reg == 0x22E || reg == 0x22F) {
            sg[ns] = (CpSeg){w + s0, i - s0, ns & 1};
            ns++;
            s0 = i;
        }
    }
    cp_case(path, sg, ns, 0);
    printf("%s: %d segments, every second called\n", path, ns);
}

int main(int argc, char** argv) {
    test_list_end();
    test_model();
    test_calls();
    for (int i = 1; i < argc; i++) {
        test_list(argv[i]);
        test_list_calls(argv[i]);
    }
    printf("%s (%d failures)\n", s_fail ? "FAILED" : "ok", s_fail);
    return s_fail != 0;
}
