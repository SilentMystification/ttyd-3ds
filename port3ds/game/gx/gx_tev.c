/* GX TEV -> PICA200 texture combiners.
 *
 * A GX stage computes, for color and alpha separately,
 *     out = clamp((d OP ((1 - c) * a + c * b) + bias) * scale)
 * and writes it to PREV, REG0, REG1 or REG2. A PICA stage computes one of
 * REPLACE, MODULATE, ADD, ADD_SIGNED, INTERPOLATE, SUBTRACT, MULTIPLY_ADD,
 * ADD_MULTIPLY on up to three sources, times 1/2/4, clamped, for color and
 * alpha in one stage, with one constant color per stage and one extra
 * register (the combiner buffer). Each GX stage becomes one PICA stage when
 * its operands allow (a lerp, a multiply-add, an add, a replace) and two in
 * general (lerp, then add or subtract d); bias and a divide-by-two scale add
 * a stage.
 *
 * Where each GX register's current value lives is tracked: its initial
 * GXSetTevColor value (a constant), the previous PICA stage's output, or the
 * combiner buffer (kept with the buffer update bits), and a half whose
 * result is a constant needs no stage. Texture units are assigned by
 * gx_draw.c, one per (map, texcoord) pair (`gx_unit_of_stage`); the raster
 * color is the vertex color. Compare ops are folded, simplified or turned
 * into thresholds (plan_cmp). Setups that need more than six PICA stages or
 * a constant layout PICA cannot express are logged once (with their stage
 * count and the reason) and drawn with the closest result; a game can give
 * exact programs for its own setups (gx_tev_game_override).
 * tools/port3ds/tev_check checks the compiler against the GX TEV in software.
 */
#include <string.h>

#include "gx_internal.h"
#include "plat/plat.h"

/* PICA register values (citro3d gpu.h). */
enum {
    SRC_PRIMARY = 0x0,
    SRC_TEX0 = 0x3,
    SRC_BUFFER = 0xD,
    SRC_CONST = 0xE,
    SRC_PREV = 0xF,
};
enum {
    OPC_COLOR = 0x0,
    OPC_INV_COLOR = 0x1,
    OPC_ALPHA = 0x2,
    OPC_INV_ALPHA = 0x3,
    OPC_R = 0x4,
    OPC_G = 0x8,
    OPC_B = 0xC,
};
enum {
    OPA_ALPHA = 0x0,
    OPA_INV_ALPHA = 0x1,
    OPA_R = 0x2,
    OPA_G = 0x4,
    OPA_B = 0x6,
};
enum {
    F_REPLACE = 0,
    F_MODULATE = 1,
    F_ADD = 2,
    F_ADD_SIGNED = 3,
    F_INTERPOLATE = 4,
    F_SUBTRACT = 5,
    F_MULT_ADD = 8,
};

/* An operand: a PICA source and operand, or a constant value. */
typedef struct {
    u8 src;
    u8 op;      /* rgb operand, or alpha operand for alpha operands */
    u8 is_const;
    u8 inv;     /* for constants: 1 - value (folded into the value) */
    u8 val[4];  /* constant RGBA */
} Opnd;

/* Where a GX register's value is: */
enum { LOC_CONST, LOC_PREV, LOC_BUF };

typedef struct {
    PlatGpuState* st;
    int n;              /* PICA stages emitted */
    int overflow;
    int unit_of_stage[GX_MAX_TEV];
    u8 loc_c[4], loc_a[4]; /* location of PREV, REG0..2 (color, alpha) */
    int buf_c_reg, buf_a_reg; /* which GX reg the buffer holds, -1 none */
    u8 kv[4][4];        /* value of a register at LOC_CONST: GXSetTevColor's, or a constant a stage wrote */
    int sharp;          /* compare ops: further x4 threshold stages still allowed (each multiplies the slope by 4) */
    const char* approx;
} Ctx;

extern int gx_unit_of_stage[GX_MAX_TEV];

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static Opnd k_const(u8 r, u8 g, u8 b, u8 a) {
    Opnd o = {SRC_CONST, OPC_COLOR, 1, 0, {r, g, b, a}};
    return o;
}

static void approx(Ctx* c, const char* why) {
    if (c->approx == NULL) {
        c->approx = why;
    }
}

