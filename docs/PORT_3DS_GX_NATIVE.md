# 3DS-native GX path: design and migration

Written 2026-11-27 from a read-only design study, after the owner's direction that GX must not be the extra load ("something is not 3DS shaped") and that all work must fit the New 3DS hardware. All ms are Azahar ms (ratios only, no cache model). Estimates are marked.

## 1. Root cause

In the steady state, frame N's PICA output is frame N-1's output with only uniform rows changed (gor_01: 0 batches formed or killed, 0 captures, 0 vsh switches, 0 CPU-path draws). Yet every frame about 33 ms of CPU rebuilds it (gor_01, C3: core-0 GX ~12.5 ms, translator 13.3 ms, render 7.4-8.1 ms; gxonehop writes 55.6 K PICA words = 222 KB again each frame).

On the GameCube a list call is 3 FIFO words and the command processor does the rest. On the 3DS the equivalent is a prebuilt PICA chunk the GPU's command processor executes. Today we memoize at six layers with six keys, and no layer keeps final, GPU-executable words across frames.

1. The GX state machine runs three times: game shadow + dedup (`dd_same`), translator replay through the GX functions (`gx_apply_call`, `consume`, `run_commands`), back end `PlatGpuState` rebuild (~700 B). Under gxnull the recording alone costs setters 5.5 ms (pik_00 9.2), `gx_rec_list` 4.5 (5.9), list entry 1.1.
2. 86% of setter calls change nothing (18,513 of 21,441 per frame in gor_01); each still pays the record macro and dedup slot.
3. The same change is detected 4-5 times: `dd_same`, translator `SAME`, `plat_gpu_set_state_dirty` region compare, render `s_dkey`/`s_om` key compare, DTab hash + key compare. State is compared as content at each layer, never as an id.
4. About 5,000 list calls per frame are re-walked one by one (translator: list call 2.38 ms, `bat_open` 2.09 ms at 8-9 us each, `call_slow` 2.41 ms) for 245 batches whose content never changes.
5. Five tables keyed by the same list address (dlcache bucket, batch head chain, vsh record, gxsdl slot, fused-list table) plus the bbox record.
6. Immediate primitives re-routed every frame (~5-7 us each: 25-word group key, `vsh_state`, route, uniforms, index build).
7. Per-frame index builds; 222 KB of words written to heap and copied again into citro3d's buffer.
8. Costs that stay in any design: game logic 7.3-10.4 ms (pik_00 12-15), immediate-vertex conversion 2.2 ms (pik_00 4.8), the ~18 K setter calls the game makes.

## 2. Target architecture

Single front end on core 0; no record ring, no translator thread.

- **Setters:** the one dirty-tracking place. Compare, store into one `GxState`, set a class dirty bit (TEV, TEX, PIX, CHAN/TG, VTX, CLIP, TEXROW). This is the verified gxdirty logic.
- **Interned class ids:** at a draw boundary, each dirty class is looked up by its GX bytes (full key compare) and gets an id that owns a prebuilt PICA block in linear memory. Below this point only ids are compared, one word each.
- **ListObj:** one table keyed by (addr, size) holds what the five tables hold today. One probe per call, or none when the call matches the open chunk's next member. Hot index ~8 B per entry (~128 KB), cold part read only on a miss.
- **Batch chunks:** a batch gets one chunk: layout block id, per draw the scale uniform and a draw packet with absolute index addresses, padded to 16 B. Validation once per chunk open (chunk generation equals ListObj generation, memstable generation unchanged). This also covers CPU-built lists (the usu_00 untrusted hits; owner: fold into this design).
- **Frame list:** written by the game thread into double-buffered linear memory: changed uniform rows, PSO class blocks whose id differs from the held id, then the batch chunk, either copied (COPY mode, works today) or called (CALL mode: CMDBUF channel 1 call, channel 0 return; needs the console probe). Both give the same register sequence.
- **Immediate primitives:** `GXEnd` converts into the ring chunk, writes uniform rows and the draw packet inline; routing becomes an eligibility mask precomputed per class id.

### Hardware units

