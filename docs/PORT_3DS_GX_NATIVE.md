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
2. **`gxone`:** single shadow, dirty bits, interned class ids, own PICA encoders; translation on core 0; downstream compares by id. `gxone=2` verifies encoder words against the citro3d capture word for word plus the `gxdirty=2` checks.
3. **`gxchunk=1` (COPY):** batch chunks and PSO blocks; the game thread writes the frame list; render on core 1 only submits. `gxchunk=2` builds the gxonehop words beside it and compares the PICA register file at every draw (register-file model treating texture-cache clear, FIXEDATTRIB, uniform-index and LUT writes as events).
3b. **`texdt`:** texture tiling by DisplayTransfer, verify mode against CPU Morton, then determ.
4. **Immediate primitives into the frame list**, eligibility mask in place of the route. Default keeps today's K=30 hysteresis (identical output); starting a group on the shader at once in a viewport with no CPU-path draw is a separate switch (open owner question; the first ~30 frames after a load would differ).
5. **`gxchunk=call` (CALL mode)**, gated on the step-0 console results.
6. **Stereo:** per-eye tail lists with per-eye rows on the CPU (byte-identical; planner default). A shader shear would be cheaper but not identical (open owner question).
7. **Chunk/stream builds as core-2 jobs at map load** (frame draws uncached meanwhile, the `dlspread` rule), targeting the ~345 ms first-frame build cost in gor_01.

## 5. Obsolete when done

- `gx_rec.c` entirely (ring, translator thread, fences, array snapshots, gx_game/gx split, `dd_slot`/`dd_same`, `gx_rec_fast`, `GXR_PRIMD`, fused-list table, `consume`) and switches `gxthread`, `gxpipeline`, `gxthreadsplit*`, `gxthreaddedup`, `gxrecfuse*`, `gxreclgroup`, `gxphase`, `gxsdl`. Frees about 4 MB of heap (the ring), which also ends pik_00's heap-margin problem.
- In `plat/gpu.c`: shader-path record commands and their `render_record` replay, `PlatGpuState` build/compare, gxdirect DTab/capture, all of gxonehop, `vunif` pending sets, `gxmerge`, `gxflushrender`; the `gx record` QTM placement becomes the frame-list placement.
- In `gx_vsh.c`/`gx_dlcache.c`/`gx_draw.c`: per-frame group keys and hashes, record hints, the `gx_vsh_frame` streak pass, decal links except for the CPU path, per-open batch epoch checks, the `s_ix` pool, `gxstatever`, `gxstatededup`. `gxdirty`, `gxstatecache`, `gxdirect`, `gximmdirect`, `gxonehop`, `gxbatch` stop being switches and become the only path.
- Kept: the CPU path (`gxvsh=0` and rare draws), the stream builder, memstable trust, the texture tier, `dlspread`, the verify modes.