/* Konst selections (GXSetTevKColorSel / KAlphaSel). */
static u8 konst_frac(u8 sel) {
    static const u8 v[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    return sel < 8 ? v[sel] : 255;
}

static Opnd konst_color(int s) {
    u8 sel = gx.tev[s].kc_sel;
    if (sel < 8) {
        u8 f = konst_frac(sel);
        return k_const(f, f, f, f);
    }
    GXColor k = gx.tev_kcolor[sel & 3];
    if (sel >= 0x0C && sel <= 0x0F) {
        return k_const(k.r, k.g, k.b, k.a);
    }
    u8 ch = sel >= 0x1C ? k.a : sel >= 0x18 ? k.b : sel >= 0x14 ? k.g : k.r;
    return k_const(ch, ch, ch, ch);
}

static Opnd konst_alpha(int s) {
    u8 sel = gx.tev[s].ka_sel;
    if (sel < 8) {
        u8 f = konst_frac(sel);
        return k_const(f, f, f, f);
    }
    GXColor k = gx.tev_kcolor[sel & 3];
    u8 ch = sel >= 0x1C ? k.a : sel >= 0x18 ? k.b : sel >= 0x14 ? k.g : k.r;
    return k_const(ch, ch, ch, ch);
}

static Opnd reg_opnd(Ctx* c, int reg, int alpha_part, int is_alpha) {
    /* reg 0..3 = PREV, REG0..2; alpha_part: the operand wants the register's
     * alpha (CPREV vs APREV); is_alpha: for the alpha combiner */
    u8 loc = is_alpha ? c->loc_a[reg] : c->loc_c[reg];
    if (alpha_part && !is_alpha) {
        loc = c->loc_a[reg];
    }
    Opnd o = {0};
    switch (loc) {
    case LOC_PREV: o.src = SRC_PREV; break;
    case LOC_BUF: o.src = SRC_BUFFER; break;
    default: {
        const u8* v = c->kv[reg];
        o = k_const(v[0], v[1], v[2], v[3]);
        if (alpha_part) {
            o.val[0] = o.val[1] = o.val[2] = o.val[3];
        }
        return o;
    }
    }
    o.op = is_alpha ? OPA_ALPHA : (alpha_part ? OPC_ALPHA : OPC_COLOR);
    return o;
}

static Opnd tex_opnd(Ctx* c, int s, int want_alpha, int is_alpha, int chan) {
    int map = gx.tev[s].map;
    Opnd o = {0};
    int unit = map < GX_MAX_TEXMAP ? c->unit_of_stage[s] : -1;
    if (unit < 0) {
        return k_const(255, 255, 255, 255); /* no texture: GX reads white */
    }
    o.src = (u8)(SRC_TEX0 + unit);
    const u8* sw = gx.swap_table[gx.tev[s].tex_swap & 3];
    if (is_alpha) {
        u8 a = sw[3];
        o.op = a == 0 ? OPA_R : a == 1 ? OPA_G : a == 2 ? OPA_B : OPA_ALPHA;
        return o;
    }
    if (want_alpha) {
        u8 a = sw[3];
        o.op = a == 0 ? OPC_R : a == 1 ? OPC_G : a == 2 ? OPC_B : OPC_ALPHA;
        return o;
    }
    if (chan >= 0) { /* TEXRRR/GGG/BBB through the swap table */
        u8 ch = sw[chan];
        o.op = ch == 0 ? OPC_R : ch == 1 ? OPC_G : ch == 2 ? OPC_B : OPC_ALPHA;
        return o;
    }
    if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2) {
        o.op = OPC_COLOR;
    } else if (sw[0] == sw[1] && sw[1] == sw[2]) {
        o.op = sw[0] == 0 ? OPC_R : sw[0] == 1 ? OPC_G : sw[0] == 2 ? OPC_B : OPC_ALPHA;
    } else {
        approx(c, "texture swap table permutes rgb");
        o.op = OPC_COLOR;
    }
    return o;
}

static Opnd ras_opnd(Ctx* c, int s, int want_alpha, int is_alpha) {
    if (gx.tev[s].color == GX_COLOR_ZERO || gx.tev[s].color == GX_COLOR_NULL) {
        return k_const(0, 0, 0, 0);
    }
    Opnd o = {SRC_PRIMARY, 0, 0, 0, {0}};
    const u8* sw = gx.swap_table[gx.tev[s].ras_swap & 3];
    if (is_alpha) {
        u8 a = sw[3];
        o.op = a == 0 ? OPA_R : a == 1 ? OPA_G : a == 2 ? OPA_B : OPA_ALPHA;
    } else if (want_alpha) {
        u8 a = sw[3];
        o.op = a == 0 ? OPC_R : a == 1 ? OPC_G : a == 2 ? OPC_B : OPC_ALPHA;
    } else if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2) {
        o.op = OPC_COLOR;
    } else if (sw[0] == sw[1] && sw[1] == sw[2]) {
        o.op = sw[0] == 0 ? OPC_R : sw[0] == 1 ? OPC_G : sw[0] == 2 ? OPC_B : OPC_ALPHA;
    } else {
        approx(c, "raster swap table permutes rgb");
        o.op = OPC_COLOR;
    }
    return o;
}

