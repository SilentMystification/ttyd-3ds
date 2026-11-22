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
/* An empty texture a copy will fill (render-to-texture). */
uint32_t plat_gpu_tex_create_target(uint32_t width, uint32_t height, uint32_t fmt);
/* Freed once the render thread no longer uses it. */
void plat_gpu_tex_free(uint32_t handle);
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
    uint8_t min_linear, mag_linear, mip_linear;
    uint8_t pad[3];
    float lod_bias;
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

typedef struct {
    uint32_t draws, vertices, state_changes;
    uint32_t record_bytes;
    uint32_t segments; /* extra submits in a frame (record or vertex arena full) */
    uint32_t render_us, gpu_us;
} PlatGpuStats;
void plat_gpu_stats(PlatGpuStats* out);
uint32_t plat_gpu_present_count(void); /* frames presented so far (the shot number) */

/* The next presented frame's EFB to `path` (a PPM file, relative to sdmc:/3ds/ttyd/); ignored while one is pending. */
void plat_gpu_shot(const char* path);

#ifdef __cplusplus
}
#endif

#endif
