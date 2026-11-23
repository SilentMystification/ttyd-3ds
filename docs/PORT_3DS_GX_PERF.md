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
| `gxscribble=N` | Test of the verify mode, not for play: at game frame N, xors one byte of an array a cached (preferably trusted) list reads, with no flush and no report. With `gxcache=2` it must show as `mismatched` and `stale`; with trust on it keeps drawing the old vertices, and the `gxstats scribble` line counts those hits (the contract) |
| `gxstatever=0/1` | 0 (default): every state version change rebuilds the PICA state. 1: matrix and projection loads do not bump the version, and a version change whose state (texgen to pixel engine without the lights, viewport) equals the last build skips `build_state` (`builds N (unchanged M)` in `gxstats`). Fewer rebuilds, but no time saved yet (see Results) |
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
