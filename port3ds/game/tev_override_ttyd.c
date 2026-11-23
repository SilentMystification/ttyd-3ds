/* TTYD's TEV overrides: exact PICA combiner programs for the game's TEV
 * setups that gx_tev.c can only approximate (gx_tev_game_override, the
 * per-game override of the portability rule, plan section 1).
 *
 * Each entry matches one setup by its stages (order, inputs, ops, clamp,
 * outputs; not the register values, which become the program's constants)
 * and writes a program with the same result, derived by hand below and
 * checked in software against the GX TEV by tools/port3ds/tev_check
 * (largest difference 1 or 2 of 255, rounding).
 *
 * Why the compiler cannot: these N64-style effects let a stage run
 * unclamped (GX keeps 11-bit signed intermediates, up to about 4.0) and
 * subtract from it in the next stage; PICA clamps every stage to 0..1.
 */
#include <string.h>

#include "gx/gx_internal.h"

extern int gx_unit_of_stage[GX_MAX_TEV];

/* ---- matching ---- */

typedef struct {
    u8 coord, map;             /* 0xFF: none */
    u8 c_in[4], a_in[4];
    u8 c_op, c_clamp, c_out;
    u8 a_op, a_clamp, a_out;
} Stage;

static int match(const Stage* want, int n) {
    if (gx.num_tev != n) {
        return 0;
    }
    for (int s = 0; s < n; s++) {
        const Stage* w = &want[s];
        int map = gx.tev[s].map >= GX_MAX_TEXMAP ? 0xFF : gx.tev[s].map;
        if (map != w->map || (map != 0xFF && gx.tev[s].coord != w->coord) ||
            memcmp(gx.tev[s].c_in, w->c_in, 4) || memcmp(gx.tev[s].a_in, w->a_in, 4) ||
            gx.tev[s].c_op != w->c_op || gx.tev[s].a_op != w->a_op || gx.tev[s].c_clamp != w->c_clamp ||
            gx.tev[s].a_clamp != w->a_clamp || (gx.tev[s].c_out & 3) != w->c_out ||
            (gx.tev[s].a_out & 3) != w->a_out || gx.tev[s].c_bias || gx.tev[s].a_bias || gx.tev[s].c_scale ||
            gx.tev[s].a_scale || gx.tev[s].tex_swap || gx.tev[s].ras_swap) {
            return 0;
        }
    }
    return 1;
}

/* ---- PICA stages (citro3d's GPU_TEVSRC / operands / GPU_COMBINEFUNC values) ---- */

enum { TEX0 = 0x3, BUF = 0xD, K = 0xE, PREV = 0xF };
enum { C = 0x0, C_INV = 0x1 };                /* rgb operands */
enum { A = 0x0, A_INV = 0x1 };                /* alpha operands */
enum { REPLACE = 0, MODULATE = 1, ADD = 2, SUB = 5, INTERP = 4, MULADD = 8 };

#define SRC3(a, b, c) ((u16)((a) | (b) << 4 | (c) << 8))

static void stage(PlatGpuState* st, int n, u8 fc, u16 src_c, u16 op_c, u8 fa, u16 src_a, u16 op_a, const u8* k) {
    PlatGpuTevStage* ps = &st->stages[n];
    memset(ps, 0, sizeof(*ps));
    ps->func_rgb = fc, ps->src_rgb = src_c, ps->op_rgb = op_c;
    ps->func_alpha = fa, ps->src_alpha = src_a, ps->op_alpha = op_a;
    ps->constant = k ? (u32)k[0] | (u32)k[1] << 8 | (u32)k[2] << 16 | (u32)k[3] << 24 : 0;
}

