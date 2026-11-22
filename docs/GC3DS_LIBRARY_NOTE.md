# Design note: a reusable GameCube layer for the New 3DS (`libgc3ds`)

Status: idea for later, written 2026-11-22. Not on the active path. Revisit once TTYD's phase 7 is done (docs/PORT_3DS_PLAN.md, section 10).

## Question

Could GameCube decomps reach the New 3DS through a shared compatibility layer, instead of a one-off port per game? [Aurora](https://github.com/encounter/aurora) does this on PC.

## Answer in short

- **Don't port Aurora.** Its main value is a GX implementation on WebGPU (Dawn), which the 3DS cannot run.
- **Do turn the TTYD port's platform code into a library** with the same role as Aurora: the Dolphin SDK headers on top, the New 3DS underneath.
- Most of that code already exists and is mostly game-agnostic: about 12,000 lines in `port3ds/game/` and `port3ds/plat/`.

## Why Aurora itself does not fit

| Aurora | New 3DS reality |
|---|---|
| GX turned into generated WGSL shaders, through Dawn to D3D12, Vulkan or Metal | No WebGPU, Vulkan or GL ES 2+. The PICA200 has programmable vertex shaders but a fixed 6-stage pixel combiner. The GX backend would be a full rewrite, and that rewrite is what `port3ds/game/gx/` already is. |
| Runtime vertex and state conversion, with pipeline caches sized for desktop GPUs | An 804 MHz ARM11 is the bottleneck. Our port depends on offline vertex conversion and zero-copy arenas. |
| "Highly accurate" GX | 6 combiner stages against GX's 16, no indirect or z-textures, fewer texture units. Some effects need per-game equivalents, which a generic layer can only approximate. |
| SDL3, nod (Rust), Dear ImGui, RmlUi | SDL3 isn't needed. nod would mean Rust on a tier-3 target. The UI libraries are desktop-only. |

What is worth borrowing from Aurora (MIT licence):
- its API surface and extension hooks for decomp projects;
- Dolphin `.gci` / `.raw` save compatibility;
- the texture-pack idea, if ever useful.

## Proposed split

### Library (`libgc3ds`): the same for every game

| Area | Current files |
|---|---|
| Platform | `plat/*`: boot, threads and cores, CPU-limit handling, memory placement (QTM), log, crash handler, watchdog, switches, HUD and Select menu, vblank, input, audio output (NDSP), disc image reader |
| SDK: OS | `game/os.c`, `os_thread.c`, `os_misc.c`, `os_link.c` (REL linking), `os_font.c`, `msl.c` |
| SDK: I/O | `game/dvd.c`, `card.c`, `pad.c`, `vi.c`, `ar.c` (ARAM/ARQ), `ai.c` |
| SDK: GX | `game/gx/*`: state tracker, TEV to PICA combiner compiler, vertex paths, textures, EFB copies, fog and blend; `plat/gpu.c` (citro3d back end, record and replay, render thread) |
| Audio middleware | the MusyX back end, as an optional module (`game/audio.c`; the MusyX swap tables stay with the game) |
| Packaging | `rsf/ttyd.rsf` as a template (New 3DS ExHeader: 2 MB QTM mapping, MaxCpu 0xD0, core 2, 124 MB), the CMake CIA and 3dsx helpers, Docker image |
| Tools | `run_azahar.py`, `run_hw.py`, `map_sweep.py`-style harnesses (with the game-specific parts pulled out), `count_black_shots.py` |

### Per game: always written for each title

- **Building the decomp for ARM:** 32-bit little-endian, MWCC intrinsics, GQR and paired-single code, address-check audit (TTYD's phase 1).
- **Data formats:** load-time byte swapping of the game's own formats. The TTYD versions are `game/formats/*`, `port_swap.c`, `musyx_swap.c`, `disc_data.c` and the generated swap tables. The *mechanism* (generator plus tables) can move into the library; the tables cannot.
- **Effect equivalents** for what the PICA can't do, each checked against Dolphin.
- **Game-time fixes** like TTYD's per-frame time budget (`port_game_time_take`). The library can provide the hook, but each game decides which clocks it caps.
- **Performance tuning,** and test scripts with menu paths and story flags.

### Boundary rules

1. Every public entry point is the standard Dolphin SDK API (`GX*`, `OS*`, `DVD*`, `CARD*`, `PAD*`, `VI*`, `AR*`, `AI*`), plus a small documented `gc3ds_*` extension API for hooks: game time, warp and test switches, HUD lines, and effect overrides.
2. Nothing in the library knows a TTYD symbol, file name or map name. Today's leaks are things to clean up during the split. The warp list is already a hook, because the game passes it in through `plat_set_map_list`, and that is the pattern to follow for any other leak.
3. Effect overrides are registered by the game: "when this TEV/indirect configuration is seen, call this function". The GX core falls back to a logged approximation, never a silent skip.
4. Hardware rules stay as they are: bounds checks, explicit cache flushes, Azahar `UnmappedAccess` treated as failure, and both QTM and FCRAM paths tested.

## Portable performance layer (decision 2026-11-22)

The optimisation *mechanisms* belong to the library. Each game only supplies *data*: what is hot, which threads do what, and which code is hot. Moving the tuning to a new game should mean writing a manifest, not changing library code.

### Hardware facts the layer is built around

| Resource | Size | Who can reach it | Notes |
|---|---|---|---|
| L1 I and D cache | 16 KB each, per core | that core | 32-byte lines |
| L2 cache | 2 MB, shared | all cores | New 3DS only; shared with every core's traffic |
| QTM RAM (application part) | 2 MB at `0x1E800000` | CPU (all cores); GPU (documented for the first 2 MB; **check on hardware**) | Offsets 0x200000+ belong to the system `qtm` service and the kernel. Retail ExHeader maps exactly these 2 MB |
| VRAM | 6 MB | GPU (CPU only through transfers) | Render targets, EFB, and hot textures |
| Linear FCRAM | 32 MB in our layout | CPU + GPU + DSP | Cache maintenance needed both ways |
| Heap FCRAM | rest of the 124 MB | CPU | Ordinary cached memory |
| Cores | 0 and 2 full; 1 at most 80% (multi mode, ExHeader MaxCpu `0xD0`); 3 system | | Core 1 is shared with networking, file system and input services |

### Mechanisms (library) and inputs (game)

1. **Memory tiers and placement.**
   - **Library:** a registration API, roughly `gc3ds_place(name, bytes, align, prefs, flags)`. `prefs` is an ordered list of tiers (QTM, VRAM, LINEAR, HEAP). The flags say who touches the buffer: `CPU_SHARED` (several cores), `GPU_READ`, `GPU_WRITE`, `DSP`.
   - **Placing:** the library packs registrations by priority into each tier's budget and checks that every chosen tier fits the flags (for example, no `GPU_*` buffer in QTM unless the GPU check passed). It falls back down the list, and reports the result in the boot log and on the HUD.
   - **A/B testing:** a generic switch, `place=<name>:<tier>`, moves one buffer at a time.
   - **Game:** a manifest listing its hot regions (TTYD: map collision `hitdrv`, animation tables, MusyX voice state), each with a priority and flags. It also registers its own buffers through the same API.
   - Today's fixed `PLAT_PLACE_*` enum in `plat/memplace.c` becomes this API.
2. **GPU/CPU shared memory policy.**
   - **Library:** one set of rules plus helpers:
     - GPU-read-only data goes to VRAM or linear memory;
     - per-frame CPU-write/GPU-read data (vertex arenas, uniform blocks) goes to linear memory, or to QTM once the GPU access check is proven;
     - cache flush and invalidate helpers that batch by range, so a game never calls `GSPGPU_FlushDataCache` ad hoc.
   - **Game:** nothing, unless it has its own GPU-visible buffers. Those go through the placement API with `GPU_*` flags.
3. **Threads and cores.**
   - **Library:** threads are created by *role* (game, render, audio, streaming, dma, log, watchdog). A role table maps roles to cores and priorities, with switches to override it. The library also handles the core-1 time budget (the CPU-limit clamp and restore we already have), and the ExHeader template carries the right `MaxCpu`.
   - **Game:** which of its own threads map to which roles. TTYD: the DVD manager is `streaming`, ARQ is `dma`.
4. **Cache hierarchy.**
   - **Library:**
     - linker-script support for a hot-code section (`.text.hot`), so hot functions sit together for I-cache and L2 locality;
     - per-file ARM/Thumb selection driven by a list;
     - 32-byte alignment and prefetch macros;
     - guidance for splitting hot loops to fit L1.
   - **Game:** a hot-function list and an ARM/Thumb list, both generated from the profiler's output, never hand-edited guesses.
5. **Measurement.**
   - **Library:**
     - the profiler (per-scope tick counters in QTM, per core, shown on the HUD);
     - the hardware probe (bandwidth per tier, per-core speed, GPU reach);
     - A/B harnesses in `tools/` that toggle `place=` or roles and compare frame times.
     - Possibly ARM11 performance-monitor counters for cache misses. These need privileged access, through the Backdoor SVC our ExHeader allows. **Unverified; probe it first.**
   - **Game:** benchmark scenes, with save states or test paths, to run the harness on.

### Manifest

Each game gets one file, for example `port3ds/game/perf_manifest.c` (data only, no logic). It holds:
- the placement registrations;
- the thread-role assignments;
- the hot-function list and the ARM/Thumb list (or paths to the generated files).

A second game copies the structure and fills in its own data.

## Performance outlook

| Game | Outlook | Why |
|---|---|---|
| Paper Mario: TTYD | In progress | 2D-heavy scenes, modest geometry |
| Pikmin, Super Monkey Ball 1/2/Deluxe | Plausible | Simple shading. Monkey Ball needs a steady 60 fps for its physics feel |
| Pokémon Colosseum / XD | Plausible to hard | Heavy battle effects |
| Super Mario Sunshine | Hard | Water and indirect-texture effects everywhere |
| Metroid Prime | Doubtful at full speed | Geometry and effect load |

The CPU budget is about 2 full cores (0 and 2) plus about 0.7 of core 1 for background work (probe: core 1 runs at about 79% of core 0's speed). The CPU cost of translating GX matters as much as GPU fill rate.

## Steps when this is picked up

1. **Finish TTYD phase 7,** so the layer is proven across a whole game.
2. **Audit the layer:** list every TTYD-specific symbol, path or assumption in `port3ds/game/` and `port3ds/plat/` (in particular `menu.c`, `debug_dump.c`, `audio.c` and `vi.c` hooks).
3. **Split in place:** move the library files to `libgc3ds/` with its own CMake target, keep TTYD building on top of it, and the GameCube build still matching. Nothing else changes.
4. **Extract the swap-table mechanism:** the generator stays in the library, and the tables move to the game.
5. **Second-game pilot:** a small decomp from the owner's set (Super Monkey Ball or Pikmin). It counts as done when it boots to its title screen in Azahar and on the New 3DS with no library changes beyond the extension hooks. Every library change the pilot needs is a boundary bug to note.
6. **Decide** on a separate repository and a release process only after the pilot.

## Risks

- **GX coverage:** TTYD uses only part of GX. A second game will hit unimplemented paths (other vertex formats, light modes, indirect textures), so expect GX work in every pilot.
- **Memory:** 124 MB of application memory, of which the game's arena, heap and linear memory each take a share. Larger games may not fit without streaming.
- **Audio:** not every game uses MusyX (some use the AX/DSP libraries directly). The audio module needs a second back end then.
- **Decomp quality:** a game must be decompiled far enough to build. Matching decomps are best; partial ones mean mixed code, which this layer can't run.