static Opnd color_arg(Ctx* c, int s, u8 arg) {
    switch (arg) {
    case GX_CC_CPREV: return reg_opnd(c, 0, 0, 0);
    case GX_CC_APREV: return reg_opnd(c, 0, 1, 0);
    case GX_CC_C0: return reg_opnd(c, 1, 0, 0);
    case GX_CC_A0: return reg_opnd(c, 1, 1, 0);
    case GX_CC_C1: return reg_opnd(c, 2, 0, 0);
    case GX_CC_A1: return reg_opnd(c, 2, 1, 0);
    case GX_CC_C2: return reg_opnd(c, 3, 0, 0);
    case GX_CC_A2: return reg_opnd(c, 3, 1, 0);
    case GX_CC_TEXC: return tex_opnd(c, s, 0, 0, -1);
    case GX_CC_TEXA: return tex_opnd(c, s, 1, 0, -1);
    case GX_CC_RASC: return ras_opnd(c, s, 0, 0);
    case GX_CC_RASA: return ras_opnd(c, s, 1, 0);
    case GX_CC_ONE: return k_const(255, 255, 255, 255);
    case GX_CC_HALF: return k_const(128, 128, 128, 128);
    case GX_CC_KONST: return konst_color(s);
    case GX_CC_ZERO: return k_const(0, 0, 0, 0);
    case GX_CC_TEXRRR: return tex_opnd(c, s, 0, 0, 0);
    case GX_CC_TEXGGG: return tex_opnd(c, s, 0, 0, 1);
    case GX_CC_TEXBBB: return tex_opnd(c, s, 0, 0, 2);
    default: return k_const(0, 0, 0, 0);
    }
}

static Opnd alpha_arg(Ctx* c, int s, u8 arg) {
    switch (arg) {
    case GX_CA_APREV: return reg_opnd(c, 0, 1, 1);
    case GX_CA_A0: return reg_opnd(c, 1, 1, 1);
    case GX_CA_A1: return reg_opnd(c, 2, 1, 1);
    case GX_CA_A2: return reg_opnd(c, 3, 1, 1);
    case GX_CA_TEXA: return tex_opnd(c, s, 1, 1, -1);
    case GX_CA_RASA: return ras_opnd(c, s, 1, 1);
    case GX_CA_KONST: return konst_alpha(s);
    default: return k_const(0, 0, 0, 0); /* ZERO */
    }
}

static int is_zero(const Opnd* o) { return o->is_const && !o->val[0] && !o->val[1] && !o->val[2] && !o->val[3]; }
static int is_one(const Opnd* o) {
    return o->is_const && o->val[0] == 255 && o->val[1] == 255 && o->val[2] == 255 && o->val[3] == 255;
}

static int same_opnd(const Opnd* x, const Opnd* y) {
    if (x->is_const || y->is_const) {
        return x->is_const && y->is_const && !memcmp(x->val, y->val, 4);
    }
    return x->src == y->src && x->op == y->op;
}

/* A planned PICA operation for one half (rgb or alpha) of a stage. */
typedef struct {
    u8 func;
    Opnd s[3];
    u8 scale;
} Half;

/* Plans d OP lerp(a, b, c) (+ bias, * scale) as one or two halves. */
static int plan(Half* out, Opnd a, Opnd b, Opnd cc, Opnd d, u8 op, u8 bias, u8 scale, int is_alpha) {
    int n = 0;
    u8 inv = is_alpha ? OPA_INV_ALPHA : OPC_INV_COLOR;
    (void)inv;
    int sub = op == GX_TEV_SUB;
    Half h = {0};
    h.scale = scale == GX_CS_SCALE_2 ? 1 : scale == GX_CS_SCALE_4 ? 2 : 0;
    /* lerp part */
    Opnd lerp;
    int lerp_is_value = 0; /* the lerp is a plain operand (no stage needed) */
    if (is_zero(&cc)) {
        lerp = a;
        lerp_is_value = 1;
    } else if (is_one(&cc)) {
        lerp = b;
        lerp_is_value = 1;
    } else if (is_zero(&a) && is_zero(&b)) {
        lerp = k_const(0, 0, 0, 0);
        lerp_is_value = 1;
    }
    if (lerp_is_value) {
        if (is_zero(&lerp) || (!sub && is_zero(&d))) {
            Opnd v = is_zero(&lerp) ? d : lerp;
            if (sub && is_zero(&lerp)) {
                v = d;
            }
            h.func = F_REPLACE;
            h.s[0] = v;
        } else if (sub) {
            h.func = F_SUBTRACT;
            h.s[0] = d;
            h.s[1] = lerp;
        } else {
            h.func = F_ADD;
            h.s[0] = lerp;
            h.s[1] = d;
        }
    } else if (is_zero(&a) && !sub) {
        /* d + b * c */
        if (is_zero(&d)) {
            h.func = F_MODULATE;
            h.s[0] = b;
            h.s[1] = cc;
        } else {
            h.func = F_MULT_ADD;
            h.s[0] = b;
            h.s[1] = cc;
            h.s[2] = d;
        }
    } else if (is_zero(&d) && !sub) {
        h.func = F_INTERPOLATE;
        h.s[0] = b;
        h.s[1] = a;
        h.s[2] = cc;
    } else if (!sub && is_zero(&b) && same_opnd(&cc, &d)) {
        /* c + (1 - c) a: a lerp toward one */
        h.func = F_INTERPOLATE;
        h.s[0] = k_const(255, 255, 255, 255);
        h.s[1] = a;
        h.s[2] = cc;
    } else {
        Half first = {F_INTERPOLATE, {b, a, cc}, 0};
        out[n++] = first;
        Opnd prev = {SRC_PREV, (u8)(is_alpha ? OPA_ALPHA : OPC_COLOR), 0, 2, {0}}; /* inv 2: this plan's own previous */
        h.func = sub ? F_SUBTRACT : F_ADD;
        h.s[0] = sub ? d : prev;
        h.s[1] = sub ? prev : d;
    }
    if (bias == GX_TB_ADDHALF || bias == GX_TB_SUBHALF || scale == GX_CS_DIVIDE_2) {
        u8 sc = h.scale;
        h.scale = 0;
        out[n++] = h;
        Opnd prev = {SRC_PREV, (u8)(is_alpha ? OPA_ALPHA : OPC_COLOR), 0, 2, {0}}; /* inv 2: this plan's own previous */
        Half t = {0};
        if (bias == GX_TB_ADDHALF) {
            t.func = F_ADD_SIGNED;
            t.s[0] = prev;
            t.s[1] = k_const(255, 255, 255, 255);
        } else if (bias == GX_TB_SUBHALF) {
            t.func = F_ADD_SIGNED;
            t.s[0] = prev;
            t.s[1] = k_const(0, 0, 0, 0);
        } else {
            t.func = F_REPLACE;
            t.s[0] = prev;
        }
        t.scale = sc;
        if (scale == GX_CS_DIVIDE_2) {
            if (t.func == F_REPLACE) {
                t.func = F_MODULATE;
                t.s[1] = k_const(128, 128, 128, 128);
            } else {
                out[n++] = t;
                t.func = F_MODULATE;
                t.s[0] = prev;
                t.s[1] = k_const(128, 128, 128, 128);
                t.scale = 0;
            }
        }
        out[n++] = t;
        return n;
    }
    out[n++] = h;
    return n;
}

