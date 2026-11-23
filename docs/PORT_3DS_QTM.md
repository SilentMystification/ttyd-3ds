# QTM, VRAM and the FCRAM bus: placement plan

Scope: what the port puts in the New 3DS extra RAM ("QTM", 2 MiB for the application at `0x1E800000`) and in VRAM (6 MiB), why, and how each choice is measured. Written 2026-11-23. Background: [PORT_3DS_PLAN.md](PORT_3DS_PLAN.md) section 5.7, [GC3DS_LIBRARY_NOTE.md](GC3DS_LIBRARY_NOTE.md) (the placement API), [PORT_3DS_60FPS.md](PORT_3DS_60FPS.md) (lever 6).

## Rules for this plan

- **Nothing is assumed about speed.** Whether QTM is faster or slower than cached FCRAM, whether the PICA can read it, and whether VRAM reads are cheaper for the GPU than FCRAM reads are console measurements. A separate benchmark CIA measures them (CPU bandwidth and latency per core and working-set size, cache attributes, flush cost, GPU reads from and writes into QTM, coherency, and CPU-against-GPU contention on FCRAM, QTM and VRAM). The console tests below run after it.
- **Default placements do not change until those numbers exist.** Everything added here is registered at the tier it has today and moves only with a switch.
- Azahar has no cache model and no bus model. Its data samples are an access-frequency ranking, and its runs show only that a placement works and that the picture is unchanged.

Owner decisions recorded here (2026-11-23):

| Question | Decision |
|---|---|
| Game textures in VRAM | Wanted. Order: format census, compact exact formats, then the VRAM tier. Target: all hot textures of a scene in VRAM |
| EFB size | A 512x608 render target with the existing 512x1024 texture view is approved (design below). The GX agent implements it; `gpu.c` and `stereo.c` are its files |
| EFB colour as RGB8 | Rejected: the game uses destination alpha (findings below) |
| 16-bit depth | Investigate only; no default changes |
| Native 400x240 rendering | Not now; only if later performance work needs it |

## 1. What is in QTM today

The CIA's ExHeader maps `1e800000-1e9fffff` read/write (`port3ds/rsf/ttyd.rsf`). `plat/memplace.c` packs the registered placements into it at boot. The HUD's `QTM 1600/2048` counts reserved budgets, not bytes in use:

| Placement | Budget | In use | Who touches it | Why it is there |
|---|---|---|---|---|
| `gx record` | 1024 KiB | 2 x 512 KiB (`plat/gpu.c`, `RECORD_BYTES`) | the game thread (core 0) writes one record per frame, the render thread (core 2) reads it; 168 to 409 KiB of it per frame (gor_01: 409) | the largest per-frame CPU-to-CPU stream; the plan's first candidate |
| `log` | 320 KiB | the log slots (`plat/log.c`) | every thread writes, the writer on core 1 reads | shared by all cores; cold in a normal run (25 lines per 420 frames) |
| `gx state` | 128 KiB | **0: nothing allocates from it** | | reserved for the GX state tracker, which is static data in the image |
| `audio state` | 128 KiB | **0: nothing allocates from it** | | reserved for MusyX state, which is static data and game heap |

So 1344 KiB are in use by two buffers, 256 KiB are dead reservations and 448 KiB are free. The two reservations stay for now (no default changes); `place=gx_state:heap,audio_state:heap` returns their room for an experiment.

In the data samples below, QTM gets 0 to 1.8% of the sampled accesses (the record, written by `memcpy` and read by `render_record`).

## 2. Measured hot data

Method: `pc_sample.py --data` through Azahar's GDB stub: at each stop the load or store instruction under each thread's PC is decoded and its address is named from the ELF's data symbols, the `[MEMMAP]` lines and the thread stacks. 150 s per scene from frame 841, one stop every 0.4 to 0.7 s (the stub is slow), default switches plus `memmap gxthreadstats=60 wdog=100000`. Scenes: `gor_01` (story 30), a battle (`gon_02`, `battle=1 btlauto=perfect`), `aaa_00`. Build #484. A thread parked in a wait is not counted.

