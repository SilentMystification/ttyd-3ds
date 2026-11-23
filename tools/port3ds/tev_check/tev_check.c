/* Host check of the GX TEV -> PICA combiner compiler (port3ds/game/gx/gx_tev.c).
 *
 *   tools/port3ds/tev_check/build.sh && build/tev_check/tev_check [name...]
 *
 * For each TEV setup in the table below (copied from the game's GXSetTev*
 * calls), gx_tev_compile() is run as in the port, then both the GX TEV and the
 * PICA program it produced are evaluated in software over many random
 * texture, raster and register inputs, and the largest and mean differences
 * of the final color and alpha are printed with the PICA program. No console,
 * no emulator: a mapping is exact when the difference stays at rounding level
 * (1 or 2 of 255).
 *
 * The GX evaluation follows the hardware (as Dolphin's TEV does): a, b and c
 * use the low 8 bits of their input, d the 11-bit signed value; the lerp, the
 * add or subtract, bias, scale, clamp to 0..255 or to -1024..1023, and the
 * compare ops (R8, GR16, BGR24, RGB8/A8; GT and EQ). The PICA evaluation
 * follows the combiner: three sources per half, the operand selections, the
 * ten functions, x1/x2/x4, a clamp to 0..1 at every stage, the combiner buffer
 * with its per-stage update bits.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../port3ds/game/gx/gx_internal.h"

GxState gx;
int gx_unit_of_stage[GX_MAX_TEV];
static int s_unit_map[3], s_unit_coord[3], s_units;
static char s_warn[256];

void plat_log(int level, const char* fmt, ...) {
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(s_warn, sizeof(s_warn), fmt, ap);
    va_end(ap);
}

void gx_tev_compile(PlatGpuState* st);

/* ---- setters with the SDK's encoding (gx_state.c) ---- */