/* ---- Constants: one per PICA stage ----
 * A PICA stage has one constant color (shared by its rgb and alpha halves,
 * rgb taking .rgb and alpha .a), a GX stage up to four. A half that needs
 * two different constants is folded (constant-only arithmetic) or split in
 * two halves, each with one constant. */

static u8 kval(const Opnd* o, int is_alpha, int i) { return is_alpha ? o->val[3] : o->val[i]; }

static int same_const(const Opnd* x, const Opnd* y, int is_alpha) {
    for (int i = 0; i < (is_alpha ? 1 : 3); i++) {
        if (kval(x, is_alpha, i) != kval(y, is_alpha, i)) {
            return 0;
        }
    }
    return 1;
}

static int nsrc(u8 func) {
    switch (func) {
    case F_REPLACE: return 1;
    case F_INTERPOLATE:
    case F_MULT_ADD: return 3;
    default: return 2;
    }
}

/* A constant computed from two: f(x, y) per channel. */
static Opnd kfold(const Opnd* x, const Opnd* y, int op) {
    Opnd r = k_const(0, 0, 0, 0);
    for (int i = 0; i < 4; i++) {
        int a = x->val[i], b = y ? y->val[i] : 0, v;
        switch (op) {
        case 0: v = a * b / 255; break;   /* product */
        case 1: v = a + b; break;         /* sum */
        case 2: v = a - b; break;         /* difference */
        case 3: v = a + b - 128; break;   /* signed sum */
        default: v = 255 - a; break;      /* 1 - x */
        }
        r.val[i] = clamp8(v);
    }
    return r;
}

/* The operand 1 - o for a non-constant operand (PICA's inverted operands
 * are the odd ones, for color and alpha alike). */
static Opnd inv_opnd(Opnd o) {
    if (o.is_const) {
        return kfold(&o, NULL, 4);
    }
    o.op ^= 1;
    return o;
}

static const Opnd k_own_prev_c = {SRC_PREV, OPC_COLOR, 0, 2, {0}};
static const Opnd k_own_prev_a = {SRC_PREV, OPA_ALPHA, 0, 2, {0}};

/* Rewrites the planned halves so that none uses two different constants.
 * Returns the new count (at most 8). */
