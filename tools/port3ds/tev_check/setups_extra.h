/* More TEV setups for tev_check.c, from the game's source. */

/* eff_explosion_n64.c, the 4-stage setup (REG0, REG1, KCOLOR0 vary with the fade) */
static void set_explosion4(void) {
    nstages(4);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    ain(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    order(1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    cop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVREG2);
    aop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_A1, GX_CC_ZERO);
    ain(1, GX_CA_APREV, GX_CA_TEXA, GX_CA_A1, GX_CA_ZERO);
    order(2, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    cop(2, GX_TEV_ADD, 0, 0, 0, GX_TEVPREV);
    aop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(2, GX_CC_ZERO, GX_CC_C0, GX_CC_C2, GX_CC_C1);
    ain(2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    order(3, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    cop(3, GX_TEV_SUB, 0, 0, 1, GX_TEVPREV);
    aop(3, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(3, GX_CC_ZERO, GX_CC_KONST, GX_CC_C2, GX_CC_CPREV);
    ain(3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    kcsel(3, GX_TEV_KCSEL_K0);
}

/* eff_explosion_n64.c, the 3-stage setup */
static void set_explosion3(void) {
    nstages(3);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    ain(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    order(1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    cop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_A1, GX_CC_ZERO);
    ain(1, GX_CA_APREV, GX_CA_TEXA, GX_CA_A1, GX_CA_ZERO);
    order(2, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    cop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(2, GX_CC_CPREV, GX_CC_ONE, GX_CC_C0, GX_CC_ZERO);
    ain(2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
}

/* eff_fire.c, the 1-stage alpha threshold */
static void set_fire1(void) {
    nstages(1);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(0, 14, 0, 0, 1, GX_TEVPREV);
    cin(0, 15, 15, 15, 14);
    ain(0, 4, 1, 6, 7);
    kcsel(0, 12);
    kasel(0, 28);
    kcolor(0, 0xFF, 0x00, 0x00, 0xFF);
    tevcolor(GX_TEVREG0, 0x00, 0x00, 0x00, 0x64);
}

/* eff_fire.c, the 4-stage setup */
static void set_fire4(void) {
    nstages(4);
    order(0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    cop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVREG0);
    aop(0, GX_TEV_ADD, 0, 0, 1, GX_TEVREG0);
    cin(0, 12, 14, 10, 15);
    ain(0, 7, 7, 7, 7);
    kcsel(0, 12);
    kcolor(0, 0xF8, 0x99, 0x28, 0xFF);
    order(1, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    cop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVREG1);
    aop(1, GX_TEV_ADD, 0, 0, 1, GX_TEVREG1);
    cin(1, 12, 14, 10, 15);
    ain(1, 7, 7, 7, 7);
    kcsel(1, 13);
    kcolor(1, 0xF1, 0x9E, 0x8F, 0xFF);
    order(2, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(2, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(2, 14, 0, 0, 1, GX_TEVPREV);
    cin(2, 15, 15, 15, 2);
    ain(2, 4, 3, 6, 7);
    kasel(2, 0);
    tevcolor(GX_TEVREG2, 0x00, 0x00, 0x00, 0x64);
    order(3, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(3, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(3, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(3, 4, 0, 1, 15);
    ain(3, 4, 7, 0, 0);
}

/* rels/sys/src/sys.c, the 4-stage letters (stageclear's, plus the raster) */
static void set_sys4(void) {
    nstages(4);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    cop(0, GX_TEV_COMP_RGB8_GT, 0, 0, 1, GX_TEVPREV);
    aop(0, GX_TEV_COMP_A8_GT, 0, 0, 1, GX_TEVPREV);
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
    order(3, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    cop(3, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    aop(3, GX_TEV_ADD, 0, 0, 1, GX_TEVPREV);
    cin(3, GX_CC_ZERO, GX_CC_RASC, GX_CC_CPREV, GX_CC_ZERO);
    ain(3, GX_CA_ZERO, GX_CA_RASA, GX_CA_APREV, GX_CA_ZERO);
}

/* Plain setups that must stay exact (regressions): modulate, replace, decal-like lerp. */
static void set_modulate(void) {
    nstages(1);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    tevop_modulate(0);
}
static void set_replace(void) {
    nstages(1);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    tevop_replace(0);
}
static void set_lerp_sub(void) {
    nstages(2);
    order(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    cin(0, GX_CC_C0, GX_CC_TEXC, GX_CC_RASC, GX_CC_C1);
    ain(0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
    cop(0, GX_TEV_SUB, 0, 0, 1, 0);
    aop(0, 0, 0, 0, 1, 0);
    order(1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    cin(1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
    ain(1, GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_ZERO);
    cop(1, 0, 0, 0, 1, 0);
    aop(1, 0, 0, 0, 1, 0);
}

#define SETUPS_EXTRA                                                                                                   \
    {"explosion4", set_explosion4, 1}, {"explosion3", set_explosion3, 1}, {"fire1", set_fire1, 0},                     \
        {"fire4", set_fire4, 0}, {"sys4", set_sys4, 0}, {"modulate", set_modulate, 1}, {"replace", set_replace, 1},   \
        {"lerp_sub", set_lerp_sub, 1},
