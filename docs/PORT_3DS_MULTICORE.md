# Multicore plan: splitting the work across cores

Status: planned 2026-11-22. Measurements so far come from Azahar, which gives shares but not hardware timings. The hardware numbers come once `ctr-devkit` is in and hardware sessions resume. Star3D references are ideas only (its repository has no licence); see [STAR3D_NOTES.md](STAR3D_NOTES.md).

## Budget

- Cores 0 and 2: the whole core each.
- Core 1: up to 80%, but every share we take slows the system services on core 1 (GPU cache flushes, file access, DSP). Star3D measured hub frames of 57.3 ms at 0%, 60.0 at 30% and 74.0 at 80%.
- Core 3: the system's.
- So about 2.8 cores at most, and core 1 is for light, latency-tolerant work only.

## Where things run today (`threadCreate` sites)

| Thread | Core | Work |
|---|---|---|
| game (`os_thread.c`) | 0 | All game logic, **plus all GX translation**: display-list parsing, vertex decode, CPU transform and lighting, texture hashing and conversion, TEV compile, recording |
| render (`plat/gpu.c`) | 2 (`gpucore`) | Replays the record into citro3d |
| gxtranslate (`game/gx/gx_rec.c`), only with `gxthread=2` | 2, priority 0x2F (below render) | Translates the recorded GX calls |
| log writer (`plat/log.c`) | 1 only (priority 0x3F), no fallback | `latest.log` writes |
| sdio (`plat/sdio.c`) | 1 only (priority 0x3E) | Every other SD write and read at run time: options, padrec, shots, wavdump, sweep list |
| audio (`plat/audio.c`) | 1 | MusyX software mix into one NDSP channel |
| dvd/io (`plat/disc.c`) | 1 | Disc image reads |
| arq (`game/ar.c`), card, alarm, vblank, watchdog | 1 | Small or waiting |

#### SD card rule: core 1 only (2026-11-22)

