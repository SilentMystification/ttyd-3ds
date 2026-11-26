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

Method: `gxthreadstats=60` (one tick read per frame), the 7 windows from frame 1141 to 1501 averaged. Scenes: `gor_01` and `gor_00` at story 30, a battle (`gon_02` story 30, `btlauto=perfect`: attacks and effects), `aaa_00` (the opening cutscene), the pause menu (`gor_01`, `padscript=wait*1000,start*3`). Identical output: `determ` with 6 consecutive shots from frame 1200, compared byte for byte (the noise floor under `determ` is zero). Each row is one `run_azahar.py --build <cia> --log 3ds/ttyd/latest.log --switches "<scene> gxthreadstats=60" --until "gxthread \d frame 1501:"` run (one instance at a time); the samples are `run_azahar.py --gdb --gdb-port <own port>` plus `pc_sample.py --port <same> --start-after "gxthread \d frame 1141" --duration 150 --interval 250`. Use a port of your own: with the default 24689, another agent's `pc_sample.py` can attach to your Azahar.

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

Target 2 is done (`textrust`, PORT_3DS_GX_PERF.md "Texture trust"): hashes per frame 62-189 -> 0 in four steady scenes, game-thread busy -4 to -6% in Azahar, shots identical, `texverify` 0 stale.

Not a target: the game's own frame pacing (6.).

### Ranked levers after Core-2 round 2 (2026-11-24)

Owner goal: GX translation should disappear, not move to core 2 (plan and results: PORT_3DS_GX_PERF.md, "Core-2 round 2"). Target 4 above is built as `gxbatch` (core-0 busy -6% to -30% in the ten scenes, byte-identical shots) and the TEV part of the state work as `gxstatecache` (-5% to -8% more). Azahar ms, gor_01, `gxthread=0`: busy 67.8 -> 48.0 -> 44.5, against the `gxnull` floor of 26.2.

| # | Lever | Evidence | Expected | State |
|---|---|---|---|---|
| 1 | **State rebuilds by class** (dirty bits set inline by the setters that change a value; textures, pixel pipeline, scissor rebuilt only when dirty; `plat_gpu_set_state` told what changed) | about 545 real rebuilds a frame in gor_01 remain; the state path was about 29% of the game thread before, TEV compile (7%) is now gone | most of the 18 ms between 44.5 and the floor in gor_01 | not built. The table-based early return (`gxstatededup`) is a measured loss (+14%) |
| 2 | **Game logic floor** (`gxnull`: 26 ms in gor_01, 10-16 ms elsewhere, Azahar) | Step 1 results 3. | port-side SDK replacements (lever 7 of Step 3) | needs the hardware ratio first |
| 3 | **Render thread writes PICA registers itself** (stage 3) | 15 ms of work in gor_01, 535 draws; `C3D_UpdateUniforms` scans 96 rows per draw | about half of the render work; matters on hardware (36-66 us per citro3d draw) | not built |
| 4 | **Immediate mode in PICA form** (stage 4) | 369 primitives of 3.8 vertices in gor_01: the cost is per primitive (state, route), so lever 1 comes first | 2-5% | not built |
| 5 | **Pipelined translation** (`gxthread=2 gxpipeline=1`) | its reason was the per-list work, now mostly gone | re-measure once lever 1 is in; delete `gx_rec.c` if it no longer wins anywhere | open |

### Future lever, measured: the bounding box (`gxbbox`, 2026-11-23)

TTYD reads `GXReadBoundingBox` in four places: NPCs drawn into an offscreen buffer (`npcdrv.c`), the paper pose (`animdrv.c`), the scissor action command (`sac_scissor.c`) and the unused E3 sequence. Every read follows its own `GXClearBoundingBox`, and the port disarms the box at the read. While the box is armed, a draw on the shader path gets its box from `xf_position` on the CPU (`gxvshbbox=1`). A CPU-path draw gets the box from the transform it does anyway.

