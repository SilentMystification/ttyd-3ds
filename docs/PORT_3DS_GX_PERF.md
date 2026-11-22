# 3DS port: GX front-end performance

Where the game thread's time goes in the GX layer (`port3ds/game/gx/`), what could make it cheaper, and in which order. The plan is in [PORT_3DS_PLAN.md](PORT_3DS_PLAN.md) (section 5.4); the portability rules are in its section 1 and in [GC3DS_LIBRARY_NOTE.md](GC3DS_LIBRARY_NOTE.md), "Portable performance layer".

All numbers here are from Azahar (New 3DS mode, no frame limit, build of 2026-11-22 on `port3ds` at `6c2b9fc8`). Azahar's timing only shows the direction of a change: its tick counter follows emulated instructions, not the console's caches and memory. The shares between stages are the useful part, not the absolute times.

## How the numbers were taken

- **Counters (switch `gxstats=N`, `port3ds/game/gx/gx_perf.c`).** Every N frames the front end logs per-frame averages: display lists run, vertices decoded, draws, triangles, features used, and system ticks spent in each stage. Without the switch each hook is a branch on one flag.
- **Repeats.** Each `GXCallDisplayList` is looked up by address and size in a 16384-slot table that keeps a hash of the list's bytes. A call is a *repeat* when the same address, size and hash were seen in an earlier frame, and *changed* when the address and size match but the bytes do not.
- **PC sampling.** `tools/port3ds/pc_sample.py --interval 200` on the game and render threads (192 stops over 90 s, gor_01).
- **Scenes.** Each started with `map=<map>` (and `fastboot`), measured from game frame 1140 to 1260 (the warp is at frame 595):
  - `gor_01`: Rogueport's central square, the busiest field scene;
  - `gon_02` with `story=30`: a battle starts on entry (found with a `sweep=150` run over candidate maps);
  - `aaa_00`: the opening's first room;
  - `dou_05`: a field map, as a fourth sample.

## Where the time goes

Per frame, `gxstats` on (the counters themselves cost a few percent):

| | gor_01 | gon_02 (battle) | aaa_00 | dou_05 |
|---|---|---|---|---|
| Game frame time | 267.5 ms | 133.8 ms | 133.8 ms | 101.7 ms |
| `GXCallDisplayList` calls | 5427 | 2698 | 2814 | 2142 |
| of which primitives only / state calls only | 5169 / 258 | 2565 / 133 | 2624 / 190 | 2009 / 133 |
| Display-list bytes | 367 KB | 187 KB | 201 KB | 152 KB |
| Vertices decoded from lists | 28881 | 12663 | 13599 | 10062 |
| Vertices per list (average) | 5.6 | 4.9 | 5.2 | 5.0 |
| Vertices from `GXBegin`/`GXEnd` | 1487 (391 primitives) | 903 (232) | 583 (160) | 803 (207) |
| `gx_draw_vertices` calls / triangles drawn / culled | 5560 / 11933 / 7290 | 2797 / 5357 / 2615 | 2784 / 6584 / 2030 | 2216 / 4026 / 2407 |
| PICA state rebuilds (`build_state`) | 583 | 268 | 272 | 243 |
| Draws, state records and record bytes sent to the render thread | 3752 / 436 / 362 KB | 1903 / 186 / 158 KB | 2129 / 237 / 198 KB | 1368 / 178 / 146 KB |
| Render thread time | 48 ms | 32 ms | 33 ms | 16 ms |

Game-thread time by stage (share of the frame):

| Stage | Where | gor_01 | gon_02 | aaa_00 | dou_05 |
|---|---|---|---|---|---|
| Display lists, inclusive | `GXCallDisplayList` | 199.5 ms (75%) | 94.3 ms (70%) | 99.8 ms (75%) | 76.7 ms (75%) |
| Vertex decode from lists | `read_vertex` in `draw_from_reader` | 59.6 ms (22%) | 27.6 ms (21%) | 29.8 ms (22%) | 21.7 ms (21%) |
| List parsing and per-call overhead | `run_commands`, `rd_u8`, the call itself (the inclusive time minus decode and the draw stages below) | about 30 ms (11%) | about 15 ms (11%) | about 18 ms (13%) | about 13 ms (13%) |
| Immediate mode, inclusive | `GXEnd` | 26.8 ms (10%) | 14.1 ms (11%) | 9.5 ms (7%) | 12.1 ms (12%) |
| PICA state rebuild | `build_state` (TEV compile, texture lookup, fog LUT, `plat_gpu_set_state`) | 36.2 ms (14%) | 16.6 ms (12%) | 14.1 ms (11%) | 15.6 ms (15%) |
| Transform, colour channels, texgen | `transform` in `gx_draw_vertices` | 57.1 ms (21%) | 26.9 ms (20%) | 28.1 ms (21%) | 21.3 ms (21%) |
| Triangle assembly, culling, record writing | the rest of `gx_draw_vertices`, `plat_gpu_draw` | 22.3 ms (8%) | 10.8 ms (8%) | 10.8 ms (8%) | 8.5 ms (8%) |

