/* GPU interface between the GX front end (game side) and the PICA200 back
 * end (plat/gpu*.c, citro3d).
 *
 * The game thread records a frame as a stream of commands into a record
 * buffer (QTM); at the end of the frame the buffer goes to the render thread
 * (core 2), which replays it into citro3d while the game thread runs the
 * next frame. Vertices are already transformed, lit and given texture
 * coordinates by the front end (clip-space positions), so the vertex shader
 * passes them through.
 *
 * All values are numeric PICA200 register values or plain numbers; no enum
 * types cross this interface (the two halves use different enum sizes). */
#ifndef PLAT_GPU_H
#define PLAT_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Render target ----
 * The EFB has the GameCube size the game sets (608x480 for TTYD), times
 * the resolution scale; the display transfer scales it to the top screen. */
#define PLAT_GPU_EFB_MAX_W 640
#define PLAT_GPU_EFB_MAX_H 528

/* ---- Vertex layout written by the front end ----
 * Every vertex: clip-space position, primary color, two texture coordinate
 * sets used by texture units 0/1 and 2 (PICA has three units; unit 2 takes
 * texcoord 2). 40 bytes. */
typedef struct {
    float pos[4];      /* clip space */
    uint8_t color[4];  /* RGBA, after lighting */
    float tc0[2];
    float tc1[2];
    float tc2[2];
} PlatGpuVertex;

/* ---- Textures ----
 * Created by the front end with pixels already in a PICA format, Morton
 * tiled, power-of-two sizes. Handles are small integers; 0 is "none". */
enum {
    PLAT_TEXFMT_RGBA8 = 0x0,
    PLAT_TEXFMT_RGB8 = 0x1,
    PLAT_TEXFMT_RGBA5551 = 0x2,
    PLAT_TEXFMT_RGB565 = 0x3,
    PLAT_TEXFMT_RGBA4 = 0x4,
    PLAT_TEXFMT_LA8 = 0x5,
    PLAT_TEXFMT_L8 = 0x7,
    PLAT_TEXFMT_A8 = 0x8,
    PLAT_TEXFMT_LA4 = 0x9,
    PLAT_TEXFMT_L4 = 0xA,
    PLAT_TEXFMT_A4 = 0xB,
    PLAT_TEXFMT_ETC1 = 0xC,
    PLAT_TEXFMT_ETC1A4 = 0xD,
};

/* Returns a handle, or 0 when out of texture memory. `pixels` is copied. */
uint32_t plat_gpu_tex_create(uint32_t width, uint32_t height, uint32_t fmt, const void* pixels, uint32_t bytes);
/* The same in two steps, for a caller that writes level 0 into the texture's memory itself (no staging copy):
 * begin gives the memory (*size bytes), end makes the written bytes visible to the GPU. */
uint32_t plat_gpu_tex_create_begin(uint32_t width, uint32_t height, uint32_t fmt, void** data, uint32_t* size);
void plat_gpu_tex_create_end(uint32_t handle);
/* As plat_gpu_tex_create, with a mip chain: `pixels` is level 0 (`bytes`), `mip_pixels` the levels
 * 1..levels in order (`mip_bytes`), every level Morton tiled in the same format, each half the size of the
 * one before (a level is at least 8x8). `levels` is the number of levels after level 0. */
uint32_t plat_gpu_tex_create_mip(uint32_t width, uint32_t height, uint32_t fmt, const void* pixels, uint32_t bytes,
                                 const void* mip_pixels, uint32_t mip_bytes, uint32_t levels);