`gxbbox=exact|cache|hull|corners` (default `exact`) applies to cached lists drawn by the shader while the box is armed:
- `cache`: keeps the list's box with its cache entry, together with the position matrix, projection, viewport and EFB size it was made under.
- `hull`: keeps the list's vertices pruned once to a superset of their model-space convex hull. A vertex strictly inside a tetrahedron of the 26-direction extreme vertices is pruned (Akl-Toussaint). The box is exact while every kept vertex is in front of the eye; otherwise the path falls back to exact.
- `corners`: uses the 8 model-space AABB corners. This box can only be larger, so the mode is for measurement only.

`gxbbox_verify=1` makes the exact box beside the chosen one and compares them at each read. `gxthreadstats=N` prints three `gxthread   bbox` lines:
- per frame: reads, armed draws and vertices on each path, and the box's time;
- totals for each path, plus the hull builds;
- the verify result.

Short runs (`fastboot`, warp at frame 595, about 300 frames after it; the battle to frame 1141):

| Scene | Reads per frame | Armed draws per frame, shader path | Armed draws per frame, CPU path | Box time |
|---|---|---|---|---|
| gor_01, story 30 | 0 | 0 | 0 | 0 |
| gor_00 (crowd), story 30 | 0 | 0 | 0 | 0 |
| eki_00, story 30 | 0.76 | 0 | 5.4 (21 vertices) | 0 us (the CPU transform makes it) |
| las_00 | 0 | 0 | 0 | 0 |
| battle (gon_02, `btlauto=perfect`) | 0 | 0 | 0 | 0 |
| pause menu (gor_01) | 0 | 0 | 0 | 0 |

In eki_00 under each of `cache`, `hull` and `corners`, `gxbbox_verify=1` showed 46 reads and 0 mismatches, and no cached list took any of the three paths. Cache hit rate, hull vertex ratio and corner growth therefore have no sample in these scenes.

**Verdict: the box is not a lever.** The boxed draws are a few skinned NPC parts per frame, and they are on the CPU path. The default stays `exact`. The 967 armed draws per 60 frames reported for eki_00 in round 3 came from `gxvshbbox=2`: its reference box stays armed after the read, so that mode counted every later draw. With the box disarmed at the read, the real count is about 5 draws per frame. Revisit only if a scene shows shader-path armed draws on the first `gxthread   bbox` line; the switches are there for that.

## Step 2: hardware baseline (owner session, through `ctr-devkit`)

Rosalina profiles plus `gxthreadstats`, `gxstats`, `stereostats` and the counters above, in the same scenes, for `gxthread=0` and `gxthread=2`. Plus a CPU and memory microbenchmark run on both Azahar and the console, to calibrate Azahar's ratios. This is the only way to know how far we are from 16.7 ms.

Two tools make one console session enough. Both run inside the development CIA; neither starts the game. Neither is in a release build.

### 2a. `calib`: CPU and memory microbenchmarks (`port3ds/plat/calib_mode.c`)

Switch `calib` (dev builds only) runs 46 benchmarks in the game binary instead of the game, as `gpuprobe` does, and keeps the devupdate server answering. Each row is the best of 3 timed runs of about 120 ms (`calibms=N` changes that), timed with `svcGetSystemTick`. It writes `sdmc:/3ds/ttyd/calib.txt` and logs `CALIB DONE`. About 20 s in Azahar.

Rows: integer ALU, multiply, 32 and 64-bit divide; float dependent and independent multiply-add, divide, sqrt; newlib `sinf`, `cosf`, `atan2f`; the game's own `sincosf`, the port's `PSMTXConcat`, `PSMTXMultVec`, `PSMTXInverse`, `PSMTXRotRad`, `PSVECNormalize` (and a local copy of the concat and of the normalize, to show call cost); `memcpy` and `memset` of 4 KB, 64 KB, 1 MB in the heap, the linear heap and QTM RAM (QTM has room for 4 KB and 64 KB only, the log queue and the 2 MB limit take the rest); sequential 32-bit reads over 16 KB (L1), 256 KB and 1 MB (L2), 16 MB (streaming); pointer chasing over the same sizes (one hop per 32-byte line, random cycle); a pointer chase with a data-dependent branch; a data-dependent branch over random against sorted bytes.

`calibpmu` adds a second pass with the ARM11 counters through `ctrdk_perf` (cycles, data-cache read misses, instruction-cache misses per operation, `calib_pmu.txt`). It needs the SVC or backdoor the ExHeader allows; when the way in is missing the run logs `CALIB pmu: not available` and goes on (Azahar: not available). It is a separate switch because the first call into the kernel may fail on a console.

