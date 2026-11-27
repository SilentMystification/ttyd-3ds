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
2. **(c), the cheap parts:** the separate state version for matrix loads, the fog LUT cache and the unlit fast path (game thread, `gx_draw.c` and `gx_state.c`), and the draw merge in `plat/gpu.c` (render thread). Each is small and can be A/B-tested alone. Coordinate the texture lookup with the texture-hash work in `gx_tex.c`, since fewer rebuilds mean fewer lookups. Done, with trusted memory for the cache's validation: see "Results: the cheap state fixes" below (the state version is off by default until it saves time).
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

## Results: the cheap state fixes (step 2) and trusted memory

Each change is behind its own switch and was A/B'd on one build. Azahar, `gxstats=60`, frames 1200-1260, game-thread ms per frame. "Old" is the build before this work (`89eb121c`); "off" is every new switch off on the new build.

**Final default (`gxstatever=0`, everything else on) against the old build:**

| | gor_01 | gon_02 (battle) | aaa_00 |
|---|---|---|---|
| Game frame | 217.4 -> **183.9** (13 -> 11 vblanks) | 117.6 -> **85.3** (7 -> 5) | 117.6 -> **85.6** (7 -> 5) |
| Display lists, inclusive | 164.4 -> 126.9 | 80.1 -> 61.1 | 84.6 -> 64.7 |
| Cache lookup and validation | 43.5 -> **19.1** (-56%) | 22.7 -> **9.8** (-57%) | 22.3 -> **9.2** (-59%) |
| Transform | 57.2 -> **42.5** (-26%) | 26.8 -> 19.9 | 28.1 -> 20.6 |
| State (`build_state`) | 17.2 -> 17.4 | 7.8 -> 7.9 | 7.7 -> 7.8 |
| Assembly and recording | 22.3 -> 23.3 | 10.8 -> 11.3 | 10.8 -> 11.3 |
| Draw commands (of draws) | 3752 -> **422** | 1903 -> 177 | 2129 -> 232 |
| Record bytes | 362 K -> 324 K | 158 K -> 138 K | 198 K -> 176 K |
| Render thread | 43 -> **20** ms | 34 -> 6 ms | 34 -> 6 ms |

The frame time moves in whole vblanks (the game waits for the next one), so the stage times are the measure. The render thread's time varies a lot between runs (38-48 ms for the same gor_01 configuration).

**Per change, gor_01, cumulative** (on the build before the word-compare fix in item 5, so every column is comparable; the last column adds that fix):

| | off | + statever | + fogcache | + unlit | + merge | + trust | + word compares |
|---|---|---|---|---|---|---|---|
| Game frame | 217.3 | 234.1 | 234.1 | 217.4 | 217.4 | 200.6 | **183.9** |
| Display lists, inclusive | 167.3 | 170.4 | 170.4 | 155.0 | 154.9 | 138.2 | 127.6 |
| Cache lookup and validation | 44.3 | 44.2 | 44.3 | 44.3 | 44.3 | 27.5 | 19.0 |
| State | 17.5 | 24.2 | 24.2 | 24.2 | 24.2 | 24.2 | 19.6 |
| Transform | 58.7 | 58.7 | 58.7 | 42.4 | 42.4 | 42.4 | 42.5 |
| Rebuilds | 583 | 439 | 439 | 439 | 439 | 439 | 439 |
| Draw commands | 3752 | 3752 | 3752 | 3752 | 420 | 422 | 422 |
| Render thread | 38.7 | 50.2 | 49.7 | 49.8 | 22.5 | 23.2 | 19.5 |

gon_02 and aaa_00 give the same picture (unlit -28%, merge 1903 -> 177 and 2129 -> 232 commands, trust -57% and -60%).

**Screenshots** (`shots=100 shotmax=13`, frames 800-1200, `shotdiff.py`), with two runs of the old build as the noise floor:

