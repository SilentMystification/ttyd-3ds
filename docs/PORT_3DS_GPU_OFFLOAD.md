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

**Effect on the record/replay design:** the per-eye clip-space shear from the shader-path stereo note is **not** used for the 3D pass. The CPU and shader paths need no stereo changes. (The projection-type flag turned out not to be what separates the 2D layers in TTYD; see below.)

### As built (2026-11-22): `port3ds/plat/stereo.c`, hooks in `gpu.c`

A presented frame, on the render thread:

1. `stereo_wanted()`: option `stereo3d` on, and `osGet3DSliderState()` (or `stereoforce`) above 0. Otherwise, or when the record has no scene draw, the frame takes the mono path unchanged (no scan, no resources; the first stereo frame creates them).
2. `stereo_split()` (`gpu.c`) scans the record once and finds where the **2D tail** starts: right after the last scene draw or EFB copy.
3. The record is replayed up to that point into the EFB, as in mono.
4. `stereo_snapshot()`: one `GX_TextureCopy` of the EFB colour rows that are shown (1.6 MB, tiled as it is) into a linear 512×1024 RGBA8 texture. Then the depth grid (see below).
5. Per eye: `stereo_warp(eye)` draws the snapshot back into the EFB through that eye's mesh; the 2D tail is replayed on top with the ordinary replay code (the state, fog LUT and shader-path uniforms in force at the split are put back for the second eye); `present()` draws the EFB into that eye's screen target as today. The right target is a second 240×400 RGB8 target linked to `GFX_RIGHT`; citro3d shows a frame in stereo when the right target was drawn, otherwise the left image goes to both eyes (`gfxSet3D(true)` is set once, on the first stereo frame).