Azahar, build #580, `calib` in the dev CIA (QTM mapped). Azahar's CPU is a JIT with no cache or memory model: every size of the read and chase rows is flat, the sorted and random branch rows are equal, and heap, linear and QTM copy at the same speed. Where the console differs, the ratio column shows how much of Azahar's time in that kind of work to believe. The "New 3DS" columns are for the owner session; the ratio is console ns / Azahar ns (above 1: the console is slower).

| group | benchmark | Azahar ns/op | Azahar MB/s | New 3DS ns/op | New 3DS MB/s | ratio (console / Azahar) |
|---|---|---|---|---|---|---|
| cpu int | `int_alu` | 22.46 |  | | | |
| cpu int | `int_mul` | 18.72 |  | | | |
| cpu int | `int_div32` | 405.40 |  | | | |
| cpu int | `int_div64` | 2,240 |  | | | |
| cpu float | `flt_madd_dep` | 22.46 |  | | | |
| cpu float | `flt_madd_x4` | 14.04 |  | | | |
| cpu float | `flt_div` | 71.13 |  | | | |
| cpu float | `flt_sqrt` | 86.10 |  | | | |
| libm | `sinf` | 257.47 |  | | | |
| libm | `cosf` | 239.12 |  | | | |
| libm | `atan2f` | 671.62 |  | | | |
| sdk | `game_sincosf` | 1,149 |  | | | |
| sdk | `mtxconcat_local` | 374.37 |  | | | |
| sdk | `PSMTXConcat` | 700.06 |  | | | |
| sdk | `PSMTXMultVec` | 142.26 |  | | | |
| sdk | `PSMTXInverse` | 572.77 |  | | | |
| sdk | `PSMTXRotRad` | 638.84 |  | | | |
| sdk | `vecnorm_local` | 168.46 |  | | | |
| sdk | `PSVECNormalize` | 228.36 |  | | | |
| memory copy | `memcpy_heap_4K` | 12,601 | 309.99 | | | |
| memory copy | `memset_heap_4K` | 6,828 | 572.06 | | | |
| memory copy | `memcpy_heap_64K` | 199,483 | 313.31 | | | |
| memory copy | `memset_heap_64K` | 107,457 | 581.63 | | | |
| memory copy | `memcpy_heap_1M` | 3,189,781 | 313.50 | | | |
| memory copy | `memset_heap_1M` | 1,717,590 | 582.21 | | | |
| memory copy | `memcpy_linear_4K` | 12,601 | 309.99 | | | |
| memory copy | `memset_linear_4K` | 6,828 | 572.06 | | | |
| memory copy | `memcpy_linear_64K` | 199,483 | 313.31 | | | |
| memory copy | `memset_linear_64K` | 107,456 | 581.63 | | | |
| memory copy | `memcpy_linear_1M` | 3,189,781 | 313.50 | | | |
| memory copy | `memset_linear_1M` | 1,717,590 | 582.21 | | | |
| memory copy | `memcpy_qtm_4K` | 12,601 | 309.99 | | | |
| memory copy | `memset_qtm_4K` | 6,828 | 572.06 | | | |
| memory copy | `memcpy_qtm_64K` | 199,483 | 313.31 | | | |
| memory copy | `memset_qtm_64K` | 107,457 | 581.63 | | | |
| memory read (op = 32-bit word) | `read_seq_16K` | 8.90 | 428.52 | | | |
| memory read (op = 32-bit word) | `read_seq_256K` | 8.89 | 429.01 | | | |
| memory read (op = 32-bit word) | `read_seq_1M` | 8.89 | 429.02 | | | |
| memory read (op = 32-bit word) | `read_seq_16M` | 8.89 | 429.17 | | | |
| pointer chase (op = hop) | `chase_16K` | 14.97 |  | | | |
| pointer chase (op = hop) | `chase_256K` | 14.97 |  | | | |
| pointer chase (op = hop) | `chase_1M` | 14.97 |  | | | |
| pointer chase (op = hop) | `chase_16M` | 14.97 |  | | | |
| pointer chase (op = hop) | `chase_branchy_256K` | 33.69 |  | | | |
| branch (op = element) | `branch_random_16K` | 18.72 |  | | | |
| branch (op = element) | `branch_sorted_16K` | 18.72 |  | | | |

