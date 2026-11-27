/* Stereoscopic 3D by reprojection (stereo.c), for the GPU back end (gpu.c).
 * Render thread only. See docs/PORT_3DS_GPU_OFFLOAD.md, "Stereo 3D".
 *
 * A stereo frame: the record's 3D part is replayed once into the EFB, then
 * stereo_snapshot() copies the EFB colour to a texture and samples its depth
 * into a coarse grid (read on the next frame). Per eye, stereo_warp() draws
 * the snapshot back into the EFB through a grid mesh whose vertices are
 * shifted by +-disparity/2, gpu.c replays the record's 2D tail on top, and
 * presents the EFB into stereo_eye_target(eye). */
#ifndef PLAT_STEREO_H
#define PLAT_STEREO_H

#include <citro3d.h>

typedef struct {
    C3D_RenderTarget* efb;    /* 512 x target_h RGBA8 + D24S8, rotated as the screen */
    C3D_RenderTarget* screen; /* the top screen's left (mono) target */
    C3D_AttrInfo* attr;       /* the CPU-path vertex layout (PlatGpuVertex) */
    int uloc_rot0, uloc_rot1;
    u32 screen_flags;         /* the screen target's output transfer flags */
    /* EFB pixel (x, y) (the game's EFB, x right, y down) -> EFB target pixel */
    void (*efb_point)(float x, float y, float* tx, float* ty);
    int target_h;             /* the EFB target's rows (1024, or 608 with efb608); the texture views are 1024 rows */
    int region_h;             /* the rows shown (800, or 608) */
} StereoInit;

void stereo_init(const StereoInit* in);
/* Before C3D_FrameBegin of a presented record: 1 when this frame is drawn in
 * stereo (option on, slider or stereoforce above 0, resources available). */
int stereo_wanted(void);
/* Any thread: stereo_wanted's answer without its side effects (the game thread's guess at a record's start;
 * gpu.c switch gxstereo). */
int stereo_maybe(void);
/* Still before C3D_FrameBegin: creates what a stereo frame needs (once);
 * 0 when it cannot (no memory), then the frame is mono. */
int stereo_begin(void);
/* After C3D_FrameBegin (the last frame's GPU work is done): reads the depth
 * grid the last stereo frame sampled and builds both eyes' meshes. */
void stereo_prepare(void);
/* In the frame, at the end of the 3D part: snapshot of the EFB colour, depth
 * grid sample. Leaves the EFB as the draw target. */
void stereo_snapshot(void);
/* In the frame: the snapshot into the EFB through eye `eye`'s mesh (0 left,
 * 1 right). Changes the vertex buffer, combiner, rot uniforms. */
void stereo_warp(int eye);
C3D_RenderTarget* stereo_eye_target(int eye);
/* After C3D_Fini and C3D_Init (a GPU reset): the right eye's target is linked to its screen again. */
void stereo_relink(void);
/* GX queue entries a stereo frame adds (frame splits and transfers). */
#define STEREO_GX_ENTRIES 4
/* The render thread's cost of a stereo frame's split and 2D tail per eye (ticks), and the tail's record
 * commands by kind (counted while the right eye replays it). */
enum { STC_WORDS, STC_FLUSH, STC_UNIF, STC_MARK, STC_OTHER, STC_N };
typedef struct {
    u32 split_ticks;   /* snapshot + left warp */
    u32 tail_ticks[2]; /* the tail per eye (eye 1 without its warp) */
    u32 warp1_ticks;   /* the right eye's warp and state restore */
    u32 kinds[STC_N];
} StereoTailCost;
static inline void stereo_tail_count(StereoTailCost* c, int kind) { c->kinds[kind]++; }
/* Before stereo_frame_done of a stereo frame. */
void stereo_tail_cost(const StereoTailCost* c);
/* After C3D_FrameEnd: cost statistics (switch stereostats). */
void stereo_frame_done(int stereo, u32 render_us, u32 gpu_us, u32 tail_cmds, u32 tail_zwrites);
/* After a shot of the EFB: the screen targets too (eye shots in stereo, the
 * mono screen with switch scrshot). `path` ends in ".ppm". */
void stereo_shot(const char* path, int stereo);

#endif
