# Native renderers: moving the boundary above GX

Written 2026-11-28 from a read-only design study. All ms are Azahar ms; numbers marked est. are derived from source structure and existing counters, not measured.

## Why

The GameCube's GX is a stream machine: a call is a few FIFO stores and the command processor does the rest. The PICA200 is a retained machine: it executes prebuilt command buffers, vertex buffers with one index for all attributes, a vertex shader and fixed combiners. Cheap on the PICA is executing a prebuilt buffer and writing a few uniform rows; expensive is every command word, vertex or state compare the CPU builds.

The game's data already holds stable identities (map object x part with its material and mesh, animation pose data x shape x draw, effect entries). The drivers flatten them into about 30 K anonymous GX events per frame, and every design at the GX API (gxone, gxchunk, gximmfl, gxsplit) spends 50-2,000 instructions per event rediscovering those identities by content. The floor of any GX-API design is events x per-event cost. A native renderer keys PICA objects by the game's object at the driver, builds them at load, and per frame sends only item + matrix + the few dynamic values.

Facts from the sources: map materials are already baked by the game (`makeDisplayList`, mapdrv.c:541, runs `mapSetMaterial` + cull into a display list per part at map load; meshes are packed lists in the map file). TTYD characters have no weighted skinning: `animPoseMain` writes morphed float positions/normals, each shape is rigid under one modeling matrix.

## Renderers (src/ under TARGET_PORT; each behind a default-off switch; GX kept as fallback)

| Renderer | Replaces | Built from | Per frame |
|---|---|---|---|
| `nmap` map geometry | the body of `_mapDispMapObj` for `MAPOBJ_FLAG_DISPLAY_LIST` objects and the 5 blend cases in `dispDraw` | build hook after `makeDisplayList` in `_mapLoad`/`bmapLoad`, freed in `mapUnLoad`/`bmapUnLoad`; per part: TEV/TEX block ids (the baked list applied once to a scratch GxState and interned with gxone), 5 PIX blocks, 2 fog variants, vertex stream + layout block, index buffer, one chunk | clip rows (same products as today), KCOLOR0, texture rows if not identity, changed block ids, copy/call the chunk: ~300-500 instructions per part (est.) vs ~40 GX events |
| `nanim` characters | `renderProc`/`materialProc` vertex setup and the `GXBegin` fan loops | built at `animPoseEntry` from `AnimationPoseData` (shapes, draws, index streams, static colour/texcoord buffers): single-index vertices, static u16 index buffer, static stream; TEV id memo keyed by `mapSetMaterialTev`'s inputs, filled by capturing the game's own call | gather pos/nrm into a dynamic stream (~15-20 instructions per vertex), rows from the modeling matrix, light block, ids, one draw packet |
| `nspr` fonts, icons, windows | glyph quads in fontmgr, icondrv, later windowdrv/win_root | call-site rectangles, UVs, colours, textures | 4 vertices + 6 indices per sprite, one draw per run |
| GX fallback | effects (180 files), shadows, fades, env, offscreen, bbox passes, everything else | - | gximmfl path; size decided by the step-0 census |

Material capture: native code never reimplements `mapSetMaterialTev`; it runs the game's code once into a scratch state and keeps the interned ids. A verify mode compares against the GX path.

Portability: the port layer gets a game-agnostic retained-object API (`bake_state`, `bake_mesh`, `bake_soup`, `draw`); map/anim glue stays in src/driver under TARGET_PORT.

## GX state the game reads back