### 2b. `tools/port3ds/hw_baseline.py`: the ten scenes, three configurations

For gor_01, gon_02, aaa_00, tik_01, hei_01, jin_00, the effects battle, eki_00, usu_00, pik_00 (the scene switches of `gx_perf_ab.py`) and for today's defaults, `gxdirty=1` and `gxdirect=1`: 30 runs. Each run is `devupdate.py restart` with the scene switches, `gxthreadstats=60`, `fastboot`, `livelog` and a run id; then `run_hw.py --no-send` follows the live log until the third counted window has ended. The counted windows are the first three that end at game frame 781 or later (781, 841, 901; the warp is at about frame 595). The table has frame ms, core-0 busy ms and render ms (render minus its vblank wait; the same fields as `gx_perf_ab.py`) as the mean of the windows, and the range of the frame time. `--dry-run` prints the commands and touches nothing. Output: `build/hw-baseline/baseline.md`, `results.json` and one log per run. About 1.5-2 minutes a run on the console (boot, warp, three windows).

The same tool ran against Azahar (`azahar-session --supervise`, `--ip 127.0.0.1 --port 5060`) to test the parsing; see the commit message.

### 2c. The console session: exact commands (planner, with the owner's go)

The console is at `192.168.8.221`; the ARP entry takes about 20 s to warm up, so retry a refused first call. Nothing here is run without the owner's go.

```bash
export N3DS_IP=192.168.8.221
# 0. The dev build with the CIA (build.sh --dev, no --no-cia), pushed with the calibration switch.
tools/port3ds/build.sh --dev
python3 libs/ctr-devkit/devupdate/devupdate.py push build/3ds/game-dev/ttyd.cia --switches "calib devupdate" --wait 120
# 1. calib runs by itself (about 20-40 s). Wait for the line, then fetch the table.
python3 libs/ctr-devkit/devupdate/devupdate.py log | grep "CALIB DONE"      # repeat until it shows
mkdir -p build/hw-calib
python3 libs/ctr-devkit/devupdate/devupdate.py files get @/calib.txt -o build/hw-calib/calib.txt
# 1b. Optional, counters: a second run of the same set with them.
python3 libs/ctr-devkit/devupdate/devupdate.py restart "calib calibpmu"
python3 libs/ctr-devkit/devupdate/devupdate.py log | grep "CALIB DONE"
python3 libs/ctr-devkit/devupdate/devupdate.py files get @/calib_pmu.txt -o build/hw-calib/calib_pmu.txt
# 2. The ten scenes, three configurations (30 runs, about 45-60 minutes); restart leaves calib mode.
tools/port3ds/hw_baseline.py
# or a part first:  tools/port3ds/hw_baseline.py --scenes gor_01,hei_01 --configs default
# 3. Back to the normal game.
python3 libs/ctr-devkit/devupdate/devupdate.py restart ""
```

Afterwards: copy the `calib.txt` ns/op values into the "New 3DS" columns above and fill the ratios; copy `build/hw-baseline/baseline.md` into PORT_3DS_GX_PERF.md next to the Azahar numbers.

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


## Game logic floor: profile and SDK-level changes (2026-11-26, Azahar)

Method: release build `-O2 + LTO + GX -O3` of port3ds `e3bac958`, `gxnull=1 gxthreadstats=60`, `pc_sample.py --interval 200 --duration 240` on the game thread from game frame 1141 (60 s for the first gor_01 try: 81 stops; 221 stops pik_00, 300 usu_00, 371 battle). Azahar's stub gives 1-1.5 stops per second, so a share is +-5 points. The profile lists the samples as the thread's samples; about 20% of them are the idle wait (`svcArbitrateAddressNoTimeout`). Local needs: `iso/game.iso` linked into `build/azahar-profile/.../ttyd/`, and `--allow-hw-faults` (every run in this checkout logs 23 `UnmappedAccess 0xA5A5A5A5` lines at boot; `gx_perf_ab.py` and `determ_ab.py` do not pass the flag, so local copies with it were used and not committed).

