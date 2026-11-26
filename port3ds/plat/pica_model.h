/* A small model of the PICA200 register file, fed with command-list words (GX native step 3, switch gxchunk=2;
 * docs/PORT_3DS_GX_NATIVE.md).
 *
 * The model keeps what a register write leaves behind, byte by byte with a "known" mask (the model starts with
 * nothing known), and treats the data ports as what they fill:
 *  - vertex-shader float uniforms (GPUREG_VSH_FLOATUNIFORM_CONFIG sets the index and mode, each 4 data words in
 *    f32 mode or 3 in f24 mode fill one row, kept as written, the index moves on);
 *  - fixed attributes (GPUREG_FIXEDATTRIB_INDEX, then the three data words);
 *  - the fog LUT (index, then data words, the index moves on);
 *  - events, not state: the texture-cache clear bit of GPUREG_TEXUNIT_CONFIG, the draw triggers, framebuffer
 *    flush/invalidate, early-depth clear, FINALIZE, vertex-cache clear, primitive restart, command-buffer jumps,
 *    the lighting, procedural-texture and gas LUT ports and the shader code/descriptor ports (counted only).
 * pica_model_cmp compares two models on what both know: equal register files at a draw mean the GPU draws the same
 * thing from either list (as far as the lists themselves set it).
 *
 * No libctru: builds on the host too (tools/port3ds/pica_enc_test). Game-agnostic. */
#ifndef PICA_MODEL_H
#define PICA_MODEL_H

#include <stdint.h>

#define PICA_NREG 0x300u

typedef struct {
    uint32_t reg[PICA_NREG];
    uint8_t known[PICA_NREG]; /* byte mask (bit k: byte k of reg is known) */
    uint32_t unif[96][4];     /* vertex-shader float uniforms, as the data words came */
    uint8_t unif_known[96];
    uint32_t fixed[16][3];    /* fixed attributes (index 0-11; 15 is the immediate-mode vertex) */
    uint8_t fixed_known[16];
    uint32_t fog[128];
    uint8_t fog_known[128];
    uint32_t uidx, usub, uf32; /* the float-uniform index, the word in the row, the f32 mode */
    uint32_t urow[4];          /* the row being filled */
    int uidx_known;
    uint32_t fxidx, fxsub, fxdata[3];
    int fxidx_known;
    uint32_t fogidx;
    int fogidx_known;
    /* events */
    uint32_t draws, texclears, luts, finals, bad; /* bad: a command past the end of the words */
} PicaModel;

void pica_model_reset(PicaModel* m);
/* Feeds n words (whole commands: parameter, header, more parameters, padding to an even count). */
void pica_model_feed(PicaModel* m, const uint32_t* w, uint32_t n);

/* What pica_model_cmp found. */
typedef struct {
    uint32_t differ;    /* registers / rows / LUT entries known to both with different values */
    uint32_t one_sided; /* known to one model only (not a mismatch: one list wrote a value the other left as it was) */
    uint32_t first_reg; /* the first difference: register id, or 0x1000 + uniform row, 0x2000 + fixed attribute,
                         * 0x3000 + fog entry */
    uint32_t first_a, first_b;
} PicaModelDiff;
void pica_model_cmp(const PicaModel* a, const PicaModel* b, PicaModelDiff* d);

#endif