- Later GX-path draws and `GXGetCullMode`/`GXGetProjectionv`/`GXGetViewportv`/`GXGetScissor`: after each native part, apply its net state delta to the GX shadow (word compare + store; gxsdlx's delta builder). Camera, projection, viewport, scissor stay GX calls.
- `GXReadBoundingBox`/`GXClearBoundingBox` callbacks stay on GX (native checks a "bbox armed" flag).
- `GXCopyTex` (22 calls): native writes into the same frame list in call order, so copies see native draws.
- `GXPeekARGB`: fence, unchanged. Draw sync: answered when core-0 work is done.

## Identity

| Class | Applies to | Gate |
|---|---|---|
| Byte-identical register file at every draw | `nmap` | gxchunk=2-style `pica_model` compare, gxfldig, determ, gw_diff, GX shadow hashes |
| Geometry-equivalent draw (new rule, owner decision) | `nanim`, `nspr` (deduplicated vertices: addresses, index values, counts differ) | register file equal except attribute/index addresses and counts; triangle lists bit-equal after decoding; determ pixels identical |
| First frames after a load (owner decision, same as `gximmnow`) | native items take the shader path from frame 1 | until decided: native follows the GX route's state and falls back while a group is undecided |

## Budget (Azahar core 0, est.)

| Scene | C3 today (c0 / c2 / render) | K today | K + nmap | + nanim | + remainder on core 2 |
|---|---|---|---|---|---|
| gor_01 | 22.6 / 13.1 / 7.4 | 35.6 | 19-21 | 13-15 | 11-12 |
| pik_00 | 27.2 / 15.4 / 7.5 | 42.9 | 28-30 | 18-22 | 13-16 |
| battle | 15.6 / 20.8 / 3.8 | 24.3 | 16-17 | 12-13 | 9-11 |

GX instructions per frame in gor_01 (est.): map ~3.5 M -> 0.16 M; characters ~1.5 M -> 0.06 M; rest ~0.9 M -> 0.8 M. Game logic (7-10 ms gor_01, ~12 ms pik_00) becomes the largest part of core 0.

## Migration

| # | Switch | Work |
|---|---|---|
| 0 | `gxown` | attribution census: tag the running draw callback class in `dispDraw` (map / anim / npc / effect / 2D / other); count GX events, list calls, primitives, vertices and CPU cost per class per scene; name the source of pik_00's 1,155 primitives. Fallback target: below a threshold (e.g. <10% of today's GX CPU per scene); any class above it gets its own native path |
| 1 | `nmap=1/2` | display-list map parts, COPY mode, on K; verify runs the GX path into a side buffer and compares register file + GX shadow per object |
| 2 | `nmapjob` | bake on core-2 jobs at map load; lit and non-display-list objects |
| 3 | `nanim=1/2` | static topology + gather + material memo; fallbacks for paper/pera, custom GX funcs, offscreen poses, bbox |
| 4 | decide | measure the GX remainder; small: keep on core 0 and retire the record ring; else gxsplit for the remainder; consider record-once-replay for static fallback work |
| 5 | `nspr` | fonts, icons, windows |
| 6 | CALL mode, QTM | after the console probes |

## Reuse

Reused: gxone intern tables and `pica_enc.c` encoders, the chunk store and frame-list writer, `oh_state_one`, the stream builder and layout blocks, `gx.v.pica`, `vsh_const` row maths, the light block, `pica_model`/`gxfldig`/gxchunk=2 verify, gxsdlx's delta builder, gximmfl (effects), the texture tier, `texdt`, `plat_job`, memplace placements, mapdispjob's outcome table.

Fallback-only from here (stop optimising): the gx_rec ring, dedup, gxrecfuse/gxreclgroup/gxsdl, batch forming and `bat_open` validation, route/group keys for map and character draws; most of gxbopen/gxlcall/gxrows/gxtexkey's gain goes away with `nmap`.

## Owner decisions (2026-11-28)

- Identity for `nanim`/`nspr`: pass = GPU register file equal except buffer addresses and index counts, decoded triangle lists bit-equal, determ shots byte-identical.
- First frames after a load: native items use the shader path from frame 1 (no K=30 hysteresis); the first ~30 frames after a load may differ, identical afterwards.

## Fallback coverage (addendum, est.)

Estimated GX CPU share by subsystem (K layout; baseline GX CPU gor_01 ~26.6 ms, pik_00 ~31, usu_00 ~23, battle ~17):

| Subsystem | gor_01 | pik_00 | usu_00 | battle |
|---|---|---|---|---|
| Map | 62% | 47% | ~60% | 47% |
| Characters (animdrv via npc/mobj/item/mario/party/battle units) | 23% | ~40% | 26% | 30% |
| Everything else (effects, shadows, windows/HUD/fonts/icons, battle UI, fades, bg, env, camera) | 15% | 13% | 14% | 23% |

usu_00's map is mostly lit / non-display-list parts (`mapSetMaterial` every frame), so `nmap` stage 2 (lit and non-DL parts) follows stage 1 directly.

Residual fallback share after each step (est.): `nmap` stage 1: 38% / 53% / ~95% / 53%; stage 2: 38% / 53% / 40% / 53%; `nanim`: 15% / 13% / 14% / 23%; shadows + `nspr` + battle UI: 6-12%; trace replay on the rest: 2-5%.

Target: fallback GX CPU <= 10% of today's GX CPU in every census scene and <= 3,000 GX events per frame. Any subsystem above 3% of today's GX time (~0.8 ms) or 1,000 events per frame gets its own native path. The census reruns after every step; fallback-reason counters must explain every native-eligible item sent to GX.

Making the fallback non-interpreting: trace replay per owner (record the event sequence between draw boundaries once; next frame each call is one compare against the trace's next member, and a full match takes the stored class ids, route and plan; mismatch drops to the interpreter and re-records; verify runs both), static fallback lists stay retained (ListObj + chunk), constant-argument setters fold to compare + store with gxfifo. Only immediate vertex conversion remains.

Revised order: 0 `gxown` census -> 1 `nmap` stage 1 -> 2 `nmap` stage 2 (lit/non-DL) -> 3 `nanim` -> 4 shadows / `nspr` / battle UI per the threshold -> 5 trace replay for the fallback -> 6 bake jobs, CALL mode, QTM.