**Finding: with `gxnull=1` the floor is mostly not game logic.** The GX front end (`port3ds/game/gx/`: `gx_rec_prim`, `gx_rec_list`, `dd_slot`, `GXSet*`, `GXCallDisplayList`, `run_pending_raw`) still runs under `gxnull` and is 25-40% of the busy game-thread samples (pik_00 about 40%, battle 33%, usu_00 31%). It belongs to the GX lanes, not to this task. SDK/port-level and game functions share the rest.

Game thread, self share of all samples (idle wait about 17-24% each), top entries:

| Function | gor_01 | pik_00 | usu_00 | battle | Kind |
|---|---|---|---|---|---|
| `gx_rec_prim` | 1 | 14.9 | 8.3 | 7.8 | GX front end (not touched) |
| `gx_rec_list` | 2.5 | 8.1 | 4.3 | 6.7 | GX front end |
| `dd_slot` + `GXSet*` + `GXCallDisplayList` | about 14 | about 12 | about 15 | about 13 | GX front end |
| `strcmp` (from `mapGetMapObj`, `mapSetMaterial`, `lightNameToPtr`) | 8.6 (+`mapGetMapObj` 2.5) | 0 | 1.0 | 0 | libc |
| `C_MTXConcat` | 1.2 | 1.8 | 5.3 | 3.0 | matrix math |
| `memcpy` | 0 | 5.0 | 0 | 1.1 | libc (from `gx_rec_prim`) |
| `hitCheckVecFilter` + `checkTriVec_xz` | 1.2 | 3.6 | 3.0 | 0 | collision |
| `animPoseMain` | 1.2 | 0 | 1.3 | 4.6 | animation |
| `mapDisp`, `renderProc`, `mapSetMaterialLight` | 5 | 5 | 6 | 3 | map driver |
| `GXLoadPosMtxImm` | 0 | 0.9 | 0 | 2.4 | port |
| `__udivmoddi4`, `finitef`/`ldexpf` | 0 | 0 | 0 | 1.6 | libgcc/libm |
| printing (`consoleDrawChar`, `__ssprint_r`, `_svfprintf_r`) | 14 | 1.8 | 0 | 0 | gor_01 only, first 60 s sample; not seen in the 240 s runs, not investigated |

Not seen above 1%: `PSVECNormalize`, `PSMTXInverse`, `PSMTXRotRad`, `sincosf` (double `sin`/`cos`; a faster exact version is not possible without the same double result, so it was not touched), 64-bit division (`__udivmoddi4` 0.8-1% in the battle only).

Changes (release build; they cannot be switched at runtime, so the A/B compares two release builds, `build/base` = `e3bac958` and `build/new`):

1. `strcmp` in `port3ds/game/msl.c`: byte loop with early exit, same return value (difference of the first differing unsigned bytes). newlib's ARM strcmp aligns first; the game's name lookups differ in the first bytes.
2. `C_MTXConcat` (generated by `gen_mtx.py`): the SDK's twelve expressions unchanged (same order, `-ffp-contract=off`), all computed into locals before any store, so the aliased case needs no temporary matrix and copy-back call.
3. `TTYD_HOT_O3` (default ON): `-O3` for `hitdrv.c`, `animdrv.c`, `mapdrv.c`, `mario_hit.c` (flags only).

Identity (base vs new, Azahar): `gw_diff.py --logs` over a 1600-frame padscript in gor_01 (`wait*700,cr*120,cl*120,a*10,wait*300,cu*120,cd*120,a*10,wait*200`, gwdump every 10): 160 of 160 records equal, all 23 fields. `determ` shots 780-785, gor_01 and battle: 12 of 12 byte-identical.

Azahar ms per frame, windows 722-841, best of 2 (busy = core-0 busy):

| Scene | gxnull busy before / after | default (`gxthread=0`) busy before / after | frame before / after |
|---|---|---|---|
| gor_01 | 24.1 / 23.4 (-0.7) | 45.7 / 44.9 (-0.8) | 50.5 / 50.5 |
| pik_00 | 32.7 / 32.3 (-0.4) | 54.6 / 54.1 (-0.5) | 66.9 / 66.9 |
| usu_00 | 23.4 / 23.0 (-0.4) | 39.3 / 38.8 (-0.5) | 50.2 / 50.2 |
| battle | 18.0 / 17.7 (-0.3) | 27.3 / 27.1 (-0.2) | 38.9 / 39.0 |

