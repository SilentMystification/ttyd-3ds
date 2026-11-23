# Plan: 60 fps on the New 3DS (definition of done)

Owner decision 2026-11-22: the port is done when the game runs at **60 fps on the New 3DS**, the GameCube's own rate, in the field, in battles and in effect-heavy scenes.

## Budget per frame (16.7 ms)

| Where | Work | Must fit in |
|---|---|---|
| Core 0 | Game logic only (target architecture, `PORT_3DS_MULTICORE.md`), plus recording GX calls | 16.7 ms, with margin for spikes |
| Core 2 | GX translation (what is left after the shader path) and replaying the record into citro3d | 16.7 ms, pipelined one frame behind core 0 |
| Core 1 | Audio (mostly on the DSP via NDSP), disc and SD I/O, card | under the 80% limit, never on the critical path |
| PICA200 | Vertex shader, rasterisation, the stereo warp when the slider is up | 16.7 ms |

A frame that misses on any one of these drops to 30 fps on the retrace wait. Frame pacing (lane C 9) decides how a miss is shown.

## What we know

- **No hardware frame times yet.** Every number so far is Azahar's, which runs the ARM11 on a JIT and fakes GPU timing: good for ratios between two builds, useless for absolute milliseconds.
- **Azahar ratios** (`gor_01`): frame 83.6 ms, game thread busy 73.5 ms, of which about two thirds is GX translation; the shader path took the CPU path from 168 to 117 ms per frame (with `gxstats` on) and 83.6 ms without.

## Step 1: sanity checks (Azahar plus the build; no hardware needed)

1. **Build flags:** `-O2 -g` today, no LTO, no per-file `-O3`. Check the toolchain's `-march=armv6k -mtune=mpcore -mfloat-abi=hard`, try LTO and `-O3` on the GX hot files, A/B for identical output.
2. **Debug overhead left on in normal runs:** logging volume per frame, assertions, verify modes, census and stats code paths, memstable checks, `PORT_IS_ADDR` calls on hot paths, the per-call `gx_rec` checks when `gxthread=0`.
3. **Pure game-logic cost:** a `gxnull` switch that skips GX translation and rendering, so core 0's time is game logic alone. This is the floor; if it is near 16.7 ms on hardware, game code itself needs work.
4. **Per-frame work counters** (hardware-independent): draw calls, vertices, state changes, texture uploads and hashes, display-list cache hit rate, CPU-path draws by reason, record size, immediate-mode bytes. Per scene: `gor_01`, Rogueport square, a battle with effects, a cutscene, the pause menu.
5. **Hot functions:** `pc_sample.py` code samples per scene, ranked, game thread and render thread separately.
6. **Retrace and pacing:** does any frame wait for two retraces when it needn't (double waits, VBlank waits on both threads, present waits)? Is the game itself frame-limited at 30 in any scene?

## Step 1 results (2026-11-23, Azahar)

**Every millisecond below is Azahar's** (New 3DS mode, no frame limit, JIT ARM11, faked GPU timing): use them as ratios between builds and between scenes, never as console times. Frames are quantised to whole vblanks (16.7 ms), so a build that saves a few ms shows in `busy`, not always in `frame`.

Method: `gxthreadstats=60` (one tick read per frame), the 7 windows from frame 1141 to 1501 averaged. Scenes: `gor_01` and `gor_00` at story 30, a battle (`gon_02` story 30, `btlauto=perfect`: attacks and effects), `aaa_00` (the opening cutscene), the pause menu (`gor_01`, `padscript=wait*1000,start*3`). Identical output: `determ` with 6 consecutive shots from frame 1200, compared byte for byte (the noise floor under `determ` is zero). Each row is one `run_azahar.py --build <cia> --log 3ds/ttyd/log.txt --switches "<scene> gxthreadstats=60" --until "gxthread \d frame 1501:"` run (one instance at a time); the samples are `run_azahar.py --gdb --gdb-port <own port>` plus `pc_sample.py --port <same> --start-after "gxthread \d frame 1141" --duration 150 --interval 250`. Use a port of your own: with the default 24689, another agent's `pc_sample.py` can attach to your Azahar.

### 1. Build flags