The samples are few (108, 222 and 153 data accesses), so a share is good to about +-5 points and anything under 2% is noise. An earlier 300 s run of the same tool in the `gor` area (672 accesses, build of 2026-11-22) agrees with them and is the fourth column.

Share of sampled data accesses by region:

| Region | gor_01 | battle | aaa_00 | gor area, earlier run |
|---|---|---|---|---|
| thread stacks (game thread first) | 24% | 20% | 18% | 17% |
| static data of the image (`.bss`, `.data`) | 21% | 27% | 22% | 28% |
| literal pools and tables in `.text`/`.rodata` | 18% | 18% | 18% | 10% |
| libctru heap (display-list cache, vertex streams' tables, record ring) | 16% | 8% | 7% | 0.2% |
| game heap 1 (map data) | 11% | 1% | 18% | 20% |
| game heap 0 | 5% | 14% | 0% | 4% |
| smart heap (animation data) | 2% | 0.5% | 0.7% | 12% |
| linear heap | 4% | 10% | 15% | 9% |
| QTM (`gx record`) | 0% | 1.8% | 1.3% | 0% |

The linear-heap accesses are the log writer scrolling the bottom screen's console (`newRow`), which these runs caused themselves with `gxthreadstats=60`; a normal run logs almost nothing.

The top entries, all scenes together:

| Entry | Size | Share | Threads | Functions |
|---|---|---|---|---|
| game thread stack (the range it uses) | 14 to 18 KiB | 16 to 19% | game | `gx_call_list`, `tev_compile_pass`, `plan`, `draw_from_reader`, `plat_gpu_vsh_draw` |
| `s_state` (the PICA state being built, `gpu.c`) | 728 B | 4 to 7% | game | `memcpy`, `memset`, `plat_gpu_set_state` |
| `gx` (the GX state) | 5 KiB | 0 to 10% | game | `draw_from_reader`, `gx_draw_vertices` |
| `gpuCmdBufOffset`, `gpuCmdBuf`, `C3D_FVUnifDirty`, `s_vunif` (citro3d and uniform state) | 1.7 KiB | 5 to 9% | render | `GPUCMD_Add`, `C3D_UpdateUniforms`, `vsh_apply_const` |
| render thread stack | 10 KiB | 1 to 4% | render | |
| audio thread stack, `sBus` | 10 KiB, 5.6 KiB | 3 to 5% | audio | `salPcProcessPb`, `snd_handle_irq` |
| `gameheap1+0x1c000..0x21000` (aaa_00: collision data) | 24 KiB | 14% in aaa_00 | game | `hitCheckVecFilter` |
| everything else | | 1 to 4 samples per 4 KiB page | | spread over game heaps 0 and 1, the smart heap and the libctru heap |

No entry is touched by two threads on different cores often enough to show (the "shared data" list of the tool is empty in all three scenes).

**Conclusion.** There is no concentrated hot buffer to move. The hot set (game stack, GX and PICA state structures, citro3d's statics, the audio stack and bus, in one scene a collision block) is about 60 KiB. It stays in the caches (16 KiB L1 data per core, the 2 MiB L2), so the RAM under it does not matter. The rest of the accesses are flat over about 8 MiB of game heaps: no 2 MiB of it is hotter than the rest, and moving a heap would mean changing the game's allocator. Under the owner's rule above, QTM is therefore not expected to speed up the CPU by itself. What it can still do is section 3's point: carry traffic that would otherwise load the FCRAM bus.

Candidates that remain, each to be decided by a console A/B:

| Candidate | Size | Why it could matter | State |
|---|---|---|---|
| `gx record` (in QTM today) | 1024 KiB | 0.3 to 0.8 MiB written and read by two cores per frame: more than L1, part of L2 | A/B by `place=gx_record:heap` |
| `gx ring` (`gxthread=1/2`: the GX call record core 0 writes and core 2 translates) | 4096 KiB by default | the same kind of stream, larger; too big for QTM unless `gxthreadkb` is small | registered now, heap; `place=gx_ring:qtm` with `gxthreadkb=256` |
| `gx index` (the records' index arenas) | 768 KiB | the CPU writes 50 to 150 KiB of indices per frame and the GPU reads them: only useful in QTM if the PICA reads QTM | registered now, linear; `place=gx_index:qtm qtmgpu` |
| game thread stack | 64 KiB would do | the most accessed data, but cache-resident | not built: needs a stack switch at boot; only if the benchmark shows QTM beating cached FCRAM at L1-miss latency |
| collision data, animation data | several MiB per map | flat, map-dependent | not built: they live in the game's heaps |

## 3. FCRAM traffic census

The owner's idea: QTM and VRAM are separate memories, so traffic moved into them stops competing for the FCRAM bus (CPU, GPU, LCD controller and DSP all use it). This is the per-frame FCRAM traffic by source, from the code and the counters of PORT_3DS_60FPS.md (gor_01 where a number depends on the scene; mono; 60 Hz). "Estimate" marks numbers derived from sizes, not counted.

| # | Source | Per frame | Kind | Can it leave FCRAM? |
|---|---|---|---|---|
| 1 | **GPU texture reads.** Every game texture is in linear FCRAM (`C3D_TexInit`, `gpu.c` `plat_gpu_tex_create`). Bound per frame: see the `vramstats` table in section 6 (an upper bound: each bound texture read once in full; a minified texture without mip levels can be read more than once, a small or magnified one less) | 1 to 2 MB bound | GPU read | **VRAM** (the tier of section 6); 1.4 MiB free today, 3 MiB after the EFB change |
| 2 | **LCD scanout.** `gfxInitDefault` puts the framebuffers in linear FCRAM: top 240x400 BGR8 (288 KB, double buffered), bottom 240x320 RGB565 (154 KB, the console) | 442 KB at 59.83 Hz (26 MB/s); +288 KB in stereo | LCD read | the top screen's buffers could be in VRAM (`gfxInit(..., true)`: 576 KB of VRAM, 864 KB with the right eye). The pause screen greys the frame on the CPU and VRAM is mapped read-only, so that path would need the GPU. The bottom screen is drawn by the CPU and stays |
| 3 | **Display transfer** of the 240x400 screen target (VRAM) into the top framebuffer (linear) | 288 KB written; twice in stereo | GPU write | leaves with the framebuffers of row 2 |
| 4 | **Vertex streams** of the shader path (linear, 6 MiB budget): the lists drawn this frame, 27.9 K list vertices and 1.4 K immediate ones in gor_01 | 0.4 to 0.7 MB (estimate: 12 to 24 bytes a vertex) | GPU read; CPU write only when a list is first cached | VRAM is not possible for data the CPU writes every frame; cached streams could be copied there like textures, but they are larger than the VRAM left. QTM only if the PICA reads it |
| 5 | **Index arenas** (linear, 2 x 384 KiB): the indices of the frame's shader-path draws | 50 to 150 KB written by core 0 and read by the GPU (estimate) | CPU write, GPU read | QTM only if the PICA reads it (`gx index` placement) |
| 6 | **citro3d command buffer** (linear, 2 MiB, half active): 536 draw commands, states and uniforms in gor_01 | see `vramstats` (command buffer per mille of 1 MiB) | CPU write (core 2), GPU read | QTM only if the PICA reads command lists from it. With prebuilt PICA command lists per display list (the GX rework) most of this becomes static GPU-read data |
| 7 | **Render record** (QTM in the CIA) | 409 KB written and read | CPU | already off FCRAM; `place=gx_record:heap` puts it back for the A/B |
| 8 | **CPU traffic of the game and the GX front end**: cache misses over the game heaps, the display-list cache (8 MiB), list bytes (355 KB of GX lists per frame in gor_01) | unknown; 1 to 3 MB is a guess from the sizes touched | CPU | needs the ARM11 performance counters (cache-miss counts) from the benchmark's API. No single buffer stands out (section 2) |
| 9 | **EFB copies** (`GXCopyTex`) | none while the targets are in VRAM; a target that fell back to linear memory is written by `TextureCopy` | GPU write | stays in VRAM as long as VRAM has room |
| 10 | **Stereo** (slider up): the colour snapshot `s_snap` is a 512x1024 RGBA8 texture in linear memory (`stereo.c:128`): the shown rows are copied into it and both eyes sample it | 1.6 MB written, up to 3 MB read (estimate: 480x800 pixels per eye) | GPU write and read | VRAM, once the EFB change frees room (1.19 MiB as 512x608 with the same view trick) |
| 11 | **DSP** (NDSP wave buffers in linear memory) | 2 KB (32 kHz stereo 16-bit) | DSP read | no (the DSP reads linear memory only); negligible |
| 12 | **DVD and ARAM DMA** | 0 in a steady scene; a streamed music track refills its ring at about 0.5 KB per frame | CPU | load time only |

Ranking for a mono frame: texture reads (1), then scanout plus the display transfer (2, 3: 730 KB), then vertex and index streams (4, 5), then the command buffer. In stereo the snapshot (10) is the largest by far.

What can move, and the bytes per frame it takes off the FCRAM bus:

| Move | Per frame | Needs |
|---|---|---|
| Hot textures to VRAM | up to the bound bytes of row 1 | the VRAM tier (built, default off) and room: compact formats and the EFB change |
| Top framebuffers to VRAM | 576 KB (288 scanout + 288 transfer) | 576 KB of VRAM; a GPU path for the pause screen |
| Stereo snapshot to VRAM | 1.6 to 4.6 MB, stereo only | 1.19 MiB of VRAM (after the EFB change) |
| Index arenas, command buffer to QTM | 0.2 to 0.4 MB | the PICA reading QTM (benchmark) |
| Render record in QTM (today) | 0.4 to 0.8 MB of CPU traffic | nothing; the A/B says whether it helps |

Whether any of this buys frame time is the benchmark's contention test (CPU against GPU on FCRAM, on QTM and on VRAM): if the GPU's FCRAM reads do not slow the CPU measurably, only the GPU-side gain (VRAM against FCRAM read speed) is left.

## 4. Data shared between CPU and GPU

Does the PICA read QTM? **Unknown on hardware.** What is known:

- Azahar: the probe's `GX_TextureCopy` between QTM and linear memory fails in both directions (PORT_3DS_TESTING.md, "Hardware probe"), although Azahar's GPU code translates QTM addresses in some paths. Azahar is no evidence either way.
- libctru's `osConvertVirtToPhys` maps `0x1E800000` to physical `0x1F000000`, so a QTM address can be written into a GPU register. Whether the PICA's bus reaches that memory is the benchmark's GPU read and write test.
- The placement table refuses `GPU_READ`/`GPU_WRITE` placements in QTM unless the switch `qtmgpu` is given; `DSP` placements never go there.

If the PICA reads QTM, the candidates are the per-frame CPU-written, GPU-read buffers: the index arenas (768 KiB, registered as `gx index`), the command buffer (citro3d allocates it itself with `linearAlloc`; it needs a hook in the renderer), and the immediate-mode part of the vertex streams. Static GPU data (textures, cached streams) belongs in VRAM, not QTM. If the PICA does not read QTM, QTM holds CPU-only data and this section is closed.

With the GX rework (prebuilt PICA command lists per cached display list) the per-frame CPU-written data shrinks to the record, the uniform values and the lists of the frame's new or changed display lists. The prebuilt lists are static GPU-read data: linear memory, or VRAM if the benchmark shows the GPU reading command lists from VRAM (not known to work; test it before planning on it).

## 5. Hot code in QTM

**Can the application run code from QTM?** Not known. The ExHeader mapping is a static mapping with read/write permission; the mapping descriptor has only a read-only flag, so execute permission cannot be asked for there. It would have to be set at run time:

- `svcControlMemory(MEMOP_PROT)` takes read and write only.
- `svcControlProcessMemory(MEMOP_PROT, R+X)` is what homebrew JITs use on heap pages under Luma3DS (which opens every SVC to every title). Whether the kernel accepts it for a static mapping is not documented.

The probe now tests exactly this (`port3ds/probe`, lines `exec heap page` and `exec QTM page`): it asks for R+X with both calls, on a heap page as the control and on the last page of QTM, and calls a two-instruction function there only if `svcQueryMemory` then reports execute permission. In Azahar both report success (it does not enforce permissions), which says nothing about the console.

**Is it worth it?** The hottest functions by `pc_sample.py` (game, render and audio threads, the three scenes together, 817 busy samples):

| Functions | Share of busy samples | Code size |
|---|---|---|
| top 10: `gx_call_list`, `GPUCMD_Add`, `plat_gpu_set_state`, `hitCheckVecFilter`, `salPcProcessPb`, `memcpy`, `draw_from_reader`, `C_MTXConcat`, `fetch1`, `animPoseMain` | 52% | 44 KiB |
| top 20 | 74% | 93 KiB |
| top 30 | 84% | 120 KiB |
| top 45 | 92% | 151 KiB |

So 150 KiB of code is 90% of the time. It would fit QTM many times over, but:

- That much code and its data fit the 2 MiB L2 cache, and code is read-only, so it is not written back. RAM speed matters for it only on an L2 miss.
- QTM at `0x1E800000` is 488 MiB from the image at `0x00100000`, far beyond the 32 MiB reach of a `BL`. Every call between moved and unmoved code goes through a veneer (an indirect jump), and the hot functions call `memcpy`, libctru and each other across that boundary thousands of times a frame.
- It needs a second load region in the link, a copy at boot, cache flushes, and crash-report and profiler support for the second code range.
- The locality gain that is available without QTM is cheaper: order the hot functions together in `.text` (the build already uses `-ffunction-sections`), so they share pages and L2 sets.

**Decision: not built.** It is unverified that it can work, and the expected gain is small against the veneer cost. Revisit only if (a) the probe's `exec QTM page` line says `call ok` on the console and (b) the benchmark shows code fetch from QTM clearly faster than from FCRAM on an L2 miss. The switch would be `qtmcode`.

## 6. VRAM

### How VRAM is used today

| Use | Bytes |
|---|---|
| EFB colour, 512x1024 RGBA8 (`gpu.c` `TARGET_W`/`TARGET_H`) | 2048 KiB |
| EFB depth, 512x1024 D24S8 | 2048 KiB |
| screen target, 240x400 RGB8 | 281 KiB |
| copy scratch target | 256 KiB |
| free for copy targets (`GXCopyTex`) | about 1.4 MiB (the boot log's `gpu: VRAM after the EFB...` line) |

The picture drawn is 480x608 of the 512x1024 target: 1.14 of each 2 MiB. All game textures are in linear FCRAM; only copy targets use the free VRAM.

### Texture formats today

`gx_tex.c` decodes every GX texture (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, C4/C8/C14 through their TLUT) to RGBA8 and then picks the PICA format from the **content**, not from the GX format (`pick_format`):

| Content of level 0 | PICA format | Bytes per texel |
|---|---|---|
| gray (R = G = B), any alpha | LA8 | 2 |
| opaque colour | RGB565 | 2 |
| colour with alpha 0 or 255 only | RGBA5551 | 2 |
| anything else | RGBA8 | 4 |

Sides are padded to powers of two by nearest resampling. RGB565 and RGBA5551 already drop colour bits of 8-bit sources (CMPR's interpolated colours, RGBA8, palettes), so "exact" is not today's bar for those two.

Bytes per scene come from `vramstats=60` (live = every texture alive; bound = the textures bound in a frame); the table is in "Azahar results" below.

### Compact formats (for the GX agent, `gx_tex.c`)

Exact mappings by content, each checked per texture by converting back and comparing:

| Content | Today | Compact | Saving |
|---|---|---|---|
| gray, opaque (I4, I8 sources, gray palettes) | LA8, 2 | **L8**, 1; **L4**, 0.5 when every value is a multiple of 17 (all I4 sources) | 2x to 4x |
| gray with alpha where both are 4-bit exact (IA4 sources) | LA8, 2 | **LA4**, 1 | 2x |
| constant colour, varying alpha (font and mask textures) | LA8 or RGBA8 | **A8** or **A4** when the colour is white (the TEV sees the same value); other constant colours stay | 2x to 8x |
| colour with alpha, all channels 4-bit exact | RGBA8, 4 | **RGBA4**, 2 | 2x |
| RGB5A3 with 3-bit alpha | RGBA8, 4 | stays: 3-bit alpha levels (0, 36, 73, ...) are not 4-bit values | none |
| CMPR | RGB565 or RGBA5551, 2 | **ETC1** (0.5) or **ETC1A4** (1) only behind a switch with a visual check: it is a lossy re-encode and needs an encoder at load time | 2x to 4x |

Needed: `PLAT_TEXFMT_L8/A8/LA4/L4/A4/RGBA4` in `plat/gpu.h`, cases in `tile_level()` and `level_bytes()`, and the `fmt == RGBA8 ? 4 : 2` byte counts (`gpu.c` in three places, `gx_tex.c` in two) replaced by a per-format table. The 4-bit formats pack two texels per byte in the tiled order. Mip chains work as today. The check is the `determ` shot comparison plus `texverify`.

### The VRAM tier (`port3ds/plat/vramtex.c`, switch `vramtex=1`, default off)

- **Candidates.** A texture created in linear memory, at most `vramtexmax` KiB (256), alive for 30 frames and bound in 8 of them. A texture the game rewrites is freed and created again by `gx_tex.c` on every change, so it never reaches that age and stays linear. Copy targets are never candidates.
- **Promotion.** After a frame is queued, the hottest candidates bound in that frame (binds per frame, smoothed) are copied linear to VRAM by `GX_TextureCopy` (at most 4 textures and `vramtexrate` KiB, 256, per frame) and the texture's data pointer is swapped. The linear copy is kept.
- **Eviction.** Because the linear copy is kept, eviction is a pointer swap. Textures not bound for 120 frames make room. The VRAM of an evicted texture is reused 3 frames later, when no queued frame can still sample it.
- **Copy targets win.** The tier leaves `vramtexreserve` KiB (512) free. When a VRAM allocation of the renderer fails, the tier evicts everything, returns its VRAM and pauses for 600 frames; that one target takes the existing linear fallback.
- **No fragmentation.** The tier takes one block per VRAM bank and sub-allocates inside, so small textures never split the heap the targets use.
- **Hooks.** It wraps citro3d at link time (`--wrap` of `C3D_TexInitWithParams`, `C3D_TexBind`, `C3D_TexDelete`, `C3D_FrameEnd`), so `gpu.c` is untouched. The direct calls that should replace the wraps once the GX rework lands:
  1. `plat_gpu_tex_create` and `plat_gpu_tex_create_mip`, after the flush: `vt_add(&t->tex)`;
  2. `apply_state`, before `C3D_TexBind(u, t)`: `vt_use(u, t)`;
  3. `tex_alloc_slot` and `reap_targets`, before `C3D_TexDelete(&t->tex)`: `vt_forget(&t->tex, 1)`;
  4. `render_record`, after `C3D_FrameEnd(0)`: `vt_frame()`; and in `plat_gpu_tex_create_target` when `C3D_TexInitVRAM` fails: set the pressure flag.
  Rule for the renderer: nothing writes a non-target texture's `data` after it was created.
- **Limits of the wrap version.** A copy target that fell back to linear memory is recognised by the failed VRAM allocation just before it, so the tier is off with `vramfail=1`. The promotion copy waits for the GPU inside the render thread's `C3D_FrameEnd`, so `render_us` includes it while textures are being promoted.
- **Ranking.** "Bytes sampled per frame" cannot be counted: the PICA has no counter for it. The tier ranks by binds per frame, and `vramstats` reports bound bytes as the upper bound.

### The EFB as 512x608 (approved; for the GX agent)

The PICA samples only power-of-two textures, and the EFB is sampled as a texture by `present()`, by every EFB copy (`copy_pass`) and by stereo (the depth view and the snapshot rows). An exact 480x608 target could not be bound by any of them, and a power-of-two copy for them is 2 MiB, more than the change frees. So:

- **Target:** 512x608, colour RGBA8 and depth D24S8: 2 x 1216 KiB = 2432 KiB instead of 4096. **Frees 1664 KiB (1.625 MiB).** (480x608 would free 152 KiB more and lose direct sampling.)
- **View:** `s_efb_tex` stays a 512x1024 texture view at the colour buffer's address. Its rows past 608 are other VRAM and are never sampled: every sampler uses rows inside the picture. A texel's v shifts by a constant: memory row r of the 608-row target is row r of the view, and v = 1 - r/1024, so target viewport pixel y (which is memory row 607 - y) is v = (y + 416) / 1024 instead of y / 1024. `stereo.c`'s depth view is the same.
- **Changes in `gpu.c`:** `TARGET_H` 1024 to 608 for the target, a separate view height 1024; `REGION_H` 800 to 608 and `EFB_X_OFFSET` 96 to 0 (the picture fills the target); the uv of `copy_pass` and `present` get the +416 rows; `present` draws the quad over the middle 304 of the 400 screen columns and clears the screen target to black first (the bars are EFB rows today); the tile index math of `GXPeekARGB`, the shots and the depth readback (`TARGET_H - 1 - ty`, the display transfers' `GX_BUFFER_DIM(TARGET_W, TARGET_H)`) uses 608.
- **Changes in `stereo.c`:** its own `TARGET_H`/`REGION_H`; the snapshot copy's first tile row and row count; the mesh uv (`ty / TARGET_H`); `s_snap` can then be 512x608 in VRAM with the same view trick (row 10 of the census).
- **Tests:** `determ` shots identical before and after in `gor_01`, a `gon_02` battle, `aaa_00`, the pause menu; `gputest` (orientation and the bars); the EFB-copy effects (`fx=indirect`, the START menu in `gor_01`, `eff_splash`'s `GXPeekARGB`); `vramfail=1` (strip copies) against the VRAM path, 0 differing bytes; `gxdepthshot`; `stereoforce=1` shots against the earlier ones; the boot log's VRAM line (expect about 3.0 MiB free).
- **Azahar check first:** Azahar's rasterizer cache sees a 512x1024 texture that overlaps a 512x608 render target and whatever VRAM follows it. It may refuse to treat the target as that texture's source, or read the tail from emulated memory every frame (slow). Test it before anything else with one EFB copy and the present; if Azahar cannot do it, the view can be 512x608 rows of a 512x1024 texture only on the console and Azahar needs a copy path behind a switch.
- **Console check:** the picture itself (the view trick is standard on the PICA, but this port has not used it).

### EFB format findings (investigate only)

- **Colour must keep alpha.** The game writes destination alpha and blends with it: `shadowdrv.c:1397-1410` and `battle_stage.c:801-819` set `GXSetAlphaUpdate(GX_TRUE)` and then `GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, ...)`. RGBA copy targets read EFB alpha too. RGB8 would break shadows and battle stages.
- **Stencil is unused** (no stencil test in `gpu.c` or `stereo.c`). D24S8 is used because the depth buffer is read back and viewed as a 4-byte format (display transfers as RGBA8, stereo's depth texture).
- **16-bit depth** would save 608 KiB (after the EFB change) but cuts depth precision by 256. The game asks for 24-bit Z (`GX_PF_RGBA6_Z24`), and the decal and z-fighting acceptance tests of the shader path (`vsh_accept.py`) were tuned against 24 bits. It also changes every depth readback and the stereo depth view. To judge it: run `vsh_accept.py` and the galleries with a D16 target behind a switch. Not done; no default change.

### Other VRAM candidates

After the EFB change about 3.0 MiB is free. Claims on it, in the order the census suggests: copy targets (up to 1 MiB at times), hot textures (1 to 2 MiB with compact formats), the stereo snapshot (1.19 MiB, only with the slider up), the top framebuffers (576 KiB). They do not all fit: textures and the stereo snapshot can share (the tier yields under pressure), the framebuffers are last.

## 7. Budget of the 2 MiB of QTM

No default changes now. The split this plan would move to, each line conditional on its console result:

| Use | KiB | Condition |
|---|---|---|
| `gx record` | 1024 | stays if `place=gx_record:heap` is not faster |
| `log` | 320 | cold; first to leave if room is needed |
| `gx state`, `audio state` | 0 (256 returned) | drop the reservations when something else needs the room |
| `gx index` | 768 | only if the PICA reads QTM and the A/B gains |
| `gx ring` | up to 512 with a small `gxthreadkb` | only with `gxthread=2`, if its A/B gains |
| hot code | 256 | only if section 5's two conditions hold |
| free | the rest | |

With the GX rework the record shrinks and the ring may go away; redo this table then.

## 8. What is implemented

| Item | Where | Default |
|---|---|---|
| `gx index` placement (the records' index arenas) | `plat/memplace.c`, `plat/gpu.c` (one allocation line) | linear, as before |
| `gx ring` placement (the `gxthread` record ring) | `game/gx/gx_rec.c` (one allocation line) | heap, as before |
| `place=` accepts a placement registered after boot | `plat/memplace.c` | |
| `plat_place_info`, `[MEMMAP] place_<name>` lines, and `datasample.py` naming them | `plat/memplace.c`, `game/memmap_dump.c`, `tools/port3ds/datasample.py` | |
| VRAM texture tier | `plat/vramtex.c`, four `--wrap`s in `port3ds/CMakeLists.txt` | off (`vramtex=1`) |
| Texture and command-buffer census | `vramstats=N` | off |
| Probe: execute permission on a heap page and on QTM | `port3ds/probe/source/main.c` | |

Not built, with the reason: hot code in QTM (section 5), a QTM stack (section 2), the EFB change, compact formats, framebuffers and the stereo snapshot in VRAM (the GX agent's files; designs above).

## 9. A/B method and the console test list

Method: one switch per run, the same scene and frame window, `gxthreadstats=60` (game busy, render and GPU time per frame; cheap) and `vramstats=60`. Compare the averages of the windows from frame 1141 to 1501, three runs each (the console's run-to-run noise is not known yet; the first pair of identical runs gives it). Scenes: `map=gor_01 story=30`, `map=gon_02 story=30 battle=1 btlauto=perfect`, `map=aaa_00`. All runs are the CIA (QTM mapped) through `devupdate`.

Run these **after the benchmark CIA**, and skip what its numbers already rule out:

| # | Question | Switches (added to the scene and `gxthreadstats=60 vramstats=60`) | Read |
|---|---|---|---|
| 0 | Probe | `ttyd_probe.cia`: lines `GPU QTM->lin`, `GPU lin->QTM`, `memcpy ...QTM`, `exec heap page`, `exec QTM page` | does the PICA reach QTM; can QTM be executable |
| 1 | Baseline and noise | none, twice | `game us: busy`, `render`, the HUD's fps |
| 2 | Is QTM worth it at all | `noqtm` | busy and render against 1 |
| 3 | The record alone | `place=gx_record:heap` | as 2 |
| 4 | The log alone | `place=log:heap` | as 2 (expected: no difference) |
| 5 | Textures in VRAM | `vramtex=1` | render and GPU time; the `vramstats` line (bound KB from VRAM against linear, pressure 0); the picture |
| 6 | Tier size | `vramtex=1 vramtexreserve=256`, `vramtex=1 vramtexmax=512` | as 5 |
| 7 | Copy targets under the tier | `vramtex=1` with `fx=indirect` and the START menu in `gor_01` | no `gpu: no VRAM` line after the first; `target pressure` count |
| 8 | Index arenas in QTM (only if test 0 says the PICA reads QTM) | `qtmgpu place=log:heap,gx_state:heap,audio_state:heap,gx_index:qtm` | the picture first, then busy and render |
| 9 | The GX ring in QTM | `gxthread=2 gxthreadkb=256` against the same with `place=gx_ring:qtm` | busy, translator time |
| 10 | Data sampling with real addresses | `memmap`, and `pc_sample.py --pid <PID> --port 4000 --data --memmap <log> --interval 20 --duration 60` (Rosalina) | the ranking of section 2 at 50 Hz instead of 2 Hz |

A result is kept only if it is outside the noise of test 1 in at least two scenes.

## 10. Azahar results (correctness only)

See the end of this file's history for the runs; the table is filled by the commit that adds the implementation.