The same split from PC sampling (gor_01, no counters, game thread busy 87%):

| Self time | Share of game-thread samples |
|---|---|
| `gx_draw_vertices` (with `transform` and `texgen` inlined) | 15.6% |
| `draw_from_reader` (with `read_vertex` and the readers inlined) | 14.1% |
| `hash_bytes` (texture lookup, called from `gx_tex_handle_for` in `build_state`) | 13.0% |
| waiting for the render thread (`submit`) | 13.0% |
| `channel_color` | 5.2% |
| `strcmp` from `mapGetMapObj`, `hitCheckVecFilter`, `mapDisp` (game code) | 15% together |
| `run_commands`, `gx_dl_replay_port_op` | 3.2% |

Per draw, in gor_01 (Azahar time): a list call costs about 37 µs, of which the decode is 11 µs and the parse about 5.5 µs; the transform costs 1.9 µs per vertex; a state rebuild costs 62 µs.

What this says:

1. **Display lists are tiny and repeat.** The map is drawn as thousands of 5-vertex triangle strips, one list each (`mapdrv.c` `callPackedLists`, straight from the map file). From the second frame on, 89% (gor_01) to 99% (aaa_00) of the calls repeat a list seen in an earlier frame, and **none** of them changed (`changed 0` in every scene). Decoding them again every frame is a fifth of the frame.
2. **Fixed per-call and per-draw costs dominate**, because a draw has 5 vertices: the parse, the state check, the triangle buffer allocation and one record command per draw. Anything that only makes the per-vertex math faster leaves those.
3. **State rebuilds are expensive and mostly needless.** `build_state` runs whenever `gx.version` changed, and every matrix load bumps it (`GXLoadPosMtxImm` per map object), although no matrix goes into the PICA state (the CPU transforms). Each rebuild also looks the textures up again (`hash_bytes`, 13% on its own) and recomputes the fog LUT (129 `powf`).
4. **The scenes use almost none of GX's transform features** (see the inventory below): no lighting, no per-vertex matrix index, and every texture coordinate is a 2x4 matrix times the vertex's texture coordinate.

## CPU work that could move to the GPU

Everything the CPU path does per vertex and per draw, with its measured share (gor_01) and whether the PICA200 can do it. The counts are vertices per frame in gor_01 (30368 transformed); gon_02, aaa_00 and dou_05 give the same picture.