/* The most levels after level 0 a texture of this size can have (0 when a side is 8). */
uint32_t plat_gpu_tex_max_levels(uint32_t width, uint32_t height);
/* Texture tiling by the GPU transfer engine (switch texdt; see plat/gpu.c). The caller writes the texels linear
 * (top row first, no padding, the texture's own format: RGBA8, RGB565, RGBA5551 or RGBA4; sides powers of two,
 * at least 8, no mip chain) into a staging block, flushes it on the writing thread, and hands it over; the
 * transfer runs on the GPU before the next frame list and the game thread never waits for it.
 *   plat_gpu_xfer_mode: 0 off, 1 transfer, 2 verify (the caller tiles on the CPU too; the results are compared)
 *   plat_gpu_stage_alloc: NULL when none is free (tile on the CPU); `reserve_half`: only if half the arena stays free
 *   plat_gpu_tex_create_xfer: the texture, or 0 (block freed in both cases; tile on the CPU) */
int plat_gpu_xfer_mode(void);
uint32_t plat_gpu_xfer_min_texels(void);
void* plat_gpu_stage_alloc(uint32_t bytes, int reserve_half);
void plat_gpu_stage_free(void* block);
void plat_gpu_stage_flush(void* block, uint32_t bytes);
uint32_t plat_gpu_tex_create_xfer(uint32_t width, uint32_t height, uint32_t fmt, void* stage, uint32_t bytes);
/* Mode 2: `handle` is CPU tiled (create_begin/end); `stage` holds the same texels linear and is taken over. */
void plat_gpu_tex_xfer_verify(uint32_t handle, void* stage, uint32_t bytes);
/* An empty texture a copy will fill (render-to-texture). */
uint32_t plat_gpu_tex_create_target(uint32_t width, uint32_t height, uint32_t fmt);
/* Freed once the render thread no longer uses it. */
void plat_gpu_tex_free(uint32_t handle);
/* Waits for the GPU, then releases every freed texture at once instead of after its frames in flight (a texture
 * allocation that failed asks for this before it retries). Game thread. */
void plat_gpu_tex_reclaim(void);
/* Reads a texture back (copies whose memory the game reads with the CPU);
 * waits for the render thread. Returns bytes written in RGBA8 linear rows. */
uint32_t plat_gpu_tex_read(uint32_t handle, void* rgba8, uint32_t width, uint32_t height);

/* ---- Draw state ----
 * One combiner stage, PICA register encoding (GPU_TEVSRC etc.). */
typedef struct {
    uint16_t src_rgb, src_alpha;   /* 3 sources, 4 bits each */
    uint16_t op_rgb, op_alpha;     /* 3 operands, 4 bits each (rgb) / 3 bits (alpha) */
    uint8_t func_rgb, func_alpha;  /* GPU_COMBINEFUNC */
    uint8_t scale_rgb, scale_alpha;/* 0: x1, 1: x2, 2: x4 */
    uint32_t constant;             /* RGBA8, R in the low byte */
} PlatGpuTevStage;

typedef struct {
    uint32_t handle;
    uint8_t wrap_s, wrap_t;  /* GPU_TEXTURE_WRAP_PARAM */
    uint8_t min_linear, mag_linear, mip_linear; /* mip_linear: linear filter between the levels (only used when the texture has levels) */
    uint8_t proj;            /* unit 0 only: projective lookup (texcoord0.w is q); the vertex shader passes q on */
    uint8_t pad[2];
    float lod_bias;              /* added to the hardware's level of detail, in levels */
} PlatGpuTexUnit;