| Work | Unit |
|---|---|
| Material/TEV/texture/pixel state, batch draws, layout | PICA command lists built once, executed by the GPU (copied or called) |
| Per-frame changes (clip/texture rows, scale words, call list) | Frame list, core 0, sequential writes, double-buffered linear memory |
| Transform, projection, texgen, channel-0 lighting | Vertex shader (built: gxvsh, gxvshlit, gxvshtg) |
| TEV | Fixed-function combiners, compiled once per TEV key |
| Fog | PICA fog LUT, sent only when its id changes |
| GX per-vertex lighting | Vertex shader (PICA fragment lighting is per pixel and would change the picture) |
| Texture tiling for RGBA8/RGB8/RGB565/RGBA5551/RGBA4 | Transfer engine (DisplayTransfer), new switch `texdt`; the CPU still decodes I4/IA8/CMPR/CI |
| Chunks/streams to VRAM, EFB copies, screen transfer | TextureCopy / DisplayTransfer (the CPU mapping of VRAM is read-only) |
| Immediate-vertex gather | Core 0 (one index for all attributes on the PICA; no other unit can gather) |
| Lines, points, bounding-box readback | Core 0 (rare) |
| Game logic, setters, list compares, class-id resolution, row products, frame-list writes | Core 0 |
| Chunk/stream builds on a miss, texture decode, parallel game subsystems | Core 2 jobs |
| Segment submit (one GSP ProcessCommandList each), clears/copies, present, stereo tail, audio, I/O | Core 1 (`gpucore=1`), about 0.5-1 ms render work (est.) |
| EFB, screen targets, hot textures, stereo snapshot | VRAM |
| Frame list in QTM | Only if the PICA reads QTM (probe); otherwise CPU-only hot data, only if the benchmark shows a gain |
| L2 hot set | ListObj index, class intern tables, `GxState` shadow, the writer's model of GPU state |

### Avoided

- Re-translation every frame, and several copies of the same frame (the 4 MB call ring, `PlatGpuState`, the render record, the gxonehop arena + copy all go).
- Per-call syscalls, GSP calls and TLS reads: one `GSPGPU_FlushDataCache` per frame for the frame list + immediate ring, one GSP submit per segment; the translator's TLS reads go with the thread.
- CPU-GPU stalls: the GPU reads only port-owned memory, so `GXReadDrawSync`/`sysWaitDrawSync` answer when core-0 work is done; only `GXPeekARGB` and CPU-read EFB copies wait. Freed chunk/stream memory is reused after 3 presented frames.
- Integer divide/modulo (power-of-two masks everywhere; encoders use shifts), doubles on the GX path, large random-access tables.

## 3. Estimates (Azahar, est.)

| | today core 0 | today core 2 (translator + render) | target core 0 | target core 1 | target core 2 |
|---|---|---|---|---|---|
| gor_01 | 22.5 | 13.3 + 7.4-8.1 | 14-20 | 0.5-1 | ~0 (misses only) |
| pik_00 | 28.1 | 15.5 + ~8 | 23-30 | 0.5-1 | ~0 |

GX CPU drops from about 33 ms to about 5-8 ms in gor_01. pik_00 core 0 stays over 16.7 ms on GX alone (game logic and immediate conversion remain): it needs the parallel game subsystems (docs: core-split plan) or option B, the boundary resolution + conversion + frame-list writer on core 2 behind a compact event ring (~5 K words per frame; est. pik_00 core 0 19-23 ms).

## 4. Migration (each step behind a default-off switch)

Gates for every step: `determ_ab.py` (10 scenes, 6 of 6), `gw_diff.py` (1600 frames), `stereo_ab.py`, the `gxcache=2` sweep, `map_sweep --area`, `vsh_accept` static pixels, `gpuhangtest` recovery, a same-build `gx_perf_ab`.

0. **Probe first** (gpuprobe cases, Azahar first, console owner-gated): `cb_jump`, `cb_call` (1 and 500 calls, nested), `cb_call_a8/a16`, `size8/16`, `cb_vram`, `cb_qtm`, `cb_irq` (P3D at a FINALIZE reached through a jump), a command-processor throughput mode, `dt_tile` (DisplayTransfer tiling per format/size vs CPU Morton). Only CALL mode and the placements depend on these.
1. **`gxlobj`:** one ListObj table replaces the five address tables (cache contents only). Gates: `gxbatch=2`, `gxcache=2`, `gxrecfuse=2`.
   Built 2026-11-27 (`game/gx/gx_lobj.h`, default 0; `gxlobj=2` adds its own verify, "gxlobj verify" log lines, must count 0 errors). Index: 8-byte slots (address, size / 32, value), power-of-two, linear probing with masks, at most half full, backward-shift delete; keys whose size is not a multiple of 32 or above 2 MiB stay on the old tables. Two instances, one per owning thread (no lock while the translator thread exists): translator side (`gx_dlcache.c`, 2^15 slots, 256 KB) maps the key to its first cache entry, replacing the bucket array; the entry already carries the pass-group record hint, the bounding-box record and the batch chain, and with `gxlobj` the key's first entry also holds the `gxsdl` slot hint for every list with an entry and the record hint for draws of a list decoded on a miss. Game side (`gx_rec.c`, 2^14 slots, 128 KB + 64 KB objects) keeps the `is_state_list` answer (yes for ever, no until the state-list tables gain an entry, never a no that hashed the bytes) and the `fl_get` slot hint. The per-call fast path stays the open batch's next-member compare. Every hint is validated against the target's own key, so a lookup finds what it found before.