static int legalize(Half* h, int n, int is_alpha) {
    Half out[8];
    int m = 0;
    Opnd own_prev = is_alpha ? k_own_prev_a : k_own_prev_c;
    for (int k = 0; k < n && m < 7; k++) {
        Half x = h[k];
        int ns = nsrc(x.func);
        const Opnd* first = NULL;
        int distinct = 0;
        for (int i = 0; i < ns; i++) {
            if (x.s[i].is_const) {
                if (first == NULL) {
                    first = &x.s[i];
                } else if (!same_const(first, &x.s[i], is_alpha)) {
                    distinct = 1;
                }
            }
        }
        if (!distinct) {
            out[m++] = x;
            continue;
        }
        Opnd* s0 = &x.s[0];
        Opnd* s1 = &x.s[1];
        Opnd* s2 = &x.s[2];
        Half a = {F_REPLACE, {{0}}, x.scale}, b = {F_REPLACE, {{0}}, x.scale};
        int two = 0;
        switch (x.func) {
        case F_MODULATE: a.s[0] = kfold(s0, s1, 0); break;
        case F_ADD: a.s[0] = kfold(s0, s1, 1); break;
        case F_SUBTRACT: a.s[0] = kfold(s0, s1, 2); break;
        case F_ADD_SIGNED: a.s[0] = kfold(s0, s1, 3); break;
        case F_MULT_ADD: /* s0 * s1 + s2 */
            if (s0->is_const && s1->is_const) {
                Opnd p = kfold(s0, s1, 0);
                if (s2->is_const) {
                    a.s[0] = kfold(&p, s2, 1);
                } else {
                    a.func = F_ADD;
                    a.s[0] = *s2;
                    a.s[1] = p;
                }
            } else { /* one of s0/s1 is the constant, s2 another */
                a.func = F_MODULATE;
                a.s[0] = *s0;
                a.s[1] = *s1;
                a.scale = 0;
                b.func = F_ADD;
                b.s[0] = own_prev;
                b.s[1] = *s2;
                two = 1;
            }
            break;
        case F_INTERPOLATE: /* s0 * s2 + s1 * (1 - s2) */
            if (s2->is_const) {
                Opnd inv2 = kfold(s2, NULL, 4);
                if (s0->is_const && s1->is_const) {
                    Opnd p = kfold(s0, s2, 0), q = kfold(s1, &inv2, 0);
                    a.s[0] = kfold(&p, &q, 1);
                } else if (s0->is_const) { /* K s2 + X (1 - s2) */
                    a.func = F_MODULATE;
                    a.s[0] = *s1;
                    a.s[1] = inv2;
                    a.scale = 0;
                    b.func = F_ADD;
                    b.s[0] = own_prev;
                    b.s[1] = kfold(s0, s2, 0);
                    two = 1;
                } else { /* X s2 + K (1 - s2) */
                    a.func = F_MODULATE;
                    a.s[0] = *s0;
                    a.s[1] = *s2;
                    a.scale = 0;
                    b.func = F_ADD;
                    b.s[0] = own_prev;
                    b.s[1] = kfold(s1, &inv2, 0);
                    two = 1;
                }
            } else { /* K0 X + K1 (1 - X) */
                a.func = F_MODULATE;
                a.s[0] = *s2;
                a.s[1] = *s0;
                a.scale = 0;
                b.func = F_MULT_ADD;
                b.s[0] = *s1;
                b.s[1] = inv_opnd(*s2);
                b.s[2] = own_prev;
                two = 1;
            }
            break;
        default: out[m++] = x; continue;
        }
        out[m++] = a;
        if (two) {
            out[m++] = b;
        }
    }
    memcpy(h, out, (size_t)m * sizeof(Half));
    return m;
}

/* Writes one PICA stage from a color half and an alpha half; resolves
 * constants into the stage's one constant color. Returns 0 if they do not
 * fit (two different constants on one side). */
static int emit(Ctx* c, const Half* hc, const Half* ha) {
    if (c->n >= 6) {
        c->overflow = 1;
        return 1;
    }
    PlatGpuTevStage* ps = &c->st->stages[c->n];
    memset(ps, 0, sizeof(*ps));
    int have_crgb = 0, have_ca = 0;
    u8 crgb[3] = {0}, ca = 0;
    for (int i = 0; i < 3; i++) {
        Opnd o = hc->s[i];
        if (o.is_const) {
            if (have_crgb && (crgb[0] != o.val[0] || crgb[1] != o.val[1] || crgb[2] != o.val[2])) {
                approx(c, "two constants in one color stage");
            }
            crgb[0] = o.val[0];
            crgb[1] = o.val[1];
            crgb[2] = o.val[2];
            have_crgb = 1;
            o.src = SRC_CONST;
            o.op = OPC_COLOR;
        }
        ps->src_rgb |= (u16)(o.src << (4 * i));
        ps->op_rgb |= (u16)(o.op << (4 * i));
    }
    for (int i = 0; i < 3; i++) {
        Opnd o = ha->s[i];
        if (o.is_const) {
            if (have_ca && ca != o.val[3]) {
                approx(c, "two constants in one alpha stage");
            }
            ca = o.val[3];
            have_ca = 1;
            o.src = SRC_CONST;
            o.op = OPA_ALPHA;
        }
        ps->src_alpha |= (u16)(o.src << (4 * i));
        ps->op_alpha |= (u16)(o.op << (4 * i));
    }
    ps->func_rgb = hc->func;
    ps->func_alpha = ha->func;
    ps->scale_rgb = hc->scale;
    ps->scale_alpha = ha->scale;
    ps->constant = (u32)crgb[0] | (u32)crgb[1] << 8 | (u32)crgb[2] << 16 | (u32)ca << 24;
    c->n++;
    return 1;
}

static const Half k_pass_c = {F_REPLACE, {{SRC_PREV, OPC_COLOR, 0, 0, {0}}}, 0};
static const Half k_pass_a = {F_REPLACE, {{SRC_PREV, OPA_ALPHA, 0, 0, {0}}}, 0};