typedef struct {
    PlatGpuTevStage stages[6];
    uint8_t num_stages;
    uint8_t buffer_input_rgb;    /* bit n: stage n+1 reads the combiner buffer from stage n's output (rgb) */
    uint8_t buffer_input_alpha;
    uint8_t pad0;
    uint32_t buffer_color;       /* initial combiner buffer, RGBA8 */
    PlatGpuTexUnit tex[3];
    /* fragment operations */
    uint8_t alpha_test_enable, alpha_test_func, alpha_test_ref, pad1;
    uint8_t blend_mode;          /* 0 off, 1 blend, 2 logic op */
    uint8_t blend_eq_rgb, blend_eq_alpha;
    uint8_t blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    uint8_t logic_op;
    uint8_t depth_test, depth_func, depth_write, color_write; /* color_write: GPU_WRITEMASK bits */
    uint8_t cull;                /* GPU_CULLMODE */
    uint8_t fog_enable;
    uint8_t pad2[2];
    uint32_t fog_color;
    /* fog_lut_id and fog_lut are not part of the state comparison: the LUT
     * goes to the GPU only when it changed. A nonzero id names the LUT's
     * contents (the same id always means the same LUT), so the back end
     * compares ids; 0 means unknown, and the back end hashes the LUT. */
    uint32_t fog_lut_id;
    float fog_lut[129];          /* PICA fog LUT input: f(z) for z = 0..128/128 (window depth) */
    uint16_t scissor_x0, scissor_y0, scissor_x1, scissor_y1; /* EFB pixels, end exclusive */
    float viewport_x, viewport_y, viewport_w, viewport_h;   /* EFB pixels */
    float depth_near, depth_far;                              /* GX window depth range 0..1 */
} PlatGpuState;

/* ---- Frame record ---- */
void plat_gpu_init(void);
/* Starts recording a frame; returns 0 when the render thread is still busy
 * with the frame before the previous one (the caller waits). */
void plat_gpu_frame_begin(void);
/* Reserves `count` vertices in this frame's vertex arena (linear memory). */
PlatGpuVertex* plat_gpu_alloc_vertices(uint32_t count);
/* State for the following draws (copied into the record). */
void plat_gpu_set_state(const PlatGpuState* state);
/* The same for a caller that knows which regions of `state` can differ from the last state it gave: only their
 * words are compared (the front end's switch gxdirty). Regions in PlatGpuState order: */
enum {
    PLAT_GPU_R_TEV = 1u << 0, /* stages .. buffer_color */
    PLAT_GPU_R_TEX = 1u << 1, /* tex[3] */
    PLAT_GPU_R_PIX = 1u << 2, /* alpha_test_enable .. fog_lut */
    PLAT_GPU_R_SCI = 1u << 3, /* scissor_x0 .. depth_far */
};
void plat_gpu_set_state_dirty(const PlatGpuState* state, uint32_t regions);
/* Switch gxone (GX native step 2, docs/PORT_3DS_GX_NATIVE.md): the caller interns each state class and names it by
 * an id that owns a block of PICA words; below this call only the ids are compared. Classes: */
enum {
    PLAT_GPU_C_TEV = 0, /* stages .. buffer_color with fog_enable and fog_color */
    PLAT_GPU_C_TEX = 1, /* tex[3] */
    PLAT_GPU_C_PIX = 2, /* alpha_test_enable .. cull, and scissor_x0 .. depth_far */
};
int plat_gpu_one_active(void); /* 0: off (the caller uses plat_gpu_set_state_dirty), 1 on, 2 verify */
/* The block of class `cls` for the class's part of `state`, built now; returns its id, 0 when the class's store is
 * full (the caller resets its table and the store with plat_gpu_one_reset, then builds again). */
uint32_t plat_gpu_one_block(int cls, const PlatGpuState* state);
uint32_t plat_gpu_one_capacity(int cls); /* blocks the class's store holds */
void plat_gpu_one_reset(int cls);        /* every id of the class becomes invalid */
/* As plat_gpu_set_state_dirty, with the ids of the classes in force (ids[PLAT_GPU_C_*]; 0: not interned, the class
 * is compared and encoded from the state). */
void plat_gpu_set_state_one(const PlatGpuState* state, uint32_t regions, const uint32_t ids[3]);
/* Triangle list of `count` vertices from plat_gpu_alloc_vertices. */
void plat_gpu_draw(const PlatGpuVertex* vertices, uint32_t count);
/* Clears the EFB rectangle (GX copy with clear). */
void plat_gpu_clear(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t rgba, uint32_t depth24,
                    int clear_color, int clear_depth);
