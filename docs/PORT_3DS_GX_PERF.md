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

1. **(a) The decoded display-list cache.** It is self-contained (one new file and hooks in `gx_pipe.c`), it removes the decode (a fifth of the frame) at the price of a full check per call, which nets 13-15% of the game thread's GX time in every measured scene, its results are bit-identical by construction (the transform gets the same input), and it is the base for the PICA-ready vertex streams that (b) needs. Done on this branch, see "Results" below.
2. **(c), the cheap parts:** the separate state version for matrix loads, the fog LUT cache and the unlit fast path (game thread, `gx_draw.c` and `gx_state.c`), and the draw merge in `plat/gpu.c` (render thread). Each is small and can be A/B-tested alone. Coordinate the texture lookup with the texture-hash work in `gx_tex.c`, since fewer rebuilds mean fewer lookups.
3. **(b) The vertex shader transform**, first for cached lists with one matrix, no lighting and `MTX2x4` texgen (every vertex measured here), fed by PICA-ready streams in the cache entries (linear memory, budgeted) and `u16` index lists, with the CPU path as the fallback. Then the immediate-mode draws, then the matrix palette and lighting with their own test scenes.

## Results: the display-list cache (step 1)

Implemented in `port3ds/game/gx/gx_dlcache.c`, hooked into `gx_pipe.c` (`GXCallDisplayList`, `draw_from_reader`, `read_vertex`). Switch `gxcache`: 0 off, 1 on (the default), 2 verify. Game-agnostic: it knows display lists, vertex state and memory, nothing about TTYD.

**How it validates.** Every call checks every input of the decode:

- a hash of all the list's bytes;
- the vertex description and the attribute formats of the formats used;
- the current matrix indices;
- the array addresses and strides;
- a hash of every array element the list's vertices read, at the indices they read (kept in the entry, 2 bytes per vertex and indexed attribute).

Nothing is trusted across frames. A first version hashed each attribute's whole index range instead. Map lists index scattered parts of their mesh's array, so it hashed megabytes per frame and saved nothing (gor_01: list stage 219 ms with the cache against 214 ms without). Hashing only the elements read fixed that.

**Memory.** Entries are malloc'd from the regular heap (the GPU never reads them) within `gxcachekb` (default 8192 KB). They are evicted least recently used first, but an entry used this frame or the last is never evicted: a frame that draws more lists than fit leaves the rest uncached instead of thrashing. A first version with 8192 entry slots thrashed right after the gor_01 load (7824 lists per frame, 0 hits); the slots are now 16384. The entry table is static (16384 x 80 bytes, 1.3 MB), plus capture buffers of 8192 vertices and 98304 indices.

**Equivalence.**

- `gxcache=2` decodes every hit again and compares it field by field with the entry: **0 mismatches in 310,140 hits** (gor_01, 60 frames).
- Screenshots: `shots=100 shotmax=13`, game frames 800-1200, compared per pixel (`shotdiff.py`), with a second cache-off run as the noise floor because the game is not deterministic between runs:

| Scene | off vs off (noise) | off vs on |
|---|---|---|
| gon_02 battle | identical (5 of 5) | **identical (5 of 5)** |
| aaa_00 | — | **identical (5 of 5)** |
| gor_01 | max 243, mean 0.50, worst shot 2.2% of pixels > 8 | max 255, mean 1.58, worst shot 5.3% > 8 |

In gor_01 every differing pixel is on animated content: the walking NPC, the swinging rope and Goombella's idle animation. The static scene is identical. The game animates by time, and frames that finish sooner shift those animations.

**Counters** (per frame, `gxstats=60`, frames 1200-1260):

| | gor_01 | gon_02 | aaa_00 |
|---|---|---|---|
| Hit rate (of all list calls; the misses are state-only lists, never cached) | 95% (5169 of 5427) | 95% (2565 of 2698) | 93% (2624 of 2814) |
| Hits as a share of cacheable lists | 100% | 100% | 100% |
| Vertices replayed per frame | 28881 | 12663 | 13599 |
| Built, changed, dynamic, evicted, full per frame | 0 | 0 | 0 |
| Entries, memory | 10784, 6843 KB | 12189, 8191 KB (gon field lists still there, the budget is full, LRU working) | 6003, 3920 KB |