/* Does a later stage (or the final output) read GX register `reg`'s color
 * (or alpha) before it is written again? */
static int read_later(int from, int reg, int alpha) {
    for (int s = from; s < gx.num_tev; s++) {
        const u8* in = alpha ? gx.tev[s].a_in : gx.tev[s].c_in;
        for (int i = 0; i < 4; i++) {
            u8 a = in[i];
            int r;
            if (alpha) {
                r = a == GX_CA_APREV ? 0 : a == GX_CA_A0 ? 1 : a == GX_CA_A1 ? 2 : a == GX_CA_A2 ? 3 : -1;
            } else {
                r = (a <= GX_CC_A2) ? (int)(a / 2) : -1;
            }
            if (r == reg) {
                return 1;
            }
        }
        if ((alpha ? gx.tev[s].a_out : gx.tev[s].c_out) == reg) {
            return 0;
        }
    }
    return 0;
}

/* ---- Compare ops ----
 * GX: out = d + (a CMP b ? c : 0), compared per channel (RGB8, and A8 for
 * alpha) or on the red, green-red or blue-green-red value of a and b's color
 * (R8, GR16, BGR24; for the alpha half too). PICA has no compare, so:
 *  - folded when a and b are constants (a mask of c);
 *  - "a > 0 ? a : 0" is a, exactly (the letters of stageclear and sys);
 *  - otherwise a threshold: t = clamp(4 (a - b)), or clamp(16 (a - b)) with
 *    a second stage when the setup has the stages, then d + t c. Exact where
 *    a and b differ by at least 1/4 (1/16), as with the 1-bit alpha of
 *    sprites; a ramp over the last 64 (16) levels above b otherwise.
 * GR16 and BGR24 compare their top channel only (green, blue); EQ is exact
 * only when folded. */

/* One channel of an operand of the color combiner, as an operand of the
 * color (all channels the same) or alpha combiner. */
static Opnd chan_sel(Opnd o, int ch, int to_alpha) {
    if (o.is_const) {
        u8 v = o.val[ch];
        return k_const(v, v, v, v);
    }
    int from; /* 0..2 r, g, b; 3 alpha */
    switch (o.op) {
    case OPC_COLOR: from = ch; break;
    case OPC_R: from = 0; break;
    case OPC_G: from = 1; break;
    case OPC_B: from = 2; break;
    default: from = 3; break;
    }
    if (to_alpha) {
        o.op = from == 3 ? OPA_ALPHA : (u8)(OPA_R + 2 * from);
    } else {
        o.op = from == 3 ? OPC_ALPHA : (u8)(OPC_R + 4 * from);
    }
    return o;
}

static int plan_cmp(Ctx* c, Half* out, Opnd a, Opnd b, Opnd cc, Opnd d, Opnd ca, Opnd cb, u8 op, int is_alpha) {
    int kind = (op - GX_TEV_COMP_R8_GT) >> 1; /* 0 R8, 1 GR16, 2 BGR24, 3 RGB8 / A8 */
    int eq = op & 1;
    Opnd A = a, B = b;
    if (kind < 3) {
        /* the color inputs' red (green, blue): the same value for every channel */
        A = chan_sel(is_alpha ? ca : a, kind, is_alpha);
        B = chan_sel(is_alpha ? cb : b, kind, is_alpha);
        if (kind > 0 && !(A.is_const && B.is_const)) {
            approx(c, "TEV GR16/BGR24 compare by its top channel");
        }
    }
    Opnd zero = k_const(0, 0, 0, 0), one = k_const(255, 255, 255, 255);
    if (A.is_const && B.is_const) {
        Opnd m = zero;
        for (int i = 0; i < 4; i++) {
            int ch = kind < 3 ? 0 : is_alpha ? 3 : i;
            int va = A.val[ch], vb = B.val[ch];
            m.val[i] = (u8)((eq ? va == vb : va > vb) ? 255 : 0);
        }
        return plan(out, zero, cc, m, d, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, is_alpha);
    }
    if (kind == 3 && !eq && is_zero(&B) && same_opnd(&A, &cc)) {
        return plan(out, zero, A, one, d, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, is_alpha); /* a > 0 ? a : 0 */
    }
    if (eq) {
        approx(c, "TEV compare EQ");
        return plan(out, zero, zero, zero, d, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, is_alpha);
    }
    Opnd own = is_alpha ? k_own_prev_a : k_own_prev_c;
    int n = 0;
    Half h = {F_SUBTRACT, {A, B}, 2};
    out[n++] = h;
    /* each further x4 stage: x16, x64, x256 (exact for 8-bit inputs) */
    for (int k = 0; k < 3 && c->sharp > 0; k++) {
        c->sharp--;
        Half x = {F_REPLACE, {own}, 2};
        out[n++] = x;
    }
    if (is_one(&cc) && is_zero(&d)) {
        return n;
    }
    Half t = {0};
    if (is_zero(&d)) {
        t.func = F_MODULATE, t.s[0] = own, t.s[1] = cc;
    } else if (is_one(&cc)) {
        t.func = F_ADD, t.s[0] = own, t.s[1] = d;
    } else {
        t.func = F_MULT_ADD, t.s[0] = own, t.s[1] = cc, t.s[2] = d;
    }
    out[n++] = t;
    return n;
}