**Design point 1, separating the 2D layers (finding: TTYD's 2D layers are perspective).** `camInit` makes the 2D camera (`"2d"`), the fade cameras and the 3D effect cameras **perspective** (fovy 25), so a projection-type split puts the HUD into the reprojected image (seen: the HUD warped with the scene). What separates them is the **viewport depth range** camdrv gives each camera: 3D and 3D image 0–1, 3D effects A/B 0.2–0.3, 2D 0.1–0.2, fade 0–0.1. A draw whose range ends at or before 0.2 is a screen layer, any other is scene. Draws that write neither colour nor depth are ignored: `camDraw` calls `sysDummyDraw` for every camera, including the debug cameras after the 2D layer, and these 0–1 draws had moved the split to the end of the frame. Real order (`stereolog=N` prints it; S/s scene, L/l layer, C copy, X clear):
   - gor_01 field: `X1 s528 l28`: clear, the scene (3D, effects), then the HUD. No interleaving.
   - gon_02 battle: `X1 s203 l29` and `X1 s216 l29`: the stage and actors, then the battle HUD and the action-command window.
   - pause menu in gor_01: `X1 s1 l269`: one scene draw (the menu's background picture of the field), then 269 window/text draws, 21 of which write depth.

   So the camera order already gives "3D then 2D"; nothing 3D was found drawn over the 2D layer. The rule is still safe if that changes: anything after the last scene draw is the tail, so a scene draw after the HUD would make the HUD part of the warped image (it is then warped by the depth beneath it), never drop or reorder anything. An EFB copy also ends the 3D part, so the tail never contains a copy (a copy in the tail would read a per-eye EFB).

**Design point 2, the depth grid.** The depth buffer is **sampled as an RGBA8 texture** (the D24S8 bytes: A = depth bits 0–7, B = 8–15, G = 16–23, R = stencil) by one quad into a 160×96 RGBA8 VRAM target (4×4 samples per cell, nearest), which a `GX_DisplayTransfer` untiles into linear memory. Works in Azahar (it reinterprets the depth surface for the texture read; a raw transfer of `depthBuf` had returned zeros) and is plain memory on hardware. The transfer is queued in the frame; the CPU reads it **on the next stereo frame** after `C3D_FrameBegin` has waited for the GPU, so there is no mid-frame stall and the disparity lags one frame. Per cell the minimum depth; per vertex the minimum of its (up to) four cells, so foreground edges keep their disparity and the background next to them is dragged along instead of the foreground being smeared.

   Depth to disparity: GX depth is d = f(D − n)/((f − n)D), so 1/D is linear in d, and P = sep·(1 − Dc/D) becomes **P(d) = sep·(d − dc)/(1 − dc)** (f ≫ n) with dc the convergence's depth value: no near/far plane needed. P is clamped to [−sep, +sep] (pop-out no larger than the depth at infinity; Star3D's "safe" budget). `sep` = `stereosep` (default 6 top-screen pixels of total parallax at the full slider) × slider. Each eye shifts by ∓P/2 screen pixels (= ∓P EFB pixels); far points move left in the left eye. The image's left and right edge columns stay put (no uncovered strip), and a row is never allowed to fold (x stays increasing).

   **Convergence:** auto, the `stereoconv`-th percentile (default 50, the median) of the grid's depths, cleared (sky) cells excluded, eased 20% per frame. Not the camera's target distance: the render thread has no camera, the record has no near/far, and the same rule works for battles, menus and cutscenes. In gor_01 the median sits at Mario's depth (disparity 0 there); in the gon_02 battle it sits on the back wall, so the actors stand just in front of the screen.

**Design point 3, the warp.** A 41×25 indexed grid (`PlatGpuVertex`, the CPU path's layout, so no shader change), one `C3D_DrawElements` per eye into the EFB at full resolution (1:1, not into the half-size screen target, so the 2D tail is drawn exactly as in mono and halved by the usual present). The fixed parts of the four meshes (two eyes, double-buffered) are written once; per frame only the clip x/y of each vertex.

**Design point 4, slider at 0:** identical to mono. Checked with `determ` runs of gor_01 (frames 600, 700, 800): no stereo switch vs `stereoforce=0`, EFB shots and screen-target shots (`scrshot`) 0 differing pixels.

**Verification in Azahar** (`stereoforce=1`, left and right screen targets saved as `shots/N.L.ppm`/`N.R.ppm`):
- gor_01: block-matching L→R (20-pixel blocks): far walls +2 px (uncrossed), Mario and Goombella 0 to +1 (the convergence), the gallows posts and the near floor −3 to −4 px (crossed). The HUD panels are identical in both eyes (only the see-through gaps between them differ).
- gon_02 battle: the action-command window and the HUD identical in both eyes; back wall 0, actors 0 to −1, the near stage floor −1 to −3 px.
- pause menu (gor_01, START): left and right **identical** (0 differing pixels): the scene part is one flat picture, the grid converges on it, and the windows are the tail.

**Artifacts seen** (gor_01 at full slider): where a near object meets the background inside a grid cell, the cell is stretched rather than disoccluded: the background next to the gallows posts is dragged a few pixels with the post, and long thin near objects (the posts, the crossbeam) pick up a slight waviness at cell boundaries (20-pixel cells, so the bends are 20 px apart). Small at 6 px total parallax; larger with `stereosep` up. A finer grid (`stereogrid=80x48`) halves the cell size. Fractional shifts resample the snapshot bilinearly, a slight softening of shifted regions; zero-disparity regions are copied exactly.

**Cost** (Azahar, relative only; gor_01 in the field, `determ`, the same 300 frames, `stereostats=300`): render thread 26.0 ms → 28.7 ms per frame (+2.7 ms, +11%); `C3D_GetDrawingTime` 0.60 → 1.01 ms. Of the CPU part, the mesh build is ~0.9 ms and the grid read (including the convergence sort) another ~0.9 ms in Azahar's tick count. The 2D tail is ~100 commands in the field and battle, ~680 in the pause menu, replayed twice. Memory: VRAM +350 KB (right screen target and the 160×96 depth target; free VRAM 1510 → 1169 KB, which copy targets would otherwise use; they fall back to the strip path), linear +2.3 MB (the colour snapshot, the depth samples, the meshes).

**Known limits:**
- The tail's draws that write depth (21 in the pause menu, none in the field) leave that depth for the second eye's replay. No visible effect in the measured scenes (pause menu L = R exactly); restoring depth per eye would cost a 1.6 MB copy each way.
- A frame whose record was split into segments draws stereo only if the last segment has a scene draw.
- One frame of disparity lag (and one wrong frame on a scene cut).

**Needs hardware:** the real slider (`osGet3DSliderState`), the perceived depth and comfort of 6 px at the full slider and the convergence choice, the parallax-barrier crosstalk on the HUD, and the real cost (the 1.6 MB `TextureCopy`, reading the snapshot from FCRAM in the warp, and the CPU time of the grid read).

**Switches:** `stereoforce=<0..1>` (simulates the slider), `stereosep=<px>` (total parallax at infinity at the full slider, default 6, max 20), `stereoconv=<1..99>` (convergence percentile, default 50), `stereogrid=<W>x<H>` (default 40x24, up to 80x48), `stereostats=<N>` (a cost line every N frames: stereo and mono render/GPU time, mesh time, tail size, tail depth writes, convergence), `stereolog=<N>` (the draw order of stereo frames N and N + 300), `scrshot` (mono shots also save the screen target as `N.M.ppm`).

