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