static u8 c8(s16 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static int unit_src(int s) { return TEX0 + (gx_unit_of_stage[s] < 0 ? 0 : gx_unit_of_stage[s]); }

/* ---- eff_flame_n64.c, types 0 and 1 ----
 *   S0  REG2 = t0                         (t0: map 0 at texcoord 0)
 *   S1  PREV.rgb = 1 + C2 C2   unclamped   PREV.a = t1a + t1a (1 - t0a)   (t1: map 1 at texcoord 1)
 *   S2  PREV.rgb = PREV - C2 t1            PREV.a = t0a PREV.a
 *   S3  PREV.rgb = C1 - C0 PREV            PREV.a = A0 PREV.a
 * clamp(1 + t0 t0 - t0 t1) is 1 where t0 >= t1 and 1 - t0 (t1 - t0) where
 * t1 > t0, that is 1 - y with y = t0 max(0, t1 - t0); then C1 - C0 (1 - y).
 *   P0  rgb t1 - t0             a t1a (1 - t0a) + t1a
 *   P1  rgb t0 prev  (= y)      a t0a prev
 *   P2  rgb C0 (1 - prev)       a A0 prev
 *   P3  rgb C1 - prev           a prev */
static const Stage k_flame01[] = {
    {0, 0, {15, 15, 15, 8}, {7, 7, 7, 4}, 0, 1, 3, 0, 1, 3},
    {1, 1, {15, 6, 6, 12}, {4, 7, 3, 4}, 0, 0, 0, 0, 1, 0},
    {1, 1, {15, 6, 8, 0}, {7, 3, 0, 7}, 1, 1, 0, 0, 1, 0},
    {0, 0xFF, {15, 2, 0, 4}, {7, 1, 0, 7}, 1, 1, 0, 0, 1, 0},
};

static void flame01(PlatGpuState* st) {
    int t0 = unit_src(0), t1 = unit_src(1);
    GXColorS10 r0 = gx.tev_reg[1], r1 = gx.tev_reg[2];
    u8 k2[4] = {c8(r0.r), c8(r0.g), c8(r0.b), c8(r0.a)};
    u8 k3[4] = {c8(r1.r), c8(r1.g), c8(r1.b), 0};
    stage(st, 0, SUB, SRC3(t1, t0, 0), SRC3(C, C, 0), MULADD, SRC3(t1, t0, t1), SRC3(A, A_INV, A), NULL);
    stage(st, 1, MODULATE, SRC3(PREV, t0, 0), SRC3(C, C, 0), MODULATE, SRC3(PREV, t0, 0), SRC3(A, A, 0), NULL);
    stage(st, 2, MODULATE, SRC3(K, PREV, 0), SRC3(C, C_INV, 0), MODULATE, SRC3(PREV, K, 0), SRC3(A, A, 0), k2);
    stage(st, 3, SUB, SRC3(K, PREV, 0), SRC3(C, C, 0), REPLACE, SRC3(PREV, 0, 0), SRC3(A, 0, 0), k3);
    st->num_stages = 4;
}

/* ---- eff_flame_n64.c, type 2 ----
 *   S0  REG2 = t0
 *   S1  PREV.rgb = C0 + C0 t1               PREV.a = A2 + A2 t1a   unclamped
 *   S2  PREV.rgb = PREV                     PREV.a = PREV.a - A1 A2
 *   S3  PREV.rgb = PREV                     PREV.a = A0 PREV.a     unclamped (in range)
 *   S4  PREV.rgb = PREV                     PREV.a = PREV.a - A1 A0
 * alpha = A0 max(0, X - A1) with X = clamp(t0a (1 - A1) + t0a t1a):
 *   P0  a t0a (1 - A1)
 *   P1  a t0a t1a + prev   (= X)
 *   P2  a prev - A1
 *   P3  a A0 prev          rgb C0 t1 + C0 */
static const Stage k_flame2[] = {
    {0, 0, {15, 15, 15, 8}, {7, 7, 7, 4}, 0, 1, 3, 0, 1, 3},
    {1, 1, {15, 2, 8, 2}, {7, 3, 4, 3}, 0, 1, 0, 0, 0, 0},
    {0, 0xFF, {15, 15, 15, 0}, {7, 2, 3, 0}, 0, 1, 0, 1, 1, 0},
    {0, 0xFF, {15, 15, 15, 0}, {7, 0, 1, 7}, 0, 1, 0, 0, 0, 0},
    {0, 0xFF, {15, 15, 15, 0}, {7, 2, 1, 0}, 0, 1, 0, 1, 1, 0},
};

static void flame2(PlatGpuState* st) {
    int t0 = unit_src(0), t1 = unit_src(1);
    GXColorS10 r0 = gx.tev_reg[1], r1 = gx.tev_reg[2];
    u8 a1 = c8(r1.a);
    u8 k0[4] = {0, 0, 0, (u8)(255 - a1)};
    u8 k2[4] = {0, 0, 0, a1};
    u8 k3[4] = {c8(r0.r), c8(r0.g), c8(r0.b), c8(r0.a)};
    stage(st, 0, REPLACE, SRC3(t0, 0, 0), 0, MODULATE, SRC3(t0, K, 0), SRC3(A, A, 0), k0);
    stage(st, 1, REPLACE, SRC3(PREV, 0, 0), 0, MULADD, SRC3(t0, t1, PREV), SRC3(A, A, A), NULL);
    stage(st, 2, REPLACE, SRC3(PREV, 0, 0), 0, SUB, SRC3(PREV, K, 0), SRC3(A, A, 0), k2);
    stage(st, 3, MULADD, SRC3(K, t1, K), SRC3(C, C, C), MODULATE, SRC3(PREV, K, 0), SRC3(A, A, 0), k3);
    st->num_stages = 4;
}

/* ---- eff_explosion_n64.c, the 4-stage setup ----
 *   S0  PREV = t0
 *   S1  REG2.rgb = t0 (1 - A1) + t1 A1      PREV.a = t0a (1 - A1) + t1a A1
 *   S2  PREV.rgb = C1 + C0 C2   unclamped   PREV.a = PREV.a
 *   S3  PREV.rgb = PREV - K C2              PREV.a = PREV.a
 * that is clamp(C1 + D C2) with D = C0 - K, positive or negative per
 * channel: Dp = max(D, 0), Dn = max(-D, 0), one of them 0 in each channel:
 *   P0  rgb lerp (C2)      a lerp           buffer <- C2
 *   P1  rgb Dn prev (= m)
 *   P2  rgb (1 - m) - (1 - C1) = C1 - m
 *   P3  rgb Dp buf + prev */
static const Stage k_explosion4[] = {
    {0, 0, {15, 15, 15, 8}, {7, 7, 7, 4}, 0, 1, 0, 0, 1, 0},
    {1, 1, {0, 8, 5, 15}, {0, 4, 2, 7}, 0, 1, 3, 0, 1, 0},
    {0, 0xFF, {15, 2, 6, 4}, {7, 7, 7, 0}, 0, 0, 0, 0, 1, 0},
    {0, 0xFF, {15, 14, 6, 0}, {7, 7, 7, 0}, 1, 1, 0, 0, 1, 0},
};

static void explosion4(PlatGpuState* st) {
    int t0 = unit_src(0), t1 = unit_src(1);
    GXColorS10 r0 = gx.tev_reg[1], r1 = gx.tev_reg[2];
    int ksel = gx.tev[3].kc_sel;
    GXColor kc = gx.tev_kcolor[ksel & 3];
    u8 kv[3] = {kc.r, kc.g, kc.b};
    if (ksel < 0x0C || ksel > 0x0F) { /* the game uses K0; other selections: one channel or a fraction */
        static const u8 f[8] = {255, 223, 191, 159, 128, 96, 64, 32};
        u8 ch = ksel < 8 ? f[ksel] : ksel >= 0x1C ? kc.a : ksel >= 0x18 ? kc.b : ksel >= 0x14 ? kc.g : kc.r;
        kv[0] = kv[1] = kv[2] = ch;
    }
    u8 c0[3] = {c8(r0.r), c8(r0.g), c8(r0.b)}, c1[3] = {c8(r1.r), c8(r1.g), c8(r1.b)};
    u8 a1 = c8(r1.a);
    u8 kl[4] = {a1, a1, a1, a1}, kn[4] = {0}, kc1[4] = {0}, kp[4] = {0};
    for (int i = 0; i < 3; i++) {
        int d = c0[i] - kv[i];
        kp[i] = (u8)(d > 0 ? d : 0);
        kn[i] = (u8)(d < 0 ? -d : 0);
        kc1[i] = (u8)(255 - c1[i]);
    }
    stage(st, 0, INTERP, SRC3(t1, t0, K), SRC3(C, C, C), INTERP, SRC3(t1, t0, K), SRC3(A, A, A), kl);
    stage(st, 1, MODULATE, SRC3(PREV, K, 0), SRC3(C, C, 0), REPLACE, SRC3(PREV, 0, 0), 0, kn);
    stage(st, 2, SUB, SRC3(PREV, K, 0), SRC3(C_INV, C, 0), REPLACE, SRC3(PREV, 0, 0), 0, kc1);
    stage(st, 3, MULADD, SRC3(BUF, K, PREV), SRC3(C, C, C), REPLACE, SRC3(PREV, 0, 0), 0, kp);
    st->num_stages = 4;
    st->buffer_input_rgb = 1; /* stage 1 onward reads stage 0's color (C2) from the buffer */
}

int gx_tev_game_override(PlatGpuState* st) {
    if (match(k_flame01, 4)) {
        flame01(st);
        return 1;
    }
    if (match(k_flame2, 5)) {
        flame2(st);
        return 1;
    }
    if (match(k_explosion4, 4)) {
        explosion4(st);
        return 1;
    }
    return 0;
}