The effective flags (devkitPro's `3DS.cmake` → `Platform/Nintendo3DS.cmake`, `CMAKE_BUILD_TYPE` Release, whose `CMAKE_C_FLAGS_RELEASE` is devkitPro's `-g -O2 -DNDEBUG`; the targets' own `-O2` comes after it):

| Code | Flags |
|---|---|
| all | `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections -D__3DS__ -g -O2 -DNDEBUG` (ARM mode; VFPv2) |
| game (`src/`, `rels/`, MusyX) | + `-std=gnu99 -O2 -g -fno-short-enums -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -ffp-contract=off -fgnu89-inline -fcommon -fdata-sections` |
| port, game side (`port3ds/game/`) | + `-std=gnu11 -O2 -g -fno-short-enums -fno-strict-aliasing -fwrapv -ffp-contract=off -fdata-sections` |
| port, plat side (`port3ds/plat/`) | + `-std=gnu11 -O2 -g -fno-strict-aliasing -fdata-sections` (short enums) |
| link | `-specs=3dsx.specs -Wl,--gc-sections`, the `--wrap`s; libctru, citro3d prebuilt |

`-mtp=soft` makes every `__thread` access a call to `__aeabi_read_tp`; the hot GX paths read `gx_tls_translator` only after `gx_rec_mode != 0`, so `gxthread=0` never pays it.

New CMake options (both off by default): `TTYD_LTO` (LTO over the DOL code, MusyX and both port halves; the static RELs stay plain objects because `rel_link.py` links them with `ld -r`; `sac_suki.c` is excluded, it declares the function `tou_gamen_screen_tev` as an array and LTO refuses to merge that) and `TTYD_GX_O3` (`-O3` on `port3ds/game/gx/*.c`).

| Build | image end (limit 0x01000000) | gor_01 frame / busy | battle frame / busy | `determ` shots vs -O2 |
|---|---|---|---|---|
| -O2 (today) | 0x00ae24a8 | 83.6 / **70.8** | 49.9 / **36.0** | reference |
| LTO | 0x00b19bfc (+226 KB) | 80.1 / **67.6** (-4.5%) | 47.9 / **34.4** (-4%) | identical, 6/6 and 6/6 |
| -O3 GX | 0x00af04a8 (+56 KB) | 80.9 / **68.0** (-4%) | 45.4 / **33.6** (-7%) | identical, 6/6 and 6/6 |
| LTO + -O3 GX | 0x00b66c7c (+530 KB) | **67.7** / **65.1** (-8%) | **40.9** / **32.7** (-9%) | identical, 6/6 and 6/6 |

Noise: two -O2 builds (before and after the counters below) gave gor_01 70.8 / 70.8 and battle 36.0 / 35.8. The combined build drops gor_01 from 5 to 4 vblanks.

LTO also found type mismatches between translation units that the per-file build cannot see (`-Wlto-type-mismatch`, 14). One is a real ABI bug on the 3DS today, independent of LTO: `partyNrmToAngle` is defined as `f32 partyNrmToAngle(Vec n)` (`party_hit.c`) but `party.c` and `party_move.c` declare and call it as `partyNrmToAngle(Vec* n)`. MWCC passes a struct by reference, so the GameCube is fine; with the ARM hard-float ABI the callee reads the vector from `s0`-`s2` while the caller passes a pointer in `r0`. The others: `marioJumpData` (array vs struct), `cloudGetStatus`/`bomheiGetStatus` (`u8` vs `s32`), `evt_cam_road_reset`, `effCoinFukidashiEntry`, `shadowFlagOn`, `shadowSetCamId`, `iconGetWidthHight`, `iconGetTexObj`, `effSparkEntry`, three `marioWeapon_*` (all in the LTO build log). Also `-Waggressive-loop-optimizations` in `sac_zubastar.c` (a loop to 17 over a 16-entry array, the game's) and the generated `mtx_sdk.c`. **Before LTO becomes the default, fix the declarations** (header prototypes), since LTO may optimise across them.

**Fixed (2026-11-23).** The GameCube build stays 100% (G8ME01: all categories, MusyX 31/31), so every fix that would change MWCC's code is under `TARGET_PORT` and the GameCube keeps its FAKE:

| Mismatch | Fix |
|---|---|
| `partyNrmToAngle` | `party.h` prototype `f32 partyNrmToAngle(Vec* n)` and a pointer definition in `party_hit.c`, port only (a pointer definition changes the GameCube code: `party_hit` fell to 97.4%); the GameCube keeps the by-value definition and the callers' local pointer externs |
| `cloudGetStatus`, `bomheiGetStatus` | `evt_party.c` includes the real headers (`u8`) under `TARGET_PORT`; the GameCube keeps the `s32` externs |
| `evt_cam_road_reset` | `seq_mapchange.c` includes `evt/evt_cam.h` instead of its `void (s32, s32)` prototype; same GameCube code |
| `effCoinFukidashiEntry` | `evt_mobj.c`: real prototype in the port, a macro keeps the position-first call order (the GameCube evaluates it in that order) |
| `effSparkEntry` | `battle_status_effect.c`: the port calls the 4-argument prototype (the two extra arguments were never read) |
| `shadowSetCamId`, `shadowFlagOn` | prototypes in `shadowdrv.h` under `TARGET_PORT` (`s16`/`u16`); `party.c` keeps its `s32`/`u32` ones on the GameCube |
| `iconGetTexObj`, `iconGetWidthHight` | `fontmgr.c`, `win_main.c`, `eff_updown.c` include `icondrv.h`; `eff_naniga.c` and `msgdrv.c` call directly (all port only) |
| `marioJumpData` | `koopa.c` declares `MarioJumpData` and reads `.jump` (port only) |
| `marioDefaultWeapon_Jump`, `marioWeapon_KururinJump`, `marioDefaultWeapon_Hammer` (the three `marioWeapon_*`) | `battle_seq.c` declares them as `BattleWeapon` (port only) |
| `sac_zubastar.c` loop to 17 | the 17th "line" is the next member of the work, `capture` (a pointer), whose first word is read as `state`; a pointer is never 5, so that pass does nothing. The port stops at 16 |
| `mtx_sdk.c` warning | it is `sac_deka.c` `yuka_main`: `prev[r][j - 1]` and `prev[r][j + 1]` run one element past each row, into the neighbouring row, `vel[1][128]` or `u[0][0..2]`, all inside the same `YukaMesh` with the same layout in the port. The port reads them through the mesh's base (`DEKA_PREV`) so the compiler sees them in bounds |

**`partyNrmToAngle` in game.** It is the slope test of `partyGetAppearPos3` (party_move.c; the demo's copy is in party.c): where the partner is put back next to Mario (Goombella, Ms. Mowz, `party_motion.c`, `party_kuribo.c`, `party_chuchu.c`). It tries nine spots 30 units from Mario and skips floors steeper than 60 degrees. With the ARM ABI bug the angle was computed from whatever was in `s0`-`s2` (the normal was never passed), so the test accepted or rejected spots at random: a partner could be placed on a cliff face, or fail all nine spots. Not measured in a run (no cheap trigger found; the owner limited emulator runs to short ones); read from the code and the ABI.

**Result, LTO + `-O3` GX, with the fixes.** No `-Wlto-type-mismatch` and no `-Waggressive-loop-optimizations` in the US, JP and PAL builds. Image ends: US 0x00b7dabc, JP 0x00b67ff8, PAL 0x00c7267c (limit 0x01000000). `determ` shots 1200 to 1205 compared with the `-O2` build of the same source: identical, 6/6 in `gor_01` and 6/6 in the battle (`gon_02 story=30 battle=1 btlauto=perfect`). Short map sweep (story 30, 120 frames): `gor_01`, `gon_02` (a battle on entry), `usu_00`, `pik_00` all ok. Both options are now on by default in `port3ds/CMakeLists.txt` (`release.py` uses the same build).

### 2. Debug overhead in a normal run

Checked by reading every per-frame and per-call path and by the PC samples (5., no switches but `gxthreadstats`): **no diagnostic code appears among the game thread's or render thread's sampled functions** (top 30 of each, 236 of 280 game-thread samples in gor_01; the remainder spread at ≤ 2 samples each).

| What | Where | Cost when off |
|---|---|---|
| Log lines | `plat_log` (async ring, writer on core 1, `fsync` at most once a second) | 25 lines per 420 frames in gor_01 and the battle, nearly all `gxthreadstats`; none without switches |
| `gxstats` counters and timers | `gx_perf_on > 0` around every counter in `gx_pipe.c`, `gx_draw.c`, `gx_dlcache.c` | one load and branch per list call / chunk |
| `texstats`, `texcensus` | `s_stats_every != 0` | one add per lookup |
| `gxdump` | `s_dump` (`gx_draw_dumping`) | a flag per draw |
| `gx_rec` checks with `gxthread=0` | `GX_CLOSE_PRIMITIVE`, `gx_dl_recording()` (an out-of-line call), `gx_rec_on()` (one load) in every setter; `gx_rec_getter` only in mode 3 | about 25 K setter calls a frame in gor_01: 3 loads and one call each (LTO inlines the call) |
| verify paths (`gxcache=2`, `gxthread=3`, `jobcheck`) | mode checks | a load |
| memstable reports | `DCFlushRange` → `plat_memstable_write` (202 a frame in gor_01) | functional (display-list trust), not a diagnostic |
| `PORT_IS_ADDR` | inline range test (`port_addr_impl.h`) | 2-4 compares; required logic |
| HUD | `plat_hud_frame`, game thread: two `snprintf` and two console lines every 500 ms | 3 of 384 battle samples in `consoleDrawChar`; negligible on the console |
| `frame_hooks` (`vi.c`) | evtdump, gwdump, fx, battle, font tests | each a cached switch and a compare |
| switch lookups | `plat_switch_has` cached; `plat_switch_value`/`_int` scan the switch text, but only from init paths | — |

Nothing there is worth compiling out: the sampled bound is below 1-2% of the game thread. No change made. What was misleading instead was a **measurement**: the render thread's `render_us` included `C3D_FrameBegin`'s vblank wait (6.), so "render 28 ms" in gor_01 is 14 ms of work and 14 ms of waiting. `gxthreadstats` now prints both.

### 3. `gxnull=1`: the game-logic floor

`gxnull=1` records GX calls as `gxthread=1` does and drops the record at each fence (`gx_rec.c`, two small hunks), so the game thread's busy time is the game's own work plus recording; nothing is drawn.

| Scene | normal: frame / busy | `gxnull`: frame / busy | GX translation (busy difference) | recorded per frame |
|---|---|---|---|---|
| gor_01 | 83.6 / 70.8 | 33.4 / **26.2** | 44.6 ms (63%) | 23.8 K commands, 5257 lists |
| gor_00 | 50.2 / 37.5 | 20.9 / **16.1** | 21.4 ms (57%) | 12.4 K, 2168 lists |
| battle (`btlauto`) | 50.1 / 35.8 | 16.7 / **10.9** | 24.9 ms (70%) | 11.3 K, 2698 lists |
| aaa_00 (cutscene) | 50.2 / 34.5 | 16.7 / **11.1** | 23.4 ms (68%) | 10.5 K, 2814 lists |
| pause menu | 33.4 / 18.5 | 16.7 / **9.6** | 8.9 ms (48%) | 4.6 K, 0 lists (immediate mode only) |

The recording itself costs about 0.4 µs a call in Azahar (PORT_3DS_MULTICORE.md), 5-10 ms of the gor_01 floor. gor_01's floor is 1.6 frames in Azahar's time; the hardware ratio decides whether that is a problem (Step 2's calibration).

### 4. Per-frame work counters

From `gxstats=60` and `texstats=60` runs (their timers inflate the times, not the counts):

| | gor_01 | gor_00 | battle | aaa_00 | pause |
|---|---|---|---|---|---|
| display-list calls (new / repeat / again in frame) | 5252 (135 / 4899 / 218) | 2178 (5 / 2054 / 119) | 2698 (29 / 2625 / 44) | 2814 (30 / 2764 / 20) | 0 |
| list bytes, list vertices | 355 KB, 27871 | 158 KB, 12143 | 187 KB, 12663 | 201 KB, 13599 | 0 |
| immediate primitives, vertices | 371, 1407 | 264, 1012 | 204, 792 | 160, 583 | 334, 1388 |
| state builds (`build_state`) | 547 | 314 | 265 | 272 | 286 |
| PICA states recorded | 404 | 217 | 179 | 237 | 143 |
| dl-cache hits (rate); state-only lists not cached | 5007 (95%); 245 | 2039 (93%); 139 | 2513 (93%); 133 | 2624 (93%); 190 | — |
| trusted hits (no hash) | 95% | 93% | 99% | 99% | — |
| cache memory | 8191 of 8192 KB, 10709 entries | 8191 KB | 8191 KB | 4733 KB | — |
| shader-path draw calls → GPU draw commands | 5378 → 536 | 2306 → 298 | 2787 → 267 | 2784 → 267 | 334 → 282 |
| uniform blocks sent | 462 | 265 | 235 | 210 | 262 |
| CPU-path draws (by reason) | **0** | 0 | 0 | 0 | 0 |
| render record | 409 KB (of 512) | 223 KB | 188 KB | 226 KB | 168 KB |
| records per frame (segments, mid-frame syncs) | 1 (0, 0) | 1 | 1 | 1 | 1 |
| textures hashed per frame (bytes) | **181 (666 KB)** | 95 (366 KB) | 82 (312 KB) | 62 (232 KB) | 76 (146 KB) |
| texture lookups, mismatches, uploads per frame | 451, 0, 0 | 259, 0, 0 | 212, 0, 0.18 | 241, 0, 0 | 277, 0, 0 |
| live textures | 193, 3.5 MB | 106, 2.3 MB | 110, 2.0 MB | 68, 1.7 MB | 85, 1.7 MB |

Every draw takes the shader path in all five scenes. The two things that stand out: every texture is hashed in full on its first use in each frame (up to 666 KB a frame) although none changed, and gor_01's render record is at 80% of its 512 KB.

### 5. Hot functions (`pc_sample.py`, 250-500 ms interval, 150 s from frame 1141)

280 stops in gor_01 and 384 in the battle (Azahar's stub is slow), so a function's share is ±2-3 points: a direction, not a measurement. Game thread (tid 1), share of its samples; `svcArbitrateAddressNoTimeout` is waiting (the retrace, the hand-off between OS threads, the render thread in `submit`).

| # | gor_01 | % | battle | % |
|---|---|---|---|---|
| 1 | (waiting: svcArbitrateAddressNoTimeout) | 15.4 | (waiting) | 12.5 |
| 2 | `gx_dlcache_call` | 10.4 | `svcSendSyncRequest` ← `GSPGPU_FlushDataCache` | 8.1 |
| 3 | `hitCheckVecFilter` (game) | 6.8 | `draw_from_reader` (immediate-mode decode) | 8.1 |
| 4 | `hash_span` (texture hash) | 6.1 | `gx_dlcache_call` | 6.5 |
| 5 | `svcSendSyncRequest` ← `GSPGPU_FlushDataCache` | 5.4 | `hash_span` | 4.7 |
| 6 | `gx_vsh_route` | 4.3 | `plan.isra.0` | 2.9 |
| 7 | `memcpy` (← `plat_gpu_vsh_draw`) | 4.3 | `C_MTXConcat` (game: `mapDisp`, `pushGXModelMtx`) | 2.9 |
| 8 | `mapDisp` (game) | 3.2 | `memcpy` | 2.9 |
| 9 | `strcmp` ← `mapGetMapObj` (game) | 2.5 | `gx_dl_replay_port_op` | 2.1 |
| 10 | `rec_for` | 2.5 | `gx_vsh_route` | 2.1 |
| 11 | `memset` | 2.1 | `memcpy_as4` ← `animPoseMain` (game) | 2.1 |
| 12 | `draw_from_reader` | 2.1 | `rec_for` | 1.8 |
| 13 | `mapGetMapObj` (game) | 1.8 | `gx_tex_handle_for` | 1.8 |
| 14 | `gx_vsh_frame` | 1.4 | `gx_vsh_frame` | 1.6 |
| 15 | `gx_dl_replay_port_op` | 1.4 | `plat_gpu_vsh_draw` | 1.3 |
| 16 | `vsh_state` | 1.4 | `gx_vsh_imm_build` | 1.3 |
| 17 | `gx_tex_handle_for` | 1.4 | `ldexpf` ← `animPoseMain` (game) | 1.3 |
| 18 | `plat_gpu_vsh_draw` | 1.4 | `legalize` (TEV) | 1.3 |
| 19 | `mapReCalcMatrix` (game) | 1.1 | `memcmp` (← `plat_gpu_set_state`) | 1.3 |
| 20 | `lru_front` (dl cache) | 1.1 | `animPoseMain` (game) | 1.0 |
| 21 | `ensure_state` | 1.1 | `gx_draw_vertices` | 1.0 |
| 22 | `run_commands` | 1.1 | `run_pending_raw` | 0.8 |
| 23 | `chkfilterVec` (game) | 1.1 | `consoleDrawChar` (HUD) | 0.8 |
| 24 | `GXSetChanCtrl` | 0.7 | `memset` | 0.8 |
| 25 | `__aeabi_uldivmod` ← `mapGetMapObj` (game) | 0.7 | `finitef` ← `ldexpf` (game) | 0.8 |

Render thread (tid 9), busy 36% (gor_01) and 21% (battle) of the stops: waiting 64% / 79% (`LightEvent_Wait` for a record, `gspWaitForAnyEvent` in `C3D_FrameBegin`), then `svcSendSyncRequest` ← `gspSubmitGxCommand` 16% / 8%, `GPUCMD_Add` 7% / 5%, `C3D_UpdateUniforms` 5% / 3%, `vsh_apply_const` 2%, `render_record` 1%. The audio mixer thread (`salPcProcessPb`) is 4-5% of its stops on core 1; the log writer's `FSFILE_Flush` shows on core 1 only.

### 6. Retrace and pacing

- **The game thread waits for one retrace per frame**: `DEMODoneRender` → `__NoHangDoneRender` → `VIWaitForRetrace` once (`src/main.c`'s loop). The only multiple waits in game code are the reset path (`L_gcResetCheck`) and the logo sequence. **No scene caps itself at 30**: the game advances by frame; `retraceDeltaTime` only feeds the clocks, which the port caps at 2 retraces a frame (`GAME_TIME_BUDGET_MS`).
- **The submit wait is small**: `GXCopyDisp` → `plat_gpu_frame_end` waits for the render thread's previous record, 0.09 ms a frame in every scene (`gxstats`' `present`).
- **The render thread waits for a vblank per record**: `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` waits for the next vblank of both screens (citro3d's `C3D_FrameSync`). With one record per frame that is no double wait (the game thread is paced by its own retrace, and the throughput limit stays max(game, render) < 16.7 ms), but it starts every frame's rendering at the vblank after the submit: one frame of latency, and 13-16 ms of every frame's `render_us` in Azahar was this wait. Every extra record in a frame (a segment when the 512 KB record fills, or a mid-frame `plat_gpu_sync`: `GXPeekARGB` in `eff_splash`/`eff_funemizu`, the vertex-shader stream reclaim, devupdate) waited for **one more vblank**, and the game thread waits for it at the next submit: a lost frame each. **Fixed**: only a frame's first record takes `C3D_FRAME_SYNCDRAW` (`plat/gpu.c`); shots identical under `determ`, gor_01 unchanged (70.8 ms busy), the splash test (10 `GXPeekARGB` syncs) runs. It matters more as gor_01's record nears 512 KB.
- The retrace reaches the game through the vblank thread on core 1 (`gspWaitForVBlank`, priority 0x20) and `OSWakeupThread`; with the 80% core-1 limit its latency is a hardware question for Step 2.

### Ranked optimisation targets

| # | Target | Evidence | Expected gain (game thread unless noted) | Risk |
|---|---|---|---|---|
| 1 | **Pipelined frame end + cheaper recording** (Step 3 lever 2) | GX is 48-70% of game-thread busy (3.) | the 21-45 ms (Azahar) of translation leaves core 0; floor 10-26 ms | high; running |
| 2 | **Texture change detection by memstable trust** instead of hashing every texture's bytes on its first use each frame (`gx_tex_handle_for` → `hash_span`) | 146-666 KB hashed a frame, 0 mismatches (4.); `hash_span` 5-6% of game samples (5.) | ~5% (≈3.5 ms gor_01 in Azahar); scales with the scene's textures | low-medium: CPU-written textures and TLUTs must report writes (they do through `DCFlushRange`); `texstats` counts mismatches for the check |
| 3 | **LTO + -O3 on the GX files** | -8 to -9% busy, identical shots (1.) | 8-9% everything, both threads | medium: fix the 14 cross-file declaration mismatches first; +530 KB image (4.8 MB of room left) |
| 4 | **Display-list hit path**: `gx_dlcache_call` + `gx_vsh_route` + `rec_for` + `lru_front` + `vsh_state` | 18-20% of game samples in gor_01 for 5252 calls, 95% of them trusted hits | 5-10%; per-call costs only (skip routing while the matrix version is unchanged, cheaper LRU) | medium (another agent's files) |
| 5 | **Per-submit `GSPGPU_FlushDataCache`** on the game thread (`stream_flush_dirty`) | 5-8% of game samples | up to 5% (Azahar's syscall cost; measure on the console) | low: flush on the render thread before replay, or one range |
| 6 | **Immediate-mode decode** (`draw_from_reader`, `gx_vsh_imm_build`): CPU-skinned characters | 8% + 1.3% of battle samples | 5-8% in battles | medium |
| 7 | **Game-side hot spots, port-side replacements only** | `hitCheckVecFilter` 7% (gor_01), `mapGetMapObj` (`strcmp` + `__aeabi_uldivmod`) 5%, `ldexpf`/`finitef` from `animPoseMain` 2% (newlib's), `C_MTXConcat` 3% | a fast exact `ldexpf`, VFP `PSMTXConcat`: 2-5% | low for the SDK functions; game logic untouched |
| 8 | **Render thread**: `gspSubmitGxCommand`, `GPUCMD_Add`, `C3D_UpdateUniforms` | 14 ms of work in gor_01 (Azahar), 536 draw commands | render thread only; the first thing to measure on hardware | low-medium |
| 9 | **Render record headroom**: gor_01 uses 409 of 512 KB | (4.) | avoids segments (each now costs a submit, no longer a vblank) | low |
| 10 | **Debug overhead** | (2.) | < 1-2%: nothing to do | — |

Not a target: the game's own frame pacing (6.).

## Step 2: hardware baseline (owner session, through `ctr-devkit`)

Rosalina profiles plus `gxthreadstats`, `gxstats`, `stereostats` and the counters above, in the same scenes, for `gxthread=0` and `gxthread=2`. Plus a CPU and memory microbenchmark run on both Azahar and the console, to calibrate Azahar's ratios. This is the only way to know how far we are from 16.7 ms.

## Step 3: the levers, in expected order of payoff

1. **Shader path completion** (running): lighting, the matrix palette, texgen, bounding box from positions. Removes per-vertex CPU work.
2. **Pipelined frame end + delta recording** (queued): core 0 game logic only, core 2 one frame behind.
3. **Texture work:** hashing and conversion moved to load time on the job queue, and cheaper change detection (memstable trust instead of rehashing).
4. **Display-list cache hit rate:** every miss costs a full decode.
5. **Compiler:** LTO, `-O3` on hot files, hot loops in ARM assembly only where the profile insists.
6. **QTM** for the hottest data (lane D 2), decided by the probe's bandwidth numbers.
7. **Game-side hot spots** only if the `gxnull` floor is too high: port-side replacements for SDK functions (math, memcpy, PSMTX in VFP), never changing game logic.

## Done when

`fps` HUD at 60 in every scene of the map sweep, the story checkpoints and the battle gallery on the console, with stereo off; and a measured cost for stereo on.

## Future levers kept open (owner, 2026-11-23)

**Bounding box (`GXReadBoundingBox`).** The exact positions-only path is the default. Alternatives stay available behind `gxbbox=` and are measured with `gxbbox_verify=1`:
- `cache`: exact box reused while the display list, matrix, projection, viewport and arrays are unchanged. Hits only with a still camera and static objects.
- `hull`: exact box from model-space convex-hull vertices (closed meshes, in front of the near plane); otherwise exact.
- `corners`: conservative box from the model AABB. Not for play: a larger box can capture neighbouring objects into offscreen effects and raise smart-heap use.
A fully-GPU box is not possible on the PICA: it has no bounding-box output or occlusion query, and the game reads the box directly after the draw, before the GPU runs it. QTM does not change this (it changes where data lives, not when it is ready).