| Work | Where now | Share | PICA can do it? | What blocks it | Effort |
|---|---|---|---|---|---|
| **Format decode, big-endian swap** (list bytes and indexed arrays to floats) | `read_vertex` | 22% + parse 11% | Partly. The vertex loader reads `u8`, `s8`, `s16` and `float` attributes, little-endian, with up to 12 attributes and a per-attribute scale through a uniform. It cannot read `u16` (only as `s16`), big-endian data, or GX's indexed arrays (one index per attribute) | Indexed arrays: each vertex has one index per attribute, the PICA one index for all. Needs a CPU repack once (de-index into a vertex stream), which is what a display-list cache does. Big-endian list bytes: swapped once at that repack | Medium (with option (a)) |
| **Position/normal matrix palette** (`PNMTXIDX`) | `transform` | 0 vertices use it here | Yes: 10 matrices x 3 rows = 30 of the 96 float uniforms, indexed by a `u8` attribute through `mova` | Nothing for TTYD, which never set the attribute in these scenes (characters are skinned on the CPU by `animdrv` and drawn with `GXBegin`) | Small, once (b) exists |
| **Position transform and projection** | `transform` | most of the 21% of `transform` (not timed apart from texgen) | Yes: 3 `dp4` for the 3x4 matrix, 4 for the projection; uniforms per draw | Per-draw uniform uploads (a matrix load per map object). `C3D_UpdateUniforms` is already 8% of the render thread with 2 uniforms | Medium |
| **Normals** | `transform` | 24851 vertices carry a normal; none is used (no lighting, no normal texgen) | Yes, `dp3` with the normal matrix and a renormalise | Nothing | Small |
| **Lighting and colour channels** (`channel_color`) | `transform` | 5.2% of samples, all of it pass-through: **0 lit vertices** in all four scenes | Material/ambient selection and vertex colour pass-through: yes, trivially. GX diffuse lighting (up to 8 lights, distance and angle attenuation, spot, specular): in the vertex shader, about 10 instructions and 4 uniforms per light; the 96 float uniforms limit it to about 7 lights next to the matrices. PICA fragment lighting is per-pixel and does not match GX's per-vertex results | No lit vertex in the measured scenes; lighting needs its own test scenes (lightdrv, battle effects) before it moves | Pass-through: none (a CPU fast path today). Lighting: medium |
| **Texgen and texture matrices** | `texgen` | 22771 vertices: all `GX_TG_MTX2x4` from `GX_TG_TEXn`, no post-transform matrix, no normalise, no `TEXnMTXIDX`, no position, normal or colour source | Yes: 2 `dp4` per unit from a texture matrix uniform; 3 units | Bump (`GX_TG_BUMP*`), `SRTG` and colour sources do not occur here; `MTX3x4` with a divide fits (output q, PICA projects texture 0 only). Post-transform matrices (`GX_PTTEXMTX`) and normalise fit too but cost uniforms | Small to medium |
| **Primitive conversion** (quads, strips and fans to triangles) | `gx_draw_vertices` | inside "assembly" 8% | Strips and fans: yes, natively (`GPU_TRIANGLE_STRIP`, `GPU_TRIANGLE_FAN`) or as `u16` triangle indices built once per cached list. Quads: through indices. Lines and points: no (PICA has no wide lines or points); they stay CPU-expanded quads | Strips of 5 vertices as separate GPU draws cost more per draw than they save; indices built once in the cache let many strips go as one draw | Small (with (a)) |
| **Culling** | `culled`, in window coordinates | 38% of triangles culled (7290 of 19223) | Yes, PICA face culling (`C3D_CullFace`); the EFB rotation in the shader flips the winding once, a fixed rule | The bounding box (`GXReadBoundingBox`, only while armed) needs window positions on the CPU; keep the CPU path for those draws | Small |
| **Clipping** | the GPU (triangles crossing the eye plane are passed, not culled) | 158 vertices behind the eye | Already the GPU's | Nothing | None |
| **Viewport transform and rotation** | window position on the CPU only for culling, lines and the bounding box; the PICA viewport and the shader's rotation uniforms do the rest | small | Already done by the PICA and the shader | Nothing | None |
| **Line and point expansion** | `line` | 0 here | No | PICA has no lines | Stays |

The PICA's limits that matter: 96 float uniforms (`c0`-`c95`), 8 integer and 16 boolean uniforms, 12 vertex attributes, a 512-instruction shader. A shader that does the measured cases (position matrix, projection, up to 3 texgen 2x4 matrices, colour pass-through, and for later a 10-matrix palette) needs about 3 + 4 + 6 + 30 = 43 float uniforms, which leaves room for about 7 lights.

## Options

### (a) Cache each display list's decoded vertices

Keep, per list, what the decode produced, and on the next call hand it to the transform without parsing or decoding.

- **Key:** list address and size; **validation on every call:** a hash of all the list's bytes; the vertex description; the attribute formats of the formats the list uses; the current matrix indices; for each indexed attribute, the array address and stride and a hash of the array bytes between the lowest and highest index the list uses. Any change means a decode. This catches the cases that matter:
  - a REL relink or a DVD read into the list's memory, and the game reusing the memory for another list (the list bytes change);
  - CPU-animated arrays and arrays reloaded in place (the array hash changes);
  - the same list drawn with another vertex state (another entry, up to 4 per list).
- **How the game uses lists:** map meshes are drawn from the map file's packed lists (`mapdrv.c` `callPackedLists`); `mapdrv.c` also records a state-only list per map part at load (`GXBeginDisplayList`, about 5% of the calls, uncacheable here and not worth caching); effects call static lists from their `.data` (`eff_*.c`); `shadowdrv.c` draws static lists with arrays it sets just before. `animdrv` (characters) never uses display lists. No list was seen changing in place in the four scenes.
- **What it saves:** the decode (21-22%) and most of the parse (11-13%), minus the validation (hashing about 70 list bytes plus the referenced array bytes per call).
- **What it costs:** memory for the decoded vertices; the cost of a hash per call.
- **Risks:** a 32-bit hash collision would draw stale vertices (about 1 in 4 billion per changed list); arrays changed outside the hashed range cannot matter (the decode never reads them).
- **The next step from there:** with the cache in place, an entry can also hold a PICA-ready form: a little-endian vertex stream in linear memory (`s16` positions with a scale, `s8`/`s16` normals, `u8` colours, `s16` or float texture coordinates) and a `u16` index list that turns the strips into one triangle list. Star3D (Star Fox Adventures on 3DS) works this way; it reports a 36-byte packed vertex that cut GPU vertex time by 17% on hardware. That form only pays off together with (b), because the CPU transform reads floats.