The gxnull floor frame time is 33.4 / 34.0 / 33.5 / 27.2 ms (two vblanks, no change). Gain is 1-3% of busy: within what two runs of one build differ by, so the three changes are not separated from each other and each is small. The profile was not repeated on the new build.

What stays hot and was not touched: the GX front end under `gxnull` (other lanes; the largest block), `gx_rec_prim`'s `memcpy`, collision (`hitCheckVecFilter`), animation (`animPoseMain`) and the map driver: game logic, only flags allowed; `-O3` on them gives no measurable gain. The floor on hardware is still unknown (Step 2). The game-logic share below the GX front end is about 25% of the game thread, so no SDK-level replacement can bring `gxnull` busy near 16.7 ms in gor_01 or pik_00 by itself; the remaining cost is in the GX recording path.

## Texture work off the game thread (`texasync`, `texframe`; Step 3 lever 3, 2026-11-26)

Azahar, ms are Azahar's (ratios only). Counters: switch `texframe=1` logs one `texframe:` line per frame (lookups, uploads, hashes and bytes, async hits, game-thread microseconds for the lookups, the conversion with decode and PICA create, and the hashes). `texstats=60` adds a `texasync:` line (queued, taken, stale, waits, rejected by reason).

**Measured before** (`texasync=0`, release build, `gxstats=1 texframe=1`, burst = first frame with more than 20 uploads, 120 frames from there):

| Scene | uploads in 120 frames | texture lookups+conversion, game thread | worst frame | steady state | display lists built in the 120 frames |
|---|---|---|---|---|---|
| gor_01 (warp at 595) | 212 | 781 ms | 255.6 ms (53 uploads) | 0 uploads, 0 hashes, lookups 562 x 3.9 us = 2.2 ms | 67129 (25595 in the first 10 frames, 0 after frame 30) |
| pik_00 | n/a | 591 ms | 167 ms | | 21586 |
| battle (gon_02, btlauto) | | 611 ms | 134 ms | | 15580 (4451 in the first 10 frames; +2511 later) |

Where it goes (gor_01 burst frame, 53 uploads, 302 ms): about two thirds is conversion (pick format and Morton tiling), a third decode, about 5% the PICA create (alloc, copy, flush); hashing was 2.7 ms of the first 120 frames' 781 (trust already covers it: 0-1 hashes a frame in steady state). So hashing is not the cost; the first-use conversion is. Steady state has nothing to move: no uploads, no hashes (the pause menu's CPU-written textures aside).

**What moved.** `portTplPrefetch` (src/sdk/texPalette.c) runs when a TPL is unpacked (`UnpackTexPalette`, and filemgr's type 4 load) and calls `GXPrefetchTexPort` (gx_tex.c) for each texture. Plain textures (not CI, not mip mode) of at most 640x512 texels (1024x512 padded) in covered memory (memstable) are queued: `plat_memstable_trust` vouches the image span at queue time (generation G, span marked relied). One runner job on the core-2 worker (`plat/job.c`) claims queue entries one by one and does decode, format pick, Morton tiling and the hash into private buffers (2 x 1.3 MB scratch, results malloc'd, at most 6 MB held, oldest finished result evicted when full, unused ones dropped after 60 frames). At the first bind (`gx_tex_handle_for` miss path) the result is used only if `plat_memstable_gen() == G` (no relied range written, released or refilled since the queue: the bytes the worker read are the bytes now), else dropped and converted as before. A queued entry the worker has not started is decoded by the bind on the game thread (same code); one being decoded is waited for. The picture comes from the same functions and the same `plat_gpu_tex_create`, so it is the synchronous picture. `texverify=1` also re-hashes at the bind and counts a mismatch (`verify-bad`, 0 in every run below). `textrust=0` or `texfilter=mip` turns the prefetch off. Not covered: CI textures (the TLUT is bound later), textures from non-TPL sources (fonts, generated images, copies), textures over 640x512 (608x448 and 608x480 backgrounds convert at the bind).

