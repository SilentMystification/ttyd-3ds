# Plan: moving GX vertex work from the CPU to the PICA200

Status: planned 2026-11-22. Steps 1 and 2 done. Step 3 and the immediate-mode part of step 4 built 2026-11-22 behind `gxvsh`, **default 1** since round 2: every acceptance test passes in gor_01, gon_02, aaa_00, tik_01, hei_01, jin_00 and an effects battle, in Azahar (`PORT_3DS_GX_PERF.md`, "Round 2"); not yet verified on hardware (`gxvsh=0` is the fallback). Measurements and options are in `PORT_3DS_GX_PERF.md` (the GX agent's design note); Star3D ideas in `STAR3D_NOTES.md`. This file sets the order and the **acceptance rules**.

## Order

1. **Display-list decode cache** (in progress): bit-identical by construction.
2. **The cheap state fixes:**
   - a state version separate from matrix loads;
   - a fog table only on fog changes;
   - an unlit fast path;
   - draw merging on the render thread.
3. **Vertex shader transform** for cached lists:
   - one matrix, the projection, up to 3 `MTX2x4`/`MTX3x4` texgens, colour pass-through;
   - fed by PICA-ready little-endian streams and `u16` indices in the cache entries;
   - with the CPU path kept as a per-draw fallback.
4. Then immediate-mode draws, the 10-matrix palette, and GX lighting in the shader. Each comes with its own test scenes.

## Precision rules: no z-fighting, no seams (owner requirement)

The PICA200 vertex shader computes in **24-bit floats**: 1 sign bit, 7 exponent bits, 16 mantissa bits, about 1.5×10⁻⁵ relative precision. The CPU path uses 32-bit floats. The shader path is only acceptable if it introduces **no new z-fighting and no seams** anywhere the CPU path has none. The rules:

1. **Same path for geometry that must line up.**
   - Coplanar or adjacent surfaces that the game draws to line up must be transformed **by the same path with the same matrices**:
     - decals and shadows on the floor;
     - map pieces sharing an edge;
     - multi-pass draws of one mesh.
   - A draw falls back to the CPU only by **whole object or pass group**, never one draw of a matched set. The fallback decision is keyed so that every draw sharing a matrix and depth function (`LEQUAL` / `EQUAL` re-draws) makes the same choice.
   - Multi-pass draws (the same list drawn again with `EQUAL` depth) must produce **bit-identical clip positions** each pass: same matrix uniforms, same shader code path, same input stream.
2. **Keep shader inputs small, and do the large maths on the CPU in 32-bit.**
   - Pre-multiply model × view × projection, or at least model × view, **on the CPU in 32-bit float** per draw, so the shader does one matrix–vector product. Errors don't build up across chained multiplies in 24-bit.
   - Use **camera-relative** matrices, with the translation already relative to the eye, so world coordinates far from the origin don't lose precision. Error then scales with distance from the camera, so it stays sub-pixel near the player and small far away.
   - Positions go in as the game's `s16` fixed-point with a per-entry scale where the cache can do so exactly (lossless); otherwise as floats. Never as values that need more than 16 mantissa bits to tell neighbouring vertices apart.
3. **Depth.**
   - Map GX's depth range to the PICA depth buffer's **full 24-bit range** (`C3D_DepthMap` scale and offset) exactly as the CPU path does today, and check the near and far planes match.
   - Evaluate **W-buffering** (PICA's depth-from-W mode) on hardware for perspective scenes. Use it only if it measurably reduces z-fighting without breaking orthographic and 2D layers.
   - The game's 2D and orthographic layers (HUD, paper-cutout sprites at fixed z) must keep their exact depth order. Check every scene with layered sprites.
4. **Seams.**
   - Shared edge vertices must come out identical. Same input bits, same matrix, same instruction order, so the PICA's deterministic arithmetic gives the same result for the same vertex in both meshes.
   - Never round or re-quantise positions per mesh (no per-mesh scale that differs between neighbours). A per-entry `s16` scale is allowed only when it represents every source value **exactly**.
5. **Hardware rules:**
   - write every vertex-shader output register exactly once, in full (partial writes hang the GPU);
   - don't use bool uniform b15.

## Acceptance tests (before the shader path is on by default)

Each runs per scene, CPU path vs shader path, in Azahar first and then on hardware through `ctr-devkit` once it's in:
1. **Image diff:** fixed frames after a warp, per-pixel difference with a small tolerance for colour rounding. Any cluster of differing pixels along edges or on coplanar surfaces fails.
2. **Depth diff:**
   - read back the depth buffer (an EFB depth copy, debug only) for the same frame on both paths;
   - compare per pixel, reporting the maximum and the distribution;
   - where two surfaces' depths are within the 24-bit quantum of each other (potential fighting), list them.
3. **Seam test:** clear to a loud colour (for example magenta), render the static scene, and count clear-coloured pixels **inside** the geometry, not at the screen edges or sky. Expected: zero on both paths. A count above the CPU path's is a seam.
4. **Flicker test (z-fighting):** a static camera with the game paused or deterministic for N frames, diffing consecutive frames. Any pixel that flips between two surfaces fails.
5. **Coverage:** the GX census proves which draw types went through the shader path. Every scene in the map sweep's set is tested, so not only `gor_01`.

**Switches:**
- `gxvsh=0/1` selects the shader path (default 0 until all tests pass);
- `gxvsh_check=1` renders both paths into offscreen targets and diffs them on the device, debug only.

A shader-path change that fails any test is fixed or kept off; it never ships "mostly right".

## Stereo 3D: render once, reproject the second view (owner decision 2026-11-22)

Rendering the scene twice is out. Stereo is done the efficient way: **one render, plus depth-based reprojection**.

1. **Render the 3D scene once** from the centre camera into the EFB, as today, without the 2D layers.
2. **Read a coarse depth grid:** a GPU copy of the depth buffer to linear memory, then the CPU samples about 40×24 cells (min-depth per cell, to keep foreground edges sharp). That's a few hundred reads, a fraction of a millisecond.
3. **Build two warp meshes:** grid vertices shifted horizontally by **± disparity/2**, where disparity = slider × f(depth). Splitting the shift symmetrically between the eyes halves the disocclusion artifacts in each eye. Objects at the convergence depth don't move.
4. **Draw each eye** as one textured grid draw from the EFB colour into that eye's screen target: two cheap full-screen draws instead of a second scene render.
5. **Draw the 2D layers** (HUD, windows, text, orthographic draws; the record already marks projection type) on top of each eye **without** disparity, or at a fixed screen-plane parallax. The record replays only those 2D commands per eye.
6. **Slider at 0:** skip steps 2–4 completely (normal mono present).

**Why this is fine for the 3DS:** the 3DS's maximum parallax is only a few pixels, so disocclusion gaps are small. The grid's own stretching fills them (as Star3D's screen-warp grid does for heat effects).

**Quality knob:** grid resolution (`stereogrid=WxH`). A full second render of the 3D pass stays possible as a later "high quality" option only if the reprojection shows visible artifacts in real scenes, and only after measuring both on hardware.

**Effect on the record/replay design:** the per-eye clip-space shear from the shader-path stereo note is **not** used for the 3D pass. Keep the projection-type flag on draws, so 2D layers can be replayed per eye. The CPU and shader paths need no stereo changes.