/* Copies an EFB rectangle into a texture (GXCopyTex): src rect in EFB
 * pixels, box-filtered by 2 when half, into texture `handle` (dst_w x dst_h). */
void plat_gpu_copy_tex(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int half, uint32_t handle,
                       uint32_t dst_w, uint32_t dst_h);
/* Ends the frame: copy the EFB rectangle to the screen and hand the record
 * to the render thread. */
void plat_gpu_frame_end(uint16_t efb_w, uint16_t efb_h);
/* Waits until the render thread has executed everything recorded so far
 * (GXDrawDone, reads of copies). */
void plat_gpu_sync(void);
/* Reads one EFB pixel (GXPeekARGB): waits for the GPU. */
uint32_t plat_gpu_peek_argb(uint16_t x, uint16_t y);

/* ---- Vertex-shader path (front end switch gxvsh) ----
 * The GPU transforms a stream the front end built once (a cached display
 * list): object-space positions, colours and texture coordinates in linear
 * memory, drawn with u16 triangle indices. Per draw the front end gives the
 * uniform block (clip rows = projection x position matrix, multiplied on the
 * CPU in 32-bit float; texture rows; colour selection) and the stream.
 *
 * Stereo: the clip rows are the GX clip space, before the screen rotation;
 * the back end composes the rotation (and, for a second eye, a per-eye
 * clip-space offset) on the render thread, so a record can be replayed once
 * per eye without re-recording. `flags` says what an eye offset needs. */
void* plat_gpu_stream_alloc(uint32_t bytes);            /* linear memory, 16-byte aligned; NULL when out */
void plat_gpu_stream_commit(const void* p, uint32_t bytes); /* the CPU wrote it: flushed before the record goes to the GPU */
void plat_gpu_stream_free(void* p);                     /* freed once no frame in flight can read it */

enum { PLAT_VSH_ORTHO = 1u, PLAT_VSH_LIT = 2u }; /* PlatGpuVshConst.flags: orthographic projection; GX lighting
                                                   (the last plat_gpu_vsh_light block applies) */

typedef struct {
    float clip[4][4];   /* clip x, y, z, w = row . (x, y, z, 1), GX clip space (unrotated) */
    float tex[9][4];    /* texture unit u: s, t, q rows 3u..3u + 2, applied to the source (q row 0 0 0 1: no divide) */
    float tgsrc[3][4];  /* texture unit u's source weights: x position, y normal, z the slot (s, t, 1, 1); w 1: divide by q */
    float clrmix[4];    /* per component: 1 takes the vertex colour, 0 takes matclr */
    float matclr[4];    /* 0..255 */
    uint32_t flags;
    uint32_t pad[3];
} PlatGpuVshConst;

/* Stream layout word: which attributes are s16 (else float), the number of
 * texture slots, whether a normal is there, the stride. The attributes lie in
 * this order: the float ones (position, slots 0..2, the normal), the s16 ones
 * (position and slots, same order), the colour (u8 x 4), so each is naturally
 * aligned; the stride is a multiple of 4. */
#define PLAT_VSH_POS_S16 0x1u
#define PLAT_VSH_NSLOTS(l) (((l) >> 1) & 3u)
#define PLAT_VSH_SLOT_S16(i) (0x8u << (i))
#define PLAT_VSH_NRM 0x40u /* float x3 normal (v5) */
#define PLAT_VSH_STRIDE(l) (((l) >> 8) & 0xFFu)