**After** (release build, 2 repeats per arm, alternating order; first-120 tex columns = game-thread texture time):

| Scene | tex ms in 120 frames, off -> on | worst texture frame | game busy, first 120 mean (gxstats timers on) | steady busy (90 frames from +150) |
|---|---|---|---|---|
| gor_01 | 781 -> 401-414 | 255.7 -> 98.4-98.6 ms | 165.2 -> 162.4 ms (-1.7%) | 72.4-72.6 -> 72.4-72.6 (unchanged) |
| pik_00 | 592 -> 375-378 | 167 -> 77.4 ms | 118.9-119.0 -> 117.3 (-1.4%) | 88.1-88.3 -> 88.3-88.3 |
| battle | 611 -> 376-378 | 133.8 -> 113.5 ms | 73.1 -> 71.4-71.6 (-2.2%) | 38.7 -> 38.8-39.1 |

Of gor_01's 212 uploads in the 120 frames, 164 came from the worker (dev build). The worst game-thread frame of the 120 does not change (1080 ms in gor_01, 861 pik_00, 597 battle, all with `gxstats=1` timers): those frames are the display-list builds (lever 4: 25595 lists built in the first 10 frames of gor_01, 683 "new" a frame after that is a stats-table artefact), not textures. Texture work after a warp fell by 35-50%; steady state is unchanged, as expected. What is left of the texture cost at a warp: the big and non-TPL textures, the create (alloc + copy), and a bind that catches the worker in the middle of a decode (one wait of 66 ms in the gor_01 first frame: the worker is slower than the game thread's first draw).

**Identity** (dev build, `determ`, `texverify=1`, shots saved at the same frames, compared byte for byte, `texasync=0` against `1`): gor_01 31 of 31 shots (frames 598-658, warp at 595), pik_00 31 of 31, the battle (gon_02, btlauto=perfect, frames 660-795) 28 of 28, a map-change run (`sweep=60`: gor_01, gor_02, gor_00; shots every 7 frames from 598) 57 of 57 compared (the texasync=0 run timed out at 60 s before its last shot, so 57 shots against 58). `gw_diff` on the 1600-frame padscript (`gwdump=10`, A2 padscript): 160 records equal, 23 fields. `texverify`: 0 stale, 0 verify-bad in every run; in the map-change run 48 results were dropped as stale (a relied range was written between queue and bind) and converted synchronously, as designed.

**Not done / open.** CI textures (animdrv's models; needs the TLUT in the key), a prefetch for textures larger than 640x512, and a smarter queue order (boot TPLs queue about 700 textures that are never drawn for a long time: the worker decodes them and they age out, harmless but wasted; a hint from the loader of which TPL is the map's would avoid it). The default stays 0 until the owner decides; no console test was run.

## Map-load hitch in the display-list cache (`dlspread`, batch eviction fix; 2026-11-26)

Azahar, release build, `gxstats=1`, game-thread busy in ms (ratios only), 120 frames from the first frame with more than 100 builds (frame 616 in all three scenes).

**Cause found by measurement.** In gor_01 the burst repeated: 9433 entries built in frame 616 (the budget of 8 MB full), then every 3-4 frames 6000-7800 entries evicted and built again (frames 620, 623, 627, 631, 635, 639, 643: about 1 s each), 66747 builds in 120 frames for a working set of 9433. With `gxbatch=0` there was no repeat. `gxbatch` draws its members without stamping `last_frame`, and `evictable()` (not used in the last 2 frames) read the stale stamp; a batch killed by one eviction then left all its members "old", and each build in the same frame evicted the next list the frame needed (an LRU over a cyclic set). Fix: `evictable()` also refuses a member of a recently used batch, and `bat_kill` stamps the members with the batch's last use. No picture change by construction (cache contents only).

**Cost of one build** (dev build, gor_01 frame 616, 9433 entries, 345 ms in `cache` of 1150 ms busy): `gx_vsh_stream_build` 39%, copies into the blob 22%, `plat_memstable_trust` 20%, `malloc` 18%. The rest of the first frame is the plain draw of 10433 uncached lists (decode 134 ms, state 365 ms, xform/emit), which a prebuild would not remove; the lists need the GX vertex state of the moment of the call (descriptor, VAT, array bases, matrix indices), which is not in the map data, so a prebuild on core 2 was not tried.