static int tev_compile_pass(PlatGpuState* st, int sharp, const char** why);

/* Per-game TEV overrides (portability rule, plan section 1): the game side
 * may give exact PICA programs for setups this compiler can only
 * approximate (unclamped intermediates, more than six stages). Returns 1 if
 * it wrote `st`'s stages. The default has none. */
__attribute__((weak)) int gx_tev_game_override(PlatGpuState* st) {
    (void)st;
    return 0;
}

/* Compare ops take the sharper threshold, the first ones first, as long as
 * the setup still fits in six stages. */
void gx_tev_compile(PlatGpuState* st) {
    const char* why = NULL;
    int has_cmp = 0;
    st->buffer_input_rgb = st->buffer_input_alpha = 0;
    if (gx_tev_game_override(st)) {
        return;
    }
    for (int s = 0; s < gx.num_tev && s < 16; s++) {
        has_cmp += (gx.tev[s].c_op > GX_TEV_SUB) + (gx.tev[s].a_op > GX_TEV_SUB);
    }
    for (int sharp = 3 * has_cmp; sharp >= 0; sharp--) {
        if (!tev_compile_pass(st, sharp, &why)) {
            break;
        }
    }
    if (why != NULL) {
        static u32 logged[64];
        int num = gx.num_tev ? gx.num_tev : 1;
        u32 key = (u32)num << 24 ^ (u32)(why[0] << 16 ^ why[5] << 8) ^ (u32)st->num_stages;
        for (int i = 0; i < 64; i++) {
            if (logged[i] == key) {
                return;
            }
            if (logged[i] == 0) {
                logged[i] = key;
                PLOG_W("TEV: %d GX stages -> %d PICA stages approximated: %s", num, st->num_stages, why);
                break;
            }
        }
    }
}