/* Byte offsets of position, colour, slots 0..2 and the normal (-1: none); returns the stride. */
static inline uint32_t plat_vsh_layout_n(uint32_t pos_s16, uint32_t nslots, uint32_t slot_s16_mask, uint32_t nrm,
                                         int32_t off[6]) {
    uint32_t o = 0;
    for (int pass = 0; pass < 2; pass++) { /* 0: floats, 1: s16 */
        if ((pos_s16 != 0) == (pass == 1)) {
            off[0] = (int32_t)o;
            o += pass ? 6u : 12u;
        }
        for (uint32_t i = 0; i < 3; i++) {
            if (i >= nslots) {
                off[2 + i] = -1;
            } else if (((slot_s16_mask >> i) & 1u) == (pass == 1 ? 1u : 0u)) {
                off[2 + i] = (int32_t)o;
                o += pass ? 4u : 8u;
            }
        }
        if (pass == 0) {
            off[5] = nrm ? (int32_t)o : -1;
            o += nrm ? 12u : 0u;
        }
    }
    off[1] = (int32_t)o;
    o += 4;
    return (o + 3u) & ~3u;
}

static inline uint32_t plat_vsh_layout(uint32_t pos_s16, uint32_t nslots, uint32_t slot_s16_mask, int32_t off[5]) {
    int32_t o6[6];
    uint32_t stride = plat_vsh_layout_n(pos_s16, nslots, slot_s16_mask, 0, o6);
    for (int i = 0; i < 5; i++) {
        off[i] = o6[i];
    }
    return stride;
}

static inline uint32_t plat_vsh_layout_word_n(uint32_t pos_s16, uint32_t nslots, uint32_t slot_s16_mask, uint32_t nrm) {
    int32_t off[6];
    uint32_t stride = plat_vsh_layout_n(pos_s16, nslots, slot_s16_mask, nrm, off);
    return (pos_s16 ? PLAT_VSH_POS_S16 : 0u) | (nslots & 3u) << 1 | (slot_s16_mask & 7u) << 3 | (nrm ? PLAT_VSH_NRM : 0u) |
           stride << 8;
}

static inline uint32_t plat_vsh_layout_word(uint32_t pos_s16, uint32_t nslots, uint32_t slot_s16_mask) {
    return plat_vsh_layout_word_n(pos_s16, nslots, slot_s16_mask, 0);
}

/* GX lighting of colour channel 0 in the shader (gxvshlit): eye-space position
 * and normal rows (the CPU's position and normal matrices; the normal is
 * normalised in the shader), the ambient colour, the diffuse and attenuation
 * selectors, and up to 8 lights in eye space, packed in mask order. */
#define PLAT_VSH_LIGHTS 8
typedef struct {
    float eye[3][4];   /* eye position = row . (x, y, z, 1) */
    float nrm[3][4];   /* eye normal = row . (nx, ny, nz, 0); identity rows when the vertex has no normal */
    float amb[4];      /* ambient colour 0..255 (rgb), w unused */
    float dmode[4];    /* diffuse: x floor (0 clamp, -1e30 sign), y weight of n.l, z = 1 - y (GX_DF_NONE) */
    float amode[4];    /* attenuation: x weight of the spot term (1 spot, 0 none), y = 1 - x */
    uint32_t count;    /* lights used */
    uint32_t pad[3];
    struct {
        float pos[4];
        float dir[4];
        float color[4]; /* 0..255 */
        float a[4];     /* a0, a1, a2 */
        float k[4];     /* k0, k1, k2 */
    } light[PLAT_VSH_LIGHTS];
} PlatGpuVshLight;
void plat_gpu_vsh_light(const PlatGpuVshLight* l, uint32_t id);

/* Scale word of a draw: bits 0-7 the position's power-of-two exponent k
 * (value = s16 x 2^-k; 0 for floats); byte 1 + u for texture unit u: bits 0-1
 * the stream slot it reads (3: none, the coordinate source is absent), bits
 * 2-7 that slot's k. */
#define PLAT_VSH_UNIT(slot, k) ((((uint32_t)(k) & 63u) << 2) | ((uint32_t)(slot) & 3u))