2. **`gxone`:** single shadow, dirty bits, interned class ids, own PICA encoders; translation on core 0; downstream compares by id. `gxone=2` verifies encoder words against the citro3d capture word for word plus the `gxdirty=2` checks.
   Built 2026-11-27 (default 0; section "Step 2 as built" below).
3. **`gxchunk=1` (COPY):** batch chunks and PSO blocks; the game thread writes the frame list; render on core 1 only submits. `gxchunk=2` builds the gxonehop words beside it and compares the PICA register file at every draw (register-file model treating texture-cache clear, FIXEDATTRIB, uniform-index and LUT writes as events).
   Built 2026-11-27 (default 0; section "Step 3 as built" below).
3b. **`texdt`:** texture tiling by DisplayTransfer, verify mode against CPU Morton, then determ. **Implemented 2026-11-27 (code and build, not yet measured):** the CPU converts decoded RGBA8 to RGBA8/RGB565/RGBA5551/RGBA4 in a linear staging block (arena in linear memory, default 512 KB), the render thread issues `GX_DisplayTransfer` (FLIP_VERT, OUT_TILED) in the next record before its command lists, so the game thread never waits; CPU tiler kept for other formats, resampled or mip textures, small textures (`texdtmin`) and as fallback; `texdt=2` verifies byte for byte. Details and measurement plan: PORT_3DS_GX_PERF.md, section `texdt`.
4. **Immediate primitives into the frame list**, eligibility mask in place of the route. Default keeps today's K=30 hysteresis (identical output); starting a group on the shader at once in a viewport with no CPU-path draw is a separate switch (open owner question; the first ~30 frames after a load would differ).
   Built 2026-11-27 (`gximmfl`, default 0; the separate switch is `gximmnow`, default 0; section "Step 4 as built" below).
5. **`gxchunk=call` (CALL mode)**, gated on the step-0 console results.
6. **Stereo:** per-eye tail lists with per-eye rows on the CPU (byte-identical; planner default). A shader shear would be cheaper but not identical (open owner question).
7. **Chunk/stream builds as core-2 jobs at map load** (frame draws uncached meanwhile, the `dlspread` rule), targeting the ~345 ms first-frame build cost in gor_01.

### Step 2 as built: `gxone` (2026-11-27)

Switch `gxone` (default 0; 2 is the verify mode). It implies `gxdirty` (at least the same level), `gxonehop=1`, `gximmdirect=1` and `gxthread=0` (C1 layout: the GX translation runs on core 0, the render thread only copies word runs and does citro3d work). Files: `port3ds/plat/pica_enc.c/.h` (encoders, no libctru or citro3d call), `port3ds/plat/gpu.c` (gxone section: block stores, `plat_gpu_set_state_one`, `oh_state_one`, the verify), `port3ds/game/gx/gx_draw.c` (gxone section: `build_one` and the intern tables), `port3ds/include/plat/gpu.h` (interface), `port3ds/plat/vramtex.c` (texture epoch), `tools/port3ds/pica_enc_test/` (host test).

- **Single shadow, dirty bits.** The game thread's `gx` is the only GX state (no record ring, `gxthread=0`). The setters are gxdirty's unchanged (compare, store, class dirty bit; no setter changed size, see the PERF doc).
- **Interned class ids at the draw boundary** (`build_one`, in place of `build_dirty`, same parts in the same order). Each dirty class is looked up by a key with a full word compare:
  - TEV: the GX TEV bytes the TEV memo keys on (stage count, the stages, TEV registers, konst colours, swap tables, the unit of each stage) with the fog stage's unit and colour. The entry holds the PICA TEV region and the fog unit after the build; a hit copies them and skips the compile. The TEV block also holds the fog mode and colour (they come from the pixel class), so a second table keyed by (TEV entry, fog word) owns the block id.
  - TEX: the three texture units as `build_tex_units` leaves them (handles from the texture tier, sampler fields). The handle lookups still run on each TEX change: they are what checks the texture memory, so GX bytes alone cannot name the texture.
  - PIX: the GX pixel-engine bytes (alpha compare, blend, depth, colour and alpha update, pixel format) with scissor, scissor offset, EFB size and viewport (17 words). A hit copies the pixel and scissor/viewport regions and skips `build_pix`/`build_sci`; `build_fog` still runs on a PIX change (the LUT and its id stay apart, as with gxdirty).
  - Tables: power-of-two index of (hash, entry) pairs with linear probing by mask, at most half full; entries and keys stored in order of first use (TEV 512 entries and 64 KB of keys, TEV block 512, TEX 1024, PIX 512; about 290 KB heap). A full table is cleared together with the back end's store of that class: every id of the class changes.