### (b) Transform on the PICA vertex shader

Move `transform` and `texgen` into the vertex shader for the draws that fit, and keep the CPU path for the rest.

- **Fits the shader:** one position/normal matrix (or the 10-matrix palette), the projection, colour pass-through with the GX material/ambient selection, up to 3 `MTX2x4`/`MTX3x4` texgens from position, normal or a texture coordinate, PICA culling. This is every vertex in the measured scenes.
- **Stays on the CPU:** GX lighting until it has test scenes (none in the measured scenes), bump and `SRTG` texgen, colour-sourced texgen, lines and points, draws while the bounding box is armed, and immediate-mode draws until they are converted too.
- **What it saves:** the transform (21%) and most of the assembly (8%) for cached lists, and it shrinks the vertex records (the GPU reads the cache's stream directly, no 40-byte transformed vertex per triangle corner).
- **What it costs:** a shader variant (selected by uniform booleans, not a program switch), per-draw uniform uploads (matrix and texture matrices: up to 7 vec4s per map object), winding and culling rules through the EFB rotation, depth range equivalence. The render thread pays more per draw (uniforms), the game thread much less.
- **Risks:** precision (GX transforms in 32-bit float, the PICA in 24-bit float), which shows as z-fighting or texture swimming on far geometry; uniform limits for lit or palette draws; Azahar is forgiving where hardware hangs (every output register must be written, Star3D reports).

### (c) Cheaper state work and recording

- **Matrix loads should not rebuild the PICA state.** No matrix goes into `PlatGpuState`, but `GXLoadPosMtxImm`, `GXLoadNrmMtxImm`, `GXLoadTexMtxImm` and `GXSetProjection` bump `gx.version`. A separate version for state that `build_state` reads would cut the 583 rebuilds in gor_01 to the real state changes (the render thread sees 436 different states; many rebuilds produce the same one). Expected: most of the 14%, and fewer texture lookups, so less `hash_bytes` too.
- **Fog LUT:** computed only when the fog parameters change, not on each rebuild (129 `powf` each time); `plat_gpu_set_state` also hashes the LUT on every call.
- **Unlit channels:** `channel_color` with lighting off is a copy; a fast path in `transform` saves most of its 5%.
- **Smaller state records:** a `PlatGpuState` is about 700 bytes, and 436 of them are 85% of the 362 KB record in gor_01. Recording only the state groups that changed (combiners, textures, blending, depth, fog, scissor and viewport) cuts the record writing and the render thread's `apply_state`.
- **Merge draws:** consecutive draws with no state change between them are contiguous in the vertex arena already; `plat_gpu_draw` can extend the previous draw command instead of adding one (`plat/gpu.c`). The render thread then issues far fewer `C3D_DrawArrays` (3752 per frame in gor_01), which is where its `GPUCMD_Add` and `C3D_UpdateUniforms` time goes.
- **Risks:** low. The state split needs a list of which setters feed `build_state`; the draw merge needs the record format to stay readable by the replay.

## Recommended order

1. **(a) The decoded display-list cache.** It is self-contained (one new file and hooks in `gx_pipe.c`), it removes about a third of the game thread's GX time in every measured scene, its results are bit-identical by construction (the transform gets the same input), and it is the base for the PICA-ready vertex streams that (b) needs. Done on this branch, see "Results" below.
2. **(c), the cheap parts:** the separate state version for matrix loads, the fog LUT cache and the unlit fast path (game thread, `gx_draw.c` and `gx_state.c`), and the draw merge in `plat/gpu.c` (render thread). Each is small and can be A/B-tested alone. Coordinate the texture lookup with the texture-hash work in `gx_tex.c`, since fewer rebuilds mean fewer lookups.
3. **(b) The vertex shader transform**, first for cached lists with one matrix, no lighting and `MTX2x4` texgen (every vertex measured here), fed by PICA-ready streams in the cache entries (linear memory, budgeted) and `u16` index lists, with the CPU path as the fallback. Then the immediate-mode draws, then the matrix palette and lighting with their own test scenes.

## For PORT_3DS_TESTING.md

Rows for the switch table:

| Switch | Effect |
|---|---|
| `gxstats=N` | Every N game frames, logs `gxstats` lines with per-frame averages: display lists (calls, bytes, vertices, how many repeat unchanged from an earlier frame), immediate-mode primitives, draws, triangles and culled triangles, PICA state rebuilds, the render thread's draws and record bytes, the game thread's time per GX stage (list decode, state rebuild, transform, triangle assembly), the GX features the vertices use (lighting, texgen kinds, matrix indices, primitive types), and with `gxcache` the cache's hits and memory. Off by default; costs a few percent when on |