All SD access at run time happens on core 1; cores 0 and 2 never block on the card.
- **Disc image:** `plat_disc_read` from core 0 or 2 posts to the io thread's queue (core 1) and waits. It blocks the caller, which is the game's own synchronous `DVDRead` by design. Job workers never read the disc.
- **Log:** the writer thread is on core 1 only, lowest priority. If it cannot be created, the log stays direct (and `sdstats` shows the direct calls).
- **Everything else** (options save, padrec flushes, test shots, wavdump, sweep list read, padfile load) goes through `plat/sdio.c`: producers copy or give up their buffer and go on; the core 1 worker does the file work. Only boot reads (switches, options load before the first frame, DVDInit, the log's start) and the crash report stay direct.
- **Watch:** `sdstats=N` counts SD calls made outside the service from cores 0 and 2 after the first frame (linker `--wrap` of fopen, open, opendir, remove, rename, mkdir, unlink, fsync, stat). It must stay 0. `game/card.c` was not moved by this change: its `CARDInit` scan and `CARDDelete` remove run on the game thread and show up there until its owner moves them.

Azahar sampling (`gor_01`, shares of all busy time):

| Thread | Busy | Share of all busy time | Notes |
|---|---|---|---|
| game | 83% | 55% | — |
| render | 27% | 18% | about 73% of its time waiting |
| audio | 24% | 16% | — |
| one more core-1 thread | 16% | 11% | — |

**Core 0 is the wall, and core 2 has room.** Star3D found the same in its first hardware run: everything costly ran on core 0.

## Target architecture (owner decision 2026-11-22)

Core 0 runs **game logic only**, at full speed, the way the GameCube CPU did. All GPU-like work (GX translation: display-list decode, vertex work, texture and TEV handling) leaves core 0 for core 2 and the PICA. The ARM11 is faster than the Gekko for this work, so with GX off it the game thread should have headroom.

How we get there:
1. **Shader path** takes per-vertex work off the CPU entirely: lighting, the matrix palette, `MTX3x4` texgen, and bounding-box draws (bbox computed on the CPU from positions only). In progress.
2. **`gxthread=2`**: core 0 records, core 2 translates. Built; default off because translation is one serial chain that saturates core 2 and the frame-end fence serialises it with the next frame.
3. **Pipelined frame end (`gxpipeline`)**: the routine frame-end fence is answered when the frame is recorded, so core 2 translates frame N while core 0 runs frame N+1 (as the GameCube overlaps CPU and GPU). Memory-release fences, array snapshots and verify mode keep it safe. At most one frame of extra display latency.
4. **Cheaper recording**: state recorded as deltas at draws instead of every setter.
5. The default becomes `gxthread=2 gxthreadsplit=0 gxpipeline=1` only with identical output and faster frames in every scene, in Azahar and then on hardware.

**Measured (2026-11-22, Azahar, `gxthreadstats=60`, the window ending at frame 1261).** Step 1 is in `PORT_3DS_GX_PERF.md` ("Round 3"). Steps 3 and 4 are built: `gxpipeline=1` and `gxthreaddedup=1` (the record skips a state call whose words equal the last recorded for the same piece of state; 70-74% of state calls skipped). Both are pixel-identical to `gxthread=0` under `determ` (gor_01 and gon_02, every pixel of 24 shots), `gw_diff` 160 records equal (all 23 fields), and the verify sweep (`gxthread=3 gxpipeline=1 gxcache=2`, gor_01/tik_01/hei_01/gon_02) shows 0 cache mismatches, 0 stale, 0 lists changed, 0 getter mismatches and 0 reads after a later report. Release fences fire 0.00-0.03 times a frame with pipelining (the battle's 0.03 is a smart-heap allocation).

| Scene | today (`gxthread=0`): frame / core-0 busy | `gxthread=2 gxthreadsplit=0 gxpipeline=1`: frame / core-0 busy / fence wait | translator / render (core 2) | frame ends that waited for the frame before |
|---|---|---|---|---|
| gor_01 | 83.6 / 74.8 | 83.6 / **32.0** / 45.2 | 70.6 / 27.5 | 30 of 60 |
| gon_02 | 49.1 / 34.2 | **41.8** / 12.9 / 23.2 | 34.7 / 15.4 | 30 |
| aaa_00 | 50.2 / 35.1 | **41.8** / 12.9 / 23.6 | 34.4 / 15.7 | 30 |
| tik_01 | 50.2 / 45.4 | 50.1 / 22.5 / 16.3 | 43.1 / 22.2 | 20 |
| hei_01 | 33.4 / 30.9 | 38.7 / 13.4 / 17.8 (**slower**) | 30.2 / 12.9 | 27 |
| jin_00 | 43.2 / 33.2 | **35.1** / 16.6 / 10.4 | 30.8 / 6.4 | 17 |
| effects battle | 49.9 / 35.8 | **42.6** / 13.4 / 24.2 | 35.8 / 14.3 | 28 |
| eki_00 | 33.4 / 28.7 | **29.5** / 10.6 / 12.5 | 25.6 / 9.8 | 23 |
| usu_00 | 66.9 / 58.3 | **61.3** / 28.9 / 27.1 | 55.4 / 18.9 | 21 |
| pik_00 | 100.3 / 85.8 | **90.3** / 40.8 / 40.4 | 81.6 / 27.5 | 23 |

- Core 0 now does a third to a half of what it did (the game's logic, its draw code and the record). Core 2 is the limit everywhere: the translator and the replay together exceed the frame (106-130% of wall time, the two counted separately), so the frame-end fence waits for the frame before in a third to a half of the frames.
- Seven of ten scenes are faster (up to 17%), gor_01 and tik_01 unchanged, **hei_01 is slower** (33.4 -> 38.7 ms): its translation (30 ms) plus the replay (13 ms) no longer fits the frame on core 2. So the default stays `gxthread=0` (step 5's rule).
- Extra latency: one frame between a frame's logic and its display when pipelined (the GameCube's own CPU/GPU overlap).
- What would flip it: less translation on core 2. The translator takes 0.9-1.0 times what the game thread spent on GX before; the replay of the frame before shares core 2. Next candidates: the display-list hit path and the immediate-mode ring (the largest translator stages), and moving the replay's CPU work (citro3d command building) down.

**Core 2 is the wall: round 1 of cutting it (2026-11-23).** A PC-sample profile of gor_01 pipelined (`gxthread=2 gxthreadsplit=0 gxpipeline=1`, 264 stops): the translator thread is 90% busy: `gx_dlcache_call` 13.6% self, `memcpy` 9.8%, waiting for the replay 9.5%, `gx_vsh_frame` 8.0%, `draw_from_reader` 6.1%, `consume` 4.9%, `hash_span` (texture checks) 4.5%, `gx_vsh_route` 3.8%, `plat_gpu_vsh_draw` 3.4%, `rec_for` 2.3%; the render thread 22% busy (`GPUCMD_Add`, `C3D_UpdateUniforms`, GSP requests). Built, each pixel-identical under `determ` (gor_01 both modes, usu_00):

- cache hits skip the vertex-state compare when nothing it reads changed (`gx_vtx_version`), and remember their pass-group record (no hash probe);
- the replay sets a shader uniform row only when its bits change (`vunif`), so citro3d uploads changed rows only;
- (e) stream flushes move to the render thread (`gxflushrender=1`), off the recording thread's submit;
- (f) states that differ in few words are recorded as deltas (`gxstatedelta=1`): gor_01's record 486 -> 178 KB a frame, usu_00 497 -> 253 KB of the 512 KB (no segments; the stereo split keeps a copy of the state and the light block for the second eye).

Same-build A/B of (e)+(f) on and off (pipelined; translator / render ms): gor_01 73.7 / 21.3 against 69.5 / 29.6, hei_01 32.0 / 20.7 against 30.7 / 23.4, pik_00 89.4 / 16.0 against 82.9 / 18.9. The replay gets cheaper, the translator (which waits for the replay at submit, counted in its wall time) does not, and core 2's total moves by -4 to +4 ms, within run-to-run noise. **Frames are unchanged**; hei_01 pipelined is still slower than `gxthread=0` (41.5 against 36.8 ms on this merge). The ten-scene table after the port3ds merge of 2026-11-23 (60 fps step 1, tooling at nice 10) shows the same picture (core 2 47-115 ms; gor_01 93.5, pik_00 115.4).

Not done, and why:
- (b) immediate ring and (d) the battle's array snapshots: the snapshot is the only safe copy (the overwrite comes before any report), and pipelining makes every CPU-skinned array need it (the next frame re-skins before the translator reads); the copy itself is a `memcpy` of the index span.
- `gx_vsh_frame` (8%): the per-frame streak pass over every touched list record; a lazy commit would halve it, at the cost of more state per record.
- The texture checks (`hash_span`) are `gx_tex.c`, now another agent's.

Core 1 keeps the light, latency-tolerant work and **all SD card access** (disc reads, saves, log, options, test output), so cores 0 and 2 never block on SD I/O.

## Splits, ranked

### A. Move GX translation from core 0 to core 2 (the big one)

Star3D's main step, and the right shape for us:
- **The game thread only records the GX calls:**
  - setters and matrices by value;
  - display lists by pointer, size and array pointers;
  - immediate-mode vertex bytes copied.
- **The render thread translates and draws:**
  - display-list cache lookup and decode;
  - vertex work (most of it moves to the vertex shader under the GPU-offload plan anyway);
  - texture checks and conversion;
  - TEV compile;
  - citro3d submission.
- **Rules this needs:**
  - **Late reads:** the render thread reads game memory up to a frame late. Data used by frame N must stay valid until frame N is drawn.
    - Check how TTYD frees model and map data (any delayed-free mechanism like Star3D's 2-tick `mmFree` delay).
    - Add a **render fence**: at the first GX call of frame N+1, wait until the render thread has finished frame N, unless the audit shows that isn't needed.
  - **Calls that return values** (`GXGetScissor`, `GXPeekARGB`, `GXReadDrawSync`, bounding box and so on) are answered on core 0 from a shadow of the recorded state. `GXPeekARGB`/z become a frame late; list where TTYD uses them and check each.
  - **Display lists built by the game at runtime** (`GXBeginDisplayList`/`GXEndDisplayList`) are still recorded on core 0, since that only writes game memory.
  - **Verify mode:** record both ways and compare (Star3D's `S3D_RT_DELTA=2` idea), behind a switch.
  - **Switchable:** `gxthread=0` translates on core 0 (today's behaviour), `=1` records and then replays inline on core 0 (this proves the recording without threads), `=2` replays on core 2.
- **Order:** this comes **after** the display-list cache and the vertex shader transform. Those cut the translation work itself, and they need a design that doesn't care which thread owns the cache. The GX agent has been told.

#### A, built (`gxthread`, 2026-11-22)

Code: `port3ds/game/gx/gx_rec.c` (the record, the translator thread, the fences, the split, verify), hooks in `gx_state.c`, `gx_pipe.c`, `gx_misc.c`; a report hook in `plat/memstable.c`. All numbers are Azahar (New 3DS, no frame limit), so they give shares and directions, not hardware times.

**Measured first (`gxthread=0`, before any change).** Two ways, because `gxstats`' own per-call timers (two or three `svcGetSystemTick` per display list, about 1.3 µs a pair in Azahar) inflate the GX stages: gor_01 runs at 117 ms per frame with `gxstats=60` and at 83.6 ms without it.

| | gor_01 | gon_02 (battle) | aaa_00 |
|---|---|---|---|
| `gxstats=60`: game busy (frame minus the retrace wait) | 109.6 ms | 52.0 ms | 52.6 ms |
| of which GX lists + immediate mode (inclusive stages) | 80.4 ms (**73%**) | 40.1 ms (**77%**) | 39.6 ms (**75%**) |
| of which everything else (game logic, its draw code, GX state calls, the frame's submit) | 29.1 ms | 11.8 ms | 12.9 ms |
| `gxthreadstats=60` (one tick read per frame): frame / game busy | 83.6 / 73.5 ms | 48.7 / 33.9 ms | 50.2 / 34.5 ms |
| render thread (core 2), last frame | 26.1 ms | 7.0 ms | 23.7 ms |

GX translation is three quarters of the game thread in all three scenes, well above the 25% threshold, so A was built. What the measurements also showed, and the results below confirm: the game's own work outside GX is small (with `gxthread=1` the game thread's busy time is 24.1 ms in gor_01 and 9-10 ms in the battle and aaa_00, recording included), so moving translation can at most overlap that much.

**The record** (a ring of 32-bit words in the regular heap, 4 MB, `gxthreadkb`):

| Command | Words | Source |
|---|---|---|
| a state call | header (id, count) + its arguments by value | every setter in `gx_state.c` and `gx_misc.c`: the same ids and words as the display-list port opcodes (`REC_*`, now in `gx_internal.h`), plus ids only the record uses (`GXSetZScaleOffset`, scissor box offset, indirect order/scale/matrix, dst alpha, pixel format, Z texture, the copy setters, `GXCopyTex`, `GXCopyDisp`, `GXDrawDone`, `GXSetDrawSync`, `GXClearBoundingBox`). Matrix loads are one command each (14 words); a texture object (`GXLoadTexObj`) and a light object are copied by value |
| `GXCallDisplayList` | list address, size, caller (+ a hash of the bytes in verify mode) | the bytes and arrays are read at translation |
| `GXBegin`/`GXEnd` | primitive and format, vertex count, caller, then each pipe write's low 32 bits | copied: the game reuses its write-gather pipe at once |
| raw pipe commands (`GXCmd` outside a primitive) | the writes | copied |

The translator replays a call through the same public GX function with `gx_tls_translator` set, which makes every function act on `gx` (`gx_apply_call`); display-list port opcodes now go through the same `gx_apply_call`. Per frame: 24.8 K commands and 494 KB in gor_01, 11.3 K and 226 KB in gon_02, 10.5 K and 198 KB in aaa_00.

**Calls that return values, answered on core 0:**

| Call | TTYD's uses | Answer |
|---|---|---|
| `GXGetProjectionv`, `GXGetViewportv` | `nameent.c`, `seq_title.c`, `seq_demo.c`, `seq_load.c`, `seq_e3.c`, `seq_logo.c`, `win_log.c`, effects (`eff_splash.c`, n64 effects) | the shadow (`gx_game`), kept from the recorded calls (`gx_shadow_call`) |
| `GXGetScissor` | `winmgr.c` (4), `eff_naniga.c`, `win_log.c`, `seq_logo.c` | shadow |
| `GXGetCullMode`, `GXGetVtxDesc(v)`, `GXGetVtxAttrFmtv` | none in the game (SDK only) | shadow |
| `GXSetDispCopyYScale` (returns lines) | `DEMOInit.c` | shadow copy source |
| display lists the game builds (`GXBeginDisplayList`: `mapdrv.c` at map load, `extdrv.c`) | encoded on the game thread from the shadow's vertex description and formats, as before | shadow |
| `GXReadDrawSync` | `sysWaitDrawSync` (`smartAlloc` after a free, `smartGarbage`, `shadowdrv.c`, `fadedrv.c`, `win_log.c`, `sac_*.c`, `seq_e3.c`, ...) and `__NoHangDoneRender` once per frame | a **fence**, then the shadow token: the token is reached once everything before it is translated |
| `GXDrawDone` | `DEMODoneRender` (not used: TTYD runs the hang workaround), `DEMOInit` | recorded (the translator waits for the GPU) and a fence |
| `GXReadBoundingBox` | `mobjdrv.c`, `shadowdrv.c` (offscreen), `sac_scissor.c`, `win_log.c`, `seq_e3.c`, always after a `sysWaitDrawSync` | a fence, then `gx.bbox` (the translator's) |
| `GXPeekARGB` | `eff_splash.c`, `eff_funemizu.c`, after a `sysWaitDrawSync` | a fence, then read directly (the translator is idle) |
| `GXGetTexObj*`, `GXGetTexBufferSize`, `GXProject`, `GXGetYScaleFactor`, FIFO queries | many | no GX state: unchanged |

None of them becomes a frame late: every read-back that needs translated results waits for them, which is what the GameCube game does too (it calls `sysWaitDrawSync` first).

**The late-read audit.** What the translator reads after the game thread has moved on, and why it is still valid:

| Memory | Read at translation | How TTYD frees or rewrites it | Safe because |
|---|---|---|---|
| map display lists and their arrays (`mapdrv.c`, from the map file) | list bytes, array elements | freed at a map change (`_mapFree`, `_mapAlloc`), never rewritten while drawn | the allocators report `plat_memstable_release`; a release reported by the game thread while translation is pending is a **fence** (`gxthreadrel=1`) |
| effect lists in REL `.data` (`eff_*.c`) | list bytes | the REL is unlinked (`OSUnlink` reports a release) | the release fence |
| lists the game builds (`mapdrv.c` at load, `extdrv.c`) | list bytes | rebuilt into the same buffer | `GXBeginDisplayList` reports a release first: fence |
| arrays of `GXBegin` primitives (`animdrv.c`: characters are skinned on the CPU into `wp->vtxArrayPos`/`Nrm`, a scratch shared by every model when `useFloatScratch` is set, flushed, drawn at once with indexed `GXBegin` primitives, then overwritten by the next model) | array elements | rewritten for the next model within the same frame | **snapshot**: at `GXEnd` the game thread walks the pipe writes with the shadow's vertex state, finds each indexed attribute's index range and copies that span into the record (56 KB per frame in gor_01); the translator draws from the copy when its vertex state (hash) and array base and stride match the shadow's, else reads live and counts it. Found by the image comparison under `determ`: without it Goombella's pose differed from `gxthread=0` |
| CPU-written arrays of display lists (`shadowdrv.c` sets arrays just before its static lists; other CPU-built data) | array elements | written by the CPU, flushed with `DCFlushRange` (a `write` report, 225 per frame in gor_01) | not fenced and not copied (the list's index range is only known once it is decoded): the GameCube's GPU reads them as late, so the game keeps them until it waits for the GPU. Verify mode checks every list's array spans against later write reports |
| textures and TLUTs (smart heap, map and REL data) | image and TLUT bytes (hashed and converted in `build_state`) | `smartFree` only marks the heap (`waitsync++`); the next `smartAlloc` calls `sysWaitDrawSync` first, and `smartGarbage` (which moves blocks) calls it before compacting: TTYD's own delayed free | `sysWaitDrawSync` is a fence (`GXReadDrawSync`) |
| EFB copy targets (`GXCopyTex` destinations) | the copy-target handle by address | in order with the draws | recorded in order |
| `GXBegin` vertices, matrices, texture and light objects, the pipe | — | — | copied into the record |
| disc reads into memory (DVD and ARQ threads, `fill` reports) | — | come after an allocation (a release, so a fence) in TTYD | not fenced themselves (the report comes after the bytes land); verify mode counts any read after a later fill |

So the memstable registry plus the fences make display lists, textures and TLUTs safe without a snapshot; `GXBegin` primitives get one (their pipe writes and the array spans they index). The verify results below are the check.

**The fences** (`gx_rec_fence`: the game thread waits until everything recorded so far is translated): `GXReadDrawSync` (once per frame at the frame's end, plus each `sysWaitDrawSync`), `GXDrawDone`, `GXReadBoundingBox`, `GXPeekARGB`, `GXInit`, a release report from the game thread while translation is pending, and a full ring. The per-frame one sits where the GameCube game waits for the GPU token (`__NoHangDoneRender`, after `marioStMain`), so translation overlaps the game's draw code and its logic. In every measured scene the only fence is the frame's one (1.00 per frame; 0 release, readback and full fences).

**Thread ownership.** Everything that translates (the display-list cache, memstable trust requests, the shader-path pass groups and the stream ring, the texture cache, the back end's record) runs on exactly one thread at a time: the translator, or the game thread at a fence or in the split's direct part, with the hand-over through the ring's sequentially consistent indices. The cache's inputs come in through `GxState`; `gx_dlc_capturing`, the frame number and the pass-group state are only touched while translating. `gx_prim_open` and display-list recording are the game thread's (`gx_prim_is_open`, `gx_dl_recording` are false while translating), and the write-gather pipe is never read by the translator.

**Verify mode (`gxthread=3`).** Records and translates like `gxthread=1`, plus:
- every getter fences and compares the shadow with the translated state (vertex description and formats, projection, viewport, scissor, cull mode, copy source);
- every list's bytes are hashed when recorded and again when translated ("changed between its call and its translation");
- every memory report the game thread makes while translation is pending is logged with its ring position, and the translator checks what each call reads (the list, the cache entry's array spans, the bound textures and TLUTs) against reports made after the call was recorded.
`gxthreadrel=0` turns the release fence off, so verify mode shows what it prevents.

**The split (`gxthread=2`).** The first build recorded everything and translated it on core 2. That moved the game thread from 73.5 to 28.3 ms busy in gor_01, but the frame stayed at 83.6 ms: core 2 then carried the translation (about 67 ms, about 14 ms more than on core 0: recording and replaying the calls costs about 0.4 µs each way per call in Azahar, 25 K calls per frame) plus the replay of the frame before (18-31 ms). Moving the replay to core 0 (`gpucore=0`) was worse (87.8 ms). So the game thread now translates the start of each frame itself, while core 2 replays the frame before, and records the rest from a split point, counted in display-list calls; the split moves by hill climbing on the frame's work time (start to the frame's fence), 8 frames per step (`gxthreadsplit=0` records everything, `gxthreadsplitat=N` fixes it). In gor_01, fixed splits gave a work time of 81.8 ms (0 lists direct), 68.9 (2000), 69.0 (3000), 69.3 (4000), 69.5 (5000) and 74.7 (5440), against 73.5 with `gxthread=0`: the translation is one serial chain, and only the game's own work (about 10-15 ms here) and the replay can overlap it.

**Frame times** (`gxthreadstats=60`, the window ending at frame 1261, adaptive split; `gxthread=2` with the array snapshots, `gxthread=1` before them, which only adds the copies):

| | gor_01 | gon_02 (battle) | aaa_00 |
|---|---|---|---|
| `gxthread=0`: frame / game busy | 83.6 / 73.5 ms | 48.7 / 33.9 ms | 50.2 / 34.5 ms |
| `gxthread=1`: frame / busy / inline translation at the fence | 100.3 / 24.1 / 67.9 ms | 50.1 / 9.1 / 33.3 ms | 50.2 / 9.9 / 32.3 ms |
| `gxthread=2`: frame / busy / fence wait | 83.6 / 43.9 / 26.1 ms | 49.9 / 25.5 / 12.8 ms | **45.4** / 17.1 / 16.1 ms |
| `gxthread=2`: lists translated directly (split) | 1959 of 5427 | 1698 of 2698 | 692 of 2814 |
| `gxthread=2`: translator busy / render (core 2) | 47.6 / 30.2 ms (93%) | 16.6 / 23.6 ms (80%) | 24.7 / 8.6 ms (73%) |
| record per frame (`gxthread=1`) | 24.8 K commands, 494 KB | 11.3 K commands, 226 KB | 10.5 K commands, 198 KB |
| before the snapshots, `gxthread=2`: frame / busy / fence | 83.6 / 36.8 / 32.6 | 49.9 / 13.8 / 20.9 | 44.0 / 18.7 / 14.5 |

The game thread's busy time drops by 40-70%, but the frame does not follow: gor_01 is unchanged (both 5 vblanks), the battle's frame work (busy + fence) is 38.2 against 33.9 ms (the same 3 vblanks), aaa_00's 33.2 against 34.5 ms and its frame 45.4 against 50.2 ms. Core 2 is nearly full in gor_01. (Core-2 utilisation adds the translator's and the render thread's busy times, both wall-clock while running, so preemption between them counts twice.)

**Verification** (all in Azahar, build of this branch):

| Test | Result |
|---|---|
| Images, `vsh_accept.py` method, 24 consecutive shots from frame 1140, `gxthread=0` vs 1 vs 2 (gor_01, gon_02, aaa_00) | static pixels: depth identical everywhere, colour within 2 (gor_01) and 1 (gon_02), identical in aaa_00 (every pixel of every shot); no seam, no flicker at static pixels. The game is not deterministic between runs, so the whole-frame differences were on animated content, as between two `gxthread=0` runs |
| The same under `determ` (one retrace per frame, so frame N is the same game state in every run) | two `gxthread=0` runs: every pixel of 24 shots identical (the noise floor is zero). `gxthread=1` and `2` first differed in gor_01 at Goombella (animdrv's shared skinning scratch read late, see the audit); with the array snapshots **every pixel of every shot is identical to `gxthread=0`**: gor_01 (modes 1 and 2), gon_02 (2), aaa_00 (2) |
| Game state: `determ gwdump=10` and the 1600-frame padscript of PORT_3DS_TEST_INFRA.md (gor_01, walking, A and B), `gw_diff.py --logs` | `gxthread=1` and `2` against `0`: **160 records equal**, frames 10-1600, all 23 fields including the raw `GlobalWork` hash; again after the snapshots |
| `gxthread=3 gxcache=2` over `map_sweep.py --maps gor_01,tik_01,gor_01,hei_01,gon_02 --story 30 --frames 300` | 4 of 4 maps ok (gor_01, tik_01, hei_01, gon_02 with its battle; the REL relinks `tik`, `hei`, `gon`): every cache hit decoded again and compared, **0 mismatched, 0 stale**, 6 trust revocations; verify counters **0** lists changed between call and translation, **0** getter mismatches, **0** reads after a later fill, write or release report. Release fences 0.05-0.08 per frame at the map changes. In verify mode every getter is a fence (66-194 per frame here, `winmgr.c`'s scissor reads), which keeps its late-read window short; the `determ` image runs above cover the full window of `gxthread=1` and `2` |
| `gxthread=2 gxcache=2`, the same sweep | 4 of 4 maps ok, **0 mismatched, 0 stale**, same revocations: the cache, trust and pass groups owned by the translator thread and the split's direct part. In the battle the array snapshots reach 1.4 MB per frame (every character is skinned on the CPU), which filled the 1 MB ring (a full fence a frame); the ring's default is now 4 MB |

**Default: `gxthread=0`.** `gxthread=2` is identical, but not faster everywhere tested (the battle's frame work is slower, gor_01 unchanged), so it stays opt-in. What would change that:
- the per-call cost of the record (a third of the translator's extra time): record state as deltas at draws, as Star3D does, instead of call by call;
- the array snapshots of `GXBegin` primitives: 56 KB a frame in gor_01 but 1.4 MB in a battle. Copying only what the game rewrites within the frame needs a signal before the rewrite (the flush report comes after it), so it stays a copy of every indexed span;
- hardware numbers: the replay's cost and the cores' relative speed are Azahar's here; the split adapts to whatever they are on the console, and `gxthreadstats` measures it there;
- less translation work in total (the shader path already removed the CPU transform; the display-list hit path and immediate mode are what is left).

### B. Fork-join jobs on core 2 (smaller, lower risk, can come first)

Self-contained loops handed to core 2, with the game thread joining before it uses the result:
- **Texture conversion at load** (big-endian GX formats to PICA tiled formats). Also at map load, which shortens load stalls.
- **Map and asset conversion at load** (our byte-swap passes in `port3ds/game/formats/`).
- **Display-list decode on a cache miss,** if A isn't done yet.
- **TTYD's animation work** (`animdrv`: pose evaluation and matrix building per model), *only if* the profile shows it's large. Each model is independent, so jobs per model; it's game code, so the hook goes in a `TARGET_PORT` wrapper.

**Work stealing is a must:** if a job hasn't started by the time the game thread joins, the game thread runs it itself, so a busy core 2 never stalls the frame. A small job queue in the platform layer (`plat_job_submit`, `plat_job_join`) carries it, and is generic for `ctr-devkit`.

### C. Core 1: keep it light, and shrink what's there

- **Audio:** today it's a software MusyX mix on core 1 (16% of busy time in Azahar). Star3D plays **one MusyX voice per NDSP channel** (21 voices), with no CPU mixing, and only the ADSR envelope and volumes updated each NDSP frame. That removes most of the audio CPU.
  - The catch: loops must start on 14-sample DSP-ADPCM frames, which Star3D handles with a build-time loop re-alignment, and software reverb is lost.
  - It's a separate audio plan, worth it once core 1 matters.
- **DVD/io, arq, card, alarm, vblank, watchdog:** waiting most of the time, and they stay. Move the log writer off core 2 only if core 2 gets busy after A.
- **The core-1 limit itself:** measure 0 / 30 / 80% on hardware (the `cpulimit` switch). If 30% or less costs nothing, use that, and move any remaining core-1 work to core 2.

### D. Parallel game logic: no

Like Star Fox Adventures, TTYD's logic isn't thread-safe: event scripts, NPC and Mario updates, the heap and the global work all share state. Out of scope. The exceptions are pure per-object computation through B's jobs, with work stealing.

### E. Waits that aren't work (found by Star3D, worth checking in TTYD)

Star3D found its game thread **spinning** for about half a frame in a pad-polling loop while it waited on an SD read. Check whether TTYD has similar busy-waits:
- on DVD reads (`DVDRead` sync loops, `dvdmgr.c`);
- on ARQ completions;
- on `GXDrawDone` / `GXReadDrawSync` (our `plat_gpu_sync`).

Where it spins, keep the file in RAM or yield instead. The PC sampler shows this as time in the wait loop.

## Portability

The job queue (B), the record/replay split (A) and the core-1 policy (C) are library mechanisms in the portable performance layer (GC3DS_LIBRARY_NOTE.md). What's TTYD-specific: which game calls return values and how the game frees memory (the late-read audit), the animation job hook, and the MusyX voice data.

## Steps

1. **Measure on hardware:** the per-thread profile and per-core busy time in three scenes, plus the core-1 limit comparison. **After `ctr-devkit` is in.**
2. **The job queue with work stealing,** then texture and asset conversion at load as the first jobs. Azahar shows whether load stalls shrink; hardware for timings.
3. **The display-list cache and the vertex shader transform** (the GX agent, and the GPU-offload plan) *before* A.
4. **A in stages:** the `gxthread=1` recording proof, then `=2`, then the fence and late-read audit, then verify mode on. **Built** 2026-11-22 ("A, built" above); default still `gxthread=0`, since in Azahar it is not faster in every scene. Next: hardware numbers with `gxthreadstats`, and a cheaper record (state deltas).
5. **Per-voice NDSP audio,** if core 1 turns out to matter.