/* The uniform block for the following shader draws (copied; recorded only when it changed).
 * `id` names its contents: the same nonzero id always means the same block,
 * so an unchanged id is skipped without comparing; 0 compares the bytes. */
void plat_gpu_vsh_const(const PlatGpuVshConst* c, uint32_t id);
/* Switch gxrows (revised plan step 6, docs/PORT_3DS_GX_NATIVE.md; needs gxone, so gxonehop=1): the uniform block given
 * by rows. Bit i of `rows` (i < PLAT_VSH_ROWS: clip 0-3, tex 4-12, tgsrc 13-15, clrmix 16, matclr 17;
 * PLAT_VSH_ROW_FLAGS: flags) is set when that row of `c` differs from the block at the previous call (the front end
 * compares each rebuilt row once); 0: nothing changed, nothing is sent. Sends what plat_gpu_vsh_const(c, a new id)
 * sends: only the rows the GPU does not hold already, compared row by row against them. With it on,
 * plat_gpu_imm_begin's `cid` is this `rows` word. */
#define PLAT_VSH_ROWS 18
#define PLAT_VSH_ROW_FLAGS (1u << PLAT_VSH_ROWS)
#define PLAT_VSH_ROWS_ALL ((PLAT_VSH_ROW_FLAGS << 1) - 1u)
int plat_gpu_rows_active(void); /* 0 off, 1 on, 2 verify */
void plat_gpu_vsh_rows(const PlatGpuVshConst* c, uint32_t rows);
/* Triangles of a stream: `nidx` u16 indices (copied) into the stream at `stream`.
 * cull: 0 none, 1 GX front faces, 2 GX back faces (GX's front is clockwise on screen). */
void plat_gpu_vsh_draw(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, const uint16_t* idx,
                       uint32_t nidx);
/* The same draw from indices that already lie in linear memory (plat_gpu_stream_alloc, committed) and stay
 * unchanged until freed with plat_gpu_stream_free: nothing is copied. `calls` is how many front-end draws it
 * stands for (the counters only). */
void plat_gpu_vsh_drawp(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, const uint16_t* idx,
                        uint32_t nidx, uint32_t calls);
/* Registers a linear index range the DRAWP indices may come from (gxbatch's pool, a stream index segment); a draw
 * outside every range is skipped at replay. No range registered: unchecked. */
void plat_gpu_set_index_pool(const void* base, uint32_t bytes);
/* A cached stream's indices drawn in place (no copy), like plat_gpu_vsh_drawp, and a draw whose indices follow the
 * previous draw's in memory (same stream, layout, scales, cull, nothing recorded between) merges into it. */
void plat_gpu_vsh_drawl(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, const uint16_t* idx,
                        uint32_t nidx);
/* Room for `max` indices at the end of the current record's index arena, or NULL. An immediate draw writes its
 * indices there and passes the pointer to plat_gpu_vsh_draw, which then does not copy them. */
uint16_t* plat_gpu_vsh_idx_reserve(uint32_t max);

/* Switch gxchunk (GX native step 3, docs/PORT_3DS_GX_NATIVE.md): batch chunks. A run of DRAWP draws that comes
 * again unchanged (a gxbatch batch) gets one prebuilt chunk of PICA words: per draw its attribute setup when it
 * changes, its scale rows when they change, the cull word, the draw packet with the index address. At a use the
 * chunk's words are copied into the frame list in place of plat_gpu_vsh_drawp's per draw (COPY mode). */
typedef struct {
    const void* stream;
    uint32_t layout, scales;
    const uint16_t* idx; /* in the index pool, committed, unchanged while the chunk is used */
    uint32_t nidx;       /* as given to plat_gpu_vsh_drawp (trimmed to whole triangles the same way) */
} PlatGpuChunkDraw;
#define PLAT_GPU_CHUNK_NEVER 0xFFFFFFFFu
int plat_gpu_chunk_active(void); /* 0 off, 1 on, 2 verify */
/* A chunk for n draws with cull mode `cull`: its id, 0 when it cannot be built yet (an attribute setup not known
 * yet: try again at a later use), PLAT_GPU_CHUNK_NEVER when it cannot be built at all. */