- **Blocks built by the port's own encoders.** On a miss the front end asks the back end for a block (`plat_gpu_one_block`): `pica_enc.c` writes the words citro3d 1.7.1's `C3Di_UpdateContext` writes after the renderer's `det` setters, read from the disassembly of the installed `libcitro3d.a` (texture-environment buffer group then the 6 stage runs, 42 words; the 3 units' runs and formats then the unit config with the cache clear and the shadow word, 30 words; viewport (when set), scissor, effect group, at most 46 words). Register values the renderer never sets (stencil, early depth, fragment-operation and shadow bits, the unit config's high bits) are read once from citro3d's context after `C3D_Init`. The float conversions are libctru's `f32tof24`/`f32tof31` re-written and compared with libctru's at start-up on 8192 values (a difference turns the switch off). Blocks live in linear memory (`linearMemAlign`, 16-byte aligned, 320 KB: TEV 512 x 44 words, TEX 1024 x 32, PIX 512 x 48), ids handed out in order (`generation << 16 | index + 1`), so a block is ready for a future CALL mode.
- **Downstream compares ids.** `plat_gpu_set_state_one` copies a class's regions into the back end's state only when its id changed (no `plat_gpu_set_state_dirty` word compare). At a shader draw `oh_state_one` sends a class block only when the GPU does not hold its id (one word compare each; no `oh_state` key build, no `oh_tex_prepare`, no 28/18/12-word key compares, no DTab hash). The three state classes' DTabs of gxonehop are not allocated (about 0.7 MB heap less); the layout keeps its (stream, layout) table.
- **Textures.** A texture block holds the objects' address, size, format and sampler words. A texture slot used again (`tex_alloc_slot`) or texture data moved by `vramtex` bumps a texture epoch; a block built in an older epoch is encoded again in place before it goes out, and a held texture block of an older epoch is sent again.
- **`gxone=2`.** The record is written as by `gxonehop=1` (keys, citro3d captures), so the picture is the reference; beside it: every block the key path sends is compared word for word with the id's block (with the kept capture words, or with the capture when the record comes back), the id path's send decision with the key path's (`missed`: a key changed while its id did not), the regions of unchanged ids with the state given (`stale`), and gxdirty=2's full-build compare runs at every rebuild (it checks every intern hit). Log lines with `gxthreadstats=N`: `gxone frame N:` (plat: blocks built and sent per class, encoded at the draw, texture re-encodes, resets; verify checks, mismatches, missed, stale, not compared) and four front-end lines (hits, misses, entries, resets per table), plus `gxdirty frame N:`.
- **Host test.** `tools/port3ds/pica_enc_test/build.sh`, then `build/pica_enc_test/pica_enc_test <list.bin>...`: splits gpudump/gpuhang command lists citro3d wrote, finds every TEV, TEX and PIX block in the shape `C3Di_UpdateContext` writes, encodes their register values again and compares word for word. On the two lists in `build/azahar-profile/.../gpuhang/` (`dump_f643_r644.bin`, `hang_f699_r752.bin`): 477 TEV, 773 TEX and 448 PIX blocks, **0 differ**, the fixed values the same in every block; viewport words reproduced from integer sizes (pica_f24/pica_f31).
- **Not changed:** the setters, the record path (`gxthread` 1-3), `gxonehop` without `gxone`, the CPU path, clears, copies, stereo, the GPU-hang reset (every record is self-contained, nothing held at its start). With `gxone` the texture key path (`oh_tex_prepare`, used by `gxone=2`) sets the lookup mode as `tex_prepare` does, so the copy's mode bits no longer depend on what the render thread's setters left on the object.

Gates (not run yet; the Azahar cap was full): see the PERF doc section "Step 2 of the native plan: `gxone`".

### Step 3 as built: `gxchunk` (2026-11-27)

Switch `gxchunk` (default 0; 2 is the verify mode; `gxchunkkb=N` the chunk store, default 256). It needs `gxone=1` (so gxonehop=1, gxthread=0) and turns itself off with `gxone=2`, `gpufence=0`, `gpudrawcut` or `gpudrawskip` (the draw filter counts single draws). Files: `port3ds/plat/gpu.c` (gxchunk section after the gxone section, `oh_close`, the render thread's `CMD_OH_WORDS`), `port3ds/game/gx/gx_dlcache.c` (`bat_close`, `bat_chunk_build`, the check in `bat_open`), `port3ds/include/plat/gpu.h` (interface), `port3ds/plat/pica_enc.c` (`pica_enc_list_end`), `port3ds/plat/pica_model.c/.h` (register-file model), `tools/port3ds/pica_enc_test/pica_model_test.c` (host test).

- **Batch chunks.** A gxbatch batch (a run of cached lists under one vertex state, drawn as up to 64 DRAWP draws) gets one chunk after a full use drawn per draw: `plat_gpu_chunk_build` writes, per draw, the attribute setup when stream or layout changes (the layout block's kept words from the gxonehop table; while its capture is pending the build answers "not yet" and the next full use tries again), the four scale rows when the scale word changes (all four, the same 32-bit values `vsh_scales_rows` makes), the cull word (first draw), and C3D_DrawElements' packet with the index address (offset from the attribute base taken at start-up) and count. Chunks are 16-byte aligned and padded (a write with byte mask 0), in the placement `gx chunks` (linear memory by default, so a future CALL mode can call them). Id = store generation << 16 | index + 1; a full store starts again (one generation bump: every id is invalid and each batch builds again at its next full use; in COPY mode the GPU never reads chunk memory, so nothing waits).
- **Validation once per use, not per member.** The batch's own open checks stay (cache epoch, memory-trust generation, vertex state, route decision); `bat_open` drops the chunk when the memory-trust generation differs from the one it was built in; `plat_gpu_chunk_draw` checks the id's generation (one compare) and the cull mode (a chunk holds one cull word). Per list call nothing changes: the open batch's next-member pointer and size compare.
- **Frame list.** The record's word arena (gxonehop's) is the placement `gx frame list` (2 x `gxonehopkb`, linear memory by default; QTM only with `place=gx_frame_list:qtm` and `qtmgpu`, which needs the `cb_qtm` console probe first; HEAP turns gxchunk off). At a chunk use the game thread does what `oh_draw` does before a batch's first draw (stereo mark, citro3d flush, the PSO class blocks whose ids the GPU does not hold), writes the pending uniform rows (the same 32-bit products as today), then copies the chunk's words (COPY mode; a batch called in part copies the first draws and patches the last count). No gx_rec record command and no per-draw `oh_draw` for these draws. The model of what the GPU holds afterwards (layout, scales, cull, uniform rows) is what the per-draw path leaves.
- **Command lists.** Each word run (`oh_close`) ends with what `C3Di_SplitFrame` + `GPUCMD_Split` write: framebuffer flush, invalidate and early-depth clear after draws, FINALIZE (twice when needed for a 16-byte length; runs start 16-byte aligned). The render thread submits a run with one `GX_ProcessCommandList` into citro3d's GX queue (citro3d's own words since its last split go first as their own list, `C3D_FrameSplit(0)`), and does only the citro3d work between runs (flushes, the `lit`/light-count uniforms, captures, clears, EFB copies, CPU-path draws, the stereo warp and tail, present). The GX queue holds 32 entries and runs only at `C3D_FrameEnd`: past 20 entries in a record the render thread copies the run's words into citro3d's buffer as gxonehop=1 does (same register sequence; counted as "copied").
- **Flushes.** `C3D_FrameEnd(0)` flushes the whole linear heap before it starts the queue (libcitro3d.a 1.7.1 disassembly): that one flush per record covers a frame list in linear memory, so gxchunk adds no `GSPGPU_FlushDataCache`. A frame list outside linear memory (place=) gets one flush per record on the render thread. (The existing per-record flushes of the vertex arena, index arena and stream ranges in `render_record` are covered by the same FrameEnd flush; left as they are.)
- **GPU reset.** Unchanged: every record (and every segment) starts with no held id; the reset is applied at a record's start.
- **Not changed:** immediate primitives (step 4), lists that are not in a batch (per-draw `oh_draw`), the CPU path, clears, copies, stereo (the marks and the tail work as with gxonehop=1; a chunk's draws count for the tail), gxchunk=0 (no change in the words; the setters are unchanged).
- **`gxchunk=2`.** At every chunk use the per-draw words `oh_draw` would write (from `s_om` and the uniform rows before the pending rows go out) are built into a side buffer, with the end of each draw in both streams. When the record comes back (after the fence, with its captures filled) both word streams go through `pica_model.c` (register values byte by byte with a known mask; float uniform rows; fixed attributes; the fog LUT; texture-cache clear, draw triggers, framebuffer flush, FINALIZE, the other LUT and shader ports as events), shared words fed to both, captured blocks fed to both, and both models start again at every citro3d step (its words are not visible). At each chunk draw the two register files are compared on what both know: "differ" must be 0; "one-sided" (a value one stream wrote again and the other left as it was) is normal. Memory: about 1.4 MB heap more.
- **Host test.** `tools/port3ds/pica_enc_test/build.sh`, then `build/pica_enc_test/pica_model_test [<list.bin>...]`: list-end words against GPUCMD_Split's rule for every even length, model unit cases, and each gpuhang/gpudump list through two models, the second with every state command written twice: 585 and 559 draws, 0 differ; the same lists with every cull value changed: differences found at every draw.

Gates (not run; the Azahar cap was full): the PERF doc section "Step 3 of the native plan: `gxchunk`".

### Step 4 as built: `gximmfl` (2026-11-27)

Switch `gximmfl` (default 0; 2 is the verify mode). It needs `gxchunk=1` or `2` (so `gxone=1`, `gxthread=0`: the game thread routes, converts and writes the frame list). Files: `port3ds/game/gx/gx_draw.c` (`gx_draw_imm_fl`), `port3ds/game/gx/gx_pipe.c` (`imm_direct`, the plan split: `gx_imm_plan`, `gx_imm_convert_plan`), `port3ds/game/gx/gx_vsh.c` (`gx_vsh_imm_vtx_reasons`, `gx_vsh_imm_nidx`, `gx_vsh_imm_commit`, `gx_vsh_imm_emit`; `gximmnow`), `port3ds/plat/gpu.c` (gximmfl section after `plat_gpu_chunk_draw`), `port3ds/include/plat/gpu.h`.

- **Eligibility by class.** The route bits of an immediate draw are the uniform block's (gxdirty: rebuilt only for the dirty classes), the vertex class's (the conversion plan, keyed by the vertex description and the format's attribute formats, holds `GXV_R_PNMTXIDX/TEXMTXIDX/SLOTS`, computed once when the plan is built) and the primitive's own bit (`gx_vsh_imm_nidx`: shifts and one multiply, no divide). The plan is found once, before the route, and the conversion runs through it (no second lookup, no TLS read).
- **Hysteresis kept.** The pass-group route (`gx_vsh_route`: group key, record, K-frame streak, decal links) is the same call with the same arguments, so the same draws take the shader path and the words are those of `gximmfl=0`. Its per-draw work (group key compare, record probe, touch) stays: removing it changes which frames draw on the shader. Owner question open: `gximmnow=1` (separate switch, default 0) starts a new immediate record decided (streak K), so its group takes the shader in its first frame when nothing in it is ineligible; a set not eligible at a frame's end drops it back to 0. Not identical: the first frames (up to K) after a load differ.
- **Straight into the frame list.** After the conversion into the ring chunk and the ring commit (as `gx_vsh_imm_done`), `plat_gpu_imm_begin` sends the light and uniform block when their ids changed (`plat_gpu_vsh_light/const`), makes `plat_gpu_vsh_draw`'s room checks and returns the place in the record's index arena; the front end writes the triangle indices there (`emit_tris`, no static buffer, no copy); `plat_gpu_imm_end` makes the merge test and writes what `oh_draw` writes for an arena draw: stereo mark, flush, the class blocks whose ids the GPU does not hold (`oh_state_one`), the layout block, the cull word, the pending uniform rows, the packet. The index address is the arena's physical base, taken once per record (linear memory, VRAM and QTM are physically contiguous; a heap placement converts per draw), plus the offset.
- **`gximmfl=2`.** On the game thread, each draw's old route bits (`gx_vsh_imm_reasons`) and old index list (into its own buffer, counted by its end) are compared with the new ones; a different route bit takes the old one. In the back end, before the draw's words (after the layout block), the words the old path writes from there (the scale rows when they changed, the cull word, the pending rows, the packet with `osConvertVirtToPhys`) are built into the record's ckref, a merge patches both packets; when the record comes back `ck_verify` (gxchunk=2's) feeds both streams through `pica_model` and compares the register file at every immediate draw. It shares gxchunk=2's buffers (about 1.4 MB heap; allocated with either switch at 2). Lines: `gximmfl frame N:` (draws, merged, none per frame; front-end checks, route differ, index differ, totals) and `gxchunk frame N:` (verify draws, differ: with `gxchunk=1` these are the immediate draws' alone).
- **Not changed:** `gximmfl=0` (no change in the words or the route), the CPU path of an immediate draw (decoded as before), the old path for draws `gx_draw_imm_route` does not take (matrix indices, bounding box, dump, `gxvsh=0`), lists, chunks, clears, copies, stereo (the same marks), the GPU-hang reset.

Gates (not run; Azahar was busy): the PERF doc section "Step 4 of the native plan: `gximmfl`".

## 4b. QTM (2 MiB extra RAM, separate bus) budget

The redesign removes the `gx record` (QTM 1 MiB) and the `gx ring` (heap 4 MiB) and takes back the empty `gx state`/`audio state` reservations. QTM then holds the streams written every frame and read once, so they stop sharing the FCRAM bus with textures, vertex streams, scanout and the game heaps. Every row is a `plat/memplace.c` placement with a fallback tier; defaults stay on today's tiers until console numbers exist.

| # | Placement | KiB | Writer / reader | Per frame (est.) | Depends on (unmeasured) | Fallback |
|---|---|---|---|---|---|---|
| 1 | Frame list, 2 x 128 KiB | 256 | core 0 / command processor | 60-100 KB | PICA command fetch from QTM | linear FCRAM |
| 2 | Immediate vertex ring, 3 frames x 256 KiB | 768 | core 0 / vertex loader | 73-234 KB | PICA vertex fetch from QTM | linear FCRAM (overflow to linear chunks) |
| 3 | PSO class blocks and layout blocks | 256 | core 0/2 at build / command processor | 50-150 KB read | as row 1 | linear FCRAM, or VRAM if `cb_vram` passes |
| 4 | Hot ListObj index (~128 KiB) + class intern tables (~64 KiB) | 192 | core 0 | cache-resident | QTM latency on an L2 miss, cacheability | heap |
| 5 | Event ring core 0 to core 2 (option B only; else static batch chunks ~220 KB) | 256 | core 0 / core 2 | ~20 KB | CPU bandwidth and coherence on QTM | heap |
| 6 | Log slots (unchanged) | 320 | all / core 1 | cold | none | - |

Priority when room is short: rows 1-2, then 3, then batch chunks, then 4. Console facts first: probes `cb_qtm`, `vb_qtm`, `ib_qtm` (each GPU unit fetches separately; also check `GSPGPU_FlushDataCache` on QTM ranges and the physical address encoding 0x1F000000>>3), calib QTM latency/bandwidth and cache attributes, and the QTM benchmark's `contend` test. Azahar decides nothing here (its TextureCopy to/from QTM fails). Step 0 adds these probes; step 3 registers rows 1-3 as placements on linear FCRAM by default.

## 5. Obsolete when done

- `gx_rec.c` entirely (ring, translator thread, fences, array snapshots, gx_game/gx split, `dd_slot`/`dd_same`, `gx_rec_fast`, `GXR_PRIMD`, fused-list table, `consume`) and switches `gxthread`, `gxpipeline`, `gxthreadsplit*`, `gxthreaddedup`, `gxrecfuse*`, `gxreclgroup`, `gxphase`, `gxsdl`. Frees about 4 MB of heap (the ring), which also ends pik_00's heap-margin problem.
- In `plat/gpu.c`: shader-path record commands and their `render_record` replay, `PlatGpuState` build/compare, gxdirect DTab/capture, all of gxonehop, `vunif` pending sets, `gxmerge`, `gxflushrender`; the `gx record` QTM placement becomes the frame-list placement.
- In `gx_vsh.c`/`gx_dlcache.c`/`gx_draw.c`: per-frame group keys and hashes, record hints, the `gx_vsh_frame` streak pass, decal links except for the CPU path, per-open batch epoch checks, the `s_ix` pool, `gxstatever`, `gxstatededup`. `gxdirty`, `gxstatecache`, `gxdirect`, `gximmdirect`, `gxonehop`, `gxbatch` stop being switches and become the only path.
- Kept: the CPU path (`gxvsh=0` and rare draws), the stream builder, memstable trust, the texture tier, `dlspread`, the verify modes.

## Revised plan after steps 2-3 (2026-11-27)

Measured: K (gxone + gxchunk, all on core 0) gives no gain in Azahar (gor_01 core 0 35.7 ms vs C3 game 22.5 + translator 13.3). Steps 2-3 removed the render-side work (~5.8 ms) but moved it to core 0 as the id path (~+3.5 ms) and left the front end unchanged: the translator's list walk, `bat_open` validation, route, `vsh_state`, uniform block, class key builds, state-list decoding and the immediate route now run on the game thread, plus ~18 K setters at 36-70 instructions each. About 30 K GX events per frame cost 50-2,000 instructions each; the GameCube does them in about 10-20.

Remaining duplicate work under K: state change detected by content twice (setter SAME, `build_one` key) and by id twice; a TEV change forces a TEX rebuild; batch validation in five epochs plus chunk validation; route recomputed though the chunk holds a decided route; uniforms built, compared, copied and compared again per row; state lists LEB128-decoded every call (no gxsdl under K) and compared again in each setter; leftover Record bookkeeping. Setter code: 121 GX setters take ~412 KB of .text (inlined record path, constprop clones), bad for the 16 KB L1-I on the console.

Option B (game thread records cheap words, core 2 interns and writes the frame list, core 1 submits) is the right layout, because core 0 holds 7-15 ms of game logic; it pays only once recording is FIFO-cheap and the front end is cut.

Ranked steps (each behind a default-off switch, gates as section 4; gains est., gor_01 / pik_00 core 0, Azahar):

| # | Switch | Change | Gain est. |
|---|---|---|---|
| 0 | `gxphx` (local) | ablation under K at fixed vblanks, to measure the phases | measurement |
| 1 | `gxsdl=1` with K | no LEB128 decode of state lists | -1.5-2.5 / -2-3 |
| 2 | `gxfifo` | packed-word shadow for the hot setters; one mode-word load, cold out-of-line record/DL/verify paths; one-word compare + store + dirty OR; SAME for VtxDesc/VtxAttrFmt/Array/Load*Mtx; getters via accessors; setter text ~412 KB to ~40 KB; `gxfifo=2` verifies the shadow against `GxState` at each draw boundary | -2.5-3.5 / -3-4 |
| 3 | `gxsdlx` | a state list becomes its net delta, applied as word compare + store | -1-1.5 / -1.5-2 |
| 4 | `gxbopen` | one world generation instead of five epochs; batch keeps class ids + gens and skips `vsh_state`, route, `bat_same_state` on a match | -1.5-2 / -2-2.5 |
| 5 | `gxlcall` | `GXCallDisplayList` tests the open batch's next member inline before the `gx_call_list` prologue | -1-1.3 / -1.2-1.5 |
| 6 | `gxrows` | uniform rows built from class parts as f24 with per-row dirty bits | -0.5-1 / -1-1.5 |
| 7 | `gxtexkey` | TEX rebuilt only when unit inputs change | -0.4-0.8 / -0.8-1.5 |
| 8 | `gximmfl=1` | step 4 (built) | -1-1.5 / -3-4 |
| 9 | `gxkold` | drop leftover Record bookkeeping and `PlatGpuState` region copies under K | -0.3-0.8 |
| 10 | `gxsplit` (option B) | compact event ring (~80 KB, not 4 MB) to core 2; core 2 does steps 4-8 work and writes the frame list; core 1 submits; then immediate conversion on core 2 | core 0 to 10-14 / 15-19 |

Steps 1-9 give gor_01 core 0 about 19-23 ms (est.); step 10 is needed for both scenes to reach 16.7.

### Steps 6 and 9 as built: `gxrows`, `gxkold` (2026-11-28)

Both switches default 0, 2 is the verify mode, both need `gxone` (so `gxonehop=1`, `gxthread=0`). Files: `port3ds/game/gx/gx_draw.c` (`vsh_state_dirty`, `vsh_rows_merge`, `vsh_send`, `vsh_rows_check`, the `plat_gpu_imm_begin` call), `port3ds/plat/gpu.c` (gxrows section after `oh_light`, `vsh_const_rows_m`, gxkold section after `plat_gpu_set_state_one`, `end_segment`, `submit`, the draw entry points), `port3ds/include/plat/gpu.h`. Gates: the PERF doc section "Steps 6 and 9 of the revised plan".

- **`gxrows`: uniform rows, compared once.** The front end keeps one block (`s_vw`, 18 rows of 4 floats and the flags). `vsh_state_dirty` builds the dirty parts into a scratch block as before (same 32-bit products, same order), then compares each rebuilt row with the kept row and stores and marks the changed ones (`s_vrows`, bit per row, bit 18 the flags). A send (`plat_gpu_vsh_rows`) gives the marked rows; the back end puts only those through `vsh_const_rows_m` (the same `vunif_u` row compare against the rows the GPU holds, the same rotation of the clip rows) and clears the marks. Removed: the 304-byte block compare and copy into `s_vsh_const`, the block id, the 304-byte copy into `s_last_vconst`, the compare of the unmarked rows. The rows are 32-bit float words (the frame list writes them with `GPUREG_VSH_FLOATUNIFORM_CONFIG` bit 31, f32 mode), as before: converting to f24 would change the words. A record's first send sets every row (the rows held were reset); `end_segment` keeps the 18 rows held and the flags and sets them again in the next record. With `gximmfl`, `plat_gpu_imm_begin`'s `cid` is the rows word. Not done: `cur_pnmtx % 10` through a table (a division by a constant is a multiply on ARM already; no gain expected).
- **`gxkold`: Record leftovers under K.** (1) `s_last_state` is a pointer (`s_lsp`); with `gxkold=1 gxone=1` it points at the front end's `PlatGpuState` after each `set_state_one`, and `set_state_one` copies no regions. The record's first state and `end_segment`'s state stay copies (`submit` and `plat_gpu_set_state` take the own copy back). (2) The shader draw entry points leave out their own record-room test, which `oh_need_room` (3 072 bytes against 96 or less, the same `flush_segment` at the same point) always covers under `gxonehop`. (3) The `vsh_calls/vsh_indices/vsh_draws` stats are not counted. Kept (needed for the same words): the index arena test and copy (`r->idx` is what the packet reads), the merge test on `s_last_vdraw`, `s_last_vconst_valid`. Not done: the `oh_c3d_cmd` compare and copy of the whole state for clears, copies and CPU-path draws (the render thread needs the CMD_STATE).
- **Verify.** `gxrows=2`: the front end runs the id path beside it and checks at every send that rows are marked exactly when the id changed since the last send and that `s_vw` equals `s_vsh_const`; the back end makes the rows from the whole block on a copy of the rows held and compares the result byte for byte, also for a send with no rows marked. `gxkold=2`: nothing is read in place; at every `set_state_one` and every read on the shader path (`oh_draw`, chunk use, immediate draw, `oh_c3d_cmd`, `oh_after_c3d`) the copy is compared with the front end's state.