/* One compile; returns 1 if the result needed more than six stages. */
static int tev_compile_pass(PlatGpuState* st, int sharp, const char** why) {
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.st = st;
    c.sharp = sharp;
    for (int r = 0; r < 4; r++) {
        GXColorS10 v = gx.tev_reg[r];
        c.kv[r][0] = clamp8(v.r), c.kv[r][1] = clamp8(v.g), c.kv[r][2] = clamp8(v.b), c.kv[r][3] = clamp8(v.a);
    }
    memcpy(c.unit_of_stage, gx_unit_of_stage, sizeof(c.unit_of_stage));
    for (int r = 0; r < 4; r++) {
        c.loc_c[r] = c.loc_a[r] = LOC_CONST;
    }
    c.buf_c_reg = c.buf_a_reg = -1;
    int num = gx.num_tev ? gx.num_tev : 1;
    if (num > 16) {
        num = 16;
    }
    st->buffer_input_rgb = st->buffer_input_alpha = 0;
    for (int s = 0; s < num; s++) {
        Half hc[8], ha[8];
        Opnd a = color_arg(&c, s, gx.tev[s].c_in[0]);
        Opnd b = color_arg(&c, s, gx.tev[s].c_in[1]);
        Opnd cc = color_arg(&c, s, gx.tev[s].c_in[2]);
        Opnd d = color_arg(&c, s, gx.tev[s].c_in[3]);
        u8 cop = gx.tev[s].c_op, aop = gx.tev[s].a_op;
        int nc = cop > GX_TEV_SUB ? plan_cmp(&c, hc, a, b, cc, d, a, b, cop, 0)
                                  : plan(hc, a, b, cc, d, cop, gx.tev[s].c_bias, gx.tev[s].c_scale, 0);
        Opnd aa = alpha_arg(&c, s, gx.tev[s].a_in[0]);
        Opnd ab = alpha_arg(&c, s, gx.tev[s].a_in[1]);
        Opnd ac = alpha_arg(&c, s, gx.tev[s].a_in[2]);
        Opnd ad = alpha_arg(&c, s, gx.tev[s].a_in[3]);
        int na = aop > GX_TEV_SUB ? plan_cmp(&c, ha, aa, ab, ac, ad, a, b, aop, 1)
                                  : plan(ha, aa, ab, ac, ad, aop, gx.tev[s].a_bias, gx.tev[s].a_scale, 1);
        nc = legalize(hc, nc, 0);
        na = legalize(ha, na, 1);
        /* A half that comes out a constant needs no stage: the register holds that constant from here on. */
        int out_c = gx.tev[s].c_out & 3, out_a = gx.tev[s].a_out & 3;
        int const_c = nc == 1 && hc[0].func == F_REPLACE && hc[0].s[0].is_const && hc[0].scale == 0;
        int const_a = na == 1 && ha[0].func == F_REPLACE && ha[0].s[0].is_const && ha[0].scale == 0;
        if (const_c) {
            memcpy(c.kv[out_c], hc[0].s[0].val, 3);
            nc = 0;
        }
        if (const_a) {
            c.kv[out_a][3] = ha[0].s[0].val[3];
            na = 0;
        }
        int steps = nc > na ? nc : na;
        /* An operand that reads the previous GX stage's result in a later
         * step of this plan would see this plan's own first step instead:
         * take it from the combiner buffer, latched before this plan. */
        int latch_c = 0, latch_a = 0;
        for (int k = 1; k < nc; k++) {
            for (int i = 0; i < 3; i++) {
                if (hc[k].s[i].src == SRC_PREV && !hc[k].s[i].is_const && hc[k].s[i].inv != 2) {
                    hc[k].s[i].src = SRC_BUFFER;
                    latch_c = 1;
                }
            }
        }
        for (int k = 1; k < na; k++) {
            for (int i = 0; i < 3; i++) {
                if (ha[k].s[i].src == SRC_PREV && !ha[k].s[i].is_const && ha[k].s[i].inv != 2) {
                    ha[k].s[i].src = SRC_BUFFER;
                    latch_a = 1;
                }
            }
        }
        if ((latch_c || latch_a) && (c.n == 0 || c.n > 4)) {
            approx(&c, "combiner buffer unavailable for a two-step stage");
        } else {
            if (latch_c) {
                st->buffer_input_rgb |= (u8)(1u << (c.n - 1));
            }
            if (latch_a) {
                st->buffer_input_alpha |= (u8)(1u << (c.n - 1));
            }
        }
        /* Before this GX stage's output replaces "previous", keep a value a
         * later stage still needs in the combiner buffer (a half with no
         * stage of its own passes "previous" through unchanged). */
        for (int r = 0; r < 4; r++) {
            int keep_c = nc > 0 && c.loc_c[r] == LOC_PREV && r != out_c && read_later(s + 1, r, 0);
            int keep_a = na > 0 && c.loc_a[r] == LOC_PREV && r != out_a && read_later(s + 1, r, 1);
            if ((keep_c || keep_a) && c.n > 0) {
                if (c.n > 4) {
                    approx(&c, "combiner buffer needed after stage 4");
                    continue;
                }
                /* buffer input for stage n takes stage n-1's output */
                if (keep_c) {
                    st->buffer_input_rgb |= (u8)(1u << (c.n - 1));
                    if (c.buf_c_reg >= 0 && c.buf_c_reg != r) {
                        c.loc_c[c.buf_c_reg] = LOC_CONST;
                        approx(&c, "two registers need the combiner buffer");
                    }
                    c.loc_c[r] = LOC_BUF;
                    c.buf_c_reg = r;
                }
                if (keep_a) {
                    st->buffer_input_alpha |= (u8)(1u << (c.n - 1));
                    if (c.buf_a_reg >= 0 && c.buf_a_reg != r) {
                        approx(&c, "two registers need the combiner buffer");
                    }
                    c.loc_a[r] = LOC_BUF;
                    c.buf_a_reg = r;
                }
            }
        }
        for (int k = 0; k < steps; k++) {
            const Half* pc = k < nc ? &hc[k] : &k_pass_c;
            const Half* pa = k < na ? &ha[k] : &k_pass_a;
            /* a shorter half finishes first: its last step must be the last
             * PICA stage, so pad at the front instead */
            if (nc < steps) {
                pc = k < steps - nc ? &k_pass_c : &hc[k - (steps - nc)];
            }
            if (na < steps) {
                pa = k < steps - na ? &k_pass_a : &ha[k - (steps - na)];
            }
            emit(&c, pc, pa);
        }
        for (int r = 0; r < 4; r++) {
            if (c.loc_c[r] == LOC_PREV && nc > 0) {
                c.loc_c[r] = LOC_CONST; /* overwritten: stale unless buffered (logged above) */
            }
            if (c.loc_a[r] == LOC_PREV && na > 0) {
                c.loc_a[r] = LOC_CONST;
            }
        }
        c.loc_c[out_c] = const_c ? LOC_CONST : LOC_PREV;
        c.loc_a[out_a] = const_a ? LOC_CONST : LOC_PREV;
        if (c.buf_c_reg == out_c) {
            c.buf_c_reg = -1;
        }
        if (c.buf_a_reg == out_a) {
            c.buf_a_reg = -1;
        }
    }
    /* The fragment color is PREV. If the last stage wrote another register,
     * PREV still holds its earlier value. */
    if (c.loc_c[0] != LOC_PREV || c.loc_a[0] != LOC_PREV) {
        Half pc = k_pass_c, pa = k_pass_a;
        if (c.loc_c[0] != LOC_PREV) {
            pc.s[0] = reg_opnd(&c, 0, 0, 0);
        }
        if (c.loc_a[0] != LOC_PREV) {
            pa.s[0] = reg_opnd(&c, 0, 1, 1);
        }
        emit(&c, &pc, &pa);
    }
    if (c.overflow) {
        approx(&c, "more than 6 combiner stages");
        c.n = 6;
    }
    st->num_stages = (u8)c.n;
    *why = c.approx;
    return c.overflow;
}