| Scene | old vs old (noise) | off vs old | old vs final |
|---|---|---|---|
| gor_01 | max 254, mean 0.40, worst shot 1.1% of pixels > 8 | worst mean 2.46, 5.1% | max 239, mean 0.65, worst mean 0.93, 1.7% |
| gon_02 | (old vs off: worst mean 21.1, 67%: the battle's attack timing shakes the camera) | | worst mean 6.2, 23% |
| aaa_00 | identical | identical | **identical (5 of 5)** |

Every differing pixel in gor_01 and gon_02 is on animated content (the walking NPC, the rope, Goombella's idle, the battle's attacks), as in the noise runs; the static scene is identical. Each switch was also compared with the configuration before it, always within the same noise.

What each change did:

1. **State version without matrix loads, and a state snapshot (`gxstatever`, off by default).** The matrix change alone changed nothing: 583 rebuilds stayed 583, because the map objects call `GXLoadTexObj`, the TEV and blend setters again before each draw, mostly with the values in force. A snapshot of the state `build_state` reads (texgen to pixel engine without the lights, plus the viewport) then skips rebuilds that would produce the same state: **583 -> 439** (gor_01), 268 -> 188 (gon_02), 272 -> 239 (aaa_00). But the state stage got slower: 17.4 -> 19.2 ms (gor_01) and 7.9 -> 8.7 ms (gon_02) even with word compares and the lights skipped. The skipped rebuilds were the cheap ones (same textures, and a record the back end drops as identical), and comparing and copying about 1.3 KB per version change cost more. Next try: dirty bits that the setters set only when a value really changes. The floor is the 436 different states the render thread sees.
2. **Fog LUT (`gxfogcache`).** No measurable change in these scenes: they rebuild with fog seldom enough that the LUT's 129 `powf` and its hash did not show. Kept on; it costs one 5-value compare per rebuild with fog.
3. **Unlit fast path (`gxunlit`).** Transform -26 to -29% in all scenes. Every measured vertex is unlit; besides `channel_color`, the eye-space normal (a 3x3 product and a square root per vertex) is skipped when no lit channel reads it, and the "does a texgen read a colour" loop moved out of the per-vertex path.
4. **Draw merge (`gxmerge`).** 3752 draw commands -> 420 (gor_01). The map's strips under one state are contiguous once the GX layer gives back the vertices it reserved for culled triangles. The render thread's time halves or better; the game thread is unchanged.
5. **Word compares.** The display-list cache's per-call state check (`memcmp` of the vertex description and the attribute formats) and the state snapshot ran byte by byte, because the copies are not word-aligned. With `gx_mem_equal` (word loads, unaligned allowed on ARMv6) the cache stage went 44.3 -> 35.9 ms with trust off and 27.5 -> 19.0 ms with it on.

**PC sampling** (`pc_sample.py --interval 200`, gor_01, no counters; Azahar's stub ran at 1-3 Hz beside other instances, 176 and 89 game-thread samples, direction only): all switches off vs the final build: `memcmp` 4.0% -> 0 (it was 16% on the build before item 5), `gather_hash` 5.1% -> 1.1%, `hash_span` (texture check) 4.0% -> 0, `channel_color` 2.3% -> 0. `gx_draw_vertices` stays the largest (22%), then the wait for the render thread (13.6% -> 14.6%), then game code (`hitCheckVecFilter`, `strcmp` from `mapGetMapObj`).

### Trusted memory: cheaper display-list validation (`gxtrust`)

Generic registry: [`port3ds/include/plat/memstable.h`](../port3ds/include/plat/memstable.h), `port3ds/plat/memstable.c`. No game knowledge.

- **Covered memory** is a sorted table of ranges (adjacent fills merge) with three kinds of report:
  - `fill`, content now stable: `DVDReadPrio` (the bytes read), an ARQ transfer into main memory, `OSLink` restoring a REL's data;
  - `write`, coverage kept but trust revoked: `DCFlushRange`, `DCStoreRange` and their NoSync forms. The GameCube needs one of them after the CPU writes memory the GPU reads, so a correct GameCube program reports every GPU-visible CPU write (`smartGarbage`'s compaction ends with one over the whole heap);
  - `release`, coverage dropped: `__memAlloc`, `_mapAlloc`, `_mapAllocTail` (a block changes owner), `_mapFree`, `smartFree`, the free space after `smartGarbage`, `OSUnlink`, and `GXBeginDisplayList` (the CPU builds a list there).
- **Trust.** After a full check of an entry (the hash of the list and of every array element read) passes, the cache asks the registry about the list and each array's span (lowest to highest index read). If all of it is covered and no covered memory changed since the check began (an epoch counter read before hashing), the entry gets the current trust generation and the ranges become *relied on*. A trusted entry whose generation is still current is a hit without hashing. The state inputs (vertex description, formats, matrix indices, array addresses and strides) are still compared on every call.
- **Revocation.** A report that touches a relied range bumps the generation: every trusted entry is checked in full once more, then asks again. Reports on memory nobody relies on (staging buffers, the 60-500 flushes per frame of CPU-built data) only bump the epoch. A trust request that raced with a report fails and is repeated after the next epoch change.
- **Contract.** Covered memory changes only through the reported paths. A CPU store into a disc-loaded list or array with no flush or store after it is not seen. The GameCube's GPU would not see such a store either (unless the line left the cache by chance), so correct GameCube code does not depend on it. The verify mode is how a game is checked.

Measured: 95% of hits unhashed in gor_01, 98% in gon_02, 99% in aaa_00 (the rest are lists in uncovered memory, hashed as before); 71, 62 and 49 ranges covering 9-12 MB; no revocation while a scene runs. Validation now costs about 3.7 µs per list call in gor_01 (Azahar ticks, counters included), from 8.0 µs.

**Proof:**

- `gxcache=2` (every hit, trusted or not, is hashed, decoded again and compared) over `map_sweep.py --maps gor_01,tik_01,gor_01,hei_01,gon_02 --story 30 --frames 300 --switches "gxcache=2 gxstats=60"`: four REL relinks (`OSLink tik`, `gor`, `hei`, `gon`), the map changes, and the battle gon_02 starts on entry. **4,893,718 hits verified, 0 mismatched, 0 stale.** Each relink revoked trust once, as it must. A separate gor_01 run: 310,140 verified, 0 mismatched.
- `gxscribble=900`, which xors one byte of a trusted entry's position array with no flush and no report:
  - with `gxcache=2`: `stale 180, mismatched 180` per 60 frames (three lists read that element) and the warning "cached list ... decodes differently now". **Verify mode catches it.**
  - with `gxcache=1`: the scribbled entry stays trusted (60 trusted hits per 60 frames, nothing counted). **Normal mode draws the old vertices**, which is the documented contract.
  - with `gxcache=1 gxtrust=0`: "the full check saw list ... change (decoded again)". The hash catches it.

**Risks of trust:**

- A game that writes GPU data without a flush (or through an uncached mirror) draws stale lists until the next revocation. Run a new game's scenes once with `gxcache=2`; any `stale` count shows it.
- Port code that changes disc-loaded data after the read (the `*_swap.c` byte swaps) does so before the data is first drawn, so trust is only granted afterwards. Port code that patches loaded data later must report a write.
- REL data is covered from `OSLink`. Game variables in the same `.data` change without a report; that is safe because trust is per byte span (the lists' and arrays' own bytes). A REL that animated a static array in place without a flush would show as `stale` in verify mode; none did in the sweep.
- One LightLock; reports come from the game, DVD and ARQ threads. A full table (8192 ranges) drops a fill (counted), which only costs speed.
- Moving GX translation to another thread: trust is checked at translation time, like the hashes it replaces, so the late-read rule in the risks below applies unchanged.

## Design: the vertex-shader path against the offload rules (for review before (b) is built)

How option (b) meets each rule in [PORT_3DS_GPU_OFFLOAD.md](PORT_3DS_GPU_OFFLOAD.md). Nothing here is implemented yet.

**What goes through the shader.** A draw from a display list (cached or not) whose vertex pipeline is: one position matrix (no `PNMTXIDX`), the projection, colour channels unlit (material from the vertex or the register, as the `gxunlit` fast path selects them), up to 3 texgens that are `MTX2x4` from a `TEXn` coordinate (`MTX3x4` only on unit 0, where the PICA divides by q), no post-transform matrix, no bounding box armed, triangles, quads, strips or fans. That is every list vertex measured in gor_01, gon_02, aaa_00 and dou_05. Everything else stays on the CPU path, which stays the reference.

**The stream** (built from the cache entry once, in linear memory, inside the cache budget, freed only after the render thread's fence for the frame that last drew it):

| Attribute | Format | Exactness |
|---|---|---|
| position | `s16` x3, the entry's power-of-two scale folded into the matrix uniform | only when exact (99.5% of gor_01's vertices); otherwise `float` x3 |
| colour | `u8` x4 | the decoded `GXColor` bytes |
| texture coordinate n | `s16` x2 with a power-of-two scale, or `float` x2 | the same exactness check as positions (TEX0 exact in every gor_01 entry) |
| indices | `u16` triangle list | in the CPU path's assembly order (strip parity, fan order, quad split), so every pass of a multi-pass draw gets the same triangles |

Attributes the draw does not read become fixed attributes (no per-vertex cost, Star3D's measurement). The normal is not streamed until lighting moves (no measured draw reads it).

**Rule 2: matrices on the CPU in 32-bit, camera-relative.** GX's position matrix is already model x view (eye space), so its translation is the object's offset from the camera. Per draw the CPU multiplies, in 32-bit float, the projection (GX's 7-value form, as the CPU path applies it) by that matrix and by the stream's position scale, and uploads 4 rows (`c0`-`c3`); the shader does one `dp4` per clip component and nothing else to the position. Texture matrices: 2 rows per unit (`c4`-`c9`), the coordinate's scale folded in the same way. Every draw that shares a matrix gets bit-identical uniforms (the same 32-bit product, the same rounding to the PICA's 24-bit floats), which is what rules 1 and 4 need. The upload is skipped when the rows equal the last ones sent (the render thread already applies state that way).

**Rule 1: the fallback is per pass group, never per draw.** A *pass group* is keyed by the bits of the position matrix, the projection and the viewport's depth range (everything that decides where a vertex lands in depth). Every draw of a group in a frame takes the same path:

- The decision for a group in frame N is the one learned in frame N-1: a small table (group key -> "all draws were shader-eligible") filled while drawing. A group seen for the first time, or one with any CPU-only draw last frame (an immediate-mode decal, a lit draw, a line, a bounding-box draw), draws entirely on the CPU. A group switches to the shader only after a whole frame in which every draw of it was eligible, and switches back the first frame one is not; that switch frame draws the whole group on the CPU (a draw found ineligible mid-frame forces the group's remaining draws to the CPU, and the group is marked for the next frame).
- So a floor and the shadow or decal drawn on it with the same matrix and `LEQUAL` or `EQUAL`, the passes of one mesh, and map parts that share a matrix are never split between the paths. Immediate-mode draws stay on the CPU in the first version, so anything drawn with `GXBegin` under a list's matrix keeps that matrix's lists on the CPU too.
- Draws under different matrices already differ in their last bits on the GameCube and on the CPU path; the shader's error is the 24-bit quantum, about 1.5x10^-5 of the distance from the camera (camera-relative matrices keep it small near the player). The depth diff in the acceptance tests measures it per scene.

**Rule 3: depth.** The shader writes the same clip z and w as the CPU path (`cz = p5*z + p6`, `cw = -z` or 1), and the back end keeps today's `C3D_DepthMap(true, far - near, far)` with the same viewport near and far for both paths. W-buffering is a separate hardware experiment. Orthographic and 2D layers keep their exact order because a group's path never changes inside a frame and the census must show which path each layer took; layers drawn with `GXBegin` stay on the CPU in the first version anyway.

**Rule 4: seams.** The same vertex in two lists comes out the same when it has the same source bits, the same matrix and the same instructions: guaranteed by the lossless stream (a power-of-two scale only moves the exponent; an entry whose values are not exact keeps floats), the shared uniforms of a pass group, and a single straight-line position path in the shader (no branch before the position is written). Per-entry scales that are not powers of two are never used.

**Rule 5: output registers.** One shader program; the variants (texgen count, `MTX3x4` on unit 0, colour from vertex or constant) are chosen by boolean uniforms `b0`-`b7` (never `b15`), branches only compute temporaries, and after the join each output (`o0` position, `o1` colour, `o2`-`o4` the three texture coordinates, `o5` texcoord0 w when used) is written exactly once, from a temporary, with the full `.xyzw` mask. A build-time check parses the shader's assembly and fails when an output is written twice, partially, or inside a branch.

**Culling and the rest.** The shader path cannot cull on the CPU: it sets `C3D_CullFace` from `GXSetCullMode`, with the front face flipped once for the EFB rotation (a fixed rule, tested with gputest). The bounding box, lines and points stay on the CPU. The render thread gets a new record command (stream, index range, uniform rows); the draw merge does not apply to it.

**Acceptance** is the list in PORT_3DS_GPU_OFFLOAD.md (image, depth, seam and flicker diffs, coverage census), with `gxvsh=0/1` and the per-scene noise floor from two CPU-path runs.

**Additions (planner, before the build):**

- **Stereo-ready.** The uniform layout and the record command must allow drawing the same streams twice with a per-eye projection, without re-recording. Stereo itself is not built yet.
- **Hysteresis.** A group moves to the shader only after K fully eligible frames in a row (K = 30, `gxvshk`), and back to the CPU at once. `gxstats` counts the switches; a count that keeps climbing in a scene is a bug.
- **Decals under another matrix.** A `GXBegin` draw with depth compare EQUAL or LEQUAL and depth writes off (decal-like) forces every shader group drawn earlier in the same viewport and depth range onto the CPU for the next frame, with the same hysteresis. How often that triggers is measured; if it is most frames of a scene, that is reported, not tuned around.

## Built: the vertex-shader path (`gxvsh`, step 3)

Implemented on `port3ds` in `port3ds/game/gx/gx_vsh.c` (streams, pass groups, hysteresis, decal links, census), `gx_draw.c` (`gx_draw_vsh`: eligibility of the state and the uniform block), `gx_dlcache.c` (a stream per entry, the hit path), `plat/gpu.c` (record commands and replay) and `port3ds/shaders/gx.v.pica`. Default off (`gxvsh=0`); see "Results" below for why.

**Shader** (`gx.v.pica`). One program; bool `vsh` (b0) picks the path per draw. The CPU path's instructions are unchanged (rotation `dp4`s, colour scale). The shader path: `mul` by the stream's power-of-two scale, one `dp4` per clip component with the CPU's clip rows, colour as vertex x mix + constant (each product is x1 or x0, so exact), and per texture unit (slot x weight, 1, 1) dotted with the texture matrix's two rows. Branches compute temporaries; after the join each of the 5 outputs is written once, in full. `tools/port3ds/check_vsh.py` runs in the build (CMake target `ttyd_gx_shader_check`) and fails it on an output written twice, partially, inside a branch or outside `main`, or on `b15`.

**Streams.** Built once per cache entry when it is made (`gxvsh=1` only), in pool segments of 16384 vertices of one stride (linear memory, `gxvshkb`, default 6144 KB), and counted in the cache budget. Attribute order: floats, then s16, then the u8 colour, so every attribute is naturally aligned and the stride is a multiple of 4. Exactness: a fixed-point source keeps its own `frac` as the exponent (checked value by value), a float source uses the smallest exponent that makes every value an integer if those fit s16, else floats (`gxvshs16=0` forces floats). Indices are u16 from the segment base, in `gx_draw_vertices`' triangle order. A freed range is reused only after 3 presented frames.

Deviations from the design, and why:

- **The stream scale is a per-draw uniform (`vscale`, `texsel`), not folded into the clip rows.** Folding it in would make the uniform block differ per entry and stop merging. The result is the same bits: the shader multiplies the s16 value by 2^-k (exact in 24-bit float), and a power-of-two factor only moves the exponent of every product in the `dp4`. The scale word is part of the draw command; consecutive draws merge only when it matches.
- **Pass-group key:** position matrix, projection and the whole viewport (not only its depth range), so a group never spans two viewports. **Across frames** a group is known by its lists: every list's record keeps the streak of the group it was drawn in, and a group's path for the frame is read from its first list's record. A key of matrix bits alone would make every group new each frame while the camera moves.
- **Decals.** The planner's rule (an immediate-mode decal-like draw: every earlier group in its viewport goes to the CPU with it) is built as a link: the decal's group and those groups are one set at the frame's end, eligible only together, with one streak. A display-list decal under another matrix is linked the same way with the earlier list groups (immediate-only groups left out), so a list decal and its floor always take one path. `gxvshdecal=0` turns links off for measuring the path itself.
- **`MTX3x4` texgen stays on the CPU** (unit 0 would need the PICA's projective texture mode); no measured vertex uses it.
- **Culling** is the PICA's (`C3D_CullFace`), mapped from GX's mode with the screen rotation's determinant. The CPU path does not cull a triangle with a vertex behind the eye; the PICA culls after clipping, as GX does. Only back faces that cross the near plane can differ.

**Stereo (planned, not built).** The record stays eye-independent. CPU-path vertices are GX clip positions, and a shader draw's `CMD_VSH_CONST` carries the unrotated GX clip rows (projection x position matrix) and `PLAT_VSH_ORTHO`; the render thread composes the screen rotation at replay (`rot0`/`rot1` for the CPU path, the clip rows for the shader path). A stereo frame replays the same record twice, once per eye target, composing a per-eye clip-space shear before the rotation: x' = x + a·z + b·w, with a and b from the 3D slider, the eye distance and the convergence depth for perspective draws, and a = 0 with a fixed layer offset b (or none) for orthographic ones. For the shader path that is row_x' = row_x + a·row_z + b·row_w, in 32-bit on the render thread; for the CPU path the same shear goes into `rot0`. The z and w rows are untouched, so both eyes keep the same depth, and every draw of a pass group still gets one set of uniforms per eye, so the seam and z-fighting rules hold per eye. Still to do for stereo: the CPU path's `CMD_DRAW` must carry the projection type (one bit) so an orthographic layer gets its own shear; a second EFB target and its present; and per-eye EFB copies.

## Results: the vertex-shader path

Azahar, build of 2026-11-22. `tools/port3ds/vsh_accept.py` per scene: the frame clear set to magenta (`gxclearrgb=FF00FF`), 24 consecutive shots from presented frame 1140 with their depth buffers (`gxdepthshot`), `gxstats=60`. Runs per scene: two CPU-path runs (`gxvsh=0`, the noise floor), the shader path with decal links off (`gxvsh=1 gxvshdecal=0`, which measures the path's own precision), and the shader path as it would ship (`gxvsh=1`). The game animates by time, and the shader runs have other frame times, so whole-frame diffs differ on every animated object; the precision tests use the **static pixels** (colour and depth unchanged over all 24 shots of both runs), and the flicker test counts A-B-A flips at pixels the CPU run keeps static, with the depth of the two frames within 16 units (a near-coplanar surface winning, not an animation).

**Precision (links off), per scene:**

| Scene | static drawn pixels | colour max diff | depth \|dz\| max (24-bit units) | dz 1-3 / 4-15 / >= 16 | order flips | seams (CPU / shader) | flicker (CPU-B / shader-default / links off) |
|---|---|---|---|---|---|---|---|
| gor_01 | 267,920 | 2 | 4 | 2817 / 4 / 0 | 0 | 0 / 0 | 0 / 4 / 4 |
| gon_02 (battle) | 285,076 | 2 | 4 | 243 / 1 / 0 | 0 | 0 / 0 | 0 / 0 / 0 |
| aaa_00 | 291,840 | 145 (1 pixel) | 4 | 5465 / 16 / 0 | 1 pixel | 0 / 0 | 0 / 0 / 0 |
| tik_01 | 278,578 | 1 | 4 | 1461 / 6 / 0 | 0 | 0 / 0 | 0 / 2 / 3 |
| hei_01 | 256,246 | 2 | 2 | 627 / 0 / 0 | 0 | 0 / 0 | 0 / 1 / 1 |
| jin_00 (fog, effects) | 291,840 | 58 (1 pixel) | 4 | 7246 / 17 / 0 | 1 pixel | 0 / 0 | 0 / 0 / 0 |

- CPU vs CPU is bit-identical on static pixels in every scene (colour and depth).
- The shader path moves depth by at most 4 units of 2^24 (one or two 24-bit-float quanta at these depths), no cluster of depth differences, no seam (no magenta inside geometry on either path), no new flicker: the flicker pixels of the links-off runs are on animated characters and appear identically in the default run of gor_01, tik_01 and hei_01, which draws everything on the CPU there.
- **One pixel each in aaa_00 and jin_00 changes owner** (aaa_00 (130, 129): a bright and a dark face meeting at an edge, depth 2 units apart; stable in all 24 frames, no flicker). That is a rasterisation tie on a shared edge between two faces at a crease, decided by the vertices' last bits. The offload rules say a cluster along an edge fails; a single stable pixel is listed here for the owner to judge.

**The decal rule as specified triggers in most frames, so it is reported, not tuned:**

| Scene | frames with an immediate-decal trigger (of 60) | what draws it | effect in the default `gxvsh=1` run |
|---|---|---|---|
| gor_01 | 60 | `animdrv.c` `renderProc` (characters, LEQUAL, no depth write) | all 207 eligible groups stay on the CPU |
| tik_01 | 60 | `animdrv.c` `renderProc` | all 27 eligible groups on the CPU |
| jin_00 | 60 | `animdrv.c` `renderProc` | all 61 eligible groups on the CPU |
| hei_01 | 8-10 | `eff_fukidashi.c` `effFukidashiDisp`, `eff_kemuri.c` `effKemuriDisp` (intermittent) | **the set of 478 lists keeps switching**: 478, 0, 0, 478, 956, 0, 0, 478 switches per 60-frame window, 2390 by frame 1260 (a count that keeps climbing) |
| gon_02 | 0 in the battle (2-39 per window during its opening) | `renderProc` | 91 groups on the shader; 3 whole-set transitions (2521 lists each) during the opening, then 0 |
| aaa_00 | 0 | — | 126 groups on the shader, no switch after the first |

Characters are drawn in immediate mode after the map with depth compare LEQUAL and depth writes off, in every field scene, so the rule (every earlier group of the viewport to the CPU) keeps whole maps on the CPU. In hei_01 an intermittent effect makes the map set flip between paths every 30-60 frames, each flip shifting its depth by up to 4 units; that is the oscillation the hysteresis was meant to prevent, caused by the rule itself. The links of display-list decals (the extension) never triggered a CPU fallback here: list decal sets were always eligible together.

**Speed** (game-thread ms per frame and stages, `gxstats` on; Azahar):

| Scene | frame CPU / links off / default | dl stage | transform + assembly | render thread | GPU draw commands of shader calls |
|---|---|---|---|---|---|
| gor_01 | 168.0 / **133.8** / 183.9 | 120.5 / 74.2 / 128.9 | 65.5 / 3.8 / 65.3 | 11.4 / 26.0 / 18.5 | 315 of 5169 |
| gon_02 | 83.6 / **66.9** / **66.9** | 57.9 / 36.8 / 36.8 | 31.0 / 2.3 / 2.3 | 9.6 / 14.8 / 15.1 | 142 of 2565 |
| aaa_00 | 83.6 / **66.9** / **66.9** | 61.5 / 39.2 / 39.2 | 31.7 / 1.5 / 1.5 | 9.3 / 16.0 / 16.0 | |
| tik_01 | 50.2 / 44.0 / 50.2 | 22.1 / 14.0 / 24.0 | 13.1 / 2.0 / 13.2 | 12.2 / 19.5 / 10.1 | |
| hei_01 | 50.2 / 50.2 / 50.2 | 19.3 / 14.4 / 18.6 | 9.4 / 2.4 / 7.0 | 11.9 / 16.8 / 18.4 | |
| jin_00 | 72.5 / **66.9** / 83.6 | 41.5 / 26.5 / 44.5 | 24.3 / 4.1 / 24.3 | 19.1 / 19.4 / 16.7 | |

- Where the shader path runs, the transform and assembly stages drop by 85-95% and frames drop by 1-3 vblanks (gor_01 -20%, gon_02 and aaa_00 -20%).
- The render thread grows (more uniform and buffer setup per draw command); on hardware each citro3d draw costs 36-66 µs, so the 142-315 shader commands per frame need measuring there.
- PC sampling (`pc_sample.py --interval 200`, gor_01, 90 s from the warp, no counters, about 265 stops each; direction only): `gxvsh=0` against `gxvsh=1 gxvshdecal=0`. Game thread: `gx_draw_vertices` 18.0% -> 3.4% of samples, waiting in `submit` 16.9% -> 12.8%; new costs: `memcmp` 9.1% (mostly `plat_gpu_vsh_const` comparing the 160-byte uniform block on every call), `gx_dlcache_call` with the inlined `gx_draw_vsh` 9.1%, `memcpy` 4.2% (indices into the record), `gx_vsh_route` 2.6%. Render thread busy 24.4% -> 32.8% of stops. The first two are the next things to cut (send the block only when it was rebuilt; skip the group lookup while the matrix version is unchanged).
- **When every group stays on the CPU (the default run in gor_01, tik_01, jin_00), routing costs time**: gor_01 +16 ms per frame (dl stage +8.4 ms: the per-call group and record lookups, the state preparation, then the replay as before), jin_00 +11 ms.

**Default: `gxvsh=0`.** The shader path's own precision passes in all six scenes (two single stable edge pixels listed above), but as specified it does not run in gor_01, tik_01 or jin_00 (the character decal rule), oscillates in hei_01, and costs 10-16 ms per frame where it does not run. Options for the planner (none built): narrow the rule to decals that can be coplanar with a group, for example by comparing the decal's screen rectangle and depth range with each earlier group's; or move the immediate-mode characters (`animdrv`) to the shader path (step 4), so a character decal and the floor share the path; or skip the routing work entirely for a viewport once an immediate decal has appeared in it this frame, to remove the cost where nothing can move.

**Hysteresis and splits.** `gxvshk=30`. Steady state: 0 switches and 0 splits in every scene except hei_01's default run (above). Switches count list records, so one set transition counts every list in it (2521 in gon_02).

## Round 2: immediate mode on the shader, narrower decal rule, default on

Planner's round-2 brief: put immediate-mode draws on the shader path too, so characters and the map share a path; link groups only for a decal drawn on the CPU path; replace the per-draw uniform-block compare; rerun the suite plus an effects-heavy battle; switch the default to 1 only if every test passes in every scene with no oscillation and no frame slower than the CPU path.

**What changed:**

- **Immediate mode and decoded lists on the shader path.** `GXBegin`/`GXEnd` draws (`animdrv` characters, effects, windows) and display lists being decoded (first call, changed or uncacheable) write their vertices to a per-frame ring: 8 chunks of 16384 vertices in linear memory, one layout (float position, three float slots, u8 colour), a chunk reused 3 presented frames after it was last written. They take the same pass-group decision as cached lists. Across frames an immediate draw is known by the code that began it, a decoded list by its address (as its cached hits are). Lighting, lines, points, the bounding box, per-vertex matrix indices and `MTX3x4` texgen stay on the CPU. `gxvshimm=0` keeps immediate draws on the CPU.
- **Decal rule.** A decal-like draw (depth EQUAL, or LEQUAL with no depth write) links groups only when it is itself on the CPU path; it links the earlier groups of its viewport that take or could take the shader path. A linked set takes the largest streak of its members, so a new decal over a floor already on the shader joins the floor's path instead of pulling the floor back to the CPU for K frames (with the smallest streak, hei_01 dropped its map set every time a speech bubble or smoke appeared).
- **Uniform block id.** `gx_draw.c` compares a rebuilt block with the last one once per rebuild and gives it a new id only when it changed; `plat_gpu_vsh_const(c, id)` compares ids. The 160-byte `memcmp` per draw is gone.
- **Stream flushes** are batched: `plat_gpu_stream_commit` records the range, and the ranges are flushed once before a record is submitted. A `GSPGPU_FlushDataCache` per immediate draw had made the immediate stage slower than the CPU path (gor_01 17 -> 40 ms).

**Results** (Azahar; `vsh_accept.py`, 24 consecutive shots from presented frame 1140, magenta clear, depth shots; two CPU runs, one `gxvsh=1` run):

| Scene | frame CPU -> gxvsh=1 (ms) | groups on the shader | static px: colour max / depth max | seams | flicker (CPU-B / shader) | steady-state switches |
|---|---|---|---|---|---|---|
| gor_01 | 168.0 -> **117.0** | 395 of 395 | 2 / 4 | 0 / 0 | 0 / 0 | 0 |
| gon_02 (battle) | 83.6 -> **66.9** | 180 of 180 | 1 / 4 | 0 / 0 | 0 / 0 | 0 |
| aaa_00 | 83.6 -> **66.9** | 191 of 191 | 145 (1 pixel) / 4 | 0 / 0 | 0 / 0 | 0 |
| tik_01 | 50.2 -> **33.5** | 98 of 98 | 1 / 4 | 0 / 0 | 0 / 26 px (see below) | 0 |
| hei_01 | 50.2 -> 50.2 (vblank-bound; dl stage 19.3 -> 13.7) | 201 of 201 | 2 / 2 | 0 / 0 | 0 / 0 | 0 |
| jin_00 (fog, effects) | 72.5 -> **50.2** | 135 of 135 | 58 (1 pixel) / 4 | 0 / 0 | 0 / 0 | 0 |
| gon_02 + `btlauto=perfect` (attacks, effects) | 84.2 -> **67.0** | 192 of 194 | (camera moves: no static pixels) | 0 / 0 | 0 / 0 | 0-6 per window, all up |

- Static-pixel depth differences stay within 4 units of 2^24 (one or two 24-bit-float quanta), with no cluster and no order flip; the single stable edge pixels of aaa_00 and jin_00 are the same as in round 1.
- The effects battle is deterministic between runs in game frames for one of the CPU runs: `gxvsh=1` against that run (cpuB) differs in 3 pixels over 24 frames (single edge pixels, frames 1151 and 1156), with depth within 15 units on 99.99% of pixels. The other CPU run (cpuA) differs from both by the battle's own timing (mean 5, 12.6% of pixels > 8).
- tik_01's 26 flicker pixels: an animated object whose colour changes at one depth in the first five shader frames and that is gone after; the CPU run is at another phase of it (the shader run's frames are 33 ms against 50 ms). Characters' idle animations give the rest. No two surfaces alternate.
- **Decal triggers:** 0 per frame in the steady state of every scene. CPU-path decal-like draws: 0 per frame in all seven scenes once groups settle; the triggers that remain happen while a new group counts up (eligible, linked, then promoted with its set).
- **Switches:** after a map's first K frames, 0 down-switches in every scene; the only ups are records seen for the first time (a new effect or window). hei_01 (round 1: 478, 0, 0, 478, 956, 0, 0, 478 per window) now 572 at the map load, then 1, 1, 0, 0, 3, 0, 0, 0, all up.
- **Render thread** (Azahar, ms): gor_01 11.4 -> 23.4, gon_02 9.6 -> 20.5, aaa_00 9.4 -> 22.5, tik_01 12.0 -> 7.7, hei_01 11.5 -> 20.7, jin_00 3.7 -> 7.4, effects battle 9.9 -> 21.3. It stays well under the game thread in every scene here; on hardware each citro3d draw costs 36-66 µs, and gor_01 now issues about 570 shader draw commands per frame (of 5560 calls), so the render thread is the first thing to measure on the console.

- **PC sampling** (gor_01, `gxvsh=1`, 170 stops, direction only): the uniform-block `memcmp` is gone (9.1% in round 1). The game thread's largest GX entries now are `gx_dlcache_call` (9.4%, with the inlined routing), `GSPGPU_FlushDataCache` from the per-record stream flush (8.2%; the next thing to cut: flush only ranges written since the last submit, which it does, so this is the syscall's own cost in Azahar), `rec_for` (4.7%), `memcpy` of indices (3.5%) and `gx_vsh_frame` (3.5%).

**Default: `gxvsh=1`.** Every test passes in every scene above, with no oscillation, and no frame is slower than the CPU path (Azahar). Not verified on hardware: the stream attribute layout, fixed attributes, index offsets, the cull mapping (set empirically in Azahar), the bool uniform, and the render-thread cost. `gxvsh=0` is the fallback for A/B and for any hardware problem.

## Round 3: the draws left on the CPU (lighting, texgen sources, the bounding box)

**Census.** In the seven acceptance scenes (gor_01, gon_02, aaa_00, tik_01, hei_01, jin_00, the effects battle) no draw is on the CPU path any more at frame 1200-1260: `xf draws 0`, no matrix index, no lit vertex, no `MTX3x4`, no line or point, no armed bounding box. A sweep of 22 further maps (`gxstats`, the new line `vsh census since boot`, CPU-path draws by reason, story 30, 300 frames each) found where the rest is:

| Reason | Where | CPU-path draws |
|---|---|---|
| GX lighting (`L`) | usu_00 (lit map materials, `mapdrv.c` `mapSetMaterial`: colour 0, up to 8 lights, diffuse clamp, spot or no attenuation, ambient from the register, alpha unlit); gor_00 for about 120 frames after the warp | about 3500 a frame in usu_00 |
| texgen source not a texture coordinate (`r`) | pik_00 (`GX_TG_POS`, `MTX2x4`) | 2458 a frame |
| bounding box armed (`B`) | from eki_00 on, every later map: `GXClearBoundingBox` armed the box and nothing disarmed it, so every draw stayed on the CPU for the rest of the run | 969 a frame in eki_00, up to 10000 in las_00 |
| per-vertex matrix index (`P`), immediate (`i`) | win_00 (`himo.c`) | about 90 a frame |
| `MTX3x4`, lines and points, post-transform matrices | none seen | 0 |

**What moved** (each behind its own switch):

- `gxvshlit=1` (default): colour channel 0 lit in the shader exactly as `light_channel` does it, in eye space: the CPU's 32-bit position and normal matrices as uniforms (the normal normalised in the shader), up to 8 lights packed in mask order, diffuse none/clamp/sign, spot or no attenuation, the result floored to the integer the CPU path truncates to. Cache streams and the immediate ring carry the normal (float). Lit alpha, specular, ambient from the vertex and channel 1 stay on the CPU.
- `gxvshtg=1` (default): texgen from the position or the normal (object space, as `texgen()`), and `MTX3x4` with the divide by q per vertex (q = 0 left undivided, as on the CPU); the factor is exactly 1 for `MTX2x4`, so existing draws do not change (gor_01 under `determ`: every pixel of 24 shots identical to the build before). Post-transform matrices stay on the CPU.
- `gxvshbbox` (default 0, see below): an armed draw takes the shader path like any other and the box gets its vertices' window positions from `xf_position`, the position part of `transform()` (shared code, so the same arithmetic); `GXReadBoundingBox` disarms the box (every TTYD read follows its own `GXClearBoundingBox`). `gxvshbbox=2` keeps the old box beside it (every draw since the clear, through `transform()`) and compares at each read.
- **Not moved:** the matrix palette (win_00's 90 small immediate draws): 10 clip-row sets are 40 uniforms, which with the lights exceed the 96; lines and points (none seen).

**Acceptance** (`vsh_accept.py` method, 24 shots from frame 1140, `determ`, so two runs of one build are identical and any difference is the change):

| Scene | Switch | Static pixels: colour max / depth max | Order flips (static) | Seams | Flicker | Frame (`gxstats` on) |
|---|---|---|---|---|---|---|
| usu_00 | `gxvshlit` 0 -> 1 | 2 / 4 units | 0 | 0 / 0 | 0 | 235 -> **102** ms |
| pik_00 | `gxvshtg` 0 -> 1 | 1 / 4 | 0 | 0 / 0 | 0 | 172 -> **140** ms |
| eki_00 | `gxvshbbox` 0 -> 1 | 65 (86 px > 8) / 4 | 6 | 0 / 0 | 0 | 50 -> **33** ms |
| gor_01 | round 3 build vs the build before | identical (every pixel) | | | | |

- The box itself is exact: `gxvshbbox=2` in eki_00, 46 reads, **0 mismatches**.
- eki_00's 86 differing static pixels are one row along a wall edge (y 388-391) and one pixel, stable over all 24 frames: the edge between two faces is owned by the other face, from the 24-bit transform. That is the shader path's own precision, which eki_00 now gets because its map no longer stays on the CPU; the offload rules fail a cluster along an edge, so `gxvshbbox` stays 0 until the owner judges it (1 fixes the never-disarmed box and takes eki_00 from 50 to 33 ms).

**The box disarms in every mode now** (planner decision): `GXReadBoundingBox` disarms with `gxvshbbox=0` too, so only the draws between a clear and its read stay on the CPU. eki_00's map then takes the shader path in the default configuration, and its acceptance run (`gxvsh=0` against the default, `determ`) shows the same row as above.

**What the eki_00 row is.** Over the 24 shots, 2064 pixels differ by more than 8, and **every one of them has a bit-identical depth** in both runs: no second surface is involved, so this is not z-fighting, not a decal and not a pass-group split (the "order flips" count of `vsh_accept.py` counts colour > 32 at dz <= 16, which includes dz = 0). At x = 550 the rows 388-389 change colour with the depth unchanged (16743543 and 16743443 in both runs): the same wall face samples the neighbouring texel row. Since the positions (and so the depth) agree, the difference must be in the texture coordinate: most likely the shader's 24-bit texture-matrix product against the CPU's 32-bit product rounded once to 24 bits (the stream's coordinates themselves are exact s16 values; not yet confirmed per draw), and this wall has a texel boundary that falls on pixel centres along a whole screen row, so the last-bit difference picks the other row for every pixel of it. It is a texel choice at an exact tie, not a geometric seam. Fixing it to bit-identity would need the CPU's rounding of the texture coordinate (the product computed before the 24-bit rounding); options for the planner: compute the texture coordinates of such draws on the CPU (a per-group texgen fallback, at a cost), or accept texel ties as a precision class separate from the seam and z-fighting rules.

## Texture trust (`textrust`, `texverify`)

Every cached texture used to be hashed on its first use in each frame (gor_01: 181 textures, 666 KB a frame, `hash_span` about 6% of the game thread) with 0 mismatches. `gx_tex.c` now uses the same content-stable registry as the display-list cache (`port3ds/plat/memstable.c`).

**Design.**
- A `CacheEntry` carries `trust_gen` and `trust_epoch`. The epoch is read before the hash that validates the entry (the hash at upload, or the first-use check of a frame). When the hash agrees with the converted texture, `plat_memstable_trust` is asked for the spans {image, `GXGetTexBufferSize` bytes} and, for a CI texture, {TLUT data, entries * 2}. It grants a generation only if every span lies in covered memory (memory the disc filled: `DVDRead` and friends call `plat_memstable_fill`) and nothing covered changed since that epoch.
- Later frames: `trust_gen == plat_memstable_gen()` means no covered memory that someone relied on was written, flushed (`DCFlushRange` -> `plat_memstable_write`), released or refilled since. The lookup then skips both hashes and uses the cached conversion. Any report touching a relied range bumps the generation, so every trusted texture falls back to the hash on its next first use (and asks for trust again once its hash agrees; `trust_epoch` stops a refused request from being repeated until covered memory changes).
- Textures the registry does not cover are never trusted and keep the per-frame hash: memory the CPU wrote that no disc read filled (presumably fonts, generated images, TLUTs in static data; not identified by address). Big textures (over 32 KB) were hashed by 64 sampled spans before; they are trusted the same way.
- A texture the CPU rewrites in covered memory without `DCFlushRange` is the one case trust cannot see. The per-frame hash would have caught it; with trust on only `texverify` does. `texverify=1` (like `gxcache=2`) hashes anyway and compares: a trusted texture whose hash changed is logged (`texverify: trust skipped a change`), counted as `stale`, and converted again. `textrust=0` switches the whole thing off.
- The glue outside `gx_tex.c` is none: the registry API and the existing hooks (`DCFlushRange`, DVD fills, memory release) are unchanged. The shader and TEV agents' files are untouched.

**Numbers** (Azahar, `gxthreadstats=60 texstats=60`, the 7 windows from frame 1141, one run each, `textrust=0` against the default; these were taken before the owner's short-run rule, the identity and verify runs below are short):

| Scene | hashes / bytes per frame, before -> after | game thread busy | frame |
|---|---|---|---|
| gor_01 | 188.7 / 675.7 KB -> 0 / 0 | 72.3 -> **68.0** ms (-6.0%) | 83.6 -> 80.8 |
| gor_00 | 95.0 / 365.7 KB -> 0 / 7 B | 38.2 -> **35.9** (-6.0%) | 50.2 -> 50.2 |
| battle (gon_02, `btlauto=perfect`) | 89.7 / 324.6 KB -> 0.1 / 246 B | 35.2 -> **33.1** (-6.0%) | 49.6 -> 41.8 |
| aaa_00 | 62.0 / 231.6 KB -> 0 / 0 | 35.2 -> **33.8** (-4.0%) | 50.2 -> 47.4 |
| pause menu | 76.0 / 146.3 KB -> 30.0 / 24.7 KB | 18.8 -> **18.1** (-3.7%) | 33.4 -> 33.4 |

The pause menu keeps 30 hashes a frame: 46 of its 76 textures are trusted, the other 30 are in memory the registry does not cover (presumably written by the CPU, not read from the disc), and none of them changed (0 mismatches in `texstats`). The busy gain is what `hash_span` cost in the profile (5-6%); frames are quantised to vblanks, so the battle's frame time drop (a whole vblank) is partly luck.

**Identity.** `determ shots=1 shotmax=4` with `textrust=0` against `textrust=1`: gor_01 (frames 780-783) 4 of 4 byte-identical, gon_02 battle (frames 1040-1043) 4 of 4. (Consecutive frames differ within each run, so the shots do show motion.)

**Verify sweep** (`texverify=1 texstats=30`, to frame 841, 1101 for the battle; `trusted` counts the trust decisions the hash re-checked): gor_01 **0 stale** (about 160 trusted decisions per frame checked), gon_02 battle 0 (88 per frame), usu_00 0 (96), pik_00 0 (114), aaa_00 0 (62), the pause menu 0 (94 per frame at the end, 46 steady), `gallery.py fx --items 0-8 --gap 40` (effects on gor_01) 0 (152 per frame). No `texverify:` line in any log. A full-length sweep (frame 1500, `fx 0-20`) was not run: the owner asked for short runs.

## Core-2 round 2: plan (2026-11-23)

Owner goal: GX translation is GPU work done on the CPU, and the bulk of it should disappear. The end state is not "translation on core 2". It is "almost no per-frame translation on any core".

### Profile that justifies it

Azahar, build `2eee30a9`, `gor_01` story 30, default switches (`gxthread=0`), `pc_sample.py` on a private port, 238 stops from frame 721 (a share is +-3 points: a ranking, not a measurement). One percent of the game thread's stops is about 0.84 ms of the 83.6 ms frame.

| Game thread, by work | Functions (self) | Share |
|---|---|---|
| **Cached list hit, per call** | `gx_call_list` with the inlined `gx_dlcache_call` and `gx_draw_vsh` 14.7, `memcpy` of indices into the record about 6, `gx_vsh_route` 3.4, `rec_for` 1.7, `GXCallDisplayList` 1.7, `plat_gpu_vsh_draw` 1.3 | **about 29%** |
| **State rebuild, per state change** | `plat_gpu_set_state` 5.9 (two 700-byte compares and the delta scan), `ensure_state`/`build_state` 4.6, TEV compile 7.1 (`tev_compile_pass`, `plan`, `color_arg`, `alpha_arg`), `memset` 2.5, `vsh_prepare` 1.3, the state lists' port opcodes and setters about 6 (`run_commands`, `gx_apply_call`, `GXSet*`) | **about 29%** |
| Immediate mode | `draw_from_reader` 3.4, `gx_draw_vertices` 2.1 | about 6% |
| Waiting (retrace, submit) | `svcArbitrateAddressNoTimeout` | 12.6% |
| Game code | `C_MTXConcat` 4.2, `hitCheckVecFilter` 3.8, `mapDisp` 2.1, others | about 23% |

Render thread: 37% busy. `gspSubmitGxCommand` 11.3, `GPUCMD_Add` 8.4, `C3D_UpdateUniforms` 4.6 (it scans all 96 uniform rows at every draw), `vsh_apply_const` 4.2, `memcpy` 2.9.

Counters per frame (`gxstats`, same scene): 5252 list calls (5007 cache hits, 245 state lists), 5378 shader draws that merge into **535** GPU draws, 545 state builds giving 405 different PICA states, 463 uniform blocks, 369 immediate primitives with 1402 vertices.

What this says:

1. A hit costs about 5 us (Azahar) of pure bookkeeping, 5000 times a frame: a hash lookup, a vertex-state compare (the map sets its arrays before every mesh, so `gx_vtx_version` changes and every hit runs the full compare), LRU links, a pass-group record lookup and touch, and a copy of its indices into the frame record. None of it produces anything new: the same 535 runs of lists come every frame.
2. The state path is as large as the hit path. Every setter bumps the state version, the next draw rebuilds all of the PICA state (TEV compile included) and compares 700 bytes twice to find that most of it is what it was.
3. With `gxthread=2` both costs move to core 2 and get two more hops (record a call, consume it). That is why core 2 is the wall there. Making the hops cheaper does not remove the work; removing the work makes the hops unnecessary.

### End state

| Thread | Per frame |
|---|---|
| Game (core 0) | GX calls do the minimum. A setter compares and stores. A list call is a pointer compare against the prebuilt batch it is expected to continue. `GXEnd` writes its vertices straight into linear memory in PICA layout. At each state change or batch end, a few words go into the frame's command stream. |
| Render (core 2) | Copies prebuilt PICA register blocks into the GPU command buffer, submits, presents, draws the stereo eyes. No citro3d state calls per draw. Final step: the game thread writes the PICA words itself and the render thread only submits them. |
| Translator (`gxthread=2`) | Gone. `gx_rec.c` (the call ring, the shadow state, the array snapshots, the fences for late reads) exists to move translation; with translation at pointer cost it only adds two hops. It stays as a switch until stage 5 is the default on hardware, then it is deleted. |

### Stages (each behind its own switch, default flips only by the acceptance rules below)

**Stage 1, `gxbatch`: prebuilt batches of cached lists.** A *batch* is a run of consecutive cache hits with no GX state, matrix or vertex-state change between them (the packed lists of one map mesh). It is built once and holds:

- the member lists (address, size, cache entry and its generation);
- the vertex state the members were validated under (description, formats of the formats used, current matrices, array bases and strides), one copy for the batch;
- the GPU draws the members merge into today (stream, layout, scale word, index range), with the **indices of all members concatenated once in linear memory** (`gxbatchkb`, default 512 KB);
- running totals per member (vertices, indices), so a batch that ends early draws a prefix.

Per frame, a list call that continues the active batch costs: three version compares, the bounding-box flag, and a compare of the list's address and size with the next member. The batch is validated once when it opens:

- no cache entry changed since the batch last checked (one global epoch, bumped by any entry drop, rebuild or stream free; on a change the members' generations are checked once);
- every member is trusted at the registry's current generation (`gxtrust`); a batch with a member that is not trusted is never used, its members take today's path and are hashed as today;
- the vertex state equals the batch's copy (one compare per batch, not per list);
- the texture-coordinate mapping of the state (which GX attribute feeds each PICA unit) is the one the batch's scale words were built for;
- every member's pass-group record says "shader" (one global epoch for decisions, bumped by every switch and record eviction).

When the batch ends (a state change is seen at the next draw, another list comes, or the frame ends) its draws go into the record as `CMD_VSH_DRAWP` (stream, layout, scales, cull, index pointer, count): no index copy, one command per merged draw. Anything that fails a check takes today's per-list path unchanged, and that path is also where batches are formed.

- **Pass groups (offload rule 1).** Unchanged decisions. A batch is used only when all its members already take the shader path, in a group whose path this frame is the shader. It records one touch (batch, group). At the frame's end a touch whose group set stayed eligible needs nothing: every member's streak is already at or above K and only grows. A touch whose set became ineligible is expanded into its members' touches, so their streaks drop to 0 exactly as today, and the decision epoch changes. Records of a live batch are not evicted.
- **Precision and texel rules.** Nothing changes: the same streams, the same uniform rows from the same 32-bit products, the same index order. The GPU gets the same vertices in the same order under the same state, so the output must be byte-identical to `gxbatch=0`, not merely within the offload tolerances.
- **Stereo.** The record stays eye-independent. `CMD_VSH_DRAWP` is a scene or layer draw to `stereo_split` like `CMD_VSH_DRAW`; the 2D tail replays twice from the same prebuilt indices.
- **`gxthread`, pipelining.** Batches are owned by whichever thread translates, like the cache. They work in every `gxthread` mode; pipelined, the translator gets the same saving.
- **Bounding box.** A batch is not used while the box is armed (those draws need positions on the CPU).
- **Memory.** Linear: the index pool (512 KB; gor_01 draws 55 K indices a frame, 110 KB). Ranges of dropped batches are reused after 3 presented frames, as streams are. Heap: 2048 batch slots and the member arrays, allocated with `gx_big_alloc`. No VRAM. Batches unused for two frames are dropped least recently used first when the pool or the slots run out; if nothing can be dropped the run is not batched.

**Stage 2, `gxstatecache`: state work only when state changes, and compiled state reused.**

- A setter whose arguments equal the last ones stored for the same piece of state returns at once (the table `gxthreaddedup` uses for the record, applied where the call is made; 70-74% of state calls in the measured scenes). No version bump, no rebuild.
- A setter that does change something marks its class dirty: TEV and texture order, textures, pixel pipeline (alpha test, blend, depth, colour mask, fog), scissor and viewport, channels and texgens.
- `build_state` rebuilds only dirty classes into the kept state. The TEV class is looked up in a table of compiled results keyed by the TEV bytes (the key is stored and compared in full, so there is no hash risk): the compile runs once per TEV setup, not once per use.
- `plat_gpu_set_state` gets the dirty classes and writes the changed words without comparing the whole state.
- The uniform block (`vsh_prepare`) rebuilds its clip rows only when a matrix or the projection changed, and its texture rows only when a texgen or texture matrix changed.

**Stage 3, `gxdirect`: the render thread writes PICA registers itself.** For shader-path draws the replay stops calling `C3D_FVUnifSet`, `C3D_UpdateUniforms` and `C3D_DrawElements`. Uniform rows are written as `GPUREG_VSH_FLOATUNIFORM` writes, and each prebuilt draw as the register words citro3d's `C3D_DrawElements` emits (the words are fixed per draw and are stored with the batch). The attribute-buffer setup per (stream segment, layout) is stored the same way. Then the PICA state itself: one prebuilt register block per distinct state, copied instead of rebuilt through citro3d setters; citro3d's own shadow is marked dirty when the replay goes back to a CPU-path draw, a clear or a copy. Prebuilt blocks are copied into the command buffer, not called with `GPUREG_CMDBUF_JUMP`: a jump and its return cost as many words as a draw, and jumps are unverified on hardware.

**Stage 4, `gximmdirect`: immediate mode written in PICA form.** `GXEnd` converts the pipe writes straight into the frame's vertex ring (float position, float texture slots, u8 colour, float normal), with indexed attributes gathered from their arrays at that moment, and no `GxVertex` in between. Because the arrays are read on the game thread at `GXEnd`, the array snapshots and their fences are no longer needed.

**Stage 5: one hop.** The game thread writes the frame's PICA command list (double-buffered linear memory) from the prebuilt blocks; the render thread splits it at clears, copies and the stereo boundary, submits and presents. The stereo split uses a side table the writer fills at each state change (colour and depth writes, viewport depth range), and a full state block is written at each layer boundary so the 2D tail is self-contained for the second eye. `gx_rec.c` and `gxthread` are deleted.

### What cannot be prebuilt, with its measured share (gor_01 unless noted)

| Work | Count per frame | Share now | After |
|---|---|---|---|
| Immediate-mode vertices (CPU-skinned characters, effects, windows) | 369 primitives, 1402 vertices (battle: 204 and 792; pause menu: 334 and 1388) | about 6% of the game thread (battle: 8% in `draw_from_reader`) | one conversion pass per vertex (stage 4) |
| Lists in memory the registry does not cover (CPU-written arrays: shadows, some effects) | about 225 hits (5%) are hashed every frame | inside the hit path | unchanged: hashed, then drawn through the per-list path |
| State lists (the map's material lists of port opcodes) | 245 | about 6% | setters that change nothing return early (stage 2); the rest is real state change |
| Matrices and uniform rows (camera and objects move) | 463 blocks | `vsh_prepare` 1.3% | 16 multiplies and 4 rows per object; stays |
| CPU-path draws (groups counting up to K) | 82 just after a map loads, 0 in the steady state | under 1% | unchanged |
| Game code that feeds GX (`C_MTXConcat`, `mapDisp`, material set-up) | | about 8% | not GX; lever 7 of PORT_3DS_60FPS.md |

### Expected cost left (estimates from the profile shares, Azahar ms; to be replaced by measurements per stage)

| Scene | core-0 busy now | of which GX | GX after stage 1 | after stages 1+2 | after 1+2+4 | render work now -> stage 3 |
|---|---|---|---|---|---|---|
| gor_01 | 70.8 | 44.6 | about 26 | about 12 | about 10 | 14 -> about 7 |
| gor_00 | 37.5 | 21.4 | about 13 | about 6 | about 5 | |
| battle (`btlauto`) | 35.8 | 24.9 | about 17 | about 9 | about 7 | |
| aaa_00 | 34.5 | 23.4 | about 14 | about 7 | about 6 | |
| pause menu | 18.5 | 8.9 (immediate only) | 8.9 | about 6 | about 4 | |

So core 0 would land near the `gxnull` floor (26 ms in gor_01, 10-16 ms elsewhere) plus about a quarter of today's translation. Whether that is 16.7 ms on the console is Step 2's question (the game-logic floor itself is 1.6 frames in gor_01 in Azahar's time).

### Alternatives considered

| Alternative | Why not (or why later) |
|---|---|
| Cheaper hops only (fewer record words per hit, direct `GPUCMD` writes in the replay, fewer uniform uploads) while keeping per-list translation | Round 1 did this (state deltas, uniform rows, flushes): the replay got cheaper, frames did not. The per-list work stays 5000 times a frame. The replay part is kept as stage 3 |
| `GPUREG_CMDBUF_JUMP` to prebuilt command lists | A jump and return are about 6 register writes, a merged draw is 14: no gain for single draws, and jumps have never run on our hardware. Possible later for whole static sub-scenes |
| A trust window (no validation for N frames) | Breaks "behaves like the original": the registry's generation already gives a free check |
| Caching whole frames or whole objects (state included) keyed by the object | Needs game knowledge (which calls belong to an object); the batch gives the same reuse from the GX call stream alone |
| A 64-bit signature of the whole GX state to look up finished PICA states | Works, but a collision draws a wrong state; the per-class table with full key compare is exact and nearly as cheap |
| Moving the replay to core 0 or 1 | Measured worse (PORT_3DS_MULTICORE.md) |

### Acceptance (per stage, before its default flips)

1. `determ` shots byte-identical to the switch off: 6 consecutive shots in gor_01, the effects battle and the scenes below.
2. `vsh_accept.py` in the ten scenes of PORT_3DS_MULTICORE.md (gor_01, gon_02, aaa_00, tik_01, hei_01, jin_00, the effects battle, eki_00, usu_00, pik_00), switch off against on.
3. `gw_diff.py` equal on the 1600-frame padscript.
4. `gxcache=2` sweep with the switch on: 0 mismatched, 0 stale.
5. Same-build A/B in the ten scenes: core-0 busy, translator, render and frame ms; the default flips only if no scene gets slower, hei_01 included.

## Core-2 round 2: results (2026-11-24)

Built: stage 1 (`gxbatch`) and part of stage 2 (`gxstatecache`, `gxstatededup`). Stages 3 to 5 are not built. All numbers are Azahar's (ratios, not console times), default `gxthread=0`, from `tools/port3ds/gx_perf_ab.py` (same build, switch off against on, windows ending at frames 722 to 841, one run per cell). The runs used `--long 140` to `280`: the warp is about 40 s of wall time in, and the machine ran other agents' builds.

**Stage 1, `gxbatch=1`** (`gx_dlcache.c`, `gx_vsh.c`, `gx_draw.c`, `CMD_VSH_DRAWP` in `plat/gpu.c`):

| Scene | frame off -> on (ms) | core-0 busy off -> on (ms) | render work (ms) | batches, lists drawn through them per frame |
|---|---|---|---|---|
| gor_01 | 80.5 -> 50.9 | 67.8 -> **48.0** (-29%) | 15.3 -> 15.3 | 245, 4782 |
| gon_02 | 44.1 -> 39.9 | 38.1 -> **28.1** (-26%) | 6.8 -> 6.9 | 132, 2514 |
| aaa_00 | 48.1 -> 33.4 | 33.8 -> **23.6** (-30%) | 8.1 -> 8.1 | 190, 2600 |
| tik_01 | 33.4 -> 22.0 | 19.9 -> **16.1** (-19%) | 4.5 -> 4.5 | 49, 933 |
| hei_01 | 33.6 -> 33.6 | 23.6 -> **22.3** (-6%) | 6.0 -> 6.1 | 140, 471 |
| jin_00 | 41.1 -> 33.4 | 32.2 -> **24.8** (-23%) | 6.3 -> 6.3 | 127, 2012 |
| effects battle | 44.9 -> 39.6 | 38.4 -> **28.5** (-26%) | 7.0 -> 7.0 | 132, 2514 |
| eki_00 | 33.4 -> 29.3 | 20.0 -> **17.1** (-15%) | 6.0 -> 5.9 | 110, 772 |
| usu_00 | 66.9 -> 50.2 | 56.9 -> **41.2** (-28%) | 12.7 -> 12.7 | 142, 3790 |
| pik_00 | 83.6 -> 66.9 | 80.0 -> **56.8** (-29%) | 15.7 -> 15.7 | 363, 5794 |

- No scene is slower; hei_01 keeps its frame (vblank-bound) and its busy time drops.
- There is no translator in this layout (`gxthread=0`), so the translator column of the brief is 0. The pipelined layout (`gxthread=2 gxpipeline=1`) was not measured again.
- The render thread is unchanged: it already drew merged draws; the batch only stops the index copies and the per-list work on the recording side.
- In gor_01, 4782 of 5007 cache hits go through 245 batches; the other 225 are lists in memory the registry does not cover (hashed on the per-list path as before). In the steady state no batch is formed, killed or refused (one "vertex state" refusal per window).
- Counters: a `gxbatch frame N:` line every `gxthreadstats` frames.

Identity, `determ`, 6 consecutive shots, byte for byte, off against on: gor_01 6 of 6 (`gxbatch=1`), the effects battle 6 of 6 (`gxbatch=1`, frames 900 to 905), hei_01 6 of 6 (`gxbatch=2`, the verify mode: every member checked the per-list way, hashes included, **0 failures**).

Both switches together (`gxbatch=1 gxstatecache=1`) against the default, same method, frames 780 to 785, in all ten scenes: **60 of 60 shots byte-identical**. (The check script's blank-shot heuristic flagged the six shots of jin_00 and of usu_00; whether those shots have content was not looked at, so count those two scenes as unconfirmed.)

**Defaults: both stay 0.** The remaining gate is `gw_diff.py` on the 1600-frame padscript, which was not run. (Later: the gate passed, and both are 1 by default since 2026-11-24, owner's decision; see below.)

**Stage 2 as measured** (on top of `gxbatch=1`):

| Switches | gor_01 busy | effects battle busy | What it showed |
|---|---|---|---|
| `gxbatch=1` | 48.2 | 28.7 | |
| + `gxstatecache=1` (compiled TEV setups kept; the key is compared in full) | **44.5** (-8%) | **27.2** (-5%) | every build reuses a kept setup (545 reused, 0 compiled per frame in gor_01); `gxstatecache=2` compiled each again: 0 mismatches |
| + `gxstatecache=1 gxstatededup=1` (setters whose words are in force return early) | 55.1 (**+14%**) | 32.0 (+11%) | 17441 of 23564 state calls per frame return early, but the table lookup and compare on 23.5 K calls cost more than the setters they skip, and the state builds stay at 545 a frame: the calls that remain are real changes. A loss; it stays off |

So the plan's "a setter compares and stores" does not pay through the record's dedup table. What is left of the state path is about 545 real rebuilds a frame: texture lookups, the pixel-pipeline fields, and `plat_gpu_set_state`'s compares. The next step there is the plan's per-class dirty bits with the compare inline in each setter, not built.

**Not built:** per-class state rebuild, stage 3 (render thread writing PICA registers), stage 4 (immediate mode written in PICA form), stage 5 (one hop, `gx_rec.c` removed). **Not run:** `gw_diff.py` on the 1600-frame padscript, `vsh_accept.py` (the byte-identical `determ` shots are the stricter image test), the pipelined layout with the new switches, a PC-sample profile with the switches on. `gxcache=2` and `gxbatch` exclude each other; `gxbatch=2` is the batch's own check.

## Core-2 round 2: gates and per-class state (2026-11-24, later)

### Gates of `gxbatch=1 gxstatecache=1` (no code change)

- **`gw_diff.py` on the 1600-frame padscript** (PORT_3DS_TEST_INFRA.md: `map=gor_01 story=30 determ gwdump=10` and the A2 padscript, to frame 1600), build `7dd7bbc5`, default switches against `gxbatch=1 gxstatecache=1`, two Azahar runs and `gw_diff.py --logs`: `gw_diff: A 160 records, B 160 records` / `gw_diff: 160 records equal (f 10..1600, fields compared: 23)`. A long run by necessity (two runs to frame 1600, about 3 minutes each).
- **jin_00 and usu_00**, the two scenes the blank-shot heuristic flagged: the shots have scene content (jin_00: the steeple's interior with the stained-glass windows, 69% of pixels off the commonest colour, about 78 000 colours; usu_00: the town under the moon, 71%, about 10 500 colours; the black rows are the letterbox). Taken again with `tools/port3ds/determ_ab.py`, default against `gxbatch=1 gxstatecache=1`, frames 780 to 785: **jin_00 6 of 6, usu_00 6 of 6 byte-identical**. So the earlier 60 of 60 stands with no unconfirmed scene.

Both defaults stayed 0 at that point (owner's decision). **Defaults since 2026-11-24 (owner's decision): `gxbatch=1`, `gxstatecache=1`;** `gxbatch=0` and `gxstatecache=0` still turn each off. `gxstatededup` and `gxdirty` stay 0.

### Stage 2 rest, `gxdirty`: per-class dirty bits (built)

Switch `gxdirty` (default 0; 2 is a verify mode). Files: `gx_state.c` (setters), `gx_draw.c` (`build_dirty`, `vsh_state_dirty`, the verify), `gx_tex.c` (`gx_tex_invalidate_all` marks textures), `plat/gpu.c` (`plat_gpu_set_state_dirty`).

- **Setters.** Each setter that writes state compares its arguments with the fields it holds, inline (`SAME(...)` after the record step: no table, unlike `gxstatededup`), and returns before it changes anything when they are equal: no version change. A setter that changes something sets the dirty bit of its class: TEV (stages, order, registers, konst colours, swap tables, indirect), textures (maps, TLUTs; also a texture copy and a dropped conversion), pixel pipeline (alpha test, blend, depth, colour and alpha update, pixel format, fog, cull, ...), scissor and viewport, channels (channel control, colours, lights, texgens). Matrix loads set `GXD_CLIP` (position and normal) or `GXD_TEXROW` (texture); the projection and the current matrix compare first.
- **`build_state` in parts.** `build_full` is the old build cut into its parts (units and TEV, texture units, transform flags, pixel pipeline, scissor and viewport); the switch-off path calls it as before. `build_dirty` runs only the parts of the dirty classes into the kept `PlatGpuState` (each part clears its own region first, so it equals a full build) and gives the back end the regions they fill. A frame start, `GXInit` and a texture copy mark the classes as before.
- **Back end.** `plat_gpu_set_state_dirty(state, regions)` compares only the words of those regions with the last state (no two 700-byte compares, no full delta scan) and records what `plat_gpu_set_state` would: the same delta in the same word order, or the whole state. With no valid last state (a new record) it is `plat_gpu_set_state`.
- **Uniform block.** In three parts: colour (TEV and channel classes; when lit, also the matrices), texture rows (TEV, channel and texture-matrix classes; position matrices only when a texgen reads one; whether the vertex has a normal), clip rows (matrices, projection, current matrix).
- **`gxdirty=2`**: wherever the switch-off path would rebuild (a version change), the full build is made too and compared with the kept state, the units, the transform flags, and `vsh_prepare`'s block, reasons, unit attributes and light block (`gxdirty verify` warnings; the `gxdirty frame N:` line every `gxthreadstats` frames counts builds per class, checks and mismatches).

Results (Azahar, build `7dd7bbc5` + this change, release build, `gxthread=0`, base `gxbatch=1 gxstatecache=1`):

| Check | Result |
|---|---|
| `gxdirty=2`, gor_01 (story 30), frames 1 to 961 | 214 291 state checks and 214 291 uniform-block checks, **0 mismatches** (dev build) |
| `gxdirty=2`, effects battle (`map=gon_02 story=30 battle=1 btlauto=perfect`), frames 1 to 961 | 104 330 + 104 330 checks, **0 mismatches** (dev build) |
| `determ`, gor_01, shots 780 to 785, `gxdirty=0` against `1` | **6 of 6 byte-identical** |
| `determ`, effects battle, shots 900 to 905 | **6 of 6 byte-identical** |

Same-build A/B, `tools/port3ds/gx_perf_ab.py`, windows ending at frames 722 to 841, one run per cell:

| Scene | frame A -> B (ms) | core-0 busy A -> B (ms) | render (ms) |
|---|---|---|---|
| gor_01 | 50.5 -> 50.3 | 45.5 -> **35.3** (-22%) | 15.4 -> 15.3 |
| effects battle | 38.9 -> 28.9 | 27.3 -> **22.3** (-18%) | 7.1 -> 7.1 |
| hei_01 | 33.4 -> 27.7 | 20.9 -> **17.2** (-18%) | 6.1 -> 6.1 |
| aaa_00 | 33.4 -> 26.1 | 22.4 -> **16.5** (-26%) | 8.2 -> 8.2 |

- Rebuilds per frame in gor_01 (verify run): about 441 partial builds (TEV 54, textures 341, pixel 227, scissor 8, channels 142) where the switch-off path rebuilds about 568 times in full; most are a texture change. The saving is the parts not rebuilt (TEV compile and texture lookups on a pixel-only change, the 700-byte compares) and the early returns.
- No scene is slower. gor_01's frame stays at 50 ms (it waits for something else there); its busy time drops.
- **Not run:** `vsh_accept.py`, `gw_diff.py` and the `gxcache=2` sweep with `gxdirty=1`; the other six scenes of the ten; the pipelined layout (`gxthread=2`) with `gxdirty`; `gxdirty` without `gxbatch`. **Default stays 0.**

`tools/port3ds/determ_ab.py` (new): `determ` shots of two switch sets of one build, compared byte for byte, with a content line per shot (share of pixels off the commonest colour, number of colours), so a blank shot is visible.

### `gxdirty` remaining gates (2026-11-24, with `gxbatch=1 gxstatecache=1` as the defaults)

Build `ff42ccc` (the new defaults), release, `gxthread=0`, `gxdirty=0` against `gxdirty=1`, no other switch:

- **`gw_diff.py` on the 1600-frame padscript** (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript, two Azahar runs to frame 1600, `--logs`): `gw_diff: A 160 records, B 160 records` / `gw_diff: 160 records equal (f 10..1600, fields compared: 23)`.
- **`determ`** (`determ_ab.py`, shots 780 to 785) in the six scenes not run before: gon_02, tik_01, jin_00, eki_00, usu_00, pik_00 **6 of 6 byte-identical each** (36 of 36). Every shot has content (69 to 87% of pixels off the commonest colour, 10 500 to 78 000 colours).
- **Same-build A/B** (`gx_perf_ab.py`, windows ending at frames 722 to 841, one run per cell):

| Scene | frame A -> B (ms) | core-0 busy A -> B (ms) | render (ms) |
|---|---|---|---|
| gon_02 | 39.0 -> 29.4 | 27.2 -> **22.8** (-16%) | 6.9 -> 6.9 |
| tik_01 | 20.4 -> 16.7 | 15.7 -> **12.8** (-18%) | 4.5 -> 4.4 |
| jin_00 | 33.4 -> 33.4 | 23.9 -> **20.0** (-16%) | 6.3 -> 6.3 |
| eki_00 | 19.5 -> 16.7 | 16.0 -> **12.8** (-20%) | 6.0 -> 5.9 |
| usu_00 | 50.2 -> 37.1 | 39.3 -> **31.4** (-20%) | 12.8 -> 12.8 |
| pik_00 | 66.9 -> 50.2 | 54.3 -> **44.9** (-17%) | 15.7 -> 15.7 |

No scene is slower. With the four scenes above, `gxdirty=1` is now measured in all ten scenes. **Not run:** `vsh_accept.py`, the `gxcache=2` sweep with `gxdirty=1`, `gxthread=2` with `gxdirty`. Default stays 0 (owner's decision).

## Stage 3, `gxdirect`: the render thread writes PICA registers itself (2026-11-26)

Switch `gxdirect` (default 0; 2 is a verify mode). File: `port3ds/plat/gpu.c` only (render thread). It applies to shader-path draws (`CMD_VSH_DRAW`, `CMD_VSH_DRAWP`); CPU-path draws, clears, copies, the stereo warp and the present stay on citro3d.

- **Draw.** The register words of `C3D_DrawElements(GPU_TRIANGLES, n, C3D_UNSIGNED_SHORT, idx)` after its `C3Di_UpdateContext`: a 30-word packet built once at start-up with libctru's `GPUCMD_*` (the encoder citro3d uses), copied per draw with the index offset and count patched. Checked against the disassembly of the installed `libcitro3d.a` (citro3d 1.7.1): same registers, values and order.
- **Uniform rows.** `vunif` keeps the rows in a pending set instead of `C3D_FVUnifSet`; before each draw every run of pending rows goes out as `C3D_UpdateUniforms` writes it (`GPUREG_VSH_FLOATUNIFORM_CONFIG` = `0x80000000 | first`, then the run's data through `GPUCMD_AddWrites`), and citro3d's `C3D_FVUnif` copy is kept equal. The `lit` bool and the `lcount` int uniform go through citro3d only when they change. `C3D_UpdateUniforms` (96 rows scanned per draw) no longer runs per draw.
- **State.** In three classes: TEV (stages, combiner buffer, fog mode and colour), texture units (the three `C3D_Tex` after the sampler setters), pixel pipeline (alpha test, blend, depth, viewport, scissor); plus the attribute setup per (stream, layout). The first time a key is seen, citro3d's own setters run and the words `C3Di_UpdateContext` emits for them are captured (command buffer pointed at a scratch array) and kept with the key (4-way tables, 406 KB of heap); after that the kept words are copied. A class goes out only when its key differs from the one the GPU holds. A `CMD_STATE` or delta is sent at the next draw, not at once. Cull is one `GPUREG_FACECULLING_CONFIG` write. Copied, never `GPUREG_CMDBUF_JUMP`.
- **Determinism of the kept words.** With the switch on, the setters give a fixed value to registers the state leaves unused (fog colour 0 with fog off; the blend factors under a logic op; logic op COPY under blending; with an empty scissor, scissor off with corners 0), so equal keys give equal words. These registers have no effect on the image.
- **citro3d's shadow.** After kept words, citro3d's copy of the state lags behind the GPU. Before any citro3d-path work (`C3D_PATH` in `render_record`: a CPU-path draw, a clear, a copy, the stereo split and warp, the present) the state in force goes through citro3d's setters again and every class is marked not held by the GPU. When citro3d emits a group on its own before a direct draw (`c3d_flush`), the classes are marked the same way. At each record start everything is marked not held (also covers an APT restore).
- **Fog LUT.** With the switch on, `CMD_FOG` writes the LUT at once (`GPUREG_FOG_LUT_INDEX` 0, then the 128 words citro3d would write) and takes it off citro3d's pending list.
- **Scissor guard (c883edf).** An empty scissor skips the draw on the direct path too; the flag is recomputed whenever the pixel block goes out (kept words do not run `apply_pix`).
- **citro3d internals used:** `__C3D_Context` word 8 (flags) and word 16 (`bufInfo.base_paddr`), checked at start-up (`C3D_SetBufInfo` must set bit 3 and the base; else `gxdirect` turns itself off with a warning), and `C3Di_UpdateContext` (a global symbol of `libcitro3d.a`). A citro3d update must be checked again.
- **Counters:** with `gxthreadstats=N`, two `gxdirect frame N:` lines (blocks copied and captured per class, citro3d flushes, uniform runs; verify checks and mismatches, uncached blocks, states with a viewport under one pixel, largest block per class). Render phases 8 (capture) and 9 (direct draw) in the hang report.

Results (Azahar, build `982e6eee` = port3ds `c883edf` + this change, release, default switches against `gxdirect=1`):

| Check | Result |
|---|---|
| `gxdirect=2`, gor_01 (story 30), frames 1 to 840 (dev build) | 0 `gxdirect verify` warnings (every block, uniform run and draw packet built through citro3d as well and compared byte for byte; about 113 000 checks per 60 frames in the map) |
| `gxdirect=2`, effects battle, frames 1 to 960 (dev build) | 50 000 checks per 60 frames in the battle, **0 mismatches** |
| `determ` (`determ_ab.py`), gor_01, hei_01, aaa_00 shots 780 to 785; effects battle 900 to 905 | **6 of 6 byte-identical in each** (24 of 24; 89 to 98% of pixels off the commonest colour) |
| `gw_diff.py`, 1600-frame padscript (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript) | `gw_diff: A 160 records, B 160 records` / `gw_diff: 160 records equal (f 10..1600, fields compared: 23)` |

Same-build A/B, `tools/port3ds/gx_perf_ab.py`, windows ending at frames 722 to 841, one run per cell:

| Scene | frame A -> B (ms) | core-0 busy A -> B (ms) | render A -> B (ms) |
|---|---|---|---|
| gor_01 | 50.5 -> 50.5 | 45.7 -> 45.7 | 16.4 -> **7.9** (-52%) |
| effects battle | 39.3 -> 38.4 | 27.3 -> 27.2 | 7.2 -> **3.8** (-47%) |
| hei_01 | 33.4 -> 33.4 | 20.9 -> 20.9 | 6.2 -> **3.4** (-45%) |
| aaa_00 | 33.4 -> 33.4 | 22.5 -> 22.5 | 8.3 -> **4.2** (-49%) |

- Render work halves in every scene (the plan's estimate for gor_01 was 14 -> about 7). Frame and core-0 times do not change: in this layout (`gxthread=0`) the render thread is on core 2 and was not the wall. It becomes the budget the stage-5 hop has to fit.
- In gor_01 per frame (verify run): about 570 direct draws, 43 TEV, 342 texture, 170 pixel and 117 layout blocks sent, about 650 uniform runs, 1 citro3d flush. No block was uncached; no state had a viewport under one pixel.
- A run before the rebase onto c883edf (build `0c4e72e3`) gave the same: identity 24 of 24, render 15.6 -> 7.8 (gor_01), 7.2 -> 3.8, 6.2 -> 3.4, 8.3 -> 4.2.

**Console-test items** (Azahar accepts these; hardware is not proven):

1. The direct path writes the same registers in the same order as citro3d for the draw and the uniform runs, but the state goes out in a different order and amount than with citro3d: only the changed classes, at the draw, and cull as a single `GPUREG_FACECULLING_CONFIG` write instead of the whole effect group. Any register-order dependence on the PICA would show here first.
2. Command-buffer alignment and padding: kept blocks are copied at an even word offset; every block is a run of whole libctru commands (padded to even length by `GPUCMD_Add`). A full buffer stops in `svcBreak` as libctru does.
3. `GPUREG_RESTART_PRIMITIVE` (0x25F), `GPUREG_PRIMITIVE_CONFIG` (0x25E) and the draw-mode registers `GPUREG_GEOSTAGE_CONFIG` (0x229), `GPUREG_GEOSTAGE_CONFIG2` (0x253), `GPUREG_START_DRAW_FUNC0` (0x245): same words as `C3D_DrawElements`, but the draws now follow each other with fewer other writes between them.
4. Fixed attributes (`GPUREG_FIXEDATTRIB_INDEX` and data) of a layout block are sent only when the layout changes, not at each layout switch through citro3d.
5. The fog LUT is written when `CMD_FOG` arrives, also while fog is off.
6. The texture cache clear (`GPUREG_TEXUNIT_CONFIG` bit 16) goes out only with a texture block, i.e. when the bound textures or their sampler fields change, after a copy or clear, and at each record start; citro3d sent it at every state change. Textures written by the GPU mid-record (EFB copies) always go through the citro3d path first, which marks the blocks invalid.
7. The empty-scissor state with `gxdirect=1` writes scissor mode off with corners 0 (draws under it are skipped).

Console command (owner-gated; not run): install the release CIA and set `gxdirect=1` in `debug3ds.txt`, then compare `gxthreadstats=60` render times and look for hangs in gor_01 and a battle.

**Not done:** `vsh_accept.py`, the other six scenes of the ten, the `gxcache=2` sweep with `gxdirect=1`, `gxthread=2` with `gxdirect`, any console run. Default stays 0.

## Stage 4, `gximmdirect`: immediate mode written in PICA form (2026-11-26)

Switch `gximmdirect` (default 0; 2 is a verify mode). Files: `gx_pipe.c` (the converter, the translator's side), `gx_draw.c` (`gx_draw_imm_route`, `gx_draw_imm_shader`, `gx_draw_vertices_cpu`), `gx_vsh.c` (the ring split into write and finish, the game thread's chunks), `gx_rec.c` (`GXR_PRIMD`). Display lists are not touched: only `GXBegin`/`GXEnd` primitives outside a list.

- **Route first.** `gx_draw_imm_route` makes the decisions `gx_draw_vertices` and `draw_vsh_imm` make, in the same order (batch flush, state, empty target, uniform block, reasons, ring room, `gx_vsh_route` with the caller as the group's identity, cull all), before any vertex is decoded. Draws it does not handle (`gxvsh=0`, a draw routed already, per-vertex matrices, the bounding box armed, `gxdump`) take the old path unchanged.
- **Shader path: no `GxVertex`.** `gx_imm_convert` builds a plan once per primitive from the vertex description and format (skip, components direct or from an array, colour direct or from an array; the first three texture attributes take slots 0 to 2), then runs it per vertex over the pipe slots into the ring chunk: float position, three float slots, float normal (0, 0, 1 when there is none), u8 colour 0, from a template with white colour and zero slots. Each step uses read_vertex's expressions, so the bytes are the ones `gx_vsh_imm_build` wrote from the decoded vertex.
- **CPU path.** A draw routed to the CPU is decoded as before (`read_vertex`) and goes to `gx_draw_vertices_cpu`, the old code after routing.
- **While recording (`gxthread=1..3`).** At `GXEnd` the game thread converts the primitive with the shadow's vertex state (`gx_game`) into chunks of its own (8 of 16384 vertices, linear memory, allocated when first needed) and records `GXR_PRIMD` (10 words: primitive, count, caller, chunk, first vertex, slot map, normal flag, vertex-state hash) instead of the pipe words and **array snapshots**. The translator routes it; on the shader path it draws from the chunk (the flush is recorded there); otherwise it rebuilds the `GxVertex` read_vertex would make from the chunk bytes (exact: the draw is taken only without colour 1, matrix indices or a fourth texture attribute). A chunk is reused when every draw in it has been translated and 3 frames have been presented since the last (atomic counters). A primitive that does not qualify, or no free chunk, is recorded the old way. `gxnull` records the old way.
- **Counters:** with `gxthreadstats=N`, a `gximmdirect frame N:` line (draws to the ring, routed to the CPU, the old way, nothing to draw; recorded at `GXEnd`, recorded the old way; verify checks and mismatches, vertex-state differences between shadow and translator).

Results (Azahar, build `df0a3544` = port3ds `48c6335a` + this change, release unless noted):

| Check | Result |
|---|---|
| `gximmdirect=2`, gor_01, `gxthread=0`, frames 1 to 840 (dev build) | 90 832 primitives converted and decoded the old way, **0 mismatches**; 369 to the ring per frame in the map, 0 the old way |
| `gximmdirect=2`, effects battle, `gxthread=0`, frames 1 to 960 (dev build) | 149 372 checks, **0 mismatches** |
| `gximmdirect=2`, gor_01, `gxthread=2`, frames 1 to 840 (dev build) | 69 124 checks, **0 mismatches**, 0 vertex-state differences; 318 to 321 primitives per frame recorded at `GXEnd`, 0 the old way; array snapshots 0 bytes per frame |
| `gximmdirect=2`, pause menu, `gxthread=2`, frames 1 to 900 (dev build) | 85 712 checks, **0 mismatches**; 44 per frame routed to the CPU in one window (the rebuild from the chunk ran) |
| `gxvsh=0 gxthread=2 gximmdirect=2`, gor_01 to frame 780 (dev build) | every primitive the old way (369 per frame), 330 recorded the old way: the CPU fallback is unchanged |
| `determ` (`determ_ab.py`), `gxdirect=1` against `gxdirect=1 gximmdirect=1`: gor_01, hei_01, aaa_00, pause shots 780 to 785; effects battle 900 to 905 | **6 of 6 byte-identical in each** (30 of 30; 77 to 98% of pixels off the commonest colour) |
| `gw_diff.py`, 1600-frame padscript (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript), `gxdirect=1` against `gxdirect=1 gximmdirect=1` | `gw_diff: A 160 records, B 160 records` / `gw_diff: 160 records equal (f 10..1600, fields compared: 23)` |

Same-build A/B, `tools/port3ds/gx_perf_ab.py`, windows ending at frames 722 to 841, base `gxdirect=1` (today's defaults otherwise), `gxthread=0`:

| Scene | frame A -> B (ms) | core-0 busy A -> B (ms) | render A -> B (ms) | runs |
|---|---|---|---|---|
| gor_01 | 50.5 -> 50.6 | 45.9 -> 45.9 | 7.9 -> 7.9 | 2 per cell (a first single run: 50.5 -> 50.5, 45.8 -> 45.9) |
| effects battle | 38.9 -> 38.5 | 27.6 -> 27.5 | 3.8 -> 3.8 | 1 |
| hei_01 | 33.4 -> 33.4 | 21.0 -> 21.0 | 3.4 -> 3.4 | 2 (single: 33.4 -> 33.6, 21.0 -> 21.1) |
| aaa_00 | 33.4 -> 33.4 | 22.6 -> 22.5 | 4.2 -> 4.2 | 2 |
| pause menu | 32.9 -> 33.3 | 18.1 -> **17.7** | 3.8 -> 3.9 | 2 |

The first A/B lost the A runs of aaa_00 and the pause menu (`run_azahar: FAIL: Azahar exited (status 134)`, base switches, which had run before for `determ`); they were run again with the others at 2 runs per cell.

With `gxthread=2` (`gxdirect=1 gxthread=2` against `+ gximmdirect=1`, one run per cell), where the plan expected the saving (no snapshots):

| Scene | frame A -> B | game busy A -> B | fences A -> B | translator A -> B | render |
|---|---|---|---|---|---|
| gor_01 | 50.3 -> 50.0 | 28.9 -> 29.2 | 8.9 -> 6.0 | 28.8 -> **26.3** | 7.9 -> 7.9 |
| pause menu | 24.1 -> 16.7 | 8.7 -> 10.0 | 6.7 -> 3.9 | 13.7 -> **10.5** | 3.9 -> 3.9 |

- In `gxthread=0` the switch changes nothing measurable: busy time moves by 0.1 ms or less, except the pause menu (-0.4 ms, -2%); frames are vblank- or wait-bound. The plan's "about 6%" was `draw_from_reader` and `gx_draw_vertices` together; what the switch removes is only the decode into `GxVertex` and the copy from it. Routing, the uniform block and the ring write stay.
- With `gxthread=2` the conversion moves to the game thread (busy +0.3 and +1.3 ms) and the translator gets shorter (-9% and -23%), the fences shorter with it. These are single runs with the split's hill climbing in the loop: a hint, not a measurement.
- No scene is slower by more than the run-to-run spread (0.1 to 0.4 ms in frame); the pause menu's frame 32.9 -> 33.3 comes with lower busy time. Whether that counts as "no scene slower" is the owner's call.

**Console-test items** (Azahar accepts these; hardware is not proven):

1. The game thread's chunks are written on core 0 and flushed by the record's flush list on the translating side (`plat_gpu_stream_commit` at translation): the ring bytes must be in memory when the GPU reads them (with `gxthread=2`, the writing and the flushing thread differ).
2. Chunk reuse with `gxthread=2`: the atomic pending count and presented-frame stamp between core 0 and core 2. A chunk reused too early shows as wrong immediate geometry (characters, windows, effects) for a frame.
3. Linear memory: up to 8 more chunks of 832 KB with `gxthread=1..3` (allocated when needed; how many a scene takes was not counted).

Console command (owner-gated; not run): install the release CIA, set `gximmdirect=1 gxthreadstats=60` in `debug3ds.txt` (once with `gxthread=0`, once with `gxthread=2`), compare busy and translator times and look at characters and menus in gor_01, a battle and the pause menu.

**Not done:** `vsh_accept.py`, the other scenes of the ten, `gxthread=1` and `3` (only 0 and 2 ran), stereo on (the record is eye-independent: the draws are ordinary `CMD_VSH_DRAW`s; not run with the 3D slider), `gxdirect=0` with `gximmdirect=1` other than the 0-mismatch verify runs (those ran with `gxdirect=0`), any console run. Default stays 0.

## Remaining acceptance gates for `gxdirty` and `gxdirect` (2026-11-26, test agent, no code change)

Build: release CIA of port3ds `e34638af` (`TTYD_CCACHE=1 tools/port3ds/build.sh`), Azahar, `gxbatch=1 gxstatecache=1` (defaults). Every run reports `run_azahar: FAIL: 23 hardware-fault line(s) ... unmapped Write32 0xA5A5A5A5 @ 0x07FFFFA4 at PC 0x004B2010` at 2 to 3 s of Azahar time (rc 1), also with default switches; the runs go on and the shots are complete. Not investigated (boot, before the game starts). The tools were run with `TTYD_ALLOW_HW_FAULTS=1`, a new environment variable of `run_azahar.py` (same effect as `--allow-hw-faults`). `tools/port3ds/stereo_ab.py` is new (gate 5). The disc image of this worktree's profiles is a symlink to the main checkout's `build/azahar-profile/.../game.iso`.

### Gate 1: `vsh_accept.py`, defaults against `gxdirty=1` and against `gxdirect=1`

Method: `vsh_accept.py run` four times per scene (default twice = noise floor, `gxdirty=1`, `gxdirect=1`), 24 shots from presented frame 1140, then `report --ref def1 --noise def2 --test dirty --test direct`. The battle is `map=gon_02 story=30 battle=1 btlauto=perfect`. Results in `build/vsh-accept/<scene>/report.md`.

Static pixels (colour and depth constant over the 24 shots of both runs), reference against test:

| Scene | static px | `gxdirty=1`: colour max / depth max | `gxdirect=1`: colour max / depth max | seams (all runs) | flicker at ref-static px (dirty / direct) |
|---|---|---|---|---|---|
| gor_01 | 277 054 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |
| gon_02 | 285 055 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |
| aaa_00 | 291 840 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |
| tik_01 | 276 909 | 0 / 0 | 0 / 0 | 0 | **87 px (105 flips)** / 0 |
| hei_01 | 256 218 | 1 / 0 | 2 / 0 | 0 | 0 / 0 |
| jin_00 | 291 840 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |
| battle | 0 (camera moves) | no static pixels | no static pixels | 0 | 1 px / 0 |
| eki_00 | 286 812 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |
| usu_00 | 281 537 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |
| pik_00 | 279 891 | 0 / 0 | 0 / 0 | 0 | 0 / 0 |

Whole-frame depth comparison (pixels both runs drew, 24 shots): mean abs dz and "order flips" (colour > 32 at 1 <= dz <= 16), test against the noise floor (def1 against def2):

| Scene | noise: mean / flips | `gxdirty=1`: mean / flips | `gxdirect=1`: mean / flips |
|---|---|---|---|
| gor_01 | 0.05 / 1 | 0.06 / 1 | 0.01 / 0 |
| gon_02 | 0.18 / 337 | **2.22 / 4562** | 0.29 / 408 |
| aaa_00 | 0 / 0 | 0 / 0 | 0 / 0 |
| tik_01 | 7.88 / 47 | **101.51 / 1020** | 9.97 / 114 |
| hei_01 | 0.04 / 25 | 0.01 / 1 | 0.25 / **900** |
| jin_00 | 0 / 0 | 0 / 0 | 0 / 0 |
| battle | 0.45 / 574 | **3.23 / 6760** | 1.03 / 1413 |
| eki_00 | 0.07 / 182 | **0.13 / 607** | 0 / 0 |
| usu_00 | 0.01 / 2 | 0.01 / 7 | 0 / 0 |
| pik_00 | 0.01 / 12 | 0.01 / 8 | 0.01 / 8 |

Frame times from `gxstats` (ms, default / `gxdirty=1` / `gxdirect=1`): gor_01 83.6 / 66.9 / 83.6, gon_02 about 44 / 34 / 47, aaa_00 50.2 / 33.4 / 50.2, usu_00 66.9 / 50.2 / 66.9, pik_00 100.3 / 83.6 / 100.3, battle about 45 / 36 / 45; eki_00, tik_01, hei_01, jin_00 33.4 in all.

Reading:
- **Pass:** every static pixel in every scene is identical in colour (max difference 0; hei_01 1 and 2, its own noise floor is 1) and in depth (0). No seam in any run. `gxdirect=1`: no flicker beyond the reference in any scene.
- **Not within the noise floor, not resolved:** the whole-frame depth statistics are above the def1/def2 floor for `gxdirty=1` in gon_02, tik_01, battle and eki_00, and for `gxdirect=1` in hei_01 (900 flips, worst shot 109; its static pixels are identical and its frame time is the same 33.4 ms) and battle (1413 against 574). `gxdirty=1` in tik_01 also has 87 flicker pixels at pixels static in the reference. In gon_02 and the battle the `gxdirty=1` frame is shorter than the default (gon_02 34 against 44 ms, battle 36 against 45), so the run is at another phase of the game's animations at the same shot numbers, which this method cannot separate from a render difference. The byte-identical `determ` shots can: `gxdirty=1` against default was 36 of 36 in gon_02, tik_01, jin_00, eki_00, usu_00, pik_00 and 6 of 6 in gor_01 and the battle earlier in this document; `gxdirect=1` is identical in all ten scenes (gate 3 and the Stage 3 section, including hei_01). No shot was inspected by eye.

### Gate 2: `gxcache=2` sweep (plan gate 4)

`map_sweep.py --maps gor_01,tik_01,gor_01,hei_01,gon_02 --story 30 --frames 300 --switches "gxcache=2 gxstats=60 gxdirty=1"` and the same with `gxdirect=1`. `gxcache=2` and `gxbatch` exclude each other: with `gxcache=2` the batch is not used (the per-list path runs with the switch on); `gxbatch=2`, the batch's own check, was run in an earlier round. Each run: `map_sweep: 4 of 4 maps ok` (the fifth entry repeats gor_01), 25 `gxstats` windows, **max `mismatched` 0, max `stale` 0**, 4 564 590 hits verified (sum of the per-window `verified`). `gxdirty` verify counters read 0+0 checks (they count only at `gxdirty=2`). **Pass for both switches.** `gxdirty=2` and `gxdirect=2` were not combined with `gxcache=2`.

### Gate 3: `gxdirect=1`, the six scenes not run before

`determ_ab.py --a "" --b "gxdirect=1"`, shots 780 to 785: gon_02, tik_01, jin_00, eki_00, usu_00, pik_00 each **6 of 6 byte-identical (36 of 36)**; every shot has content (69 to 83% of pixels off the commonest colour, 10 500 to 78 300 colours). With gor_01, hei_01, aaa_00 and the battle from the Stage 3 section, `gxdirect=1` is identical in all ten scenes.

`gx_perf_ab.py --a "" --b "gxdirect=1"` (windows ending at frames 722 to 841, one run per cell, ms per frame):

| Scene | frame A -> B | core-0 busy A -> B | render A -> B |
|---|---|---|---|
| gon_02 | 38.7 -> 39.3 | 27.2 -> 27.1 | 7.1 -> **3.7** |
| tik_01 | 19.0 -> 18.8 | 15.1 -> 15.2 | 4.6 -> **2.6** |
| jin_00 | 33.4 -> 33.4 | 23.9 -> 23.7 | 6.4 -> **3.6** |
| eki_00 | 19.8 -> 19.8 | 16.1 -> 16.1 | 6.1 -> **3.4** |
| usu_00 | 50.2 -> 50.2 | 39.2 -> 39.3 | 13.1 -> **6.8** |
| pik_00 | 66.9 -> 66.9 | 54.7 -> 54.6 | 16.0 -> **8.0** |

Render work falls by 43 to 50% in all six; frame and core-0 times do not change (gon_02 frame +0.6 ms, within the run-to-run spread seen elsewhere). With the four scenes of the Stage 3 section, `gxdirect=1` is measured in all ten scenes.

### Gate 4: `gxthread=2 gxpipeline=1` with today's defaults (lever 5)

`determ_ab.py`, A = switch set, B = the same plus `gxthread=2 gxpipeline=1`, shots 780 to 785:

| Switch set | gor_01 | battle | pik_00 |
|---|---|---|---|
| defaults | 6 of 6 identical | 6 of 6 | **0 of 6** |
| + `gxdirty=1` | 6 of 6 | 6 of 6 | **0 of 6** |
| + `gxdirect=1` | 6 of 6 | 6 of 6 | **0 of 6** |

**pik_00 fails the identity gate in all three sets.** In the last set's shots (780 to 785): 6572 to 6797 pixels differ (2.2%), 1530 of them by more than 32, max 255; all lie in x 0 to 572, y 14 to 129 (the top of the EFB). Not investigated (GX code is owned by other agents).

`gx_perf_ab.py`, A = `gxthread=0`, B = `gxthread=2 gxpipeline=1`, same extra switch in both (ms per frame; busy = core 0, transl = translator thread):

| Extra | Scene | frame A -> B | busy A -> B | fence B | transl B | render A -> B |
|---|---|---|---|---|---|---|
| none | gor_01 | 50.5 -> 55.9 | 45.7 -> 24.5 | 20.8 | 45.9 | 16.5 -> 16.4 |
| none | battle | 38.4 -> 39.5 | 27.3 -> 15.2 | 13.6 | 26.5 | 7.3 -> 7.4 |
| none | pik_00 | 66.9 -> 61.9 | 54.7 -> 33.2 | 20.5 | 55.4 | 16.0 -> 15.7 |
| `gxdirty=1` | gor_01 | 50.2 -> 43.6 | 35.4 -> 24.4 | 9.1 | 37.4 | 16.4 -> 16.5 |
| `gxdirty=1` | battle | 29.1 -> 35.1 | 22.7 -> 15.5 | 12.6 | 26.3 | 7.3 -> 7.4 |
| `gxdirty=1` | pik_00 | 50.2 -> 52.9 | 45.1 -> 33.2 | 8.3 | 48.1 | 16.0 -> 15.7 |
| `gxdirect=1` | gor_01 | 50.5 -> 46.1 | 45.7 -> 24.5 | 11.3 | 40.7 | 7.9 -> 7.8 |
| `gxdirect=1` | battle | 39.1 -> 40.7 | 27.2 -> 18.0 | 13.3 | 28.9 | 3.8 -> 3.7 |
| `gxdirect=1` | pik_00 | 66.9 -> 53.5 | 54.7 -> 33.2 | 10.8 | 50.5 | 8.0 -> 7.7 |

Reading: core-0 busy time drops 33 to 46%, but the translator (26 to 55 ms) is the new wall. The frame is faster in 4 of 9 cells (gor_01 with `gxdirty` and with `gxdirect`, pik_00 with none and with `gxdirect`, by 4 to 13 ms) and slower in 5 (the battle in all three sets, gor_01 with no extra switch, pik_00 with `gxdirty=1`). With `gxdirty=1`, `gxthread=0` is already as fast or faster in the battle and pik_00. So the pipelined layout does not win everywhere, and it is not image-identical in pik_00. One run per cell; no decision taken here.

### Gate 5: stereo

New `tools/port3ds/stereo_ab.py`: `determ` plus `stereoforce=1` (simulated 3D slider; the `stereo3d` option is on by default), shots 780 and 781, the EFB (`N.ppm`) and both eye targets (`N.L.ppm`, `N.R.ppm`, 400x240) compared byte for byte, defaults against `gxdirect=1`:

- gor_01: `00780` and `00781`, EFB, L and R: **6 of 6 identical**; left differs from right in both shots (the shots are stereo).
- battle: **6 of 6 identical**; left differs from right.

### Not run (this round)

`gxdirty=2` or `gxdirect=2` together with `gxcache=2`; `gw_diff.py` with `gxdirty` and `gxdirect` together; the combination `gxdirty=1 gxdirect=1`; more than one run per cell for the timing; stereo in the other eight scenes; any console run.

## Stage 5, `gxonehop`: one hop (2026-11-26)

Switch `gxonehop` (default 0; 2 is a verify mode; `gxonehopkb=N` the word arena per record, default 512). Files: `port3ds/plat/gpu.c` (recording side and replay), `gx_rec.c` (`gxthread` off with it), `gx_draw.c` (`gximmdirect=1` with it). It implies `gxdirect=1` and `gximmdirect=1`. `gx_rec.c` and `gxthread` stay.

- **Game thread writes the words.** The plat calls that recorded `CMD_STATE`, deltas, `CMD_VSH_CONST`, `CMD_VSH_LIGHT`, `CMD_FOG`, `CMD_VSH_DRAW(P)` now write, at the call, what the gxdirect replay would write for them into the record's word arena (heap, 2 x 512 KB): the three state class blocks and the attribute setup copied from the game thread's own tables of kept words, the uniform rows, the cull word, the draw packet (index offset and count patched; merged draws patch the count), the fog LUT. The model of what the GPU holds follows the replay's rules exactly: state sent at the next draw, a block only when its key differs from the held one, a record starts with nothing held, a clear, copy or CPU-path draw leaves nothing held. The command encoder is libctru's `GPUCMD_Add` layout (runs of 256, even padding), checked against `GPUCMD_Add` at start-up for 0 to 520 parameters (else the switch turns itself off).
- **Render thread.** Copies the word runs (`CMD_OH_WORDS`) into citro3d's command buffer and does only what needs citro3d: `CMD_OH_FLUSH` (vsh switch on and citro3d's pending words, the replay's `c3d_flush`, before the first draw after citro3d work), `CMD_OH_UNIF` (the `lit` bool and light-count int uniforms, set only when they change, as the replay), `CMD_OH_CAPTURE` (a key the game thread's tables do not hold yet: citro3d's setters and a capture, as gxdirect; the game thread takes the words into its tables when the record comes back; a second capture of the same key in the same record copies the first), `CMD_STATE` (whole, before a clear, copy or CPU-path draw when it changed), and clears, copies, CPU-path draws, present as before.
- **Stereo.** A mark (`CMD_OH_MARK`) after a scene draw or copy, before the next screen-layer or no-write draw, clear, CPU-path draw, or the frame's end: the last mark is the split of `stereo_split`. At a mark the game model holds nothing (the next draw sends its full state and layout) and the mark carries the uniform rows set so far, the last fog LUT and the `lit`/light-count values, so the tail after it is self-contained. The render thread applies the mark's words only at the split, for each eye; in mono a mark does nothing.
- **Counters:** with `gxthreadstats=N`, two `gxonehop frame N:` lines.
- **gxonehop=2:** renders as gxdirect=1 from the full record and builds the words of mode 1 beside it (side list; the marks also go into the record, where the replay drops what it holds as mode 1 does). After each mono record the render thread compares the one-hop words with the words its replay wrote itself, word for word (a capture's words come from the replay's own table). Its tables are larger in this mode (512/2048/512/1024 slots; with gxdirect's 512 texture slots some keys of gor_01 were evicted within every frame).
- **Tables:** 512, 2048, 512, 1024 slots (about 1 MB heap) on the game thread; with 512 texture slots gor_01 re-captured about 5 blocks per frame.
- **GPU hang reset:** every record is self-contained (nothing held at its start, all its rows sent again), and the reset is applied only at a record's start, so nothing needs rebuilding. Captured words do not depend on GPU state.
- **gpudrawcut / gpudrawskip:** with gxonehop=1 the game thread asks the filter for the shader draws and the CPU-path draws, in record order with one count (the CPU-path decision travels in `CMD_DRAW`'s count bit 31). gxdirect=1 does not filter its shader draws. gpudump's command-list dumps see the copied words.

Results (Azahar; release build `3dc7d695` = port3ds `63cabb92` + this change unless noted):

| Check | Result |
|---|---|
| `gxonehop=2`, gor_01, frames 1 to 840 (dev build) | 893 records, 14 781 622 words compared, **0 mismatches**, 0 not compared |
| `gxonehop=2`, effects battle, frames 1 to 960 (dev build) | 967 records, 8 615 926 words, **0 mismatches**, 0 not compared |
| `gxonehop=2`, pause menu, frames 1 to 900 (dev build) | 953 records, 10 634 078 words, **0 mismatches**, 0 not compared |
| `determ` (`determ_ab.py`), `gxdirect=1 gximmdirect=1` against `gxonehop=1`: gor_01, hei_01, aaa_00, pause shots 780 to 785; effects battle 900 to 905 | **6 of 6 byte-identical in each** (30 of 30; 77 to 98% of pixels off the commonest colour) |
| Stereo, gor_01 `stereoforce=1`, shots 780 to 785 | **18 of 18 byte-identical** (EFB, left and right screen per shot) |
| `gw_diff.py`, 1600-frame padscript (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript), same A against B | `gw_diff: A 160 records, B 160 records` / `gw_diff: 160 records equal (f 10..1600, fields compared: 23)` |
| `gpuhangtest=700 gpuhangrecover=1` with gxonehop=1, gor_01 | `reset #1 done`, `citro3d state marked dirty after GPU reset #1`; the shot at frame 820 has 576 draws and content (92% of pixels off the commonest colour) |

Per frame in gor_01 (mode 1): about 573 draws, 303 merged calls, kept blocks tev 43 tex 336 pix 169 lay 137, 0 captures in the steady state (with the larger tables), 3 flushes, 1 uniform op, 2 marks, about 55 600 words (222 KB).

Same-build A/B, `gx_perf_ab.py --allow-hw-faults`, windows ending at frames 722 to 841, 2 runs per cell (a first pass at 1 run gave the same within 0.8 ms):

| Scene | frame A -> B (ms) | core-0 busy A -> B (ms) | render A -> B (ms) | gxthread=2 baseline: frame, game busy, translator, render (1 run) |
|---|---|---|---|---|
| gor_01 | 50.5 -> 50.6 | 45.8 -> 46.2 | 8.1 -> **1.4** | 50.2, 31.0, 23.1, 8.2 |
| effects battle | 39.6 -> 40.4 | 27.6 -> 27.8 | 3.9 -> **1.0** | 35.9, 14.1, 20.5, 3.9 |
| hei_01 | 33.4 -> 33.6 | 21.1 -> 21.7 | 3.5 -> **1.0** | 21.5, 12.4, 12.2, 3.6 |
| aaa_00 | 33.4 -> 33.4 | 22.6 -> 22.2 | 4.3 -> **1.0** | 33.5, 13.7, 12.4, 4.4 |
| pause menu | 32.9 -> 33.4 | 17.8 -> 18.2 | 3.9 -> **1.0** | 16.7, 10.0, 11.3, 4.0 |

A is `gxdirect=1 gximmdirect=1` (gxthread=0), B `gxonehop=1`.

- The render thread's work drops by 65 to 83%: it only copies runs. The game thread's busy time moves by -0.4 to +0.6 ms: writing the words costs about what recording the commands did.
- **The acceptance gate fails.** Against the better baseline per scene (gxthread=2 in battle, hei_01 and the pause menu), gxonehop=1 is slower in frame time: 40.4 against 35.9, 33.6 against 21.5, 33.4 against 16.7 ms. With gxthread=2 the translation runs on core 2 beside the game; with one hop all of it is on core 0 again (46 ms busy in gor_01 against 31 + 23 split). The hop is gone, the work is not: the plan's end state needs the per-frame translation itself to shrink (front end state path, list calls), which this stage does not touch. gor_01 and aaa_00 are no slower than either baseline.
- Against gxthread=0 the frame times are within the run-to-run spread except the effects battle (+0.8 ms, busy +0.2) and the pause menu (+0.5 ms, busy +0.4).
- The release build logs 65 Azahar fault lines at boot in every configuration (A included): `unmapped Write32 0xA5A5A5A5 @ 0x07FFFEFC` in `crash_thread_register` (`libs/ctr-devkit/crash/crash.c:195`, the stack fill below a thread's stack). Not from this change; `gx_perf_ab.py` got `--allow-hw-faults` to measure anyway. Dev builds do not show it.

**Default stays 0.**

**Console-test items** (Azahar accepts these; hardware is not proven):

1. The words are written on core 0 into cached heap memory and copied by the render thread on core 2 into citro3d's command buffer after the record handover (the same handover as the record itself). Any word the render thread sees stale would show as a wrong state or a GPU hang.
2. Captures happen on the render thread with citro3d's setters while the game thread reads `s_tex` entries for its texture keys (copies, then the sampler setters on the copy): the fields the render thread writes are all rewritten on the copy, so the key does not depend on the timing. A key from a torn read would show as a capture per frame (counters) or a wrong texture filter.
3. The draw packet's index offset uses `BufInfo_Init`'s base address taken at start-up instead of citro3d's context word at the draw (gxdirect=1 reads the context). The same value in Azahar; checked by the verify mode only there.
4. Stereo on hardware: the split mark's words (all uniform rows, the fog LUT) go out after the warp, before the tail of each eye.
5. Memory: 1 MB heap for two word arenas and about 1 MB for the tables (the verify mode needs 384 KB more for side lists and 512 KB for the compare buffer).

Console command (owner-gated; not run): install the release CIA, set `gxonehop=1 gxthreadstats=60` in `debug3ds.txt`, compare frame, busy and render times with `gxdirect=1 gximmdirect=1` and with `gxdirect=1 gximmdirect=1 gxthread=2` in gor_01, a battle and the pause menu; then once with the 3D slider up.

**Not done:** `vsh_accept.py`, the other scenes of the ten, the `gxcache=2` sweep with gxonehop, gxonehop with `gxdirty=1` (its regions are not used to skip key compares), building the layout block's words without a capture (the immediate-mode ring's stream pointers make new layout keys; 0 per frame in the steady state measured here), deleting `gx_rec.c` and `gxthread` (kept until hardware numbers), any console run.

## Risks of the cache

- **Hash collisions.** A 32-bit hash over the list bytes, and another over the array elements read. A changed list or array that happens to hash the same draws its old vertices (about 1 in 4 billion per change). `gxcache=2` checks by decoding again.
- **Memory.** Up to 8 MB of regular heap by default, plus 1.3 MB of static tables and about 1 MB of capture buffers. If `malloc` fails, the entry is dropped and counted (`alloc-fail`); the list is still drawn. gon_02 reached the budget, and LRU kept up (`full 0`).
- **Validation cost.** The cache saves less than the decode because every hit is fully checked (see Results). Large indexed meshes get cheaper per vertex; tiny lists stay dominated by per-call costs.
- **Not covered by a test here:** a list rewritten in place, or an array rewritten between frames, in a real scene. None changed in the four measured scenes. The code paths (changed, then dynamic after 4 changes) ran only in the verify mode's comparisons, not with real changes. A dedicated test would draw a list, change one byte of it and of its array, and expect `changed 1` and the new picture.
- **Moving GX translation to the render thread** (`PORT_3DS_MULTICORE.md`). The cache's state is module-private and owned by whichever thread translates GX. Its inputs come in through a `GxState` pointer (`gx_dlcache_call(st, ...)`), not the global, so a translator thread with its own `GxState` replica can own it. Two things would block a translation that runs up to a frame late:
  - It reads the list and the arrays **in game memory at translation time**, to hash and, on a miss, to decode. If the game thread has rewritten that memory in the meantime (the next frame's skinned arrays, a reused buffer), the translator sees the new bytes. That holds for the decode with or without the cache. The recorded call would have to carry a snapshot, or the game thread would have to keep the memory stable until the translator is done (double-buffered frames). The cache adds nothing new here, but its hash must be taken from the same bytes the decode reads.
  - `gx_dlc_capturing`, the index capture and the frame number (`gx_perf_frame_no`) are globals of the translating thread. They must move with it, never be shared by two threads.
  - **Built** (`gxthread`, PORT_3DS_MULTICORE.md "A, built"): the cache, trust, pass groups and stream ring stay globals, owned by whichever thread translates at the moment (the translator thread, or the game thread at a fence or in the frame's directly translated start), handed over through the record's indices. The first point is met by the fences, not by snapshots: a release reported by the game thread while translation is pending waits for it, and `sysWaitDrawSync` (TTYD's delayed free) waits too. `gxthread=3` verifies it. With `gxthread` the per-call timers of `gxstats` measure the translating thread; `gxthreadstats=N` gives the game thread's frame, busy, fence and idle times without them (`gxstats` inflates gor_01 from 83.6 to 117 ms per frame in Azahar).

## For PORT_3DS_TESTING.md

Rows for the switch table:

| Switch | Effect |
|---|---|
| `gxstats=N` | Every N game frames, logs `gxstats` lines with per-frame averages: display lists (calls, bytes, vertices, how many repeat unchanged from an earlier frame), immediate-mode primitives, draws, triangles and culled triangles, PICA state rebuilds, the render thread's draws and record bytes, the game thread's time per GX stage (list decode, state rebuild, transform, triangle assembly), the GX features the vertices use (lighting, texgen kinds, matrix indices, primitive types), and with `gxcache` the cache's hits and memory. Off by default; costs a few percent when on |
| `gxcache=0/1/2` | The decoded display-list cache (`port3ds/game/gx/gx_dlcache.c`): 1 (default) draws repeated lists from their decoded vertices after checking every input; 0 decodes every list every time (A/B); 2 decodes every hit again and compares it with the cache (`mismatched` in the `gxstats` cache line, and one warning line) |
| `gxcachekb=N` | The cache's memory budget in KB (default 8192) |
| `gxtrust=0/1` | 1 (default): a cache entry whose list and array spans lie in covered memory of the content-stable registry (`port3ds/plat/memstable.c`), unchanged since its last full check, is a hit without hashing; 0 hashes every hit (A/B). `gxcache=2` still hashes and decodes trusted hits and counts what trust skipped (`stale`, `mismatched`) |
| `gxsdl=0/1` | 1 (default since 2026-11-27): keeps the decoded leading state calls (NOPs and port opcodes) of each display list that misses the display-list cache, with a copy of their bytes, so a repeated list skips the LEB128 decode (`gx_sdl_run`, gx_state.c); the bytes are compared on every call, so changed memory decodes again. Same calls in the same order. Log `gxsdl:` every 262144 lookups. |
| `gxscribble=N` | Test of the verify mode, not for play: at game frame N, xors one byte of an array a cached (preferably trusted) list reads, with no flush and no report. With `gxcache=2` it must show as `mismatched` and `stale`; with trust on it keeps drawing the old vertices, and the `gxstats scribble` line counts those hits (the contract) |
| `gxstatever=0/1` | 0 (default): every state version change rebuilds the PICA state. 1: matrix and projection loads do not bump the version, and a version change whose state (texgen to pixel engine without the lights, viewport) equals the last build skips `build_state` (`builds N (unchanged M)` in `gxstats`). Fewer rebuilds, but no time saved yet (see Results) |
| `gxdirty=0/1/2` | 0 (default): every state version change rebuilds the whole PICA state. 1: setters return early when their values are in force, others mark their class dirty; only dirty classes are rebuilt and sent (`plat_gpu_set_state_dirty`), and the uniform block by parts. 2: as 1, plus a full rebuild compared at every switch-off rebuild (`gxdirty verify` warnings, `gxdirty frame N:` line with `gxthreadstats`) |
| `gxone=0/1/2` | GX native step 2 (docs/PORT_3DS_GX_NATIVE.md, "Step 2 as built"). 0 (default) off. 1: the dirty state classes (TEV with fog, textures, pixel with scissor/viewport) are interned at the draw boundary by their GX bytes; each id owns a PICA block built once by the port's own encoders (`plat/pica_enc.c`); the back end compares only ids and copies the blocks into the record. Implies `gxdirty`, `gxonehop=1`, `gximmdirect=1`, `gxthread=0`. 2: as `gxonehop=1` (keys and citro3d captures write the record) with every id block compared to the key path's words, the send decisions and the regions checked, and `gxdirty=2`; `gxone frame N:` lines with `gxthreadstats=N` |
| `gxchunk=0/1/2` | GX native step 3 (docs/PORT_3DS_GX_NATIVE.md, "Step 3 as built"). Needs `gxone=1` (off with `gxone=2`, `gpufence=0`, `gpudrawcut`, `gpudrawskip`). 0 (default) off. 1: each gxbatch batch gets one prebuilt chunk of PICA words (attribute setup, scale rows, cull, draw packets) that the game thread copies into the frame list at a use; every word run of the frame list (placement `gx frame list`, linear memory) is submitted by the render thread as a command list of its own (`GX_ProcessCommandList`) instead of being copied into citro3d's buffer. 2: as 1, and the per-draw words of every chunk use are built beside it; when the record comes back both go through a PICA register-file model, compared at every chunk draw (`gxchunk verify` warnings). `gxchunkkb=N`: the chunk store (placement `gx chunks`, default 256). With `gxthreadstats=N`: a `gxchunk frame N:` line (chunk uses, draws, words, builds, store resets, lists submitted, citro3d splits, runs copied; verify draws compared, differ, one-sided, not compared) |
| `gximmfl=0/1/2` | GX native step 4 (docs/PORT_3DS_GX_NATIVE.md, "Step 4 as built"). Needs `gxchunk=1` or `2` (so `gxone=1`, `gxthread=0`). 0 (default) off. 1: `GXEnd` routes an immediate primitive with the eligibility bits of its classes (vertex class plan, uniform block parts), converts it into the ring chunk and writes the draw into the frame list in one pass; the triangle indices go straight into the record's index arena. The pass-group route and the K hysteresis are unchanged (same words as 0). 2: as 1, and each immediate draw's old route bits and index list are compared on the game thread, its words (the old path's, built beside) through the register-file model (`gximmfl verify` and `gxchunk verify` warnings). With `gxthreadstats=N`: a `gximmfl frame N:` line (draws, merged, none; front-end checks, route differ, index differ) |
| `gxlcall=0/1/2` | Revised native plan step 5 (docs/PORT_3DS_GX_NATIVE.md, "Revised steps 5 and 4 as built"). Needs `gxbatch=1`. 0 (default) off. 1: `GXCallDisplayList` (mode word 0: C1, K) and `gx_call_list` (the translator under `gxthread`) test the open batch's next member (list, size, state/xf/vertex versions, no bounding box, empty pipe) before the per-list path; a hit only advances the batch. Off while `gxstats` counts (the per-list counters). 2: every call takes the per-list path; the test's decision is compared with it (`gxlcall verify` warnings). With `gxthreadstats=N`: a `gxlcall frame N:` line (members by the inline test per frame; verify checks, differ) |
| `gxbopen=0/1/2` | Revised native plan step 4 (same section). Needs `gxbatch=1`. 0 (default) off. 1: a batch open checks one world generation (sum of cache epoch, memory-trust generation, decision epoch) in place of the three loops; batches with equal vertex-state records share a content id, so a batch opened at the vertex version another batch of its id matched at skips the compare; the route tries the batch's pass group of its last open before the group key's hash and probe. 2: the full checks run too, and each shortcut is compared with them (`gxbopen verify` warnings). With `gxthreadstats=N`: a `gxbopen frame N:` line (opens with the generations skipped, vertex-state compares skipped, groups by the hint per frame; verify checks, differ) |
| `gxbopenxf=0/1` | With `gxbopen` (default 1 then; 0 leaves gxbopen as before). Per-slot change ticks for the position, normal and texture matrices, projection and viewport (`gx_xft`, gx_state.c). A batch open skips the description/format compare when `gx_vdesc_ver` did not move (the current-matrix indices and arrays are still compared); `group_for` keeps its last group when the key's slots did not change; the uniform block drops GXD_CLIP / GXD_TEXROW when the slots it reads did not change. `gxbopen=2` verifies each (full compare, `key_is_state`, full `vsh_prepare`). Line `gxbopenxf frame N:` with `gxthreadstats=N`. PORT_3DS_GX_NATIVE.md, "Revised steps 5 and 4 as built" |
| `gximmnow=0/1` | 0 (default). 1: a new immediate draw's pass-group record starts with the streak K (its group takes the shader path in its first frame when nothing in it is ineligible). **Not identical** to 0: up to K frames after a load, immediate draws that 0 keeps on the CPU path take the shader. Owner question open |
| `gxfogcache=0/1` | 1 (default): the fog LUT is recomputed only when the fog parameters change and goes to the back end with an id instead of being hashed; 0 recomputes and hashes it on every rebuild (A/B) |
| `gxunlit=0/1` | 1 (default): unlit colour channels skip `channel_color` and the eye-space normal; 0 the general path (A/B) |
| `gxmerge=0/1` | 1 (default): consecutive draws with nothing recorded between them extend one draw command (`plat/gpu.c`); 0 one command per draw (A/B). `gxstats` shows `gpu draws N (of M calls)` |
| `gxvsh=0/1` | The vertex-shader path (`port3ds/game/gx/gx_vsh.c`). 1 (default): each display-list cache entry gets a PICA stream, immediate-mode draws and decoded lists go through a per-frame ring, and the draws of a pass group (same position matrix, projection and viewport) that was fully eligible for `gxvshk` frames in a row are transformed by the vertex shader. 0: every vertex is transformed on the CPU (A/B, hardware fallback). `gxstats` adds `vsh` lines: shader, CPU-eligible and ineligible draws, groups, switches, splits, decal links and triggers, streams and the reasons draws stay on the CPU |
| `gxvshk=N` | Hysteresis of the shader path: fully eligible frames in a row before a group moves to the shader (default 30). It moves back to the CPU at once |
| `gxvshdecal=0/1` | 1 (default): a decal-like draw (depth compare EQUAL, or LEQUAL with no depth write) made on the CPU path links its group with the groups drawn earlier in its viewport that take or could take the shader path, so they take one path. 0: links off, only counted; for measuring the shader path itself, it breaks the offload rules |
| `gxvshimm=0/1` | 1 (default): immediate-mode draws and decoded display lists may take the shader path (per-frame ring); 0 keeps them on the CPU |
| `gxvsh_check=N` | Logs every pass group of game frame N (`gxvsh census`): path, eligibility, streak, draws per path, the reasons for CPU draws, decal links, first list, depth range, translation, projection type |
| `gxvshs16=0/1` | 1 (default): stream positions and texture coordinates as s16 with a power-of-two scale where that is exact; 0 always floats (A/B) |
| `gxvshkb=N` | Linear memory for the stream segments in KB (default 6144) |
| `gxdepthshot` | Each shot (`shots=N`) also saves the EFB depth buffer as `shots/NNNNN.dep` (`DEP1 608 480`, one little-endian D24S8 word per pixel). Debug only |
| `gxclearrgb=RRGGBB` | Test only: the frame clear uses this colour instead of the game's (the seam test clears to `FF00FF`) |
| `shotfrom=N` | The first presented frame a shot may be. With `shots=1 shotmax=K`, K consecutive frames (the flicker test) |

## Cross-check of all switches, 2026-11-26

Build `bff86be7` (port3ds after the visual round 2 merge), dev build, Azahar. `determ`, one default run per scene against each switch set, 4 consecutive shots, byte for byte (script: `determ_ab.run` with a cached default run per scene; all runs with `nodevupdate`). Scenes: gor_01 shots 780-783, effects battle (`map=gon_02 story=30 battle=1 btlauto=perfect`) 900-903, pik_00 780-783, fog `storyjump=STG1_16` (gon_11) 700-703, Grotto water `storyjump=STG5_19_01` (dou_00) 1100-1103, pause menu 780-783.

Sets: S1 `gxdirty=1`; S2 `gxdirect=1`; S3 `gxdirect=1 gximmdirect=1`; S4 `gxonehop=1`; S5 `gxdirty=1 gxdirect=1 gximmdirect=1`; S6 `gxonehop=1 gxdirty=1`; S7 `texasync=1 texfast=1 texcompact=1`; S8 `gxdirty=1 gxonehop=1 texasync=1 texfast=1 texcompact=1`.

| Scene (content: % pixels off commonest colour / colours) | S1 | S2 | S3 | S4 | S5 | S6 | S7 | S8 |
|---|---|---|---|---|---|---|---|---|
| gor_01 (91.6 / 36 569) | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 |
| effects battle (98.2 / 47 498) | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 |
| pik_00 (72.9 / 21 841) | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 |
| fog, gon_11 (80.6 / 19 997) | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 |
| Grotto water, dou_00 (27.3 / 2 259) | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 |
| pause menu (77.0 / 18 973) | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 |

All 192 comparisons identical; no differing pixels. The water shots have low content (27%, 2 259 colours) but are not blank.

- **Stereo** (`stereo_ab.py`, `stereoforce=1`, gor_01 780-783, default against S8): 12 of 12 identical (EFB, left and right screens); left differs from right in every shot, so the shots are stereo.
- **GPU hang recovery** (S8 + `gpuhangtest=700 gpuhangrecover=1`, gor_01): log shows the fence-wait hang at record 752 (frame 699), dump written, `reset #1 done`, citro3d state marked dirty, one 2306 ms slow frame at 703, then the run reached frame 900. Shots 720-725 after recovery have content (91.4% off the commonest colour, about 36 150 colours).

## Pre-decoded state lists (`gxsdl`, 2026-11-26)

`gx_call_list` on a cache miss ran `run_commands`, which decodes the LEB128 words of every port opcode of the list and calls `gx_apply_call`. With `gxsdl=1` the leading NOPs and port opcodes are decoded once and kept with a copy of the bytes (keyed by address and size, compared with `memcmp` on each call: a changed list decodes again, 0 stale in gor_01 to frame 1600); the rest of the list runs through `run_commands` from the first other opcode. Release build, windows 722-841, best of 2, ms per frame (translator / game-thread busy), `gxsdl=0` to `gxsdl=1`:

| Scene | C1 busy | C3 translator |
|---|---|---|
| gor_01 | 33.9 to 32.0 | 23.7 to 22.1 |
| pik_00 | 42.4 to 40.0 | 34.7 to 32.6 |
| usu_00 | 30.4 to 30.3 | 16.8 to 16.8 |
| battle | 22.1 to 21.0 | 20.5 to 18.9 |

Identity: `determ` shots 780-785 byte-identical in all four scenes in C1 and C3; `gw_diff` (gor_01, 1600 frames padscript) 160 records equal in C1 and C3.

## Fused state on the record (`gxrecfuse`, 2026-11-26)

Switch `gxrecfuse` (default 0; 2 is a verify mode; `gxrecfuseparts=N`, bits 1 state lists, 2 vertex description, 4 channels, default 7, for measuring). File: `port3ds/game/gx/gx_rec.c` (recording side of `gxthread`; `gx_sdl_scan` exported from `gx_state.c`). No effect with `gxthread=0` (C1).

Counts first (gor_01, C3 + `gxsdl=1`, a temporary per-call probe that compared `gx` before and after every call the translator applied): 9660 recorded calls a frame, of which 2615 changed nothing and 1918 changed only a version counter (`GXSetArray`, `GXSetVtxAttrFmt` with the values in force); plus 5855 calls from the 245 material state lists, 5519 of them with the values in force. Causes: the record's dedup table forgot everything at each state-list call, `GXClearVtxDesc` forgot the vertex-description slots, and overlapping channel ids (COLOR0A0 against COLOR0, ALPHA0) made `GXSetChanCtrl` count as changed. Cost, measured by applying each call twice (release, translator ms): +7.9 ms for the second application of the ring calls, +2.6 ms for the list calls.

What it does:
- A state list of NOPs and port opcodes only, whose bytes equal a copy kept on the game thread (compared in full on every call; a change decodes again), is not recorded as a list call: its calls go through the dedup table in order, and only those that change the translator's state are recorded. Its calls do not go into the game thread's shadow (they did not before; on the GameCube a display list does not change the SDK's register copy). Lists with vertex-description calls, primitives or anything else take the old way.
- `GXClearVtxDesc`/`GXSetVtxDesc` are applied to a copy on the game thread and recorded, before the next non-state command or fence, as the `GXSetVtxDesc` calls that take the translator's description to the new one. With the translator's description unknown (start, after a forget) the calls go as made from their last `GXClearVtxDesc`.
- `GXSetChanCtrl` and the channel colours are compared by channel and colour component.
- `gxrecfuse=2`: on the translator, after each fused list the list is applied again and must change nothing in `gx`; after each vertex-description flush `gx.vtx_desc` must equal the shadow's. `gxrecfuse frame N:` line with `gxthreadstats`.

Per frame after (gor_01): 245 lists fused, 5856 of their calls checked, 336 recorded; 2420 vertex-description calls, 80 recorded; calls applied on the translator 3102 (was 15 515), none of them without effect.

Results, release, windows 722-841, best of 2, ms (A = port3ds `5bbfc235` `gxsdl=1`, B = this build `gxsdl=1 gxrecfuse=1`), C3:

| Scene | frame A -> B | game busy A -> B | translator A -> B | render |
|---|---|---|---|---|
| gor_01 | 33.4 -> 33.5 | 23.4 -> 24.6 | 21.4 -> **14.8** | 7.4 |
| pik_00 | 33.4 -> 33.6 | 29.9 -> 31.8 | 31.5 -> **17.1** | 7.5 |
| usu_00 | 33.5 -> 33.5 | 21.5 -> 21.1 | 16.3 -> **14.5** | 6.4 |
| battle | 32.5 -> 32.5 | 15.5 -> 16.2 (fence 7.8 -> 6.3) | 19.2 -> **15.4** | 3.7 |

C1 (no translator): unchanged within 0.2 ms (gor_01 34.1/31.5, pik_00 50.2/39.6, usu_00 35.0/29.9, battle 26.8/20.7 frame/busy in both).

The work moved in part to the game thread: the list's calls are compared there (about 0.4 us each in Azahar, as the setters' own compare on the translator was); without the list part the game thread is unchanged. pik_00's game thread (31.8) is close to the 33.4 ms vblank step. Frames stay vblank-bound at 2 frames.

Identity: `determ` shots 780-785 byte-identical in gor_01, pik_00, usu_00, battle in C1 and C3 (48 of 48); `gw_diff` 1600-frame padscript gor_01 160 records equal in C1 and C3; `gxrecfuse=2` 0 mismatches in gor_01, pik_00, battle (to frame 841) and a `map_sweep.py --area tik` (22 maps, 828 K checks) and `--area jin` (12 maps, 451 K checks), every map ok. Stereo (`stereo_ab.py`, `stereoforce=1`, C3, shots 780-781): gor_01 6 of 6 identical (EFB, left, right); pik_00 left and right 4 of 4 identical, the EFB files missing in A (780) and in both runs (781), a capture gap of the run, not compared.

**Default stays 0.** Not done: console run, the other six scenes, `vsh_accept.py`.

### Follow-up 2026-11-26: list groups (`gxreclgroup`), and what did not pay

Switch `gxreclgroup` (default 0; `gx_rec.c`). Display-list calls that follow each other in the record go into one `GXR_LIST` command, 3 words a call, extended while the translator has not been given it: every publish (and, with `gxthread=1/3`, every consume) closes the open group, and a group stops at 85 calls, the ring's end and the room seen when it began. A list call no longer kicks a publish; the group goes out with the next `KICK_WORDS` (256) words or the next kicking command. The translator calls the lists in the same order. Off with `gxthread=3` (its list records carry a hash).

Counts (gor_01): 5010 list calls in 353-362 commands a frame (pik_00 5886 in 443, battle 2565 in 172).

Results, release, windows 722-841, best of 2, ms (A = port3ds `b592f907` `gxsdl=1 gxrecfuse=1`, B = this build, the same plus `gxreclgroup=1`), C3:

| Scene | frame A -> B | game busy A -> B | translator A -> B | render |
|---|---|---|---|---|
| gor_01 | 33.4 -> 33.4 | 24.6 -> 24.5 | 14.8 -> **13.9** | 7.4 |
| pik_00 | 33.6 -> 33.4 | 31.7 -> 31.5 | 17.0 -> **16.0** | 7.5 |
| usu_00 | 33.5 -> 33.5 | 21.1 -> 21.0 | 14.5 -> **13.8** | 6.5 |
| battle | 32.5 -> 32.4 | 16.1 -> 16.1 (fence 6.2) | 15.4 -> **14.9** | 3.8 |

C1: unchanged within 0.4 ms (frame/busy B: gor_01 33.9/31.5, pik_00 50.2/39.6, usu_00 34.4/29.8, battle 27.2/20.9). Game thread alone (`gxnull=1`): gor_01 24.4 -> 24.3, usu_00 21.3 -> 21.4. A first version that checked the ring position on each call cost the game thread 0.5-1.0 ms (`gxnull`); closing the group in `publish` removed that.

Identity: `determ` shots 780-785 byte-identical against A in gor_01, pik_00, usu_00, battle, C1 and C3 (48 of 48); `gw_diff` 1600-frame padscript gor_01 160 records equal in C1 and C3; `gxrecfuse=2 gxreclgroup=1` 0 mismatches in gor_01, pik_00, battle (to frame 841), `map_sweep.py --area tik` (22 maps ok, 820 K checks) and `--area jin` (12 maps ok, 458 K checks). Stereo (`stereoforce=1`, C3, shots 780-781 against A): gor_01 6 of 6 identical (EFB, left, right); pik_00 5 of 5 (both runs timed out before the last file, as in the run above).

Not kept (measured, reverted):
- Change stamps per dedup slot, to skip the compares of a fused list (task: skip whole lists). A list's slots are written by other lists and calls between two calls of it (with other values, then back), so no list was ever skipped whole; comparing only the calls whose slots changed since the list's last pass skipped 46% of the compares (3161 of 5856 still compared, gor_01), but stamping the 2900 changed calls a frame cost more: `gxnull` game thread gor_01 24.4 -> 25.5, pik_00 33.9 -> 34.7. `gxrecfuse=2` found 0 wrong skips.
- **Inlining trap**: the first stamp builds made LTO stop inlining `gx_rec_call` (and `dd_same`) into the GX setters (each setter shrank about 1 KB, `.text` 64 KB smaller): the game thread then took the generic path for 21 000 calls a frame, +5.4 ms (gor_01 C3 24.6 -> 30.0, pik_00 C3 to 3 vblanks), with `gxrecfuse=0` too. Check `GXSetVtxDesc`/`GXSetChanCtrl` sizes in the ELF after any change to `gx_rec_call`, `dd_same` or what they inline; a cold, `noinline` helper for added work keeps them inlined.
- The "old way" lists (up to 83 a frame in tik_21, 44 in jin_05): the reason counters added to the `gxrecfuse` line show all of them as "more bytes": lists the game built with state calls that start with a primitive (no leading state calls; first byte 0x98). Fusing them needs the primitive sizes, which come from the translator's vertex state at the call; not done. Dropping a state-list address when the game rebuilds the buffer without state calls was tried: 0 such rebuilds in tik.

Translator profile: `pc_sample` puts 21% of the translator's busy samples in `gx_draw_frame_begin`, which the CopyDisp timer bounds at 0.5 ms a frame; a timer per record adds about 3 us each (translator 14.8 -> 42.6 ms). Neither gives a breakdown of the remaining 13.9 ms. Counters: 243 batch opens drawing 4780 of 5010 lists, 3000 state calls applied, 369 immediate primitives a frame (gor_01).

## Where the record path's time goes (2026-11-26)

Release builds (LTO, GX `-O3`, `TTYD_HOT_O3`), Azahar, ms per frame over windows 722-841 (frame N lines at 781 and 841 for the counters). C3 here is C1 + `gxthread=2 gxpipeline=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1`. Azahar ms are ratios, not console ms.

### Method

- **Translator: switch `gxphase=N`** (default 0, logs with `gxthreadstats`, `gx_rec.c` `consume_ph`; nested hooks in `gx_dlcache.c` and `gx_draw.c`). One outer command in N is timed (each list call of a `GXR_LIST` group counts as one), and inside it every nested phase; time is exclusive. Every timed command starts with an empty nested phase; its segment cost (1.42-1.52 us) is taken off each segment. With `gxphase=1` every command is timed and the sum of the phases equals the translator's busy time without the timers (gor_01 13.9 against 13.9-14.0 ms), so the subtraction holds there. Off cost (port3ds against this build, `gxphase=0`): translator 13.9 / 14.0 ms, game thread 24.6 / 24.5 (C3) and 24.3 / 24.4 (`gxnull`).
- **Game thread: no timers.** A first version timed the record path too (`gx_rec_call`, lists, primitives, `GXEnd`, `GXCallDisplayList`). It was not usable: (1) its hooks cost 0.3 ms in `gx_rec_call` and about 1 ms more elsewhere with `gxphase=0` (`gxnull` gor_01 24.4 -> 25.7); (2) the timed path ran the generic `gx_rec_call` body (call id not a constant), not the setters' inlined one, so the dedup compare read 7.6 ms where doubling it costs 1.1 ms; the sum of the phases came to 23.8 ms of 24.4. It was removed (commit `14a7bdfa`).
- **Game thread: ablation under `gxnull=1`.** With `gxnull` the record is dropped, so a part of the record path can be left out without an effect on the game (the shadow that the getters read is kept up to date). A local build (not committed) with switch `gxphx=mask` returns early at one point per bit; the busy time that goes away is that part's cost. Parts that cannot be left out are measured by doubling them (`dd_same` and `gx_imm_convert` give the same result twice). The comparisons are valid only between runs at the same frame period: when the frame changes vblank count, busy also changes by other core-0 work (pik_00 with `gxnull` runs at 50 ms, the others at 33.4 ms; marked "n/a" below).

### Game thread (ms per frame; `gxnull=1` unless marked C3)

| Part (calls per frame, gor_01) | gor_01 | usu_00 | battle | pik_00 |
|---|---|---|---|---|
| busy, `gxnull=1` (port3ds) | 24.3 | 21.4 | 18.1 | about 35 (50 ms frames) |
| all setter recording, the REC macro and in it (15 600 calls + 2 420 vertex-description calls) | 5.5 | n/a | n/a | n/a |
| - the macro around `gx_rec_call` (primitive close check, `gx_dl_recording`, `gx_rec_on` with its TLS read, argument array) | 1.7 | n/a | n/a | n/a |
| - `gx_rec_call` body | 3.8-4.1 | n/a | 1.8 | n/a |
| - - dedup compare `dd_same` (doubled) | 1.1 | 1.3 | 0.5 | 0.9 |
| - - ring write of the recorded calls (`put`/`commit`, 2 930 of them) | 0.3 | 0.6 | 0.3 | 0.5 |
| - - vertex-description log / flush | 0.4 / 0.3 | | | |
| `gx_rec_list` (5 010 list calls) | 4.6 | 2.8 | 2.4 | n/a |
| - fused state lists (245 lists, 5 856 calls) | 2.3 | 0.1 | 1.2 | n/a |
| - - their per-call compare loop | 2.0 | 0.1 | 1.0 | n/a |
| - - `fl_get` (find, compare the bytes) | 0.3 | | 0.2 | |
| - `is_state_list` (all calls) | about 0.7 | | | |
| - group write (`rec_list_grouped`) | 0.8 | | | |
| primitives, `gxnull` path (`gx_rec_prim`: index ranges, snapshot copies; memcpy 0.3) | 2.7 | n/a | n/a | n/a |
| primitives, C3 path: `gx_imm_convert` (doubled; 376 / 478 / - / 1 155 primitives) | 1.9 | 2.6 | n/a | n/a |
| left: game logic and the GX entry points not above (`GXCallDisplayList`, `GXBegin`/`GXEnd`, pipe writes, texture binds) | about 11.6 | | | |

Counts per frame, C3, frame 841: gor_01 21 441 state calls through the dedup table (15 585 setter + 5 856 fused-list calls), 18 513 not recorded; 5 010 list calls in 353 group commands; 2 420 vertex-description calls, 80 recorded; 376 immediate primitives. pik_00: 21 649 / 18 954; 5 886 lists in 444 commands; 314 fused lists (7 507 calls); 1 155 primitives. usu_00: 16 196 / 13 897; 4 105 lists; 10 fused lists; 478 primitives.

What this changes: the earlier "game logic is about 25% of the game thread" came from `pc_sample`. The parts above that were measured add up to 12.8 ms of gor_01's 24.4 (setters 5.5, lists 4.6, primitives 2.7); the rest, about 11.6 ms, is game logic plus GX entry-point code not measured here. No single part is larger than 5.5 ms.

A Bloom filter in front of `is_state_list`'s address probe (same answers) was tried: no change (gor_01 `gxnull` 24.4 / 24.4), not kept. Copying `GX_F32` components as bits in `gx_imm_convert` (same bytes) was tried: no change (gor_01 24.3 / 24.4, usu_00 21.1 / 21.3, pik_00 31.2 / 31.8), not kept.

### Translator (C3, `gxphase=1`, frame 841, ms per frame, calls per frame)

| Phase | gor_01 | pik_00 | usu_00 | battle |
|---|---|---|---|---|
| list call, own time (batch-member fast path, `gx_call_list`, `gx_sdl_run`, decode of uncached lists) | 2.38 (5 007) | 1.19 (5 886) | 2.88 (4 108) | 5.43 (2 585) |
| batch open `bat_open` (incl. `gx_draw_batch_begin`) | 2.09 (242) | 2.88 (358) | 2.52 (150) | 0.98 (123) |
| batch close `bat_close` | 0.19 (242) | 0.28 (358) | 0.12 (150) | 0.10 (123) |
| per-list slow path `call_slow` | 2.41 (230) | 0.99 (102) | 2.20 (243) | 0.85 (175) |
| batch forming `form_add`/`form_end` | 0.00 (484) | 0.00 (458) | 0.00 (359) | 0.15 (262) |
| state build `ensure_state` | 2.30 (846) | 2.34 (1 606) | 1.55 (840) | 1.69 (465) |
| state calls `gx_apply_call` | 1.24 (3 003) | 0.73 (2 842) | 0.85 (2 345) | 0.68 (1 359) |
| immediate primitives `GXR_PRIMD` (`gx_draw_prim_ring`) | 2.76 (375) | 6.18 (1 154) | 3.13 (478) | 1.93 (209) |
| other (CopyDisp) | 0.49 | 0.60 | 0.46 | 0.42 |
| sum | 13.9 | 15.2 | 13.7 | 12.2 |
| translator busy, `gxphase=0` | 13.9 | 16.2 | 14.0 | 15.0 |

The battle's content changes from window to window (frame 781: 798 primitives, 6.3 ms; frame 841: 209, 1.9 ms). Per call the large ones are `bat_open` (8-9 us), `call_slow` (10 us in gor_01) and an immediate primitive (5-7 us); a list call is 0.2-0.5 us. With `gxphase=1` the frame takes 77-92 ms (the translator is then the critical path), so render-thread preemption on core 2 may fall inside timed segments; the per-frame sums still match the untimed busy within 1 ms except in the battle.

### Change kept: the fused-list compare loop

`fuse_list`'s loop keeps the unchanged calls (5 519 of 5 856 in gor_01) in a loop with its values in registers and an unrolled compare; a call that compares its own way (channels), has no slot or differs goes to `fuse_call_slow`, which notes and records it as before (same table writes, same records, same order). Release, best of 2, ms (A = port3ds `aa01c44c`, B = this build):

| Scene | C1 frame/busy A -> B | C3 game busy A -> B | C3 translator A -> B | `gxnull` busy A -> B |
|---|---|---|---|---|
| gor_01 | 34.3/31.5 -> 34.7/31.6 | 24.5 -> **24.2** | 13.9 -> 14.1 | 24.3 -> **24.1** |
| pik_00 | 50.2/39.6 -> 50.2/39.8 | 31.6 -> **31.2** | 16.0 -> 16.3 | 33.7 -> **33.4** |
| usu_00 | 35.0/29.8 -> 34.6/29.9 | 20.9 -> 21.2 | 13.8 -> 14.0 | 20.9 -> 21.1 |
| battle | 27.0/21.0 -> 26.8/20.9 | 16.1 -> 16.0 | 14.9 -> 14.9 | 18.0 -> 17.9 |

C3 frames stay at 33.4-33.6 (battle 32.7 -> 32.3). The gain is small (0.2-0.4 ms where state lists are fused: 245 a frame in gor_01, 314 in pik_00, 133 in the battle; usu_00 fuses 10 and its +0.2-0.3 is within the spread of two runs). B includes the `gxphase` hooks (translator +0.1-0.3, within the spread; the hooks alone measured 13.9 / 14.0).

Identity (B against A, both `gxsdl=1 gxrecfuse=1 gxreclgroup=1`): `determ` shots 780-785 byte-identical in gor_01, usu_00, battle (C1 and C3) and pik_00 C1, 6 of 6 each. In pik_00 C3 the B runs saved 4 of 6 shots three times (missing 782/785, 782/784, and in one run all 6); every saved shot was identical. A build with only the `gxphase` code loses shots in the same way (782/785), and so does the stereo run in the section above, so this is a capture gap of the run and not a picture change. Shot 782 was not compared in pik_00 C3. `gw_diff` 1600-frame padscript gor_01: 160 records equal in C1 and C3. `gxrecfuse=2`: 0 mismatches in gor_01, pik_00, battle (to frame 841), `map_sweep.py --area tik` (22 of 22 maps ok, 820 K checks) and `--area jin` (12 of 12 ok, 458 K checks). Stereo (`stereoforce=1`, C3, gor_01 780-782): 9 of 9 identical (EFB, left, right); left differs from right.

### Next steps (from the tables)

- Game thread: no part is larger than 5.5 ms, and the measured parts add up to about half of it. To reach 16.7 ms in gor_01 (-7.7 ms) several parts must go at once, or work must move off the thread. The largest: the setters (5.5 ms, of which 1.7 ms is the macro around `gx_rec_call`: TLS read in `gx_rec_on`, `gx_dl_recording`, primitive close check, argument array), the list path (4.6 ms, of which 2.0 ms is the fused-list compare), and immediate-mode conversion in C3 (1.9-2.6 ms; with `gximmdirect=0` it goes back to the translator).
- The macro cost is a candidate: one flag that combines "recording for a translator, not in a display list, no primitive open" and that the setters read in one load in place of three tests and a TLS read.
- The unmeasured 11.6 ms (gor_01) needs a split between game logic and the GX entry points that are not measured here (`GXCallDisplayList`, `GXBegin`/`GXEnd`, pipe writes), by the same ablation under `gxnull`.
- Translator: `bat_open` (8-9 us a call), `call_slow` and immediate primitives (5-7 us each, 6.2 ms in pik_00) are the largest per call. pik_00's translator (16.0-16.3 ms) is close to 16.7.

## The setters' one-word test, the remainder split, and small translator cuts (2026-11-27)

Release builds, Azahar, windows 722-841, best of 2, ms per frame. C1 = `gxdirty=1 gxdirect=1 gximmdirect=1 texfast=1 texasync=1`; C3 = C1 + `gxthread=2 gxpipeline=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1`; N = C3 + `gxnull=1` (game thread alone). Base = port3ds `38bf6170`.

### Changes (no switch; same records, same order, same pictures)

1. **One word for the setters' test** (`gx_internal.h` `gx_rec_fast`, `gx_state.c` REC/REC0/RECT, `GX_CLOSE_PRIMITIVE`). `gx_rec_fast.w`: 0 = nothing records and no display list or primitive is open (every thread applies the call); 1 = the game thread records with no display list and no primitive open (`gx_rec_call`); 2 = the full test as before. The translator thread always applies; it is told apart by its stack range (`tr_lo`, `tr_len`, set by `translator_main`), not by a TLS read. `gx_rec_fast_update()` runs at every change of `gx_rec_mode`, `gx_rec_direct`, the display list being recorded, `gx_prim_open` and the game thread's `gx_tls_translator`. Setters stay inlined (no call of the generic `gx_rec_call` from `GXSetVtxDesc`, `GXSetChanCtrl`, `GXSetTevOrder`, `GXSetTevColorIn`, `GXLoadPosMtxImm`, `GXSetNumChans`, `GXSetVtxAttrFmt`, `GXSetArray`, `GXClearVtxDesc`, `GXSetCurrentMtx`, `GXSetTevOp`; `.text` +4 KB).
   - Not kept: the thread's TPIDRURO (`mrc p15, 0, rX, c13, c0, 3`) as the word. Game thread C3 -1.0 / -0.9 / -1.4 / -0.4 ms, but C1 +1.2..+1.8 ms and the translator +0.1..+0.4: an `mrc` (and so a TLS read, `__aeabi_read_tp`) costs about 0.1 us in Azahar. The word 0/1/2 with the stack test gives up part of the game-thread gain and has no C1 cost.
2. **Immediate primitives** (`gx_rec.c`, `gx_vsh.c`): the vertex-state hash of each `GXR_PRIMD` (game thread at `GXEnd`, translator at the draw) only fed the "vertex-state differences" counter (the primitive is drawn as written, never seen non-zero); both threads compute it with `gximmdirect=2` only. `group_for` compares the last group's key with the state in place before it builds a key.
3. **`GXCallDisplayList` and `run_pending_raw`** (`gx_pipe.c`): with `w == 1` off the translator they go straight to `gx_rec_raw`/`gx_rec_list`; `run_pending_raw` reads `gx_rec_mode` before the TLS word.

Results, base -> this build (game busy; translator in brackets; N is game busy with `gxnull`):

| Scene | C1 busy | C3 game busy | C3 translator | N busy |
|---|---|---|---|---|
| gor_01 | 33.5 -> 33.0 | 24.2 -> **23.1** | 14.0 -> 13.8 | 24.2 -> **23.4** |
| pik_00 | 42.3 -> 42.4 | 31.2 -> **30.4** | 16.2 -> 16.4 | 33.4 -> 32.8 (frame 46.8 -> 36.6, n/a) |
| usu_00 | 30.0 -> 29.6 | 21.0 -> **20.0** | 13.9 -> 13.8 | 21.2 -> **20.2** |
| battle | 21.7 -> 20.8 | 16.0 -> 15.8 | 15.0 -> **13.4** | 18.0 -> 17.6 |

Frames stay vblank-bound (C3 33.3-33.4, battle 32.1-32.3). Per change (C3 game / N): change 1: gor_01 -0.6 / -0.5, pik_00 -0.3 / n/a, usu_00 -0.8 / -0.6, battle 0.0 / -0.1; change 3: -0.5 / -0.3, -0.4 / -0.3, -0.5 / -0.5, -0.2 / -0.3; change 2: game -0.1..-0.2, translator -0.1..-0.2. pik_00's translator read 16.6 in all three builds with change 1 against 16.2-16.4 for base (render-thread preemption on core 2 is in that number; not resolved). The battle translator (15.0 -> 13.4) moves with the battle's content in the window and is not claimed as a gain of these changes.

Identity (this build against base, both `gxsdl=1 gxrecfuse=1 gxreclgroup=1`): `determ` shots 780-785 byte-identical in gor_01, usu_00, battle (C1 and C3) and pik_00 C1, 6 of 6 each; pik_00 C3: the first run of this build timed out (no shots); in the second, base saved 4 of 6 shots (782 and 785 missing, the capture gap described above) and all 4 were identical. `gw_diff` 1600-frame padscript gor_01: 160 records equal in C1 and C3. `gxrecfuse=2 gxreclgroup=1`: 0 mismatches in gor_01, pik_00, battle (to frame 841), `map_sweep.py --area tik` (22 of 22 maps ok, 819 K checks) and `--area jin` (12 of 12 ok, 458 K checks). `gximmdirect=2` (C3): gor_01 to frame 841, 91 923 checks, 0 mismatches, 0 vertex-state differences; pik_00 0 mismatches to frame 601, then a fatal "no memory to convert 41756 bytes" (texture conversion, frame 631), the same with base: the verify buffers leave too little memory in pik_00 (not a change of this round). Stereo (`stereo_ab.py`, `stereoforce=1`, this build, `gxrecfuse=0` against `gxrecfuse=1 gxreclgroup=1`, gor_01 780-781): 6 of 6 identical (EFB, left, right); left differs from right. (`stereo_ab.py` uses `build/azahar-profile`, which needs `game.iso` in its sdmc; without it the runs stall at the boot screen.)

### The game thread's remainder, split (ablation under `gxnull=1`)

Method as above: a local build (not committed) with switch `gxphx=mask` returns early at one point per bit; the busy time that goes away is that part's cost. New here: a local switch `phxvbl=N` makes every frame last at least N retraces, so all runs of a scene have the same frame period (2 retraces, pik_00 3) and can be compared; without it 8 of 24 runs changed period. With `phxvbl=2` the battle reaches a different part of its script in the window (busy 10.2 against 18 without), so its column is a different content. Counters per frame from the same build. Bits: 1 `GXCallDisplayList` at entry; 16 only its `gx_rec_list`; 2 `GXEnd` after the state reset; 4 `GXLoadTexObj`/`GXLoadTlut`; 8 the setters' `gx_rec_call` (after the one-word test); 64 the setters at the start of REC.

| Part (calls per frame gor_01 / usu_00 / battle / pik_00) | gor_01 | usu_00 | battle (other content) | pik_00 (50 ms frames) |
|---|---|---|---|---|
| busy, `gxnull=1` | 24.8 | 20.7 | 10.2 | 34.0 |
| `GXCallDisplayList` in all (5 264 / 4 106 / 2 698 / 6 200) | 5.6 | 2.9 | 2.9 | 7.2 |
| - `gx_rec_list` | 4.5 | 2.0 | 2.3 | 5.9 |
| - the entry point around it (primitive, display-list and TLS tests, `run_pending_raw`) | **1.1** | 0.9 | 0.6 | 1.3 |
| `GXEnd` (391 / 478 / 228 / 1 155 primitives: `gx_rec_prim_direct`, `gx_imm_convert`, ring write) | 3.0 | 3.8 | 1.4 | 10.1 |
| texture loads (254 / 343 / 107 / 178) | 0.3 | 0.3 | 0.1 | 0.3 |
| setters from REC (about 18 000 calls in gor_01) | 8.6 | 7.2 | 3.9 | 11.9 |
| - `gx_rec_call` and the argument array | 5.5 | 5.7 | 2.3 | 9.2 |
| - the test before it and what the setter does before REC (includes the two ablation tests, about 0.3-0.5 ms) | 3.1 | 1.5 | 1.6 | 2.7 |
| the four parts at once (bits 1, 2, 4, 64) | 14.4 | 12.3 | 6.8 | 22.0 |
| sum of the four alone | 17.5 | 14.2 | 8.3 | 29.5 |
| **left with all four off: game logic, pipe writes, `GXBegin`, other GX entry points (`GXInitTexObj`, getters, SDK matrix code)** | **10.4** | **8.4** | 3.4 | 12.0 |

- The parts are not additive: the four alone sum to 1.5-7.5 ms more than all four at once. Work that runs at a list or a primitive for the setters before it (the vertex-description flush, `fuse_vd_flush`) is counted in both. So "game logic" in gor_01 is at most 10.4 ms and at least 24.8 - 17.5 = 7.3 ms.
- Pipe writes (the game's `GXPosition`/`GXColor`/... inline writes) were not ablated: they cannot be left out without changing what the game draws, and doubling them changes the slot layout. Count: 5 995 slots a frame in gor_01, 7 343 usu_00, 4 502 battle, 15 947 pik_00; at 4-6 instructions a write and about 4.5 ns an instruction (Azahar, from the 0.21 us of the 50-instruction `GXCallDisplayList` entry) that is about 0.1-0.4 ms (estimate).
- The largest GX-side part not measured before was the `GXCallDisplayList` entry (1.1 ms in gor_01, 0.2 us a call): change 3 above cut it (-0.3..-0.5 ms).

### Translator, immediate primitives and batch open (C3, `gxphase=1`, local probe phases, frame 841)

Local nested phases (not committed) inside `gx_draw_imm_route`, `gx_draw_imm_shader`, `gx_draw_prim_ring` and `gx_draw_batch_begin`, ms per frame (calls):

| Phase | gor_01 | pik_00 | usu_00 | battle |
|---|---|---|---|---|
| primitive, own time (dispatch, the vertex-state hash, route checks) | 0.24 (375) | 2.93 (1 154) | 0.94 (479) | 0.21 (209) |
| `vsh_state` (primitives and batch opens) | 0.88 (618) | 1.50 (1 513) | 1.84 (629) | 0.72 (333) |
| route: `gx_vsh_route` / `gx_vsh_batch_route` (group key, hash, record) | 1.77 (618) | **2.80** (1 513) | 1.59 (629) | 0.83 (333) |
| uniforms `plat_gpu_vsh_const`/`_light` | 0.98 (618) | 1.34 (1 513) | 1.64 (629) | 0.49 (330) |
| draw `plat_gpu_vsh_draw` | 0.26 (375) | 1.13 (1 154) | 0.42 (479) | 0.24 (206) |
| index build `gx_vsh_imm_at` | 0.13 (375) | 0.92 (1 154) | 0.27 (479) | 0.07 (206) |
| batch member state check `bat_same_state` | 0.43 (242) | 0.48 (358) | 0.20 (150) | 0.27 (123) |
| `bat_open` own | 0.07 | 0.21 | 0.19 | 0.05 |
| state build `ensure_state` (as before) | 2.22 | 2.37 | 1.76 | 1.64 |

So a pik_00 primitive is about 2.5 us own + 1.0 `vsh_state` + 1.9 route + 0.9 uniforms + 1.0 draw + 0.8 indices + its share of `ensure_state`. Change 2 took the vertex-state hash out of the own time (pik_00 translator -0.2 ms). The rest is spread; the route (group key of 25 words, its hash and the record probe, 1.8 us a call in pik_00) is the largest single piece.

### Next steps

- Game thread (gor_01 C3 23.1 ms, 6.4 over 16.7): the setters' `gx_rec_call` (5.5 ms) and `gx_rec_list` (4.5 ms) stay the largest GX parts; `GXEnd` is 10 ms in pik_00 (1 155 primitives, 8.7 us each). Game logic is 7-10 ms in gor_01, so the GX side must lose 6+ ms, or work must move off core 0 (owner decision pending).
- Translator: the vertex-shader route per draw (group key and hash) and `vsh_state` per draw; a cache of the group per loaded matrix slot would skip the key build when only another slot changed.
- C1 is not changed (within ±0.5 ms).

## Immediate primitives on the game thread, the vsh route, and the verify mode's heap (2026-11-27, later)

Release builds, Azahar, windows 722-841, best of 2, ms per frame. C1, C3 and N as in the section above. Base = port3ds `e22f2ea0`.

### Changes (no switch; same records, same order, same pictures)

1. **`gximmdirect=2` ran pik_00 out of heap** (`gx_pipe.c`). The fatal "a/p_dokan_x: no memory to convert 41756 bytes" at frame ~630 is a 5 KB `calloc` in `port_swap_begin` that failed. Heap use (a local per-frame `mallinfo` probe) in pik_00 C3: 61.8 MB of 83.1 MB before the warp, then 81.4-81.8 MB at the first map load (gxcache's tables come in at once, about +12 MB, then the cache's blobs), 75.8 MB after it. The verify mode's two fixed sets (VERTS_MAX vertices of ring bytes and `GxVertex`, 1.4 MB) took the rest; the crash came in some runs and not in others (the load's peak moves with the texture worker). The buffers now grow to the largest primitive seen; a failed growth skips the check and counts it (`skipped for want of memory` in the `gximmdirect frame` line). A failed conversion bitmap logs the heap before the fatal. `gximmdirect=1` does not take these buffers; its chunks are linear memory with a fallback (no chunk: the primitive is recorded the old way), and primitives larger than VERTS_MAX take the old way, so it has no fixed-size limit of this kind. **Its margin in pik_00 C3 is still only about 1.3-1.7 MB of heap at the first map load**: any new heap use in C3 can hit the same fatal.
2. **`gx_imm_convert`** (`gx_pipe.c`). Ablation in C3 (local switches, `phxvbl=2`): `GXEnd` on the game thread is 7.2 ms in pik_00 (1 155 primitives, 4 511 vertices a frame), of which 6.2 ms is the conversion: 1.5 ms building the plan, 4.7 ms the vertices (about 1 us a vertex); a scratch destination in place of the ring chunk changes nothing (the ring memory is not the cost). gor_01: 2.5 ms, 2.2 ms conversion. pik_00's plan: about 2.4 indexed float steps and one indexed RGBA8 colour a vertex. Now: the plan is kept (4 per thread, keyed by the vertex description and the format's attribute formats; array bases and strides are taken from the state at each primitive; 4 builds a frame in pik_00 instead of 1 155); the template words no step writes come from a fill list; each step runs over all vertices with its kind, type and count decided once; float components (direct and indexed) and RGBA8/RGBX8/RGB8 indexed colours are word copies (the same bytes as the f32 load and store). A first version with only the step-by-step loop and the plan cache was not faster (pik_00 C3 30.4 -> 30.7).
3. **The vsh route** (`gx_vsh.c`). `gxphase=1` with local nested phases (pik_00, 1 247 routes a frame): `rec_for` and `touch` 0.86-0.89 ms, `group_for` 0.34-0.48, `vsh_state` 0.36-0.39, reasons and room 0.31-0.33, decal 0. Counters: every immediate draw (1 153 a frame) probed the record table (no hint: the caller is the list); `group_for` took the version fast path 1 098 times, the last key 102, found 32 and created 371 groups a frame. Now a table of 512 hints by caller is checked first (a hint counts only when its record is the caller's; a record is never in the table twice, so the result is the probe's), and `group_for` hashes and compares the state in place (the key is built only for a new group; the hash only orders the probe, a group is found by its whole key).

Not done: **task "specialise the record for the most frequent setters"**. Counts (C3, frame 781, calls to `gx_rec_call` a frame / of them not recorded): pik_00 `SetVtxDesc` 1 899 (vertex-description log), `SetVtxAttrFmt` 1 899 / 1 857, `SetArray` 1 829 / 1 568, `ClearVtxDesc` 533, `SetTexCoordGen2` 472 / 472, `SetTevKColor` 473 / 473, `LoadPosMtx` 467 / 60, `SetNumTexGens` 460 / 397, TEV order/in/op 402 each / 306-402, `SetChanCtrl` 377 / 345; gor_01 the same order (1 875, 1 875 / 1 793, 1 835 / 1 344, ...). The setters are already inlined with the call id constant: `GXSetVtxAttrFmt`'s dedup path is about 45 instructions (one-word test, stack-range test, five word compares and stores, counters) with no call and no generic array, and `GXSetVtxDesc`'s log path about 40. What a specialised record could still remove (the compare of words the slot implies, the two statistics counters) is a few instructions a call, 0.1-0.3 ms; not tried, for the inlining risk.

The ELF diff (`symdiff.py`, base -> this build): `gx_imm_convert` +1 148 bytes, `group_for` +644, `rec_for` +60; no GX setter changed size.

### Results, base -> this build

| Scene | C1 frame / busy | C3 game busy | C3 translator | N busy |
|---|---|---|---|---|
| gor_01 | 39.7 / 32.9 -> 34.4 / 31.7 | 23.2 -> **22.5** | 13.8 -> **13.3** | 23.3 -> 23.4 |
| pik_00 | 50.2 / 42.4 -> 50.2 / 39.5 | 30.4 -> **28.1** | 16.4 -> **15.5** | 32.8 -> 32.7 |
| usu_00 | 34.3 / 29.6 -> 33.6 / 28.3 | 19.8 -> **18.9** | 13.8 -> **13.5** | 20.0 -> 19.9 |
| battle | 27.5 / 20.6 -> 27.1 / 19.6 | 15.9 -> 15.9 | 13.5 -> 13.7 (content changes) | 17.7 -> 17.8 |

C3 frames stay vblank-bound (33.4-33.5; usu_00 32.2, battle 27.4-31.6 with its content). N (`gxnull=1`) does not change, as it should: with `gxnull` the game thread records primitives the old way (`gx_rec_prim`, index ranges and snapshots), not through `gx_imm_convert`. **So the "GXEnd" row of the remainder split above (10.1 ms in pik_00 under `gxnull`) measured the old-way record, not the C3 path**; the C3 path is the 7.2 ms of change 2. Per change (C3 game / translator): change 2: pik_00 30.4 -> 28.0, gor_01 23.3 -> 22.4 (same-build runs); change 3: translator gor_01 13.8 -> 13.3, pik_00 16.4 -> 15.5, usu_00 13.9 -> 13.4, battle 14.2 -> 13.8, game thread +0.1..0.2 (within the spread).

Identity (this build against base, both `gxsdl=1 gxrecfuse=1 gxreclgroup=1` in C3): `determ` shots 780-785 byte-identical in gor_01, pik_00, usu_00, battle in C1 and C3 (48 of 48; pik_00 C3's base run timed out once and was run again: 6 of 6). `gw_diff` 1600-frame padscript gor_01: 160 records equal in C1 and C3. `gximmdirect=2`: C3 pik_00 265 343 checks, gor_01 91 923, battle 169 152, usu_00 104 072; C1 pik_00 264 228, gor_01 90 832; 0 mismatches, 0 skipped, to frame 841. `gxrecfuse=2 gxreclgroup=1`: 0 mismatches in gor_01, pik_00, battle (to frame 841), `map_sweep.py --area tik` (22 of 22 maps ok, 814 K checks) and `--area jin` (12 of 12 ok, 458 K checks). Stereo (`stereoforce=1`, C3, gor_01 780-782, against base): 9 of 9 identical (EFB, left, right).

### Next steps

- Game thread (C3 gor_01 22.5, pik_00 28.1 ms): pik_00's `GXEnd` is now about 4.8 ms (7.2 - 2.4); the vertex loop (indexed float steps) is still the most of it. The list path (4.5-5.9 ms) and the setters (5.5-9.2 ms by the old ablation) are next, and game logic stays 7-10 ms: 16.7 ms on core 0 needs work off the thread (owner decision pending).
- Translator (pik_00 15.5 ms): `bat_open` (2.9 ms), `ensure_state` (2.1), state calls (1.4-2.3), and the primitive path (2.3).
- pik_00's heap margin in C3 at the first map load (about 1.3 MB): gxcache's tables (about 12 MB, allocated at the first map) are the place to look before more C3 heap use is added.

## Step 2 of the native plan: `gxone` (2026-11-27)

Design and files: docs/PORT_3DS_GX_NATIVE.md, "Step 2 as built". Switch row: "For PORT_3DS_TESTING.md" above. Default 0. Built on the step 0+1 branch (`67b4603b`).

### What changes on the shader path (C1 + `gxone=1`)

| Per state change | C1 (`gxdirty=1 gxdirect=1 gximmdirect=1`) | `gxone=1` |
|---|---|---|
| TEV rebuild | `assign_units`, memo key + hash + compare, fog stage | `assign_units`, one key (memo bytes + fog unit and colour) + hash + compare; a hit copies the region, no compile |
| PIX/SCI rebuild | `build_pix` (+ `build_fog`), `build_sci` | 17-word GX key + hash + compare; a hit copies the regions (`build_fog` still runs on a PIX change) |
| TEX rebuild | handle lookups | handle lookups, then a 12-word key + hash + compare |
| hand-over to the back end | `plat_gpu_set_state_dirty`: word compare of the dirty regions, a delta record | `plat_gpu_set_state_one`: one word per class; region copy only on an id change |
| at the draw | render thread (core 2): `direct_state` (28/18/12-word key compares, `tex_prepare` with citro3d's sampler setters, DTab hash + compare), copy | game thread (core 0): one id compare per class, copy of the id's block (the texture epoch is one more word) |
| block on a miss | citro3d setters + `C3Di_UpdateContext` capture on the render thread | `pica_enc.c` on the game thread, once per id |

The render thread under `gxone=1` only copies word runs (gxonehop=1). Expected effect (not measured): core-0 time about as gxonehop=1 minus the key work it moved there (`oh_state`, `oh_tex_prepare`, `plat_gpu_set_state_dirty`'s compares, the TEV memo and `build_pix`/`build_sci` on hits); render thread as gxonehop=1 (1.0-1.4 ms against 3.5-8.1 in C1, Stage 5 table).

### Checks done without the emulator

- **Hot setter sizes** (release, LTO, `nm -S`, this build against a release build of `67b4603b` made the same way): every `GXSet*`, `GXLoad*`, `GXBegin`, `GXCallDisplayList`, `GXClearVtxDesc`, `gx_rec_call`, `gx_rec_list`, `vsh_prepare`, `gx_draw_vertices`, `gx_imm_convert` (+4 bytes) are the same size. Functions that changed size and do the same calls (LTO partition layout): `encode_vertex` (gx_pipe.c, display-list recording only: 4860 -> 3988, 5 instead of 10 self-call sites), `vsh_light_rows` (3000 -> 2700), `render_record` (-276), `bat_open` (-56), `gx_call_list` (-24); the call lists of `bat_open`, `gx_call_list` and `render_record` are the same.
- **Encoders against citro3d's words** (`tools/port3ds/pica_enc_test`, host build): 477 TEV, 773 TEX and 448 PIX blocks from the two citro3d command lists in `build/azahar-profile/data/azahar-emu/sdmc/3ds/ttyd/gpuhang/`, **0 differ**, the fixed context values the same in all blocks (stencil `ff000010 00000000`, blend colour 0, fragment mode `00e40000`, shadow `80003c00`, early depth 0/1/0, texture-environment high bits 0, unit config high bits `00001000`, texture shadow 1).
- Builds: dev and release (`--no-cia`) clean; no `src/` change, so the GameCube matching build is unchanged.

### Gates (to run; Azahar was not available)

Build the release CIA first: `TTYD_CCACHE=1 tools/port3ds/build.sh` (QTM placements need the CIA). C1 = `gxdirty=1 gxdirect=1 gximmdirect=1 texfast=1 texasync=1`; C3 = C1 + `gxthread=2 gxpipeline=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1`; G = C1 + `gxone=1`; V = C1 + `gxone=2`.

1. Verify mode, 0 mismatches each: `run_azahar.py --build build/3ds/game/ttyd.cia --frames 841 --switches "<scene> V gxthreadstats=60"` for gor_01 (`map=gor_01 story=30`), pik_00 (`map=pik_00 story=30`), usu_00 (`map=usu_00 story=30`), battle (`map=gon_02 story=30 battle=1 btlauto=perfect`). Pass: every `gxone frame N:` line shows `mismatches 0 missed 0 stale 0` (and "not compared" small), `gxdirty frame N:` shows `0+0 mismatches`, no `gxone verify` or `gxdirty verify` warning, and the `gxone=2: class blocks by id` start line (no "differ from libctru's").
2. `map_sweep.py --area tik --story 30 --switches "V gxthreadstats=60"` and `--area jin`: every map ok, and the same log conditions in every map's log.
3. `determ_ab.py --a "C1" --b "G" --scenes gor_01,pik_00,usu_00,battle` (shots 780-785, 6 of 6 per scene), then `--a "C3" --b "G"` with the same scenes.
4. `gw_diff.py` 1600-frame padscript in gor_01 (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript), A = C1, B = G: 160 records equal.
5. Stereo: `stereo_ab.py --a "C1 stereoforce=1" --b "G stereoforce=1" --scenes gor_01 --count 3`: EFB, left, right identical.
6. GPU hang: `run_azahar.py --build build/3ds/game/ttyd.cia --frames 840 --switches "map=gor_01 story=30 G gpuhangtest=700 gpuhangrecover=1 shots=1 shotfrom=820 shotmax=1"`: `reset #1 done`, the shot has content.
7. Timings per thread: `gx_perf_ab.py --repeat 2 --scenes gor_01,pik_00,usu_00,battle --a "C1" --b "G"` and `--a "C3" --b "G"` (`--allow-hw-faults` if the boot fault lines of the release build appear, as in the Stage 5 runs). Report frame, core-0 busy, render; with `gxthreadstats=60` the `gxone frame` lines give blocks built/sent per frame, texture re-encodes and resets (a reset per frame or many re-encodes means a table or the texture epoch is too small/too busy).

## Step 3 of the native plan: `gxchunk` (2026-11-27)

Design and files: docs/PORT_3DS_GX_NATIVE.md, "Step 3 as built". Switch row: "For PORT_3DS_TESTING.md" above. Default 0. Built on `gxone` (step 2, `18709394`).

### What changes on the shader path (C1 + `gxone=1 gxchunk=1`)

| Per frame | `gxone=1` | `gxone=1 gxchunk=1` |
|---|---|---|
| a batch use (gor_01: about 245 per frame) | per draw `plat_gpu_vsh_drawp`: room checks, `oh_draw` (layout key compare and DTab hash + copy, scale rows through the row cache, cull, row flush, packet) | one `plat_gpu_chunk_draw`: one id compare, the prelude once, the pending rows, one copy of the chunk's words |
| word runs on the render thread | copied into citro3d's command buffer (`cmd_put`, about 222 KB per frame in gor_01) | one `GX_ProcessCommandList` each (plus a `C3D_FrameSplit` when citro3d has words pending); copied only when the GX queue is near full |
| flushes for the words | covered by `C3D_FrameEnd(0)`'s flush of the linear heap (citro3d's buffer) | the same flush covers the frame list (placement in linear memory); none added |
| end of a run | none | framebuffer flush/invalidate, early-depth clear (after draws), FINALIZE: what citro3d writes at a split |

Expected effect (not measured): core 0 loses the per-draw work of batch draws (`oh_draw` and the drawp room checks per draw, the layout DTab hash); the render thread loses the 222 KB copy per frame and keeps the citro3d work. Some extra GPU work: one framebuffer flush per word run with draws (a handful per frame).

### Checks done without the emulator

- Builds: dev and release (`--no-cia`) clean, no new warnings; no `src/` change, so the GameCube matching build is unchanged.
- Host test (`tools/port3ds/pica_enc_test/build.sh`): `pica_model_test` ok (list ends for every even length against GPUCMD_Split's rule; model cases; the two gpuhang/gpudump lists, 585 and 559 draws: 0 differ with every state command written twice, and a difference at every draw with every cull value changed); `pica_enc_test` on the same lists still OK.
- Hot setter sizes (release, LTO, `nm -S`, this build against a release build of `18709394` made the same way): every `GXSet*`, `GXLoad*`, `GXBegin`, `GXCallDisplayList`, `gx_rec_call`, `gx_rec_list` the same size, except `GXSetDispCopyDst` (+120 bytes, once per frame: the same code with its statics addressed through literal words instead of one base register, LTO section layout; the same calls). Changed with the same calls: `vsh_prepare` (+52, its call to `vsh_light_block` now goes to an LTO-private copy), `gx_call_list` (+52, `bat_close` inlined: one more call, `plat_gpu_chunk_active` at init), `bat_open` (+64, the chunk check), `bat_close` (+324, the chunk use), `plat_gpu_vsh_drawp` (+4), `oh_draw` (-476, `oh_unif_flush` and `oh_state_one` no longer inlined into it), `oh_close` (+256, the list end), `vsh_part_color` (+36) and `vsh_state` (-64) (LTO layout, the same calls).

### Gates (to run; Azahar was not available)

Build the release CIA first: `TTYD_CCACHE=1 tools/port3ds/build.sh`. C1 = `gxdirty=1 gxdirect=1 gximmdirect=1 texfast=1 texasync=1`; C3 = C1 + `gxthread=2 gxpipeline=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1`; G = C1 + `gxone=1`; K = G + `gxchunk=1`; KV = G + `gxchunk=2`.

1. Verify mode, 4 scenes: `run_azahar.py --build build/3ds/game/ttyd.cia --frames 841 --switches "<scene> KV gxthreadstats=60"` for gor_01 (`map=gor_01 story=30`), pik_00 (`map=pik_00 story=30`), usu_00 (`map=usu_00 story=30`), battle (`map=gon_02 story=30 battle=1 btlauto=perfect`). Pass: the start line `gxchunk=2: batch chunks (...)`, every `gxchunk frame N:` line with chunk uses above 0, `verify draws` above 0, `differ 0`, `records over 0`, "not compared" small; no `gxchunk verify` warning; the gxone lines as in step 2 (`mismatches 0 missed 0 stale 0`).
2. Sweeps: `map_sweep.py --area tik --story 30 --switches "KV gxthreadstats=60"` and `--area jin`: every map ok, the same log conditions in every map's log.
3. `determ_ab.py --a "C1" --b "K" --scenes gor_01,pik_00,usu_00,battle` (shots 780-785, 6 of 6 per scene), then `--a "C3" --b "K"`.
4. `gw_diff.py` 1600-frame padscript in gor_01 (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript), A = C1, B = K: 160 records equal.
5. Stereo: `stereo_ab.py --a "C1 stereoforce=1" --b "K stereoforce=1" --scenes gor_01 --count 3`: EFB, left, right identical.
6. GPU hang: `run_azahar.py --build build/3ds/game/ttyd.cia --frames 840 --switches "map=gor_01 story=30 K gpuhangtest=700 gpuhangrecover=1 shots=1 shotfrom=820 shotmax=1"`: `reset #1 done`, the shot has content.
7. Timings per thread: `gx_perf_ab.py --repeat 2 --scenes gor_01,pik_00,usu_00,battle --a "G" --b "K"`, `--a "C1" --b "K"`, `--a "C3" --b "K"`, and with the render thread on core 1: `--a "G gpucore=1" --b "K gpucore=1"` (`--allow-hw-faults` if the release build's boot fault lines appear). Report frame, core-0 busy, render. With `gxthreadstats=60` the `gxchunk frame` line gives chunk uses and words per frame, builds ("not yet" should fall to 0 after the first frames of a scene), store resets (a reset per frame means `gxchunkkb` is too small), lists submitted, citro3d splits, and runs copied (more than a few per frame means the GX queue budget is too small). Note: docs/PORT_3DS_QTM_BENCH.md says Azahar delivers GSP completions only to threads on core 0; if `gpucore=1` stalls in Azahar, that is the emulator.

**Console-test items** (owner-gated; Azahar proves none of them):

1. The frame list is written on core 0 and flushed by `C3D_FrameEnd(0)` on the render thread's core (as the vertex and index arenas today). A stale word would show as a wrong state or a GPU hang (gpuhang dump: the frame list is not in citro3d's buffer, so gpudump's list dumps do not contain these runs).
2. One `GX_ProcessCommandList` per run (a handful per frame) instead of one list per record: GSP and P3D interrupt cost per list.
3. The framebuffer flush at the end of every run with draws (more flushes than citro3d's one per split).
4. QTM for the frame list (`place=gx_frame_list:qtm qtmgpu`): only after `cb_qtm` passes; GSP's address check of a QTM command list is unknown.

### Gate results of step 3 (`gxchunk`, 2026-11-27, branch gate-gxchunk = gxchunk + port3ds, Azahar)

Identity runs on the dev build (3dsx), verify, sweep, hang and timing runs on the release build (CIA or 3dsx). Verify runs need `--long 200` (the 60 s default times out).

1. **KV verify** (C1 + `gxone=1 gxchunk=2 gxthreadstats=60`, to frame 841): gor_01, pik_00, usu_00, battle all pass. Start line present; every `gxchunk frame` line from the map on has chunk uses above 0 (gor_01 244-245, pik_00 358, usu_00 135-153, battle 124-132), verify draws 14 880 / 22 060 / 9 313 / 7 508 in the last window, `differ 0`; no `gxchunk verify` warning; gxone `mismatches 0 missed 0 stale 0` in every line. **Not checked: `records over`.** The log line is cut at 255 characters, so its last fields (`not compared N records over N`) are missing from the log in the busy scenes (`not compared` is cut in battle). Shorten the format string in `plat/gpu.c` (the `verify draws %lu differ %lu one-sided %lu not compared %lu records over %lu` line) or split it into two lines.
2. **Sweeps** (KV, `--story 30`): `--area tik` 22 of 22 ok, `--area jin` 12 of 12 ok; every map segment of verify.log has a chunk line with uses above 0, `differ 0` everywhere, 0 `gxchunk verify` warnings, every gxone line `mismatches 0 missed 0 stale 0` (58 and 41 lines).
3. **determ_ab** shots 780-785 in gor_01, pik_00, usu_00, battle: C1 vs K 24 of 24 identical, C3 vs K 24 of 24 identical.
4. **gw_diff** 1600 frames gor_01: C1 vs K `160 records equal (f 10..1600, fields compared: 32)`.
5. **Stereo** (`stereoforce=1`, gor_01, 780-782): C1 vs K 9 of 9 identical (EFB, left, right).
6. **GPU hang** (K, `gpuhangtest=700 gpuhangrecover=1`): `reset #1 done`, `citro3d state marked dirty after GPU reset #1`, shot 820 has content (36 970 colours, 92 % of pixels off the commonest colour).
7. **Timing** (release 3dsx, `gx_perf_ab --repeat 2`, windows 722-841, ms per frame: frame / core-0 busy / render):

| Scene | G | K | C1 | C3 (busy / translator / render) | G gpucore=1 | K gpucore=1 |
|---|---|---|---|---|---|---|
| gor_01 | 50.0 / 35.2 / 1.4 | 50.0 / 35.7 / 0.9 | 34.8 / 31.7 / 7.2 | 33.4 / 22.5 / 13.3 / 7.4 | 50.3 / 35.5 / 2.4 | 50.3 / 36.1 / 1.2 |
| pik_00 | 50.2 / 42.6 / 1.5 | 50.2 / 43.2 / 0.9 | 50.2 / 38.7 / 7.5 | 33.4 / 27.3 / 15.5 / 7.5 | 50.2 / 42.9 / 1.7 | 50.2 / 43.2 / 1.1 |
| usu_00 | 36.4 / 30.9 / 1.5 | 36.8 / 31.7 / 1.6 | 33.6 / 28.2 / 6.4 | 32.2 / 18.9 / 13.2 / 6.3 | 36.0 / 31.1 / 1.7 | 38.5 / 32.0 / 2.4 |
| battle | 28.4 / 21.0 / 1.1 | 29.5 / 21.2 / 0.9 | 27.3 / 19.4 / 3.7 | 25.3 / 15.8 / 14.4 / 3.8 (fence 5.4) | 29.5 / 21.4 / 2.4 | 30.7 / 21.6 / 2.0 |

  (K measured four times, in the four A/B runs: busy 35.7, 43.0-43.2, 31.7-31.8, 21.2-21.3.) **`gxchunk=1` brings no gain in Azahar:** core-0 busy is 0.2-0.8 ms above G (the chunk use costs as much as the per-draw `oh_draw` work it replaces), the render thread is 0.2-0.6 ms lower, and the frame is the same or up to 1 ms worse (usu_00, battle). `gpucore=1` timings are suspect (Azahar delivers GSP completions to core 0 only) and show the same picture.

   Counters (K, `gxthreadstats=60`, frame 840): chunk uses per frame gor_01 245 (draws 247, 28 752 words), pik_00 358 (367, 42 987), usu_00 153 (155, 18 034), battle 125 (126, 14 789). "not yet" 0 and "never" 0 in all four, "built" 0 (battle 2: after its TEV table reset); store resets 0 (store 54 000-62 600 of 65 536 words, 449-530 chunks in gor_01, pik_00, battle; 42 880 and 366 chunks in usu_00); lists submitted 3 / 3 / 10 / 2, citro3d splits 1 / 1 / 9 / 1, runs copied 0 / 0 / 21 / 0 (usu_00 splits often: 9 splits a frame, and 21 copied runs means the GX queue budget is near full there).

**Flush finding (report only, no change).** `render_record` flushes the record's stream ranges (`r->dirty`), `r->verts` and `r->idx` with `GSPGPU_FlushDataCache` before it replays the record (3 calls, 68-259 KB a frame, `gxdirect ... GSP data-cache flushes` line). The vertex arena (`linearAlloc`), the stream blocks (`linearMemAlign`) and the index arena (placed in LINEAR) are all in the linear heap, and `C3D_FrameEnd(0)` flushes the whole linear heap before the GX queue runs (docs/PORT_3DS_GX_NATIVE.md, libcitro3d 1.7.1 disassembly; the same call covers the gxchunk frame list). The GX queue only runs at `C3D_FrameEnd`, after these flushes, so for linear memory the three explicit flushes are duplicate work. They are needed only for memory outside the linear heap (QTM or `place=` tiers), which the code does not use for these three. Not measured: the saving (a local patch with the flushes removed, A/B in `gx_perf_ab`, would show it; the ranges are small next to the whole-heap flush, so it is likely small).

### Gate results of steps 0-2 (2026-11-27, merged on port3ds `1630266e`, Azahar)

C1, C3, G, V as above. Identity runs on the dev build (`--dev --no-cia`), verify, hang and timing runs on the release build.

- **Step 1 (`gxlobj`)**: `map_sweep --area tik` (22 of 22) and `--area jin` (12 of 12) ok with `gxlobj=2 gxbatch=2`, with `gxlobj=2 gxcache=2` (tik) and with `gxlobj=2` + C3 with `gxrecfuse=2` (game-thread check up to 458 752 checks, 0 mismatches, no `gxlobj verify` warning in any chunk). `determ_ab` shots 780-785 in gor_01, pik_00, usu_00, battle: "" vs `gxlobj=1`, C1 vs C1+`gxlobj=1`, C3 vs C3+`gxlobj=1`: 6 of 6 identical in every scene (72 of 72). `gw_diff` 1600 frames: C1 vs C1+`gxlobj=1` and C3 vs C3+`gxlobj=1`: 160 records equal (32 fields). `gxbatch=2` and `gxcache=2` exclude each other: they were run apart.
- **Step 2 (`gxone`)**: V in gor_01, pik_00, usu_00, battle to frame 841: every `gxone frame` line `mismatches 0 missed 0 stale 0` (last window: 33 710, 36 077, 24 562, 13 683 checks; not compared 6, 0, 0, 2), `gxdirty` 0+0 mismatches, no verify warning; battle has one TEV table reset. V sweeps with `map_sweep` tik 22 of 22 and jin 12 of 12 ok, 58 and 41 `gxone` lines, none with a mismatch. `determ_ab`: C1 vs G and C3 vs G, 24 of 24 shots identical. `gw_diff` C1 vs G: 160 records equal. Stereo (`stereoforce=1`, gor_01, 780-782): 9 of 9 identical. `gpuhangtest=700 gpuhangrecover=1` with G: `reset #1 done`, shot 820 has content.
- **Timing (release 3dsx, `gx_perf_ab --repeat 2`, ms per frame: frame / core-0 busy / render)**:

| Scene | C1 | G (C1+gxone=1) | C3 (busy / translator / render) |
|---|---|---|---|
| gor_01 | 34.2 / 31.8 / 7.2 | 50.0 / 35.3 / 1.4 | 33.5 / 22.6 / 13.2 / 7.4 |
| pik_00 | 50.2 / 38.7 / 7.5 | 50.2 / 42.6 / 1.5 | 33.4 / 27.3 / 15.5 / 7.5 |
| usu_00 | 33.7 / 28.1 / 6.4 | 36.0 / 30.9 / 1.5 | 31.7 / 18.8 / 13.2 / 6.2 |
| battle | 27.6 / 19.6 / 3.7 | 28.9 / 20.9 / 1.1 | 27.1 / 15.6 / 13.9 / 3.8 (fence 5.4) |

  `gxone=1` moves the state work to core 0: render thread -5.0..-5.8 ms, core-0 busy +1.3..+3.9 ms, frame not better (gor_01 50.0 against 34.2: the game thread now misses the second vblank). C3 stays the faster config (core-0 busy 15.6-27.3 ms). The base build (port3ds `1630266e`, release) gives the same C1 and C3 numbers as this build (C3 busy 22.5, 27.2, 18.8, 15.6; C1 31.7, 38.7, 28.2, 19.4): the merge adds no cost with the switches off.

## Step 4 of the native plan: `gximmfl` (2026-11-27)

Design and files: docs/PORT_3DS_GX_NATIVE.md, "Step 4 as built". Switch rows: "For PORT_3DS_TESTING.md" above. Default 0. Built on `gxchunk` (step 3, `f1d66a5d`).

### What changes per immediate draw (C1 + `gxone=1 gxchunk=1 gximmfl=1`)

| Per immediate draw (pik_00: about 1 150 per frame) | `gxchunk=1` | `+ gximmfl=1` |
|---|---|---|
| route bits | `gx_vsh_imm_reasons` (8-attribute loop, `tri_count` with divides) | the vertex class plan's bits (computed once per plan) + the primitive's bit (shifts and a multiply) |
| plan | looked up in `gx_imm_convert`, after the route | looked up once, before the route, and given to the conversion |
| pass-group route | `gx_vsh_route` | the same call (K hysteresis, decal links unchanged) |
| index list | `emit_tris` into a static buffer, `memcpy` into the record's index arena | `emit_tris` straight into the index arena |
| hand-over | `gx_vsh_imm_done` (a `GxVshImm`), `plat_gpu_vsh_light`, `plat_gpu_vsh_const`, `plat_gpu_vsh_draw`, `oh_draw` (with its pool and DRAWP tests) | `plat_gpu_imm_begin` (light and block by id, room), `plat_gpu_imm_end` (merge test, `oh_draw`'s words for a draw from the arena) |
| index address | `osConvertVirtToPhys` per draw | the arena's physical base, once per record (linear memory; a heap placement converts per draw) |

Not removed (needed for the same words): the pass-group key and record probe (`group_for`, `rec_for`, `touch`), `vsh_state` (gxdirty parts), the uniform block compare by id, the room checks, the class blocks by id (`oh_state_one`), the uniform row diff (`vsh_const_rows`).

### Checks done without the emulator

- Builds: dev and release (`--no-cia`) clean, no new warnings in the changed files; no `src/` change, so the GameCube matching build is unchanged.
- Host: `gx_vsh_imm_nidx` against `tri_count` x 3 and against `emit_tris`' count, every primitive type, n = 0..65535: 0 mismatches (scratch test). `pica_model_test` on the two gpuhang/gpudump lists: ok, 0 differ (585 and 559 draws).
- Hot sizes (release, LTO, `nm -S`, this build against a release build of `f1d66a5d` made the same way): no `GXSet*`, `GXLoad*`, `GXBegin`, `GXCallDisplayList`, `gx_rec_call`, `gx_rec_list`, `bat_open`, `vsh_prepare` changed size. Changed: `GXInit` +168 (start-up: the switch), `gx_rec_frame` +52 (the verify note in the gxthreadstats log), `rec_for` +100 (`gximmnow`), `gx_imm_convert` 5784 -> 3136 with `imm_plan_build` (2680) now out of line (it is built on a plan miss only, about 4 a frame in pik_00; it is shared with `gx_imm_plan`), `draw_from_reader` 1440 -> 5120 (`gx_draw_imm_fl` with the conversion inlined), `plat_gpu_imm_end` 2356 and `plat_gpu_imm_begin` 268 new, `plat_gpu_vsh_const` -4, `capture_efb` renamed by LTO (same size -16).

### Gates (to run; Azahar was busy)

Build the release CIA first: `TTYD_CCACHE=1 tools/port3ds/build.sh`. C1 = `gxdirty=1 gxdirect=1 gximmdirect=1 texfast=1 texasync=1`; C3 = C1 + `gxthread=2 gxpipeline=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1`; G = C1 + `gxone=1`; K = G + `gxchunk=1`; I = K + `gximmfl=1`; IV = K + `gximmfl=2`.

1. Verify mode, 4 scenes: `run_azahar.py --build build/3ds/game/ttyd.cia --frames 841 --switches "<scene> IV gxthreadstats=60"` for gor_01 (`map=gor_01 story=30`), pik_00 (`map=pik_00 story=30`), usu_00 (`map=usu_00 story=30`), battle (`map=gon_02 story=30 battle=1 btlauto=perfect`). Pass: start line `gximmfl=2: immediate draws straight into the frame list, ...`; every `gximmfl frame N:` line with draws above 0 (pik_00 about 1 150 with merged), `route differ 0 index differ 0`; the `gxchunk frame N:` line (with `gxchunk=1` its verify counts are the immediate draws' alone) `verify draws` above 0, `differ 0`, `records over 0`, "not compared" small; no `gximmfl verify` or `gxchunk verify` warning.
2. Sweeps: `map_sweep.py --area tik --story 30 --switches "IV gxthreadstats=60"` and `--area jin`: every map ok, the same log conditions in every map's log.
3. `determ_ab.py --a "C1" --b "I" --scenes gor_01,pik_00,usu_00,battle` (shots 780-785, 6 of 6 per scene), then `--a "C3" --b "I"` and `--a "K" --b "I"`.
4. `gw_diff.py` 1600-frame padscript in gor_01 (`map=gor_01 story=30 determ gwdump=10` and the A2 padscript), A = C1, B = I: 160 records equal.
5. Stereo: `stereo_ab.py --a "C1 stereoforce=1" --b "I stereoforce=1" --scenes gor_01 --count 3`: EFB, left, right identical.
6. GPU hang: `run_azahar.py --build build/3ds/game/ttyd.cia --frames 840 --switches "map=gor_01 story=30 I gpuhangtest=700 gpuhangrecover=1 shots=1 shotfrom=820 shotmax=1"`: `reset #1 done`, the shot has content.
7. Timings per thread: `gx_perf_ab.py --repeat 2 --scenes gor_01,pik_00,usu_00,battle --a "K" --b "I"`, `--a "C1" --b "I"`, `--a "C3" --b "I"`, and `--a "K gpucore=1" --b "I gpucore=1"` (`--allow-hw-faults` if the release build's boot fault lines appear). Report frame, core-0 busy, render. Expected (not measured): core 0 less by the per-draw hand-over and index copy only; the route and uniform work stay.
8. Optional, not identical by design: `gximmnow=1` (`determ_ab.py --a "I" --b "I gximmnow=1"`): differences expected only in the first K (30) frames after a map load; shots 780-785 should be identical once the scene is steady.

**Console-test items** (owner-gated): the index arena's physical base taken once per record (linear memory is physically contiguous: the packet's index address must equal `osConvertVirtToPhys` of the draw's indices; `gximmfl=2` compares exactly that on Azahar).

### Gate results of steps 3 and 4 on D2 (2026-11-27, branch native34 = port3ds `a92665c5` + gate-gxchunk + gximm, Azahar)

Merge fixes (D2 draws a batch member's indices in place, `BatDraw.lin`, and `Batch.ix_count` counts only the copied draws' indices): `bat_chunk_build` takes each draw's real index address (`lin`, else the pool copy at `pool_off`) and the last draw's count from the members' `idx_end` (with `ix_count` every in-place batch got 0 indices and was marked "never": gor_01 6 chunk uses a frame instead of 245); `plat_gpu_chunk_build` checks the range with `ixpool_ok` (D2's range list); `plat_gpu_imm_end` uses `s_oh_idx_end` as an address. The `gxchunk frame` line is split in two, so `records over` is in the log.

Release CIA; verify runs `--long 200`, `--allow-hw-faults`.

1. **KV** (C1 + `gxone=1 gxchunk=2`), gor_01, pik_00, usu_00, battle to frame 841: chunk uses per frame 245 / 358 / 153 / 124 (as on gate-gxchunk), verify draws 14 880 / 22 060 / 9 313 / 7 502 in the last window, `differ 0`, **`records over 0`**, not compared 0; no `gxchunk verify` warning; no `drawp indices ... outside the index ranges`; gxone `mismatches 0 missed 0 stale 0`. (`one-sided` is 2 per compared draw in the field scenes: the chunk writes all four scale rows; not a mismatch.) Sweeps: tik 22 of 22, jin 12 of 12 ok, 58 and 41 chunk lines, 0 differ, 0 verify warnings.
2. **Step 3 identity:** `determ_ab` C1 vs K and C3 vs K 24 of 24 each; `gw_diff` C1 vs K 160 records equal (32 fields).
3. **IV** (K + `gximmfl=2`), the same 4 scenes: `gximmfl frame` route differ 0, index differ 0 (front-end checks 47 153 / 196 043 / 75 576 / 154 873), gxchunk verify differ 0, records over 0; immediate draws per frame 231 / 122 / 204 / 112 (merged 138 / 1 032 / 273 / 95). Sweeps tik 22 of 22, jin 12 of 12 ok, 0 differ, 0 `gximmfl verify` warnings (map_sweep keeps only lines with "verify", so the `gximmfl frame` totals are not in its log).
4. **Step 4 identity:** `determ_ab` C1 vs I, C3 vs I, K vs I 24 of 24 each; `gw_diff` C1 vs I 160 records equal; stereo C1 vs I 9 of 9; `gpuhangtest=700 gpuhangrecover=1` with I: `reset #1 done`, state marked dirty, shot 820 has content (36 976 colours, 92 %).
5. **Timing** (release 3dsx, `gx_perf_ab --repeat 2`, windows 722-841, frame / core-0 busy / render ms):

| Scene | K | I | C3 (busy / translator / render) |
|---|---|---|---|
| gor_01 | 50.3 / 35.6 / 0.8 | 50.0 / 35.4 / 0.9 (C3 run: 50.3 / 35.2) | 33.4 / 22.6 / 13.1 / 7.4 |
| pik_00 | 50.2 / 42.9 / 0.9 | 50.2 / 41.8 / 0.9 | 33.4 / 27.2 / 15.4 / 7.5 |
| usu_00 | 36.9 / 31.7 / 1.6 | 36.4 / 31.3 / 1.6 | 32.5 / 18.9 / 13.3 / 6.4 |
| battle | 31.7 / 24.3 / 0.9 | 32.0 / 23.8 / 0.9 | 33.4 / 15.6 / 20.8 / 3.8 (fence 12.4) |

  `gximmfl=1` takes 0.2-1.1 ms of core-0 busy off K (pik_00 most: 1 150 immediate draws a frame); the frame does not change. C3 stays the faster config (busy 15.6-27.2 ms).

## Heap margin (2026-11-27)

Release builds, Azahar (3dsx: heap 83 100 KiB, linear heap 32 768 KiB), runs to frame 841. C0 = no switches (the defaults); C1 and C3 as above. Base = port3ds `156cf717`.

### Method

- **The peak without a walk** (committed, `plat/boot.c`): newlib's malloc takes all memory with `sbrk` and gives the top back once more than 128 KiB is free there, so its arena (`__malloc_current_mallinfo.arena`) is in-use memory plus holes plus at most 128 KiB, and newlib keeps the arena's high-water mark (`__malloc_max_sbrked_mem`). `plat_heap_peak_take()` reads that mark and starts a new period (two loads, no walk). heap size - peak arena is the least room any allocation had in the period: a lower bound of the free margin (holes not counted).
- **Log lines** (committed, cheap): `heap until map change from <map>: peak arena ... at least N KiB free` at every map change (the period holds that map's load and play), with switch `heaplog=N` also every N frames; `heap: <table> N KiB (arena ...)` for each GX table (`gx_big_alloc`); the texasync line (`texstats`) shows `results at most N KB`.
- **Census** (local build, not committed): `--wrap` of malloc/calloc/realloc/memalign/free logged every allocation of 32 KiB or more with its caller (symbolised from the ELF) and whether it was freed; `--wrap` of linearAlloc/linearMemAlign/linearFree counted linear memory in use and its peak; counters for the gxcache blobs and the gxsdl/gxrecfuse pools.

### Where the heap goes (pik_00 C3, KiB; base -> this build)

| Owner | Base | Now | Notes |
|---|---|---|---|
| Game arena (`OSInit`, the GameCube's main memory) | 20 178 | 20 178 | game |
| ARAM (`ARInit`) | 16 384 | 16 384 | game (`arammgr` sizes its heap from `ARGetSize`) |
| gxcache blobs (`gxcachekb` budget 8 MB, heap + linear streams) | 6 535 | 6 535 | 10 100 entries; the budget is full in every scene |
| Placed memory (`memplace`: gx ring 4 096 with `gxthread` only, gx record 1 024, log 320, gx state 128, audio 128) | 5 696 | 5 696 | C0/C1: 1 600 |
| texasync results (transient, at map loads) | about 6 700 at the peak | at most 6 144 | see below |
| Texture conversion scratch `s_rgba` + `s_tiled` | 8 192 | 4 096 | `s_tiled` only with `texfilter=mip` |
| REL data snapshots | 4 073 | 1 579 | all configs |
| gxcache tables (entries 1 920, indices 192, capture 864, gxbatch 360) | 3 336 | 3 336 | |
| GXInit tables (pipe 2 x 512, pipe vertices 432, transformed vertices 224, vsh records/groups/touches 720) | 2 400 | 2 400 | |
| texasync decode scratch | 2 560 | 1 280 | |
| gxsdl pool (`gxsdl=1`) | 2 048 + 96 | 64 + 96 | in use under 32 KiB in all four scenes |
| gxrecfuse pool (`gxrecfuse=1`) | 1 024 + 40 | 512 + 40 | in use at most 421 KiB (gor_01) |
| Threads, devupdate, direct-write buffers, TEV memo, DVD, small | about 1 900 | about 1 900 | |

### Changes (no switch; same pictures)

1. **Textures tiled straight into their own memory** (`gx_tex.c` `upload_rgba8`, `plat_gpu_tex_create_begin/_end` in `plat/gpu.c`): a texture without a mip chain was tiled into the 4 MB `s_tiled` scratch and then copied by `plat_gpu_tex_create`; it is now tiled into the PICA texture's linear memory and flushed (the bytes the copy wrote; one copy less). `s_tiled` comes with the mip scratch (`texfilter=mip`). -4 096 KiB in every config.
2. **texasync** (`gx_tex.c`): one decode scratch, not two. One runner decodes at a time (`pre_kick` joins the last runner before it submits another); a decode at the bind uses the binding thread's own scratch (`s_rgba`, which the bind's own conversion uses right after). -1 280 KiB. The result cap (`PRE_BYTES_MAX`, 6 MB) counted 2 bytes a texel; an RGBA8 result takes 4, so the results could take up to twice the cap. A queued texture now counts 4 bytes a texel and, once decoded, its real size: the cap holds for the heap the results take. Counts (C3, frame 841, base -> now): taken pik_00 111 -> 93, gor_01 175 -> 164, battle 84 -> 64 of 1 300-2 000 queued; most queued results are never bound (unused 1 300-1 900). A texture not taken converts at its bind, as before (load frames only).
3. **gxsdl and gxrecfuse pools grow** (`gx_state.c`, `gx_rec.c`): they start at 64 KiB and 128 KiB and double, up to the old sizes (2 MB, 1 MB); the words, positions and reset points are those of the old full-size pools, so the decoded calls are the same. A growth that fails runs the list as before (gxsdl: the byte decoder; gxrecfuse: the old way).
4. **REL data snapshots packed** (`os_link.c`, `plat/lz.c`, a small LZ77 codec): at boot each REL's data is packed, unpacked again and compared (a fault stops the boot there), and kept packed; `OSLink` unpacks over the REL's data (the bytes the copy wrote). Boot cost 400 ms in Azahar (pack and check of 4 MB); an `OSLink` unpacks one REL (100-500 KiB).
5. `REQUIRED_HEAP`/`REQUIRED_LINEAR` comments (`plat/boot.c`) carry the measured peaks; `REQUIRED_LINEAR` 24 -> 28 MB (libctru gives 32 MB).

### Results: least free heap to frame 841 (KiB, base -> this build)

| Scene | C0 | C1 | C3 |
|---|---|---|---|
| pik_00 | 17 220 -> **23 804** | 7 500 -> **16 488** | **384 -> 11 352** |
| gor_01 | 17 440 -> 24 020 | 7 872 -> 16 264 | 2 000 -> 13 964 |
| usu_00 | 18 972 -> 25 556 | 9 548 -> 18 508 | 2 264 -> 14 016 |
| battle | 17 580 -> 24 168 | 8 652 -> 18 320 | 2 668 -> 15 344 |

The low point is the first map load in every run (frames 630-720): gxcache's tables and blobs, the texasync results and the map's own data come in at once. `map_sweep.py --area tik` and `--area jin` (C3 + `gxrecfuse=2`, this build): least free 8 292 KiB (tik_05) and 11 404 KiB (jin_07) per map. Peak arena now: C0 57.5-59.3 MB, C1 64.6-66.8 MB, C3 67.8-71.7 MB.

**Linear memory is not a donor:** in use at most 22.0-26.6 MB with C0 and 28.2 MB with C3 (pik_00; `linearSpaceFree` sampled every 30 frames down to 4.5 MB), of 32 MB. Moving heap tables there, or shrinking the linear heap, would eat the texture memory (a texture that does not fit is drawn without it).

Not done (measured, left): the gx ring (4 MB, C3) carries 50-55 K words (215 KiB) a frame, but one command may take half the ring (`put_slow`), so a smaller ring needs the largest command known first; the gxcache entry table (16 384 entries, 10 100 live) and the 8 MB blob budget are cache sizes (a smaller one changes hit rates); ARAM and the game arena are the game's.

Steady-state time does not change (windows 722-841, best of 2, game busy / translator ms, base -> this build): C3 gor_01 22.5/13.3 -> 22.5/13.3, pik_00 28.1/15.5 -> 28.2/15.5, usu_00 18.9/13.5 -> 18.9/13.3, battle 15.9/13.7 -> 15.7/14.1 (content changes); C1 busy gor_01 31.8 -> 31.7, pik_00 39.5 -> 39.6, usu_00 28.3 -> 28.3, battle 19.5 -> 19.6. The GX setters' sizes in the ELF are unchanged (inlining check).

Identity (this build against base): `determ` shots 780-785 byte-identical in gor_01, pik_00, usu_00, battle in C0, C1 and C3 (72 of 72); `gw_diff` 1600-frame padscript gor_01: 160 records equal in C1 and C3; `gxrecfuse=2 gxreclgroup=1` (C3): `map_sweep.py --area tik` 22 of 22 maps ok, 0 mismatches of 247 K + 565 K checks, `--area jin` 12 of 12 ok, 0 of 124 K + 334 K (the sweeps load a REL at each area change, so they also run the packed snapshots); stereo (`stereoforce=1`, C3, gor_01 780-782): 9 of 9 identical (EFB, left, right).

## GXEnd on the game thread: the plan lookup (2026-11-27)

Ablation in C3 (local build, not committed; switch `gxphx=mask` returns early at one point, `phxvbl=2` holds every frame at 2 retraces; pik_00, windows 722-841, best of 2, game busy ms):

| Left out | busy | cost |
|---|---|---|
| nothing | 28.5 | |
| the whole direct record (`gx_rec_prim_direct` at entry) | 23.6 | **4.9** |
| everything after `gx_imm_rec_ok` | 24.0 | check 0.4 |
| the conversion (`gx_imm_convert`) | 24.4 | **4.1** |
| the conversion's vertices (convert with n = 0) | 26.8 | vertices 1.7; **per primitive 2.4** |
| indexed float steps / the fill / indexed colours | 27.7 / 27.8 / 28.1 | 0.8 / 0.7 / 0.4 |
| the conversion twice | 32.7 | +4.2 |

So the vertex loop is not the largest part any more: 2.4 ms of the 4.1 is per primitive (1 155 a frame, 2 us each), and most of that is the plan lookup, which compared the plan's vertex description and formats (104 bytes) with the state's, for up to four plans, at every primitive.

Changes (no switch; same plans, same bytes):
1. **`gx_imm_convert`** (`gx_pipe.c`): every write to the vertex description or formats of `gx` or `gx_game` bumps that state's `gx_vdesc_ver` (`VDESC_TOUCH`, and the two whole-state copies). A plan keeps the version at which it last matched; while the state's version is the same, the plan matches without the compare. A changed version takes the old compare (and a match sets the version).
2. **`gx_imm_rec_ok`**: the vertex-description part of the check is kept per `gx_vdesc_ver[1]` (game thread only).

Results, release, windows 722-841, best of 2 (A = the heap-margin build above, B = this build), game busy ms:

| Scene | C1 busy A -> B | C3 game busy A -> B | C3 translator |
|---|---|---|---|
| gor_01 | 31.7 -> 31.7 | 22.5 -> 22.4 | 13.3 -> 13.3 |
| pik_00 | 39.5 -> **38.7** | 28.2 -> **27.2** | 15.5 -> 15.5 |
| usu_00 | 28.3 -> 28.1 | 19.0 -> 18.7 | 13.4 -> 13.4 |
| battle | 19.5 -> 19.3 | 15.6 -> 15.7 | 13.4 -> 14.2 (content changes) |

Change 1 gave all of it (pik_00 C3 28.2 -> 27.3); change 2 is within the spread (27.3 -> 27.2). The rest of the per-primitive time (about 1.4 ms in pik_00) is the step set-up, `put` and the TLS read of `gx_tls_translator`. No GX setter changed size; `gx_imm_convert` +560 bytes.

Identity (B against port3ds `156cf717`): `determ` shots 780-785 byte-identical in gor_01, pik_00, usu_00, battle in C1 and C3; `gw_diff` 1600-frame padscript gor_01 equal in C1 and C3; `gximmdirect=2` to frame 841, 0 mismatches, 0 vertex-state differences, 0 skipped: C3 pik_00 265 343 checks, gor_01 91 923, battle 156 629-196 043, C1 pik_00 264 228. With the heap margin above, `gximmdirect=2` now runs pik_00 C3 to frame 841 with 11.4 MB free at the map load (it ran out of heap before).
Stereo (`stereoforce=1`, C3, gor_01 780-782, against `156cf717`): 9 of 9 identical (EFB, left, right).

Not done: the translator task (pik_00 15.5 ms). Phases now (C3, `gxphase=1`, frame 841, calls / us per frame): primitives `GXR_PRIMD` 1 154 / 6 375, batch open 358 / 2 620, state build 1 606 / 2 616, list calls 5 885 / 1 519, slow path 102 / 982, state calls 2 843 / 799, batch close 358 / 295, other 603. Batch open and the primitives share `ensure_state`, `vsh_state`, the route and the uniforms (section "Translator, immediate primitives and batch open" above); no cut was tried in this round.

## `texdt`: texture tiling by the transfer engine (plan step 3b, 2026-11-27)

Status: code and build only. No emulator run and no console run yet; "ready for measurement". Default `texdt=0`: the build is as before.

### What it does

The CPU decodes a GX texture to RGBA8 as before and picks the PICA format as before. If the format is RGBA8, RGB565, RGBA5551 or RGBA4 (the engine's formats; RGB8 is never picked), the texture needs no resampling (sides already powers of two, at least 8) and no mip chain, and it has at least `texdtmin` texels, then:

1. the CPU converts the RGBA8 rows into the target format in a **linear staging block** (one sequential pass, no Morton scatter), and flushes it on the writing thread;
2. `plat_gpu_tex_create_xfer` makes the texture (`C3D_TexInit`, no copy) and queues a job;
3. the render thread, right after `C3D_FrameBegin` of its next record, issues each job as `GX_DisplayTransfer` (`FLIP_VERT(1) | OUT_TILED(1)`, in and out format equal, no scaling, `RAW_COPY(0)`). The GX queue runs entries in order, so the transfer finishes before that record's command list; every record that samples the texture is submitted after the texture was made. The game thread never waits for the GPU.

Everything else is tiled by the CPU as before: other formats (LA8, L8, L4, LA4, ETC), resampled textures (non power of two), mip chains (`texfilter=mip`), textures under `texdtmin` texels (default 2048), and any texture that finds no free staging block. There is no waiting fallback: an allocation failure or a full job table tiles on the CPU at once.

### Staging buffer and memory

- One arena in linear memory, `texdtarena` KB (default 512, range 256..1024), 16 KB granules, first-fit blocks. A texture takes at most half the arena (256 KB: 256x256 RGBA8, 512x256 RGB565). The arena is allocated once, only when `texdt` is on. Linear free is about 4.5 MB in the worst config: 512 KB is 11 %; `texdtarena=256` allows only 128 KB blocks.
- A block is busy from the write until the record that carried its transfer is done on the GPU (`s_gpu_done_seq`, the `gpufence` of the records), then it is released by the render thread. The texture memory is busy for the same time (`TexSlot.dt_busy`): a texture freed before that is released only afterwards (`tex_alloc_slot`, `plat_gpu_tex_reclaim`, which also reaps after its sync).
- Needs `gpufence=1` (the default); with `gpufence=0` `texdt` turns itself off and logs it.
- 64 job slots. A job that the record cannot issue because the 32-entry GX queue is nearly full (the 24-entry budget that the strip copies use, `s_gx_entries`) is tiled by the CPU **on the render thread** from the same staging block (counted as "cpu on render thread").

### texasync

The core-2 runner decodes as before. With `texdt=1` an eligible result keeps its staging block instead of a heap-tiled copy (the runner converts and flushes on core 2; the result takes 0 heap bytes). The arena keeps half its room free for the binds' own textures (`reserve_half`): when the runner finds less room it tiles on the CPU as before. At the first bind `pre_take` hands the block to `plat_gpu_tex_create_xfer`; an unused result (aged out, stale generation) frees its block. With `texdt=2` the runner makes the CPU tiled copy as well and the bind queues the verify job.

### Verify: `texdt=2`

The CPU tiles as before and that texture is the one drawn. The same texels are also transferred into a second block; when the record is done both are compared with `memcmp`. Lines in the log:

- `texdt: verify MISMATCH WxH fmt F flip N: B of T bytes differ, first at K (engine XX, cpu YY)` (first 24 only);
- `texdt: made .. issued .. verify queued .. ok .. BAD ..` every 600 records, `texdt: frame ..` every 600 frames.

Verify uses `texdtmin` 64 (every size from 8x8 up), so the engine's minimum size and shape limits show up as mismatches by size. Expected result: `BAD 0`. If every size is BAD, try `texdtflip=0` (the flip convention; the probe `dt_tile_*` on branch `gxone` tests both and was not run on the console yet).

### Switches

| Switch | Default | Meaning |
| --- | --- | --- |
| `texdt` | 0 | 0 CPU tiler; 1 transfer engine; 2 verify (CPU texture, engine copy compared) |
| `texdtflip` | 1 | `GX_TRANSFER_FLIP_VERT` value; 1 puts row 0 in the last tile row (t = 0 at the last memory row) |
| `texdtmin` | 2048 | smallest texture (texels) that uses the engine; verify uses 64 |
| `texdtarena` | 512 | staging arena KB (256..1024) |

### Measurement plan (planner)

1. `texdt=2` verify, expect `BAD 0`: `gor_01`, `pik_00`, `usu_00`, a battle, `map_sweep.py --area tik`; also once with `texasync=1`.
2. `determ_ab.py` `texdt=0` against `texdt=1` (must be byte identical).
3. Map-load frame times (`texframe=1`: conv and create columns) `texdt=0` against `1`, with and without `texasync=1`.
4. Console (owner-gated): `dt_tile_*` probe from `gxone` for the size and flip rules, then `texdt=2`.

Known limits: after a GPU reset (`gpu_reset_apply`) a transfer that was in the lost queue is not repeated; the texture would show stale bytes until it is rebuilt (the hang path already loses the frame). The CPU still decodes everything and writes the staging copy, so the saving is the Morton scatter and the cache behaviour of the scatter, not the decode.
## Defaults on, exact textures (2026-11-27, owner decision)

**Switches now on by default** (each still `name=0`): `gxdirty=1 gxdirect=1 gximmdirect=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1 texasync=1 texfast=1 dlspread=1500`. Not changed: `gxthread=0` (`gxthread=2` waits for an identity fix), `gxpipeline=0`, `gxonehop=0`, `efb608` (on only after a console test), `gpucore`. All were identity-proven (`determ` shots byte-identical) in the sections above. N for `dlspread` is the 1500 of PORT_3DS_60FPS.md "Map-load hitch".

**Coherent set at `gxthread=0`:**

- Active and effective: `gxdirty`, `gxdirect` (gain of C1 comes from the two together; `gxdirect` alone was neutral), `gxsdl` (on cache misses), `texfast`, `texasync` (needs `textrust=1`), `dlspread` (needs `gxcache=1`).
- Active, about neutral: `gximmdirect` (stage 4: busy time moves by 0.1 ms or less at `gxthread=0`; the gain is with `gxthread=1..3`).
- No-op at `gxthread=0`: `gxrecfuse` and `gxreclgroup` (both read in `gx_rec_init` after the `gxthread=0` return, and `gxrecfuse` also needs `gxthreaddedup=1`). They act on the record path, so they take effect together with `gxthread=2`.
- `gxonehop=1` still forces `gxdirect=1`, `gximmdirect=1` and `gxthread=0`.

**Verify with the new defaults:** `determ_ab.py` shots 780-785 in gor_01, pik_00, usu_00 and the battle, new build default against the old build with `gxdirty=1 gxdirect=1 gximmdirect=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1 texasync=1 texfast=1 dlspread=1500 tex16=1` (must be byte-identical).

### Exact texture formats (`tex16`)

`pick_format_ex` (`gx_tex.c`) put an opaque texture in RGB565 and a one-bit-alpha texture in RGBA5551 whatever the decoded colours were (RGBA8 from CMPR, RGBA8 sources and palettes with 8-bit colours lose their low bits; gor_01 had 39 + 198 such textures). Now (`tex16=0`, default): a channel value v fits n bits when `v == code*255/(2^n-1)` or `v == (code << (8-n)) | (code >> (2n-8))` with `code = v >> (8-n)` (the first is what GX's RGB565 and RGB5A3 decode gives, the second is the PICA's widening). An opaque texture whose colours all fit (R and B 5 bits, G 6 bits) stays RGB565; one that does not fit is stored as **RGB8** (24 bit, bytes B, G, R as the PICA reads them; `tile_level` and `sample_tiled` know it, so `texcompact=2` verifies it). A one-bit-alpha texture whose colours all fit 5 bits stays RGBA5551; otherwise **RGBA8**. A cut-out in RGBA8 keeps no mip chain, as RGBA5551 did, so the picture changes only in the low colour bits. Colours of fully transparent texels count too (they bleed in with bilinear filtering). Grey textures stay LA8; `texcompact` formats are unchanged (a texture that fails the fit and has every channel a multiple of 17 goes to RGBA4 with `texcompact=1`, exact). `tex16=1` is the old choice. Cost: 2 bytes a texel become 3 (RGB8) or 4 (RGBA8) for the textures that change.

Counters: `texstats=N` prints `texexact:` every 120 frames (uploads checked, textures changed per kind, level-0 KB before and after). Linear heap and VRAM: the changed textures grow by 1.5x (RGB8) or 2x (RGBA8); `vramtex` is off by default and its tier works on bytes (`vramtexmax` 256 KB per texture), so a texture that was under the limit as 16 bit may now be over it; the texture cache has no byte cap of its own (`CACHE_SIZE` 1024 entries, 120-frame eviction). The numbers per scene (textures and bytes changed, live texture KB with `tex16=0` and `tex16=1`) come from the measurement runs: see the report of the build that introduced this section.