**Time** (game thread, Azahar ticks, `gxstats` on in both runs):

| | gor_01 off → on | gon_02 off → on | aaa_00 off → on |
|---|---|---|---|
| Display lists, inclusive | 210.2 → 174.3 ms | 99.5 → 84.3 ms | 105.3 → 87.8 ms |
| of which decode | 69.6 → 0 | 32.4 → 0 | 35.0 → 0 |
| of which cache lookup and validation | 0 → 43.5 | 0 → 22.7 | 0 → 22.3 |
| GX total (lists + immediate mode) | 237.3 → 201.4 ms (**-15%**) | 113.6 → 98.6 ms (**-13%**) | 114.9 → 97.4 ms (**-15%**) |
| Game frame | 267.8 → 234.1 ms (16 → 14 vblanks) | 134.1 → 117.6 ms (8 → 7 vblanks) | 134.0 → 117.6 ms (8 → 7 vblanks) |

The frame times are whole vblanks (the game waits for the next one), so the stage times are the measure. The validation costs about 8 µs per list call in Azahar ticks, which includes the counters' own `svcGetSystemTick` calls (three per call). That is two thirds of what the decode cost. The remaining cost is the hashing: about 70 list bytes and 36 array bytes per vertex. The PC samples below show its share without the counters.

**PC sampling** (`pc_sample.py --interval 200`, gor_01, no counters, same build, `gxcache=0` then `gxcache=1`). Azahar's stub slowed to about 1 Hz while other Azahar instances ran, so these are 81 and 46 game-thread samples: a direction, not a measurement.

| Game-thread self time | cache off (81 samples) | cache on (46 samples) |
|---|---|---|
| `draw_from_reader` (decode) | 21.0% | 2.2% |
| `run_commands` | 4.9% | 0% |
| cache: `gather_hash`, `gx_hash32`, `gx_dlcache_call` | — | 8.7% (plus part of `memcmp`, 6.5%) |
| `gx_draw_vertices` + `channel_color` (transform, unchanged) | 21.0% | 30.4% |
| `hash_bytes` (texture lookup in `build_state`) | 6.2% | 13.0% |
| waiting for the render thread | 16.0% | 6.5% |

The decode and parse (26%) become about 9-15% of checking. The transform, the state rebuild and the texture lookup are now the largest shares: they are steps 2 and 3.

**Ways to make a hit cheaper**, not done here because they trade correctness or need more measurement:

- Hash the array elements once per frame per array, not per list. Unsafe if the game rewrites an array between two draws in one frame; it would need a write signal from the game (a per-game hook such as "this memory is static") or `DCStoreRange` hooks.
- A trust window (Star3D: one full check, then none for 512 frames). Faster, but a list or array changed inside the window draws stale data. Not acceptable as the default under the "nothing stubbed, behaves like the original" rule; possible as an opt-in switch.
- Fewer, larger draws through (c) and (b), so the per-call overhead counts less.

## What the cache entries carry for the shader path

The shader path (b) has to meet the rules in [PORT_3DS_GPU_OFFLOAD.md](PORT_3DS_GPU_OFFLOAD.md): no new z-fighting and no seams. A cache entry today holds:

- the decoded vertices, exactly as `read_vertex` produced them (32-bit floats after the GX dequantisation);
- the draws in call order (primitive, first vertex, count), already split as the CPU path splits them;
- the attribute formats (type and `frac`) of the vertex formats used, and the array ranges.

For (b), an entry gets a second, optional part built from the first, in linear memory and inside the same budget:

- **Positions and texture coordinates as `s16` with one power-of-two scale per entry, only where that is exact.** For a fixed-point source (`GX_S16`, `GX_S8`, `GX_U8`, or `GX_U16` below 32768) the source value is the `s16` and the scale is `2^-frac`: exact by definition. For a float source the cache checks, for the entry's values, the largest number of fractional bits `k` any value needs and whether every `|v|·2^k` fits 32767. A power-of-two scale only moves the exponent, so the same source value comes out as the same 24-bit float whatever entry it is in: shared edges between entries stay identical (seam rule 4). An entry that fails the check keeps `float` attributes, never a rounded `s16`. `gxstats` logs how many built entries pass the check. In gor_01 (10122 entries built, 48851 vertices): **positions are exact in 10112 entries (99.5% of vertices)**, and **TEX0 is exact in all 8530 entries that have it**. So (b) can use `s16` streams for nearly everything, and floats for the few others.
- **Normals as `s16` x 2^-14** (exact for `GX_S16` sources, `s8` x 2^-6 for `GX_S8`), colours as `u8`.
- **`u16` triangle indices** for the entry's strips and fans, so one entry is one GPU draw, and the same index order as the CPU path's triangle assembly, so every pass of a multi-pass draw gets the same triangles.
- The per-draw matrices (model x view pre-multiplied in 32-bit on the CPU, camera-relative) are not part of the entry: they come with the call, so every draw that shares a matrix uses bit-identical uniforms.

## Risks of the cache

- **Hash collisions.** A 32-bit hash over the list bytes, and another over the array elements read. A changed list or array that happens to hash the same draws its old vertices (about 1 in 4 billion per change). `gxcache=2` checks by decoding again.
- **Memory.** Up to 8 MB of regular heap by default, plus 1.3 MB of static tables and about 1 MB of capture buffers. If `malloc` fails, the entry is dropped and counted (`alloc-fail`); the list is still drawn. gon_02 reached the budget, and LRU kept up (`full 0`).
- **Validation cost.** The cache saves less than the decode because every hit is fully checked (see Results). Large indexed meshes get cheaper per vertex; tiny lists stay dominated by per-call costs.
- **Not covered by a test here:** a list rewritten in place, or an array rewritten between frames, in a real scene. None changed in the four measured scenes. The code paths (changed, then dynamic after 4 changes) ran only in the verify mode's comparisons, not with real changes. A dedicated test would draw a list, change one byte of it and of its array, and expect `changed 1` and the new picture.
- **Moving GX translation to the render thread** (`PORT_3DS_MULTICORE.md`). The cache's state is module-private and owned by whichever thread translates GX. Its inputs come in through a `GxState` pointer (`gx_dlcache_call(st, ...)`), not the global, so a translator thread with its own `GxState` replica can own it. Two things would block a translation that runs up to a frame late:
  - It reads the list and the arrays **in game memory at translation time**, to hash and, on a miss, to decode. If the game thread has rewritten that memory in the meantime (the next frame's skinned arrays, a reused buffer), the translator sees the new bytes. That holds for the decode with or without the cache. The recorded call would have to carry a snapshot, or the game thread would have to keep the memory stable until the translator is done (double-buffered frames). The cache adds nothing new here, but its hash must be taken from the same bytes the decode reads.
  - `gx_dlc_capturing`, the index capture and the frame number (`gx_perf_frame_no`) are globals of the translating thread. They must move with it, never be shared by two threads.

## For PORT_3DS_TESTING.md

Rows for the switch table:

| Switch | Effect |
|---|---|
| `gxstats=N` | Every N game frames, logs `gxstats` lines with per-frame averages: display lists (calls, bytes, vertices, how many repeat unchanged from an earlier frame), immediate-mode primitives, draws, triangles and culled triangles, PICA state rebuilds, the render thread's draws and record bytes, the game thread's time per GX stage (list decode, state rebuild, transform, triangle assembly), the GX features the vertices use (lighting, texgen kinds, matrix indices, primitive types), and with `gxcache` the cache's hits and memory. Off by default; costs a few percent when on |
| `gxcache=0/1/2` | The decoded display-list cache (`port3ds/game/gx/gx_dlcache.c`): 1 (default) draws repeated lists from their decoded vertices after checking every input; 0 decodes every list every time (A/B); 2 decodes every hit again and compares it with the cache (`mismatched` in the `gxstats` cache line, and one warning line) |
| `gxcachekb=N` | The cache's memory budget in KB (default 8192) |