uint32_t plat_gpu_chunk_build(const PlatGpuChunkDraw* d, uint32_t n, uint32_t cull);
/* Draws the first `ndraws` draws of chunk `id` (the last with `last_nidx` indices, at most its own), as
 * plat_gpu_vsh_drawp for each would; `calls` front-end draws (counters). 0: the id is not valid any more (or the
 * cull mode differs): nothing was drawn, the caller draws per draw and builds again. */
int plat_gpu_chunk_draw(uint32_t id, uint32_t cull, uint32_t ndraws, uint32_t last_nidx, uint32_t calls);

/* Switch gximmfl (GX native step 4, docs/PORT_3DS_GX_NATIVE.md; needs gxchunk): an immediate draw straight into the
 * frame list. plat_gpu_imm_begin does what plat_gpu_vsh_light (when `l` is not NULL), plat_gpu_vsh_const and the
 * room checks of plat_gpu_vsh_draw do, and returns where the draw's `nidx` u16 indices go (in the record's index
 * arena: the caller writes them there, nothing is copied), or NULL when plat_gpu_vsh_draw would draw nothing.
 * plat_gpu_imm_end then draws them as plat_gpu_vsh_draw would (the same words, the same merges). nidx: a whole
 * number of triangles, at least 3. */
int plat_gpu_immfl_active(void); /* 0 off, 1 on, 2 verify */
uint16_t* plat_gpu_imm_begin(const PlatGpuVshConst* c, uint32_t cid, const PlatGpuVshLight* l, uint32_t lid,
                             uint32_t layout, uint32_t nidx); /* gxrows: cid is plat_gpu_vsh_rows' rows word */
void plat_gpu_imm_end(const void* stream, uint32_t layout, uint32_t scales, uint32_t cull, uint32_t nidx);
/* gximmfl=2: the front end's own checks (route and indices), for the `gximmfl frame` line. */
void plat_gpu_immfl_note(uint32_t checks, uint32_t route_differ, uint32_t idx_differ);

typedef struct {
    uint32_t draws, vertices, state_changes;
    uint32_t record_bytes;
    uint32_t segments; /* extra submits in a frame (record or vertex arena full) */
    uint32_t render_us, gpu_us;
    uint32_t calls; /* plat_gpu_draw calls; `draws` is the draw commands after merging */
    uint32_t vsh_draws, vsh_calls, vsh_indices, vsh_consts; /* shader path: commands after merging, calls, indices, uniform blocks */
    uint32_t stream_kb, stream_pending; /* linear memory of live streams; frees waiting for the frames in flight */
    /* render thread, last presented frame: records replayed (1 + segments + mid-frame syncs) and the time spent
     * in C3D_FrameBegin's vblank wait over them (included in render_us) */
    uint32_t records, begin_wait_us;
    uint32_t syncs; /* plat_gpu_sync calls since boot (each ends a record mid-frame) */
} PlatGpuStats;
void plat_gpu_stats(PlatGpuStats* out);
uint32_t plat_gpu_present_count(void); /* frames presented so far (the shot number) */

/* The EFB of the next frame submitted for presentation after this call, on the recording thread, to `path` (a PPM
 * file, relative to sdmc:/3ds/ttyd/); ignored while one is pending. */
void plat_gpu_shot(const char* path);
/* A layer that records GPU calls for another thread to submit (a deferred GX translator) puts the request in its
 * record, so the shot follows the same frame as without it: the hook returns 1 when it took the request; the
 * submitting thread later calls plat_gpu_shot again (the hook returns 0 there). NULL removes it. */
void plat_gpu_set_shot_hook(int (*hook)(const char* path));

#ifdef __cplusplus
}
#endif

#endif