**`dlspread=N`** (default 0 = off): at most N new entries are built per frame; the other lists of that frame draw through the plain decode (what a miss draws anyway, so the picture is the same) and build in a later frame. `gxstats` cache line: `deferred`.

| Scene | before: sum of 120 busy / worst | fix only (default) | fix + `dlspread=1500` | steady busy (90 frames from +150) |
|---|---|---|---|---|
| gor_01 | 19004 / 1074 ms, 66747 builds | 11177 / 1047 ms, 9760 builds | 11707 / 778 ms | 71.3 / 71.1 / 71.0 |
| pik_00 | 13554 / 901 ms, 21586 builds | 13862-13890 / 1121-1129 ms | 14824 / 642 ms | 87.6 / 87.7-88.0 / 87.7 |
| battle (gon_02, btlauto) | 8702 / 605 ms, 15580 builds | 8710 / 604 ms | 9039 / 408 ms | 38.4 / 38.9 / 38.6 |

gor_01: the fix removes 41% of the sum and six of the seven 1 s frames. pik_00 and the battle had no thrash; pik_00 shows a second burst at frame 626 (a different list set from frame 623, the old 8 MB of entries cannot go for 2 frames, then 8645 builds in one frame): the fix moves it from partly spread to one frame (worst 901 to 1129 ms), `dlspread` cuts it to 642 ms. `dlspread` costs 3-6% of the sum (more frames draw uncached) and takes the worst frame down by 25-40%; with 1500 the ramp is 852, 393, 348, 312, 271, 229, 161 ms in gor_01 (dev build). The remaining worst frame is the plain draw and the first 1500 builds; not spread further.

**Identity** (dev build, `determ`, shots saved at the same frames, byte for byte, base = port3ds before the change; fix default and fix + `dlspread=1500`): gor_01 31/31 and 31/31 (frames 598-658 step 2), pik_00 31/31 and 31/31, battle (gon_02, btlauto=perfect, frames 660-720) 31/31 and 31/31, map-change run (`sweep=60`, gor_01, gor_02, gor_00, frames 598-800 step 7) 29/29 and 29/29. `gxcache=2` with `dlspread=1500`, gor_01 to frame 800: 300420 verified, 0 mismatched. `gw_diff` (1600-frame padscript, base against `dlspread=1500`): 160 records equal, 32 fields.

**Not done.** Prebuild (see above); a smaller per-build cost (the vsh stream build and `malloc` are the two largest parts); a time budget instead of a count; a default for `dlspread` (owner decision, no console test run).

## texfast (gx_tex.c lookup cache, switch `texfast=0/1/2`, default 0)

A 256-slot direct-mapped cache in front of the copy-slot scan (64 entries) and the 16-probe hash table in `handle_for_impl`. Key: image, w, h, format, is_ci, TLUT data and format. Value: the s_cache or s_copies slot. A hit is used only if the slot still holds that key, the copy registry is unchanged (`s_copy_gen`, bumped when a copy target is registered for a new address or evicted), and the entry needs no hash this frame (checked this frame, or trusted by the current memstable generation with texverify off). Otherwise the full lookup runs and refills the slot. `texfast=2` runs both and counts mismatches (log `texfast:` every 300 frames).

Results (Azahar, dev build):
- `texfast=2` mismatches: gor_01 0 (421200 lookups, 330470 fast hits to frame 1500), pik_00 0 (405263 / 347319), battle 0 (387071 / 323680 to frame 2400), map-change run (map_sweep gor_01,gor_00,hei_01,tik_01,usu_00,pik_00, 6/6 ok) 0.
- determ shots 780..785 `texfast=0` vs `1`: gor_01 6/6 and battle 6/6 byte-identical.
- Release A/B game-thread busy: NOT measured. `gx_perf_ab.py` does not pass `--allow-hw-faults` and every local run logs the known boot `UnmappedAccess 0xA5A5A5A5` lines, so all runs fail. A local copy with the flag is needed (the auto-mode check refused the agent's edit of the tool).