static void reset(void) {
    memset(&gx, 0, sizeof(gx));
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            gx.swap_table[i][j] = (u8)j;
        }
    }
    for (int s = 0; s < GX_MAX_TEV; s++) {
        gx.tev[s].kc_sel = GX_TEV_KCSEL_1;
        gx.tev[s].ka_sel = GX_TEV_KASEL_1;
    }
}
static void nstages(int n) { gx.num_tev = (u8)n; }
static void order(int s, int coord, int map, int color) {
    gx.tev[s].coord = (u8)coord;
    gx.tev[s].map = (u8)(map == GX_TEXMAP_NULL ? 0xFF : map);
    gx.tev[s].color = (u8)color;
}
static void cin(int s, int a, int b, int c, int d) {
    gx.tev[s].c_in[0] = (u8)a, gx.tev[s].c_in[1] = (u8)b, gx.tev[s].c_in[2] = (u8)c, gx.tev[s].c_in[3] = (u8)d;
}
static void ain(int s, int a, int b, int c, int d) {
    gx.tev[s].a_in[0] = (u8)a, gx.tev[s].a_in[1] = (u8)b, gx.tev[s].a_in[2] = (u8)c, gx.tev[s].a_in[3] = (u8)d;
}
static void cop(int s, int op, int bias, int scale, int clamp, int out) {
    gx.tev[s].c_op = (u8)op, gx.tev[s].c_bias = (u8)bias, gx.tev[s].c_scale = (u8)scale;
    gx.tev[s].c_clamp = (u8)clamp, gx.tev[s].c_out = (u8)out;
}
static void aop(int s, int op, int bias, int scale, int clamp, int out) {
    gx.tev[s].a_op = (u8)op, gx.tev[s].a_bias = (u8)bias, gx.tev[s].a_scale = (u8)scale;
    gx.tev[s].a_clamp = (u8)clamp, gx.tev[s].a_out = (u8)out;
}
static void kcsel(int s, int v) { gx.tev[s].kc_sel = (u8)v; }
static void kasel(int s, int v) { gx.tev[s].ka_sel = (u8)v; }
static void swapmode(int s, int ras, int tex) { gx.tev[s].ras_swap = (u8)ras, gx.tev[s].tex_swap = (u8)tex; }
static void swaptable(int t, int r, int g, int b, int a) {
    gx.swap_table[t][0] = (u8)r, gx.swap_table[t][1] = (u8)g, gx.swap_table[t][2] = (u8)b, gx.swap_table[t][3] = (u8)a;
}
static void tevcolor(int reg, int r, int g, int b, int a) { gx.tev_reg[reg] = (GXColorS10){r, g, b, a}; }
static void kcolor(int k, int r, int g, int b, int a) { gx.tev_kcolor[k] = (GXColor){r, g, b, a}; }
/* GXSetTevOp presets */
static void tevop_modulate(int s) {
    cin(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    ain(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
    cop(s, 0, 0, 0, 1, 0);
    aop(s, 0, 0, 0, 1, 0);
}
static void tevop_replace(int s) {
    cin(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    ain(s, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    cop(s, 0, 0, 0, 1, 0);
    aop(s, 0, 0, 0, 1, 0);
}

/* ---- the setups (from the game's source) ---- */

typedef struct {
    const char* name;
    void (*set)(void);
    int regs_random; /* GXSetTevColor values vary per draw: randomize them */
} Setup;

/* eff_bomb.c effBombDisp */
static void set_bomb(void) {
    nstages(3);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    cop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVREG0);
    aop(0, GX_TEV_COMP_A8_GT, 0, 0, 1, GX_TEVREG0);
    cin(0, GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
    ain(0, GX_CA_TEXA, GX_CA_KONST, GX_CA_RASA, GX_CA_ZERO);
    kasel(0, GX_TEV_KASEL_1_2);
    order(1, GX_TEXCOORD1, GX_TEXMAP0, GX_COLOR_NULL);
    cop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(1, GX_TEV_COMP_A8_GT, 0, 0, 1, GX_TEVPREV);
    cin(1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    ain(1, GX_CA_TEXA, GX_CA_KONST, GX_CA_A1, GX_CA_ZERO);
    kasel(1, GX_TEV_KASEL_1_2);
    tevcolor(GX_TEVREG1, 0xFF, 0xFF, 0xFF, 0x30);
    order(2, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    cop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(2, GX_CC_CPREV, GX_CC_C0, GX_CC_A0, GX_CC_ZERO);
    ain(2, GX_CA_APREV, GX_CA_ZERO, GX_CA_A0, GX_CA_A0);
}

/* eff_stageclear.c, the 3-stage letters */
static void set_stageclear(void) {
    nstages(3);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(0, 14, 0, 0, 1, GX_TEVPREV);
    aop(0, 14, 0, 0, 1, GX_TEVPREV);
    cin(0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ZERO);
    ain(0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_TEXA, GX_CA_ZERO);
    swaptable(1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
    swapmode(0, 1, 1);
    order(1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    cop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
    ain(1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    order(2, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(2, GX_CC_C0, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO);
    ain(2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    tevcolor(GX_TEVREG0, 0x36, 0x36, 0x36, 0xFF);
}

/* eff_itemget.c, the halftone background */
static void set_itemget(void) {
    nstages(2);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(0, GX_CC_C0, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO);
    ain(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    order(1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
    cop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
    ain(1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
}

/* eff_flame_n64.c, the 4-stage setup */
static void set_flame4(void) {
    nstages(4);
    order(0, 0, 0, 255);
    cop(0, 0, 0, 0, 1, 3);
    aop(0, 0, 0, 0, 1, 3);
    cin(0, 15, 15, 15, 8);
    ain(0, 7, 7, 7, 4);
    order(1, 1, 1, 255);
    cop(1, 0, 0, 0, 0, 0);
    aop(1, 0, 0, 0, 1, 0);
    cin(1, 15, 6, 6, 12);
    ain(1, 4, 7, 3, 4);
    order(2, 1, 1, 255);
    cop(2, 1, 0, 0, 1, 0);
    aop(2, 0, 0, 0, 1, 0);
    cin(2, 15, 6, 8, 0);
    ain(2, 7, 3, 0, 7);
    order(3, 255, 255, 255);
    cop(3, 1, 0, 0, 1, 0);
    aop(3, 0, 0, 0, 1, 0);
    cin(3, 15, 2, 0, 4);
    ain(3, 7, 1, 0, 7);
}

/* eff_flame_n64.c, the 5-stage setup */
static void set_flame5(void) {
    nstages(5);
    order(0, 0, 0, 255);
    cop(0, 0, 0, 0, 1, 3);
    aop(0, 0, 0, 0, 1, 3);
    cin(0, 15, 15, 15, 8);
    ain(0, 7, 7, 7, 4);
    order(1, 1, 1, 255);
    cop(1, 0, 0, 0, 1, 0);
    aop(1, 0, 0, 0, 0, 0);
    cin(1, 15, 2, 8, 2);
    ain(1, 7, 3, 4, 3);
    order(2, 255, 255, 255);
    cop(2, 0, 0, 0, 1, 0);
    aop(2, 1, 0, 0, 1, 0);
    cin(2, 15, 15, 15, 0);
    ain(2, 7, 2, 3, 0);
    order(3, 255, 255, 255);
    cop(3, 0, 0, 0, 1, 0);
    aop(3, 0, 0, 0, 0, 0);
    cin(3, 15, 15, 15, 0);
    ain(3, 7, 0, 1, 7);
    order(4, 255, 255, 255);
    cop(4, 0, 0, 0, 1, 0);
    aop(4, 1, 0, 0, 1, 0);
    cin(4, 15, 15, 15, 0);
    ain(4, 7, 2, 1, 0);
}

#include "setups_extra.h"

static const Setup k_setups[] = {
    {"bomb", set_bomb, 0},
    {"stageclear", set_stageclear, 0},
    {"itemget", set_itemget, 1},
    {"flame4", set_flame4, 1},
    {"flame5", set_flame5, 1},
    SETUPS_EXTRA
};

/* ---- GX TEV in software ---- */

typedef struct {
    int v[4];
} C4;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static C4 s_tex[GX_MAX_TEXMAP][8], s_ras; /* a texture sample per (map, texcoord) */

static C4 swz(C4 in, int table) {
    C4 o;
    for (int i = 0; i < 4; i++) {
        o.v[i] = in.v[gx.swap_table[table & 3][i] & 3];
    }
    return o;
}

static int konst_c(int s, int ch) {
    int sel = gx.tev[s].kc_sel;
    if (sel < 8) {
        static const int f[8] = {255, 223, 191, 159, 128, 96, 64, 32};
        return f[sel];
    }
    GXColor k = gx.tev_kcolor[sel & 3];
    int kv[4] = {k.r, k.g, k.b, k.a};
    if (sel >= 0x0C && sel <= 0x0F) {
        return kv[ch];
    }
    return kv[sel >= 0x1C ? 3 : sel >= 0x18 ? 2 : sel >= 0x14 ? 1 : 0];
}

static int konst_a(int s) {
    int sel = gx.tev[s].ka_sel;
    if (sel < 8) {
        static const int f[8] = {255, 223, 191, 159, 128, 96, 64, 32};
        return f[sel];
    }
    GXColor k = gx.tev_kcolor[sel & 3];
    int kv[4] = {k.r, k.g, k.b, k.a};
    return kv[sel >= 0x1C ? 3 : sel >= 0x18 ? 2 : sel >= 0x14 ? 1 : 0];
}

static void gx_eval(int* out) {
    C4 reg[4];
    for (int r = 0; r < 4; r++) {
        reg[r] = (C4){{gx.tev_reg[r].r, gx.tev_reg[r].g, gx.tev_reg[r].b, gx.tev_reg[r].a}};
    }
    int n = gx.num_tev ? gx.num_tev : 1;
    for (int s = 0; s < n; s++) {
        int map = gx.tev[s].map;
        C4 tex = map < GX_MAX_TEXMAP ? swz(s_tex[map][gx.tev[s].coord & 7], gx.tev[s].tex_swap) : (C4){{255, 255, 255, 255}};
        C4 ras = gx.tev[s].color == GX_COLOR_NULL || gx.tev[s].color == GX_COLOR_ZERO ? (C4){{0, 0, 0, 0}}
                                                                                       : swz(s_ras, gx.tev[s].ras_swap);
        int in_c[4][3], in_a[4];
        for (int i = 0; i < 4; i++) {
            int arg = gx.tev[s].c_in[i];
            for (int ch = 0; ch < 3; ch++) {
                int v;
                switch (arg) {
                case GX_CC_CPREV: v = reg[0].v[ch]; break;
                case GX_CC_APREV: v = reg[0].v[3]; break;
                case GX_CC_C0: v = reg[1].v[ch]; break;
                case GX_CC_A0: v = reg[1].v[3]; break;
                case GX_CC_C1: v = reg[2].v[ch]; break;
                case GX_CC_A1: v = reg[2].v[3]; break;
                case GX_CC_C2: v = reg[3].v[ch]; break;
                case GX_CC_A2: v = reg[3].v[3]; break;
                case GX_CC_TEXC: v = tex.v[ch]; break;
                case GX_CC_TEXA: v = tex.v[3]; break;
                case GX_CC_RASC: v = ras.v[ch]; break;
                case GX_CC_RASA: v = ras.v[3]; break;
                case GX_CC_ONE: v = 255; break;
                case GX_CC_HALF: v = 128; break;
                case GX_CC_KONST: v = konst_c(s, ch); break;
                case GX_CC_TEXRRR: v = tex.v[0]; break;
                case GX_CC_TEXGGG: v = tex.v[1]; break;
                case GX_CC_TEXBBB: v = tex.v[2]; break;
                default: v = 0; break;
                }
                in_c[i][ch] = v;
            }
            int aa = gx.tev[s].a_in[i], v;
            switch (aa) {
            case GX_CA_APREV: v = reg[0].v[3]; break;
            case GX_CA_A0: v = reg[1].v[3]; break;
            case GX_CA_A1: v = reg[2].v[3]; break;
            case GX_CA_A2: v = reg[3].v[3]; break;
            case GX_CA_TEXA: v = tex.v[3]; break;
            case GX_CA_RASA: v = ras.v[3]; break;
            case GX_CA_KONST: v = konst_a(s); break;
            default: v = 0; break;
            }
            in_a[i] = v;
        }
        int res[4];
        for (int ch = 0; ch < 4; ch++) {
            int alpha = ch == 3;
            int op = alpha ? gx.tev[s].a_op : gx.tev[s].c_op;
            int bias = alpha ? gx.tev[s].a_bias : gx.tev[s].c_bias;
            int scale = alpha ? gx.tev[s].a_scale : gx.tev[s].c_scale;
            int clamp = alpha ? gx.tev[s].a_clamp : gx.tev[s].c_clamp;
            int a = (alpha ? in_a[0] : in_c[0][ch]) & 255;
            int b = (alpha ? in_a[1] : in_c[1][ch]) & 255;
            int c = (alpha ? in_a[2] : in_c[2][ch]) & 255;
            int d = alpha ? in_a[3] : in_c[3][ch];
            int r;
            if (op <= GX_TEV_SUB) {
                int cc = c + (c >> 7);
                int lerp = (a * (256 - cc) + b * cc + 128) >> 8;
                r = op == GX_TEV_SUB ? d - lerp : d + lerp;
                r += bias == GX_TB_ADDHALF ? 128 : bias == GX_TB_SUBHALF ? -128 : 0;
                if (scale == GX_CS_SCALE_2) {
                    r *= 2;
                } else if (scale == GX_CS_SCALE_4) {
                    r *= 4;
                } else if (scale == GX_CS_DIVIDE_2) {
                    r /= 2;
                }
            } else {
                /* compare: the R8/GR16/BGR24 kinds compare the color inputs' channels, for color and alpha */
                int cmp = (op - 8) >> 1, eq = op & 1, cond;
                if (cmp == 3) {
                    cond = eq ? a == b : a > b;
                } else {
                    int va = 0, vb = 0;
                    for (int k = cmp; k >= 0; k--) {
                        va = va << 8 | (in_c[0][k] & 255);
                        vb = vb << 8 | (in_c[1][k] & 255);
                    }
                    cond = eq ? va == vb : va > vb;
                }
                r = d + (cond ? c : 0);
            }
            res[ch] = clamp ? clampi(r, 0, 255) : clampi(r, -1024, 1023);
        }
        int oc = gx.tev[s].c_out & 3, oa = gx.tev[s].a_out & 3;
        reg[oc].v[0] = res[0], reg[oc].v[1] = res[1], reg[oc].v[2] = res[2];
        reg[oa].v[3] = res[3];
    }
    for (int i = 0; i < 4; i++) {
        out[i] = clampi(reg[0].v[i], 0, 255);
    }
}

/* ---- PICA combiners in software ---- */

static double s_unit[3][4];

static double pica_src(int src, const double* prev, const double* buf, const double* k, int ch, int* bad) {
    switch (src) {
    case 0x0: return s_ras.v[ch] / 255.0;
    case 0x3: case 0x4: case 0x5: return s_unit[src - 3][ch];
    case 0xD: return buf[ch];
    case 0xE: return k[ch];
    case 0xF: return prev[ch];
    default: *bad = 1; return 0;
    }
}

static double rgb_operand(int op, int src, const double* prev, const double* buf, const double* k, int ch, int* bad) {
    int sel = op >> 1, inv = op & 1, from;
    double v;
    if (op >= 4) {
        sel = 0;
        from = (op >> 2) - 1; /* 4: r, 8: g, 12: b */
        inv = op & 1;
        v = pica_src(src, prev, buf, k, from, bad);
    } else {
        v = pica_src(src, prev, buf, k, sel == 1 ? 3 : ch, bad);
    }
    return inv ? 1.0 - v : v;
}

static double a_operand(int op, int src, const double* prev, const double* buf, const double* k, int* bad) {
    static const int from[4] = {3, 0, 1, 2};
    double v = pica_src(src, prev, buf, k, from[(op >> 1) & 3], bad);
    return op & 1 ? 1.0 - v : v;
}

static double combine(int func, double s0, double s1, double s2) {
    switch (func) {
    case 0: return s0;
    case 1: return s0 * s1;
    case 2: return s0 + s1;
    case 3: return s0 + s1 - 0.5;
    case 4: return s0 * s2 + s1 * (1.0 - s2);
    case 5: return s0 - s1;
    case 8: return s0 * s1 + s2;
    case 9: { double t = s0 + s1; return (t > 1 ? 1 : t) * s2; }
    default: return 0;
    }
}

static double q8(double v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return floor(v * 255.0 + 0.5) / 255.0;
}

static int pica_eval(const PlatGpuState* st, int* out) {
    double prev[4] = {0, 0, 0, 0}, buf[4], next_buf[4];
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        buf[i] = ((st->buffer_color >> (8 * i)) & 255) / 255.0;
    }
    memcpy(next_buf, buf, sizeof(buf));
    for (int n = 0; n < st->num_stages; n++) {
        const PlatGpuTevStage* ps = &st->stages[n];
        double k[4], res[4];
        for (int i = 0; i < 4; i++) {
            k[i] = ((ps->constant >> (8 * i)) & 255) / 255.0;
        }
        memcpy(buf, next_buf, sizeof(buf));
        for (int ch = 0; ch < 3; ch++) {
            double s[3];
            for (int i = 0; i < 3; i++) {
                s[i] = rgb_operand((ps->op_rgb >> (4 * i)) & 15, (ps->src_rgb >> (4 * i)) & 15, prev, buf, k, ch, &bad);
            }
            double v = combine(ps->func_rgb, s[0], s[1], s[2]) * (1 << ps->scale_rgb);
            res[ch] = q8(v);
        }
        {
            double s[3];
            for (int i = 0; i < 3; i++) {
                s[i] = a_operand((ps->op_alpha >> (4 * i)) & 15, (ps->src_alpha >> (4 * i)) & 15, prev, buf, k, &bad);
            }
            res[3] = q8(combine(ps->func_alpha, s[0], s[1], s[2]) * (1 << ps->scale_alpha));
        }
        /* the buffer stage n+1 reads: stage n's output where its update bit is set */
        if (n < 4) {
            for (int ch = 0; ch < 3; ch++) {
                if (st->buffer_input_rgb >> n & 1) {
                    next_buf[ch] = res[ch];
                }
            }
            if (st->buffer_input_alpha >> n & 1) {
                next_buf[3] = res[3];
            }
        }
        memcpy(prev, res, sizeof(prev));
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (int)floor(prev[i] * 255.0 + 0.5);
    }
    return bad;
}

/* ---- the check ---- */

/* gx_draw.c assign_units: one texture unit per (map, texcoord) pair, three at most */
static void assign_units(void) {
    s_units = 0;
    for (int s = 0; s < GX_MAX_TEV; s++) {
        gx_unit_of_stage[s] = -1;
        int map = gx.tev[s].map, coord = gx.tev[s].coord & 7;
        if (s >= gx.num_tev || map >= GX_MAX_TEXMAP) {
            continue;
        }
        int u = 0;
        while (u < s_units && !(s_unit_map[u] == map && s_unit_coord[u] == coord)) {
            u++;
        }
        if (u == s_units && s_units < 3) {
            s_unit_map[u] = map, s_unit_coord[u] = coord, s_units++;
        }
        gx_unit_of_stage[s] = u < s_units ? u : -1;
    }
}

static const char* src_name(int s) {
    switch (s) {
    case 0x0: return "prim";
    case 0x3: return "tex0";
    case 0x4: return "tex1";
    case 0x5: return "tex2";
    case 0xD: return "buf";
    case 0xE: return "k";
    case 0xF: return "prev";
    default: return "?";
    }
}

static void print_program(const PlatGpuState* st) {
    static const char* fn[] = {"REPLACE", "MODULATE", "ADD", "ADD_SIGNED", "INTERP", "SUB", "DOT3", "DOT3A", "MULADD",
                               "ADDMUL"};
    for (int n = 0; n < st->num_stages; n++) {
        const PlatGpuTevStage* ps = &st->stages[n];
        printf("    %d: rgb %s(", n, fn[ps->func_rgb % 10]);
        for (int i = 0; i < 3; i++) {
            printf("%s%s.%x", i ? "," : "", src_name((ps->src_rgb >> (4 * i)) & 15), (ps->op_rgb >> (4 * i)) & 15);
        }
        printf(")x%d  a %s(", 1 << ps->scale_rgb, fn[ps->func_alpha % 10]);
        for (int i = 0; i < 3; i++) {
            printf("%s%s.%x", i ? "," : "", src_name((ps->src_alpha >> (4 * i)) & 15), (ps->op_alpha >> (4 * i)) & 15);
        }
        printf(")x%d  k %08x%s%s\n", 1 << ps->scale_alpha, (unsigned)ps->constant,
               n < 4 && (st->buffer_input_rgb >> n & 1) ? "  buf.rgb<-" : "",
               n < 4 && (st->buffer_input_alpha >> n & 1) ? "  buf.a<-" : "");
    }
}

static unsigned s_seed = 12345;
static int s_binary;
static int rnd(void) {
    s_seed = s_seed * 1103515245u + 12345u;
    return (int)((s_seed >> 16) & 0x7FFF);
}
/* inputs: random, with extra weight on 0, 255 and the GX thresholds 128/129 */
static int rnd8(void) {
    int r = rnd() % 16;
    return r == 0 ? 0 : r == 1 ? 255 : r == 2 ? 128 : r == 3 ? 129 : rnd() & 255;
}

static int check(const Setup* su, int verbose) {
    PlatGpuState st;
    double sum[4] = {0};
    int worst[4] = {0}, worst_in = -1;
    const int N = 20000;
    reset();
    su->set();
    assign_units(); /* as gx_draw.c: a unit per (map, texcoord) */
    GXColorS10 regs0[4];
    memcpy(regs0, gx.tev_reg, sizeof(regs0));
    int fixed_regs = !su->regs_random;
    memset(&st, 0, sizeof(st));
    s_warn[0] = '\0';
    gx_tev_compile(&st);
    char warn[256];
    snprintf(warn, sizeof(warn), "%s", s_warn);
    for (int it = 0; it < N; it++) {
        for (int m = 0; m < GX_MAX_TEXMAP; m++) {
            for (int t = 0; t < 8; t++) {
                for (int i = 0; i < 4; i++) {
                    s_tex[m][t].v[i] = rnd8();
                }
                if (s_binary) { /* -b: texture alpha 0 or 255 (1-bit sprites), where thresholds are exact */
                    s_tex[m][t].v[3] = s_tex[m][t].v[3] & 128 ? 255 : 0;
                }
            }
        }
        for (int i = 0; i < 4; i++) {
            s_ras.v[i] = rnd8();
        }
        if (!fixed_regs) {
            for (int r = 0; r < 4; r++) {
                gx.tev_reg[r] = (GXColorS10){rnd8(), rnd8(), rnd8(), rnd8()};
            }
            for (int k = 0; k < 4; k++) {
                gx.tev_kcolor[k] = (GXColor){rnd8(), rnd8(), rnd8(), rnd8()};
            }
            /* the program depends on the register values (constants): compile per draw, as the port does */
            memset(&st, 0, sizeof(st));
            gx_tev_compile(&st);
        } else {
            memcpy(gx.tev_reg, regs0, sizeof(regs0));
        }
        for (int u = 0; u < s_units; u++) {
            for (int i = 0; i < 4; i++) {
                s_unit[u][i] = s_tex[s_unit_map[u]][s_unit_coord[u]].v[i] / 255.0;
            }
        }
        int g[4], p[4];
        gx_eval(g);
        if (pica_eval(&st, p)) {
            printf("%-12s PICA program reads an unknown source\n", su->name);
            return 1;
        }
        for (int i = 0; i < 4; i++) {
            int d = abs(g[i] - p[i]);
            sum[i] += d;
            if (d > worst[i]) {
                worst[i] = d;
                worst_in = it;
            }
        }
    }
    (void)worst_in;
    int maxd = worst[0] > worst[1] ? worst[0] : worst[1];
    maxd = maxd > worst[2] ? maxd : worst[2];
    printf("%-12s %d PICA stages  rgb max %3d mean %6.2f  alpha max %3d mean %6.2f  %s%s\n", su->name, st.num_stages,
           maxd, (sum[0] + sum[1] + sum[2]) / (3.0 * N), worst[3], sum[3] / N, warn[0] ? "warn: " : "", warn);
    if (verbose) {
        reset();
        su->set();
        memset(&st, 0, sizeof(st));
        gx_tev_compile(&st);
        print_program(&st);
    }
    return 0;
}

int main(int argc, char** argv) {
    int verbose = 0, nsel = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-b") == 0) {
            s_binary = 1;
        }
    }
    for (size_t k = 0; k < sizeof(k_setups) / sizeof(k_setups[0]); k++) {
        int want = 1;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                want = 0;
                nsel = 1;
            }
        }
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-' && strcmp(argv[i], k_setups[k].name) == 0) {
                want = 1;
            }
        }
        (void)nsel;
        if (want) {
            check(&k_setups[k], verbose);
        }
    }
    return 0;
}
