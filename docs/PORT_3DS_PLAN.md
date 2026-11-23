# Paper Mario: The Thousand-Year Door on Nintendo 3DS: port plan

Status: in progress. Phases 0 to 4 are done and phases 5 and 6 are under way; the table in section 10 gives each phase's state.

This repository is a 100% matching C decompilation. The 3DS port compiles that same C for the ARM11 and replaces the Dolphin SDK underneath it. The GameCube build (`configure.py` + ninja, `30 files OK` for G8ME01) must keep matching. Every port change to shared code sits behind `TARGET_3DS` (or a shared `TARGET_PORT`), and the GameCube build is checked after each change.

## 1. Goals and rules

- Run the full retail game (G8ME01 first, then G8MJ01 and G8MP01) on New 3DS. **Decision (2026-11-21): N3DS only.** The port may assume 804 MHz, the L2 cache, core 2 and the extended memory modes. At startup it checks `APT_CheckNew3DS` and shows a clear message on an Old 3DS instead of running.
- Nothing is stubbed. A feature is not done until it behaves like the original. Unimplemented SDK functions show up as link errors, never as empty bodies.
- The 3dsx and the CIA contain no game data. The user supplies a disc image, and the port extracts and converts what it needs (section 6).
- All GPU and memory access is bounded. Azahar forgives out-of-bounds GPU state and unmapped reads; hardware hangs or faults. Every draw, buffer write, viewport and scissor is validated.
- A global exception handler catches crashes and hangs, writes a report and exits to the HOME Menu or the Homebrew Launcher.
- Azahar is used for functional tests. Performance numbers are measured on hardware; Azahar numbers only show the direction of a change.
- Hot loops must fit the 16 KiB L1 caches (instruction and data, per core). The N3DS L2 (2 MiB) and QTM RAM are used deliberately, not by accident.
- The Linux build and the Windows build (Docker) are the same CMake project, and both must work from the first commit.
- **Release goals (decision 2026-11-22):**
  - **Frame rate:** the GameCube's original rates (60 fps where the original runs 60, 30 where it runs 30), on a New 3DS.
  - **Stereo 3D:** the 3D slider is in scope. Budget the GPU for two eyes, with a cheaper quality profile allowed when the slider is up if needed.
  - **Regions:** the first release covers US, JP and PAL (50/60 Hz, all languages) together.
  - **Extras:** an in-game options menu (separate from the debug Select menu), Dolphin `.gci` save import, and a user README/guide (dumping your own disc, installing, controls).
- **Portability (decision 2026-11-22).** The platform and SDK layers should be reusable for other GameCube decomps. See [GC3DS_LIBRARY_NOTE.md](GC3DS_LIBRARY_NOTE.md) for the later split into `libgc3ds`. Rules from now on:
  - `port3ds/plat/` and the SDK implementations (`port3ds/game/os*.c`, `dvd.c`, `card.c`, `pad.c`, `vi.c`, `ar.c`, `ai.c`, `gx/`) contain no TTYD symbol, file name, map name or story knowledge. Anything game-specific is passed in through a hook, as the warp list is through `plat_set_map_list`.
  - Game-specific port code lives in clearly separate files (`port3ds/game/formats/`, `*_swap.c`, `disc_data.c`, `debug_dump.c`), or under `#ifdef TARGET_PORT` in `src/`.
  - GX effects the PICA can't do are handled by per-game overrides registered with the GX layer. The GX core itself only has generic paths and logged fallbacks.
  - Tools take game-specific details (title ID, menu paths, map lists) as arguments or config, not hard-coded values.
  - Each new hook gets a one-line entry in GC3DS_LIBRARY_NOTE.md, so the extension API is recorded as it grows.
  - Optimisation work follows the same split. The library owns the mechanisms: memory tiers and placement (QTM, VRAM, linear, heap), GPU/CPU sharing and cache-flush policy, thread roles and core budgets, hot-code layout, the profiler and the A/B harnesses. The game supplies only data, in a per-game manifest: hot regions with priorities, thread roles, and hot-function and ARM/Thumb lists generated from profiles. Details are in GC3DS_LIBRARY_NOTE.md, "Portable performance layer". The fixed placement enum in `plat/memplace.c` turns into the registration API described there.

## 2. What we start from

| Source | What it gives us |
|---|---|
| This repo (`src/`, `rels/`, `include/`) | All game code as C: 422 files and 340K lines in `src/`, 745 files and 654K lines in the 29 US RELs |
| `../paper-cut` (MIT) | PC port of this same decomp, based on our commit `2ee4796f`. Reference for: static REL linking, big-endian file decoding (`port/bedecode/*.spec`), the MusyX software mixer (`libs/musyx` `hw_pcmixer.c`), the list of 64-bit/compiler bug classes, test harness (map warp, input scripts, screenshots, `dolphin_compare.py`) |
| `../aurora` (MIT) | GX/SDK reimplementation for PC (FIFO emulation, WGSL shader generation, GPU vertex pulling). Reference for GX semantics; its CPU/GPU split does not fit the 3DS (section 5.4) |
| `../ACGC-3DS-Port` | Animal Crossing (GameCube decomp) on 3DS. Reuse: TEV-to-combiner folding (`3ds/src/n3ds_tev.c`), GX vertex shader with lighting and texgen (`3ds/shaders/gx.v.pica`), texture decoders and Morton tiling, GPU-side EFB copy, fog LUT, heap and core setup, crash ring |
| [Star3D](https://github.com/EzequielLuppino/Star3D) (added 2026-11-22; **no licence stated, so ideas only, no copied code**) | Star Fox Adventures (SFA-Decomp, through the foxhollow PC port) on New 3DS. Its "Stellaris" layer replaces foxhollow's Aurora back end, the same idea as our `libgc3ds`. Reference for:<br>- GX on the PICA200 with display-list caching and repacking;<br>- EFB copies, depth of field, fur, grass, water and heat warps;<br>- MusyX on the DSP, with samples converted to DSP-ADPCM at build time;<br>- render thread on core 2 and a network server thread on core 3;<br>- build-time mesh reduction (`POLY_REDUCE`, hub GPU time 47 to 39 ms/frame) and texture scaling;<br>- per-zone `PROFILE=1` timings, `S3D_*_AB` A/B switches and a state-delta checker;<br>- an HTTP server for pushing builds and pulling logs, and stereo 3D |
| `../papermario-3ds-port` | Paper Mario 64 on 3DS. Reuse: lock-free log with SD file, TCP live log and bottom console (`pm_log.c`), crash handler and watchdog, `debug3ds.txt` switches, phase profiler, Select-button options menu, bottom HUD and build number, Azahar runner, scripted input, golden-image tests, `3dslink` push and live-log scripts |

### Lessons carried over from the two earlier 3DS ports

- O3DS was CPU-bound in both (about 20 fps). The GPU was not the limit. The fixes that mattered were structural: zero-copy vertex buffers, fewer draws and state changes, and LTO (about 13%). Faster trig and divides helped very little.
- Each citro3d draw costs about 36 to 66 µs of CPU time on hardware. Batching draws matters more than anything inside the shaders.
- Hardware hangs that Azahar does not show:
  - shader outputs written only partially (always write all of `.xyzw`);
  - empty viewports or scissors;
  - zero-vertex draws.
- Azahar never calls the guest exception handler, returns 0 for unmapped reads, writes SD files late, and does not model CPU timing.
- `GSPGPU_FlushDataCache` costs about 0.8 ms per frame on hardware and nothing in Azahar.
- Build flags: `-fsigned-char` was required for those games. TTYD was built with `-char unsigned`, so we keep the ARM default. Also use `-fno-strict-aliasing -fwrapv` and always define `NDEBUG` (decomp asserts have side effects).

## 3. O3DS feasibility

This section records the analysis behind the N3DS-only decision. O3DS support could be revisited after the optimization pass.

Short answer: Old 3DS can run the game, but not at full speed. Make N3DS the performance target and keep O3DS as a supported, slower mode of the same binary.

**Memory is not the problem.** The game was designed for 24 MiB of main RAM plus 16 MiB of ARAM. On 3DS ARAM becomes plain RAM, so we need about 40 MiB, plus about 11 MiB of statically linked code and data, plus GPU buffers. A 3dsx on O3DS gets about 57 MiB of heap in the earlier ports, and a CIA can request the 80 MiB mode. Map data and textures are already streamed per area by the game (the 670 KiB REL block and the 3.4 MiB map heap).

**CPU is the problem.** The GameCube CPU is a 486 MHz PowerPC with paired-single FMA, and the game logic runs at 60 Hz (US/JP). The O3DS has a 268 MHz ARM11 with VFPv2 (no FMA, no NEON), 16 KiB L1 caches and no L2. The two earlier ports reached about 20 fps on O3DS with lighter games.

Our estimate before measuring: O3DS reaches 20 to 30 fps in the field and less in busy battles and effect-heavy scenes, with the CPU (game logic plus GX translation) as the limit. On N3DS (804 MHz, L2, three usable cores), 60 fps is a realistic target once rendering runs on its own core.

**Possible O3DS lever.** The code already varies its tick rate per version (`FPS_FRAMES` / `FPS_EASE`, 50 Hz for PAL). A 30 Hz logic mode for O3DS might be possible through the same macros. It is not free: some timers, physics and action-command windows may not scale cleanly. We test it in phase 7 and do not promise it.

**The decision does not block any work.** One binary runs on both models, and the code checks `APT_CheckNew3DS` at startup. Phase 0 includes a hardware probe so the estimate becomes a measurement.

## 4. Verified tooling (2026-11-21)

| Tool | State | How it was verified |
|---|---|---|
| devkitARM | GCC 16.1.0, libctru 2.7.0, citro3d 1.7.1, citro2d 1.7.0, picasso, tex3ds, 3dsxtool, smdhtool, 3dslink | Built `examples/3ds/graphics/printing/hello-world` |
| makerom, bannertool | **Missing on the host** | Needed for the CIA. Phase 0 adds them as pinned downloads inside the Docker build image |
| Docker | 29.1.3, image `devkitpro/devkitarm:latest` present | Used for the Windows build (section 9) |
| Dolphin | 2512 (Debian package), `dolphin-emu-nogui`, `dolphin-tool` | Headless boot: `dolphin-emu-nogui -u <userdir> -p headless -v Null -C Dolphin.General.GDBPort=55555 -e iso/game.iso`. The GDB stub on port 55555 read `G8ME01` from 0x80000000 |
| Decomp GameCube build | `python3 configure.py --version G8ME01 && ninja` | `30 files OK`, with `orig/G8ME01/game.iso` a symlink to `iso/game.iso`. This gives a byte-identical DOL with a symbol map, used for symbol-aware memory reads in Dolphin |
| Azahar | Flatpak `org.azahar_emu.Azahar` 2126.2 | `flatpak run org.azahar_emu.Azahar -g 24689 <file.3dsx>` boots, and the GDB stub reads guest memory (homebrew `.text` at 0x00100000) |
| paper-cut | Built with clang 21 (see below) | Booted to `gor_01` and wrote a correct screenshot |

Azahar runs inside a Flatpak sandbox that mounts the host read-only and has its own `/tmp`. Anything Azahar loads must be under the home directory (we use `build/`). There is no headless mode, and its window opens on the desktop.

The paper-cut build command:

```sh
cd ../paper-cut
PATH=/usr/lib/llvm-21/bin:$PATH cmake -S . -B build -G Ninja \
  --toolchain cmake/toolchains/linux-clang.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DTTYD_VERSION=G8ME01 -DTTYD_BACKEND=aurora -DSDL_X11_XTEST=OFF
ninja -C build
```

The paper-cut automated run:

```sh
printf 'skip_intro=1\nfullscreen=0\n' > $U/settings.txt
TTYD_OFFSCREEN=1 SDL_VIDEODRIVER=x11 TTYD_FAST=1 TTYD_USER_DIR=$U TTYD_NO_LAUNCHER=1 \
TTYD_MAP=gor_01 TTYD_EXIT_FRAMES=600 TTYD_SHOT=400 TTYD_SHOT_DIR=$D TTYD_WATCHDOG=60 \
  ./build/ttyd-us orig/G8ME01/game.iso
```

Notes on these commands:

- `-DSDL_X11_XTEST=OFF` is needed because `libxtst-dev` is not installed.
- In the run, `skip_intro=1` is required; without it the run stalls on the Health & Safety screen.
- paper-cut tries to reach Discord over IPC by default.

Phase 0 turns these commands into the scripts in section 8.

## 5. Architecture

### 5.1 Repository layout

```text
port3ds/
  CMakeLists.txt        3DS build: game sources from configure.py, the port SDK, shaders
  sources.cmake         generated from configure.py (same idea as paper-cut's gen_sources.py)
  rsf/                  ttyd.rsf for the CIA, banner and icon sources (original art only)
  sdk/                  Dolphin SDK replacement: os, dvd, aram/arq, pad, card, vi, mtx, gx, gd, ai
  gx/                   GX state tracker, TEV compiler, texture cache, EFB copies, PICA shaders
  audio/                MusyX hardware backend (hw_3ds.c) and the mixer
  sys/                  boot, heaps, cores, APT hooks, crash handler, watchdog, log, profiler,
                        debug switches, Select menu, bottom-screen HUD and console
  rel/                  static REL table, data snapshots, prolog/epilog dispatch
tools/port3ds/
  extract/              disc reader and asset converter (section 6)
  run_azahar.py         launch, wait, capture, stop (section 8)
  run_dolphin.py        same for Dolphin, with symbol-aware memory reads through the GDB stub
  run_papercut.py       same for paper-cut
  memcompare.py         read the same game structures from all three and diff them
  golden.py             screenshot regression tests
  docker/               Dockerfile and build scripts for Windows and CI
docs/PORT_3DS_*.md      this plan, the tooling guide and hardware findings
iso/                    user disc image (ignored by git)
```

The decomp's `src/` and `rels/` stay shared. Port changes inside game code use `#if TARGET_PORT` blocks, kept as small as possible. Where paper-cut already made the same change for the same reason, we take its version so the two ports stay easy to diff.

### 5.2 Compiling the game for ARM11

- Use devkitARM GCC with `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -ffunction-sections -fdata-sections`, LTO and `--gc-sections`. Hot units get per-file overrides such as `-O3` or `-marm` versus `-mthumb`, chosen by measurement. Thumb code is smaller and gives fewer I-cache misses; ARM code is faster per instruction.
- Pointers and `EvtWord` are 32 bits, the same as on GameCube, so event scripts and most structs keep their layout. We do not need paper-cut's 64-bit work.
- Fixes needed by every non-MWCC build:
  - `__OSs16tof32` loses its GQR5 scale (map collision in `hitdrv.c:592`, `animdrv.c`, effects). Replace it with explicit shifts.
  - MWCC intrinsics map to GCC builtins: `__fabs`, `__fabsf`, `__abs`, `__frsqrte`.
  - Direct `GXWGFifo` writes in `fontmgr.c:1271`, `himo.c:175` and `sac_deka.c:1277` become GX calls.
  - `sound.c:2571` swaps AI output by hand to big-endian; that swap is removed.
  - Embedded big-endian texture words in REL data (`rels/win/.../win_04.c:1110+`) get swapped at build time.
  - `PSMTX*` / `C_MTX*` become C, followed by hand-written VFP vector-mode routines (`fmacs` with `FPSCR.LEN=4`) for the hot ones (`PSMTXConcat`, `PSMTXMultVec*`, `PSMTXReorder`, `PSMTXMultVecArray`). VFP vector mode is the fastest matrix path on ARM11.
- Address checks that assume GameCube addresses (above 0x80000000). These are real bugs on 3DS, where every user address is below 0x80000000:
  - `mapdrv.c:813` and `texPalette.c:21`: `<= 0x80000000` decides whether a pointer is already relocated, so a reload would relocate twice. The converter writes data that needs no relocation (section 6), and the loaders skip this step.
  - `evt_nannpc.c:29,63,494`: `& 0x80000000` as the "is a pointer" test.
  - `evtmgr_cmd.c:2823` (`evtSearchLabel`).
  - The `SfxNameWord` pointer-versus-ID test that paper-cut documents.
  - Phase 1 includes an audit script that greps for `0x80000000`, `0x8000`-range constants, `< 0x81800000` and `ADDR_MAX`, and lists every hit for review.
- RELs: all 29 are linked statically, following paper-cut's scheme. Symbols get a per-area prefix through a rename step. Each REL's `.data` and `.bss` go into their own linker section. At boot we snapshot each REL's initial `.data`. "Loading" a REL copies the snapshot back, zeroes its `.bss` and calls its prolog. The RELs total 7.2 MiB, which fits.

### 5.3 Threads and cores

| Core | Work |
|---|---|
| 0 (app core) | game thread: logic and GX recording |
| 1 (system core, limited to 80%) | NDSP voice management and the software aux/reverb mix, file streaming, log writer, watchdog |
| 2 | render thread: replays the GX record into citro3d, runtime texture conversion |
| 3 | system, not available |

Core 1 is limited by `APT_SetAppCpuTimeLimit(80)`, which drops to 30 on suspend through an APT hook. Where each thread runs is decided by measurement in phase 0 and phase 7.

The GX layer records each frame into a compact command buffer: state deltas plus vertex references, not GameCube FIFO bytes. Core 2 replays it while core 0 runs the next `marioStMain`. A `gxsync` debug switch replays on core 0 instead, which helps isolate threading bugs.

The game's only real thread is the cooperative DVD manager thread (`dvdmgr.c`). It maps to a libctru thread on the same core with the same priority, plus a real asynchronous SD reader on core 1. MusyX runs from the AI DMA interrupt on GameCube. On 3DS it runs from the NDSP frame callback, on the audio thread.

### 5.4 GX on the PICA200

Aurora encodes every GX call into FIFO bytes, parses them again, and lets the GPU pull and decode big-endian vertices. The PICA200 cannot pull vertices, and a 268 MHz CPU cannot afford a second encode and decode. Our GX layer therefore tracks state directly in C structs, with dirty bits per state group.

**Vertices**

- Map and model display lists are converted offline (section 6) into PICA vertex buffers in linear memory. Each gets an index list, native little-endian attributes, and fixed-point positions kept as `s16` with a scale uniform. PICA reads `s16`, `u8` and `float` attributes natively.
- At runtime, `GXCallDisplayList` on converted data becomes a few draws with no per-vertex CPU work.
- Immediate-mode drawing (`GXBegin` / `GXPosition*` / `GXEnd`, 862 call sites; the biggest user is the 2D and window code) writes straight into a per-frame linear-memory vertex arena (zero copy).
- Quads become triangle lists through a shared static index buffer. Consecutive draws with the same state are merged.
- `animdrv` (skinned models through indexed `GXSetArray` data) is the hard case. Its matrix palette goes into vertex shader uniforms (`mova`-indexed, the ACGC approach). Array data is converted once, at load time.

**Transform and lighting**

- One vertex shader family (picasso) handles position, normal and texture matrices, up to two texgens (`MTX2x4` / `MTX3x4`, sources POS, NRM and TEX0/1), and GX channel lighting with up to 8 lights: directional, point and spot, with distance attenuation.
- Variants are selected by uniform booleans and loops, not separate programs, because shader switches are costly.
- PICA fragment lighting is evaluated for later as an optimization.

**TEV**

- A TEV compiler turns the GX state into PICA combiner state, cached by a hash of the TEV state.
- It starts from ACGC's folding of `d OP ((1-c)a + bc)` into at most 3 combiners, extended with:
  - TEVREG0 to 2 through the combiner buffer;
  - bias and scale;
  - compare ops (`GX_TEV_COMP_*`);
  - konst selection;
  - swap tables (folded into the texture or source swizzle).
- The 6-stage limit:
  - 1 to 3 TEV stages fit directly; this covers 330 of the 345 static stage counts.
  - 4 to 6 stages fit if they fold.
  - Setups that need more than 6 (the 8-stage `bom_01` and `gor_10` setups, the 12-stage `win_log.c:1887`, the 5-stage flame effects) are split into multiple passes. The intermediate result goes into a render-target texture, and the pass is drawn again with blending.
- Each of these setups gets its own test scene and is checked against paper-cut or Dolphin screenshots.

**Textures**

- The converter writes every disc texture already in PICA formats with Morton tiling: RGB565, RGBA5551, RGBA4, RGBA8, LA8/L8/A8/LA4/L4/A4, and ETC1/ETC1A4 for CMPR.
- CI4/CI8 (palette) textures have no PICA equivalent. They are expanded at conversion time, or at runtime when the game swaps a TLUT (the converter records which textures are used with more than one TLUT).
- Runtime-generated textures (font glyphs, icons, render targets) are converted on upload and cached by object pointer plus a load generation, not by hashing texels.
- Non-power-of-two sizes are padded and the texture coordinates scaled through the texture matrix.
- Wrap and clamp mode, LOD bias and mipmaps map directly.

**EFB copies (`GXCopyTex`, 33 files)**

- The game renders into a citro3d render target. A copy becomes a GPU transfer into a VRAM or linear texture through `C3D_SyncTextureCopy` or `GX_DisplayTransfer`, with format conversion done by the transfer engine where it can.
- Where the game reads copy memory with the CPU or memcpys it (paper-cut patch 0010), the copy is also made in linear memory, with a cache flush.
- `GXPeekARGB` and `GXReadBoundingBox` (17 files): the bounding box is computed on the CPU from the transformed vertices of armed draws, only while armed. Pixel peeks read the render target after a GPU sync.

**Z-textures and depth copies** (`fadedrv`, `imgdrv`, `envdrv`, `GX_CTF_Z8M`)

- PICA can neither sample depth nor write depth from a texture.
- Depth copies become colour copies of depth encoded by a fog-LUT pass or an alternate shader output.
- Z-texture writes become stencil and depth draws that produce the same visible result.
- Every case gets a dedicated test scene and is compared against Dolphin.

**Indirect textures** (`eff_indirect.c`, `envdrv.c`, `hom_00`, `jon`, `eki_03`)

- PICA has no dependent texture reads. The distortion happens on the CPU instead: the target region is drawn as a tessellated grid, and each grid vertex's texture coordinate is offset by the indirect texture sampled at that vertex (CPU-side, from the converted indirect map) and by the indirect matrix.
- This reproduces heat-haze and water warps at grid resolution. The result is verified against Dolphin for each scene, and grid density is tuned by comparison.

**Fog, blend, alpha and depth state**

- All five GX fog types are computed into the PICA fog LUT, as in ACGC. The fog-range adjustment is applied per draw.
- Blend modes, logic ops, subtract, colour and alpha update masks, alpha compare (two-function forms use the PICA alpha test plus a combiner `discard`-equivalent through alpha), z-modes, `ZCompLoc`, cull mode, scissor and the scissor offset all map to citro3d state.

**Screens**

- The top screen (400x240) uses the game's 4:3 viewport scaled to 320x240, with pillarbox or an optional stretch.
- The bottom screen holds the debug console and HUD (section 7). Stereo 3D is not planned.

**Bounds checks (all builds)**

- Draws with 0 vertices are skipped.
- Viewports and scissors are clamped to the target and skipped when empty.
- Vertex and index counts are checked against buffer sizes before submission.
- Texture dimensions are checked against PICA limits (8 to 1024).
- Command-buffer space is checked before every write, with a flush and a new buffer on overflow, never a write past the end.
- A GPU hang watchdog (from ACGC) prints the draw records of the unfinished frame.

### 5.5 Audio

- MusyX (`libs/musyx`) builds with a 3DS hardware backend, `hw_3ds.c`. The game-side sound code (`sound.c`, `pmario_sound.c`) stays as decompiled, plus the endian fixes for bank headers (paper-cut's `musyx_swap.c` logic, or done by the converter).
- **Decision (2026-11-21): NDSP voices from the start.**
  - Each MusyX voice maps to an NDSP channel (24 available). The DSP decodes GameCube DSP-ADPCM natively, with the coefficients and loop context from the sample header, and does the resampling, per-channel mix volumes and the biquad filter in hardware.
  - MusyX features NDSP does not have (the aux A/B buses with reverb, chorus and delay, the compressor, ITD panning) are handled as follows:
    - aux sends use NDSP's aux outputs, and the effect runs in software on core 1 over that 32 kHz stereo stream;
    - ITD becomes per-channel delay and panning;
    - envelopes and pitch run in the MusyX voice update.
  - paper-cut's `hw_pcmixer.c` is built only for the host test tools, as the reference implementation. A regression test renders the same sound sequence through both and compares the envelope and spectrum. Exact sample equality is not expected, because the resamplers differ.
- Streamed music and DTK audio: the converter keeps streams as DSP-ADPCM. They are read from SD by the streaming thread into a ring and played through NDSP.
- Needs `sdmc:/3ds/dspfirm.cdc`. If it is missing, the boot screen says so and explains how to dump it with DSP1. The game does not continue without sound.

### 5.6 OS, files, input and saves

- **OS:** arena and heaps follow `memInit` with the US size table. ARAM becomes a 16 MiB buffer. Tick conversion (`OSGetTime`, `OSTicksTo*`) is scaled from `svcGetSystemTick` to the GameCube timebase (40.5 MHz). `OSDisableInterrupts` and `OSRestoreInterrupts` become a lock against the audio callback. Message queues, alarms, `OSReport` (routed to the log) and `OSPanic` (routed to the crash handler) are implemented.
- **DVD:** reads go to the converted file tree on SD through a path table built from the FST. Asynchronous reads use the streaming thread. `DVDGetDriveStatus` and the rest of the API behave as with a healthy drive.
- **PAD:** HID becomes a GameCube pad. Defaults: A=A, B=B, X=X, Y=Y, L/R=L/R, ZR=Z, Start=Start, circle pad=stick, C-stick (N3DS) or the D-pad and ZL combination (O3DS, configurable)=C-stick. Remapping lives in the Select menu.
- **CARD:** a folder of `.gci` files on SD, one per card file, as Dolphin's GCI folders. The file's data is big-endian as on the GameCube, so saves transfer to and from Dolphin; the game's structs stay native in memory, and `port3ds/game/save_swap.c` converts at the CARD boundary (card.c) with tables generated by `gen_swap.py` (the `save` format). The game's own `cardmgr.c` computes the checksums, byte sums that do not depend on the order. Saves from earlier little-endian builds are recognised by their checksums and converted on load.
- **VI:** a frame pacer locked to the 3DS's 59.83 Hz vsync. `VIWaitForRetrace` waits on GSP VBlank. Post- and pre-retrace callbacks run.

### 5.7 Memory placement and QTM

The N3DS extra RAM ("QTM RAM") is 4 MiB at physical 0x1F000000. A process maps it at virtual 0x1E800000 through an ExHeader memory mapping. The application part is the first 2 MiB (`0x1E800000-0x1E9FFFFF`). Offsets 0x200000-0x37FFFF belong to the system `qtm` service and the kernel (the last 128 KiB to the kernel), and retail titles map only the first 2 MiB. Writing past it crashed the qtm sysmodule, so the port maps and uses only the first 2 MiB. Azahar emulates it:

- `src/core/memory.h` defines `N3DS_EXTRA_RAM_*`.
- `src/core/hle/kernel/memory.cpp` maps 4 MiB minus 128 KiB from the ExHeader. The port asks for 2 MiB.
- `src/video_core/gpu.cpp` translates QTM addresses for command lists, transfers and texture copies.

How the port uses it:

**QTM with an FCRAM fallback for development.** The CIA is the deliverable, and its `.rsf` requests the mapping.

- At boot the port checks the mapping with `svcQueryMemory` at 0x1E800000.
- If QTM is missing, the placement table puts the same buffers in FCRAM. This is the case for a 3dsx in Azahar, or under a Homebrew Launcher started without a QTM-mapping title. The game behaves the same; only memory speed differs.
- The fallback is never silent:
  - the boot log prints `[MEM] QTM unavailable, using FCRAM`;
  - the HUD shows `QTM:off`;
  - performance runs (`phase`, `bench`) refuse to start without QTM unless the `allowfcram` switch is set, so numbers are never mixed up.
- The O3DS check is separate (`APT_CheckNew3DS`).

**What goes where.** The GPU works from VRAM and linear FCRAM as usual: render targets, textures, vertex and index buffers, command lists. The DSP buffers (NDSP wave and aux buffers) stay in linear memory. QTM holds CPU-side state that several cores touch every frame, so that this traffic stays off the FCRAM bus the GPU and DSP use:

- the GX record buffer that core 0 writes and core 2 reads;
- the GX state tracker, the TEV-to-combiner cache and the texture-object table, which are read by both the recorder and the replayer;
- the MusyX voice state and the aux and reverb working state, shared between the game thread and the audio thread on core 1;
- the log queue and the profiler counters, which every thread writes;
- map collision data (`hitdrv`) and other hot game-logic tables, if the phase 8 profile shows they pay off.

**How placement is managed.**

- A `qtm_alloc` arena owns the region.
- Every placement is listed in one table, `port3ds/plat/memplace.c` (becoming the registration API in GC3DS_LIBRARY_NOTE.md), with a size budget. This gives one place to see what lives in QTM and to move it, and a boot-time check that the total fits.
- Azahar does not model memory timing, so it shows that the placement works but not whether it helps. The final list is tuned in phase 8 from hardware profiles, using debug-switch A/B toggles that move one buffer to FCRAM for comparison. Those toggles are a measurement tool, not a shipping fallback.

## 6. Game data: read from the disc image, decoded at load

**Revised 2026-11-21 (phase 1 findings).** The earlier plan converted the disc offline into a tree of little-endian files. Two phase 1 results make that unnecessary:

- Pointers are 32 bits on both machines, and every struct has the GameCube layout (check_layout.py), so a loaded file needs only its fields byte-swapped. No struct needs rebuilding as paper-cut does for 64-bit pointers.
- Textures and display lists are converted by the GX layer's caches anyway (section 5.4), including the ones the game builds at runtime.

So the port works like this:

1. The user copies a G8MJ01, G8ME01 or G8MP01 disc image to `sdmc:/3ds/ttyd/game.iso`. The port checks the game ID and revision at boot and refuses anything else with a clear message on screen. There is no extraction step and nothing to install.
2. `port3ds/game/dvd.c` reads files straight from the image through the disc's FST, on a file thread. The calling game thread sleeps meanwhile, so the DVD manager's background loads overlap gameplay as on the GameCube. `rel/<area>.rel` is answered with a module header that names the static REL (section 5.2).
3. Each format is byte-swapped in place right after it loads, at the same points where paper-cut decodes on PC: TPL, anim models, maps, fonts, `icon.bin`, camera roads and MusyX banks.
   - The swap tables (which words are 2, 4 or 8 bytes in each struct) are generated at build time from the port's own ARM debug info. Pointer graphs and counts follow paper-cut's `.spec` files.
   - GPU payloads stay big-endian for the GX decoders: texels, palettes and vertex colour bytes.
   - Display lists inside files are rewritten to the port's little-endian form, so `hitdrv`'s collision reads and the GX layer read one format.
4. Data the decomp compiles into code (the card icon and banner, a few textures in REL sources; section 6.1) is read from `main.dol` and the RELs inside the image at boot.
5. The same decoders also build for the host (`tools/port3ds/discdecode`). A test runs every file of the image through them and checks the result against paper-cut's decoders.

A pre-decoded cache on the SD card stays possible as an optimization if load-time decoding ever shows up in the profiles.

### 6.1 Data compiled into the decomp

The port binary contains no game data.

- **Card icon, palette and banner.** The matching build extracts them from `main.dol` into `build/<version>/include/assets/`, and `cardmgr.c` includes them. On ports, `cardmgr.c` declares them extern, and the port fills them from the image's `main.dol` at boot.
- **Results of the phase 1 scan (2026-11-21).** It found 36 data arrays of 64 words or more in game sources (85 KB).
  - **Game-logic tables stay compiled in.** They are code-like data written in the decomp's sources: item, sound, tattle and battle-stage tables.
  - **Image data is filled from the image at boot**, by symbol address from `config/<version>/symbols.txt` or the REL's symbol table:
    - `win_04.c` `texture_0` (4,992 bytes);
    - `mapdrv.c` `texture_0` (256 bytes);
    - `eff_torch.c` `torch_obj2_texture` and `torch_obj22t_texture` (2,032 bytes);
    - `mobjdrv.c` `cubeDN` (4,920 bytes).
- **Enforcement.** The port build checks the list, so no new image array is compiled in.
- **`dspSlave`** (GameCube DSP code) is not needed.
## 7. Developer tools inside the port

These are taken from the PM64 3DS port and adapted:

- **Log:** a lock-free 1024-slot queue with a writer on core 1. It writes to `sdmc:/3ds/ttyd/log.txt` and to a TCP live log on port 17492, with replay on connect. Lines carry `#seq` numbers, so a gap shows dropped lines. The bottom console also shows the log.
- **Debug switches:** `sdmc:/3ds/ttyd/debug3ds.txt` plus 3dslink arguments: `map=`, `bero=`, `padscript=`, `shots`, `fixedseed`, `fast`, `phase`, `wdog=` and others.
- **Profiler:** a push/pop phase profiler using `svcGetSystemTick`, with counters (draws, vertices, texture uploads, state changes, cache misses from the ARM11 performance monitor where available). It writes `[PERF]` and `[PHASE]` log lines, and `tools/port3ds/phase_report.py` parses them.
- **Crash handler:** `threadOnException` on every thread, with a static handler stack. It writes registers, a stack scan with symbols resolved offline from the archived ELF, the last profiler phase, the current map and the build number to the log, `crash.txt` and the live log. Then it calls `svcExitProcess`, which returns to the Homebrew Launcher or HOME Menu.
  - A watchdog on core 1 treats a game loop with no progress for N seconds as a crash. `OSPanic` and failed bounds checks go through the same path.
  - Self-test switches: `crashtest=1|2|3`.
- **Select menu** on the bottom screen:
  - warp to any map and entrance;
  - toggle the HUD, stats and logging;
  - remap controls;
  - frame step;
  - texture, fog and lighting toggles for debugging;
  - return to title;
  - exit.
- **Bottom HUD:** build number, current map, FPS (drawn and logic), frame ms, draws and vertices per frame, heap, linear and VRAM use, CPU limit, and O3DS/N3DS.

## 8. Testing (programmatic and documented)

Every test is one command that returns a non-zero exit code on failure. All of them are listed in `docs/PORT_3DS_TESTING.md`.

**Runners**

- `tools/port3ds/run_azahar.py --build <3dsx|cia> --switches "map=gor_01 shots exitframe=600" --until "<regex>" --timeout 120`
  - Writes `debug3ds.txt` into Azahar's `sdmc`, sets turbo and vsync in the config (and restores the config afterwards), and starts Azahar with the GDB stub.
  - Tails `log.txt`. It stops on the regex, a `[CRASH]` line, log silence or the timeout, and always kills the emulator.
  - Collects the log, the screenshots and any `UnmappedAccess` lines from Azahar's own log. Those lines count as failures, because they fault on hardware.
- `tools/port3ds/run_dolphin.py --map gor_01 --frames 600 --read GlobalWork`
  - Boots the user ISO, or the instrumented decomp build, headless with the GDB stub.
  - Reads named structures by symbol from `build/G8ME01/main.elf` / the map.
  - Optional frame dumps through Dolphin's `-C` frame-dump settings.
  - Warps by writing the same globals the game's own debug warp uses.
- `tools/port3ds/run_papercut.py`: wraps the paper-cut environment variables shown in section 4.
- `tools/port3ds/memcompare.py --scene gor_01_idle`: runs the same scene in all three and compares the selected structures, such as `marioGetPtr()`, `GlobalWork`, the event work and the party. It prints field-level differences, using struct layouts taken from the headers through `pycparser` or a generated offset table.
- `tools/port3ds/push_3ds.py` (needs hardware): finds the console, sends the build with `3dslink`, captures the live log, and fetches `log.txt`, `crash.txt` and Luma dumps over ftpd.

**Test suites**

| Suite | What it covers | When it runs |
|---|---|---|
| Unit tests | Converter and endian decoders, the TEV compiler (state in, combiner config out), texture tiling, `OSTicks` maths. Host build, `ctest` | Every commit |
| Golden screenshots | About 20 named scenes (logo, title, file select, `gor_01`, a battle, the pause menu, each indirect, EFB-copy and fog scene). Compared with a tolerance against reference shots from paper-cut or Dolphin and checked in as hashes, not images | At milestones, plus every change to the GX layer |
| Map sweep | Warp to every map and every entrance; pass means no crash, no `UnmappedAccess` and no GPU hang within N frames | At milestones |
| Story sweep | Uses paper-cut's story rows to set the story flag | At milestones |
| GameCube match check | `ninja` in the decomp; must report `30 files OK` | Every change to shared code |

## 9. Build and packaging

- **CMake:** `cmake -S port3ds -B build/3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DTTYD_VERSION=G8ME01 && ninja -C build/3ds` produces `ttyd.3dsx`, `ttyd.cia` and an archived `ttyd_<build>.elf`.
- **Docker:** `tools/port3ds/docker/Dockerfile` builds on `devkitpro/devkitarm` (version pinned) and adds makerom and bannertool (pinned release downloads with checksums). One script for each host:
  - `tools/port3ds/build.sh` on Linux;
  - `tools/port3ds/build.ps1` on Windows, which uses a Docker volume with rsync because bind mounts are slow on Windows (as in the earlier ports).
  - CI runs the same container.
- **RSF (`port3ds/rsf/ttyd.rsf`):** written in phase 0 and checked on hardware with the probe app:
  - Fields:
    - `SystemControlInfo/AppType`
    - `ProcessInfo` with `MemoryType: Application`
    - `SystemModeExt: 124MB`, or `178MB` if the probe shows it is stable with the HOME Menu (`SystemMode` stays at the default, because the title is N3DS-only)
    - `CpuSpeed: 804MHz`
    - `EnableL2Cache: true`
    - `AffinityMask` and `IdealProcessor` so core 2 is available
    - `CoreVersion`
    - a memory mapping for the N3DS extra RAM at 0x1E800000, first 2 MiB only (section 5.7)
    - the services list (`ndsp`/`dsp::DSP`, `hid`, `fs:USER`, `ptm:u`, `soc:U`, `ac:u`, `qtm:u`)
    - `FileSystemAccess: DirectSdmc`
  - Original banner and icon art only.
- **The CIA is the deliverable and the main test target.** Its ExHeader requests:
  - the 804 MHz clock;
  - the L2 cache;
  - core 2;
  - the extended memory mode;
  - the QTM mapping.
- **3dsx:** a developer convenience for fast Azahar runs and 3dslink pushes. Without QTM it uses the FCRAM fallback (section 5.7). It still needs N3DS mode, the 804 MHz clock and core 2; if those are missing, the boot check shows what is missing and exits.
- **Azahar tests:**
  - Day-to-day runs use the 3dsx, which is quicker to deploy.
  - The golden set and milestone checks also run the CIA, so the QTM path stays tested. Azahar gives a 3dsx a default ExHeader without the QTM mapping.
  - Phase 0 works out the exact CIA flow: either `azahar -i ttyd.cia` followed by booting the installed title's content, or booting the CIA's NCCH directly.

### 9.1 Regions (added 2026-11-22)

**One build per region.** The code differs per version at compile time: 6932 `VERSION` blocks in 864 files, and different struct layouts, data tables, REL sets (JP adds `dig` and `qiz`, PAL also `tst`) and symbol addresses. A single binary that picks the region at boot would need three renamed copies of all game code and data. So there are three builds of one CMake project:

| Region | Configure with | Output | Unique ID (title) | Product code |
|---|---|---|---|---|
| US | `-DREGION=US` (default, `-DTTYD_VERSION=G8ME01`) | `build/3ds/game` | `0xFD886` (`000400000FD88600`) | `KTR-N-TTYD` |
| JP | `-DREGION=JP` (`-DTTYD_VERSION=G8MJ01`) | `build/3ds/game_jp` | `0xFD88A` (`000400000FD88A00`) | `KTR-N-TTYJ` |
| PAL | `-DREGION=PAL` (`-DTTYD_VERSION=G8MP01`) | `build/3ds/game_pal` | `0xFD88B` (`000400000FD88B00`) | `KTR-N-TTYE` |

`tools/port3ds/build.sh . -DREGION=PAL` picks the output folder. The probe is `0xFD887`/`KTR-N-TTYP` and the ctr-devkit examples are `0xFD888` and `0xFD889`, so PAL takes `KTR-N-TTYE` (E for Europe). The HOME Menu name is "Paper Mario: The Thousand Year Door" for all three; the banner subtitle names the region for JP and PAL. Each build has its own generated sources (`port3ds/sources_<version>.cmake`, from `gen_sources.py --version`), swap tables (the probe is compiled with the version's defines), disc-data table and arena (`gen_disc_data.py --version`), and static REL set.

**The disc check.** `DVDInit` reads the disc header before anything region-specific (the FST, main.dol, the RELs) and refuses another region's disc: "This build is for the US version (G8ME01); the disc is G8MP01."

**The arena's high end** (where the IPL puts the FST, from the disc's maximum FST size) is a committed table, `ARENA_HI` in `gen_disc_data.py`; no build reads a disc for it. The US value comes from the US disc. JP and PAL are unknown until someone with that disc runs `gen_disc_data.py --print-arena iso/G8MJ01.iso` (or `G8MP01`) once and commits the printed line; until then their dev builds use the US value with a warning and `release.py` refuses to package them (docs/RELEASE.md).

**PAL 50/60 Hz.** As on the GameCube, the TV format follows the render mode the game configures (`VIConfigure`): the PAL build starts in `VI_PAL` (50 Hz) and the start-up prompt stays there or configures its EURGB60 mode (60 Hz); `VIGetTvFormat` returns the current format, so `gp->framerate` and every `FPS_FRAMES`/`FPS_EASE` follow it. NTSC, EURGB60 and MPAL retrace on every LCD vertical blank (59.83 Hz). In 50 Hz mode `vi.c` emulates a 20 ms retrace on the LCD's blanks: the retrace runs on the blank nearest each 20 ms tick (5 of every 6 blanks), so game frames and the game clocks run at 50 Hz, paced by this emulation and not by the LCD, and a frame is shown for one or two LCD refreshes. The game-time budget per frame is 2 retraces: 33 ms at 60 Hz, 40 ms at 50 Hz. The prompt preselects 60 Hz (the 3DS screen's rate); switch `hz=50` preselects 50 Hz. Tools that count game frames as 1/60 s (the deterministic test mode in 7.1) must use 1/50 s in 50 Hz mode.

**Languages.** G8MP01's `marioStInit` takes its language from `OSGetLanguage` (the IPL setting). The port maps the 3DS system language (`CFGU_GetSystemLanguage`) onto it: French, German, Italian and Spanish as themselves, Dutch as Dutch (which the game shows in English, as on the GameCube), every other language as English. Switch `lang=en|de|fr|es|it|nl` overrides it. G8ME01 is always English and G8MJ01 always Japanese (its font encoding is SJIS).

**Known gaps for JP and PAL** (none can be checked without their discs):
- The digest map (`rels/dig`, JP and PAL) plays `demo.thp` through the SDK's THP decoder, which is paired-single assembly; `port3ds/game/thp.c` decodes every frame to black and the sound to silence, so the movie runs its length as a black screen.
- JP's IPL font is SJIS; the port's ROM font (`os_font.c`) is ASCII only, and `romfont.c` draws the system messages from it.
- PAL's 50 Hz render modes have a 568-line XFB (`xfbHeight` 0x238); the EFB is 480 lines in every mode and the port presents the EFB, so the image should be unchanged, but the display-copy scale is unchecked.
- The arena's high end is the US value until the region's disc is seen by the generator (above).

**Once the discs are available:** put them at `iso/G8MJ01.iso` and `iso/G8MP01.iso`, then for each region: fill the region's `ARENA_HI` line (`gen_disc_data.py --print-arena`), rebuild (`tools/port3ds/build.sh . -DREGION=JP`) and check that `build/3ds/game_jp/generated/port_disc_data_gen.h` says `PORT_GAME_ARENA_HI_KNOWN 1`; build `host_decode` with `-DTTYD_VERSION=G8MJ01` and run `host_decode --disc iso/G8MJ01.iso all` (every format must decode); copy the image to the Azahar profile's `sdmc:/3ds/ttyd/game.iso` and boot to the title, a new game and `map=gor_01`; run the 7.2 map sweep. For PAL also boot with `hz=50` and pick 50 Hz at the prompt (the log shows each `VI: TV format` change and `EURGB60:` choice), and boot once per `lang=` value.

## 10. Phases and exit criteria

Each phase ends with a commit, updated docs, and passing tests for everything earlier.

| Phase | Work | Done when |
|---|---|---|
| 0. Tooling (**done 2026-11-21**; see PORT_3DS_TESTING.md) | Scripts from section 8 (runners for all three, each reading `GlobalWork`; `memcompare.py` moved to phase 1 because it needs the 3DS ELF's layouts), Docker image with makerom and bannertool, Windows script, first `.rsf` with a hello-world CIA that installs and runs in Azahar. The hardware probe app (memory modes, core 2 availability, measured L1/L2 and memory bandwidth, QTM mapping through Luma, `svcGetSystemTick` cost) is written here but first run on hardware in phase 8 | Every runner works from a single command; the CIA and the probe run in Azahar |
| 1. Compile and link (**done**) | `port3ds/CMakeLists.txt` compiles all game code and MusyX for ARM; static REL linking; the address-check audit; the MWCC-intrinsic and GQR fixes; the GameCube build still matches | Links with only the port SDK left as link errors; that list becomes the phase 3 to 5 work list |
| 2. Disc access and decoders (**done**: the host decode tool and round-trip tests against paper-cut are in, 2026-11-22; 3204 files, 0 failures) | ISO reader and FST, DVD layer, load-time swap of every format (TPL, anim, map, font, icon.bin, camera roads, MusyX banks) from generated swap tables, data compiled into the decomp read from the image, host decode tool with tests against paper-cut's decoders | Every file on the US disc decodes on the host and in the port, and the round-trip tests pass |
| 3. Platform (**done**) | OS, DVD, ARAM/ARQ, PAD, CARD, VI; log, crash handler, watchdog, switches, HUD, Select menu | The game reaches its first `GXBegin` on Azahar with logs; the crash handler's self-test path works in Azahar |
| 4. GX (**done**: logo, title, file select, `gor_01`, field and battle scenes render) | State tracker, vertex paths, shaders, TEV compiler, textures, EFB copies, fog and blend, bbox, z-texture, indirect, multipass, bounds checks | Logo, title and file select match paper-cut shots; `gor_01` renders |
| 5. Audio (**done in Azahar 2026-11-22**: compared with paper-cut within tolerance; see the baseline in PORT_3DS_AUDIO.md. NDSP voices behind `ndspvoices=1`) | MusyX `hw_3ds` software backend, streams, DTK; then NDSP voices | Title music and `gor_01` sound effects play; matches paper-cut's audio golden hashes within tolerance |
| 6. Bring-up (**under way**: new game, Rogueport, field walking and a full first battle work in Azahar) | New game to Rogueport, first battle, save and load, partner, menus | Golden set passes on Azahar |
| 7. Functional completion in Azahar (**started**: map sweep about 230 of 334 maps; broken into 7.1 to 7.9 below) | Map sweep, story sweep, every special effect scene, the remaining regions (JP, PAL with 50 Hz and languages), CIA install path | Sweeps clean in Azahar; game completable in Azahar |
| 8. First hardware run | Run the probe app and record results in `docs/PORT_3DS_HARDWARE.md`; settle the section 12 assumptions; boot the functional build on the N3DS over 3dslink; crash-handler and watchdog tests on hardware; fix hardware-only faults (hangs, unmapped accesses, cache maintenance) | The phase 6 path runs on the N3DS without a crash or hang; ready for the separate bug-fix and optimization passes |

### Phase 7 in steps (added 2026-11-22)

Each step ends with a commit, a short entry in PORT_3DS_STATUS.md, and the earlier steps' tests still passing. Steps 7.3 to 7.7 can overlap: a chapter in 7.3 brings its effects (7.5) and battles (7.4) with it.

**Approach: find bugs through coverage, confirm through playthroughs** (owner's direction, 2026-11-22). Porting bugs come from a few shared mechanisms (GX features, engine subsystems, data formats), not from individual scenes. So bugs are found with automated, coverage-driven tools, and the chapter playthroughs in 7.3 are acceptance passes, not the discovery tool:

- **Galleries:** switches `fx=<effect>` and `battle=<id>` spawn any effect or battle in a fixed test scene. Screenshots are taken in the port and in Dolphin and compared automatically.
- **GX feature census:** the GX layer logs each distinct TEV, blend, texture-format, indirect and copy configuration it meets, as hashes. A static scan of `src/` lists the configurations the game can produce. The difference between the two is the untested list.
- **Differential runs against Dolphin:** the same inputs play in Dolphin and the port. The runners read `GlobalWork` (story flags, sequence, positions), and the first divergence points at a logic bug. Luigi's walk at the start of the opening (fixed 2026-11-22) is the kind of bug this catches.
- **Event-script coverage:** count which `evt` opcodes and `USER_FUNC`s have run. Those never run are the untested list for the story sweep.

| Step | Work | Done when |
|---|---|---|
| 7.1 Test infrastructure | Three foundations for unattended testing: (a) a **deterministic test mode**, where game time advances exactly 1/60 s per game frame, so the same inputs replay the same way; (b) **recorded inputs**, converting a Dolphin input movie (DTM) or an existing TTYD TAS into padscripts; (c) a **GameCube debug DOL** built from the same source with the same test switches (non-matching, separate from the matching build), so `fx=`, `battle=` and story jumps behave the same in Dolphin and in the port. Then: the `fx=` and `battle=` galleries, a battle test hook that picks moves and forces action-command results, the GX census, and `GlobalWork` comparison against Dolphin. Map sweep fixes (**done 2026-11-22**: battles on entry pass and continue; the full sweep takes 24 min) (a battle on entry counts as a pass; warp on without restarting Azahar; fewer frames per map). A "story jump" set-up for each chapter's start: story value (GSW 0), party, key items, flags, and padscripts that get past menus. Dolphin reference screenshots for chosen scenes, compared automatically with a tolerance. The `npctrace`/`gxdump`/`evtdump` tools documented per use case | One command runs the map sweep in under an hour. Any chapter start is reachable by switches |
| 7.2 Map sweep clean | All 334 maps, plus the storybook `stg` maps through their own entry path | Every map loads, runs N frames and is left again with no fault, at its normal story value and at one later value |
| 7.3 Story sweep, chapter by chapter | Prologue and Rogueport; ch. 1 (Petal Meadows, Hooktail Castle); ch. 2 (Boggly Woods, Great Tree); ch. 3 (Glitz Pit); ch. 4 (Twilight Town, Creepy Steeple); ch. 5 (Keelhaul Key, Pirate's Grotto); ch. 6 (Excess Express, Poshley Heights); ch. 7 (Fahr Outpost, the Moon, X-Naut Fortress); ch. 8 (Palace of Shadow) and the ending and credits; the Peach and Bowser interludes between chapters, including Bowser's side-scrolling stages | Each chapter played from its story jump to its end in Azahar with no crash, softlock or obvious visual fault, and its key scenes match Dolphin |
| 7.4 Battle coverage | Every action command (`src/battle/sac/`), every partner and Mario move, special moves, stage hazards and audience events, every boss | A battle test list (one save per boss and per move set) passes; each action command is checked once for timing against Dolphin |
| 7.5 Effects | Every effect (`src/effect/`, 77 files, plus 103 N64-style ones in `src/effect/n64/`) and every screen-copy, indirect-texture or z-texture scene. PICA-impossible ones get an equivalent registered as a per-game GX override (portability rule, section 1) | Each effect is seen at least once and compared with Dolphin; each equivalent is listed with a screenshot pair |
| 7.6 Side content | Pit of 100 Trials, the Trouble Center, the Pianta Parlor games, the quiz shows, cooking, shops and badges, the ship/plane/paper/tube/boat transformations | Each one is entered and completed once |
| 7.7 Systems | Save and load in every slot (copy and delete too), game over and continue, soft reset (B+X+Start) and the Select menu's Return to title, every pause-menu tab, mail and journal, options | All of it works in Azahar, and a save made on the 3DS loads after a restart |
| 7.8 Regions (**builds ready, 2026-11-22**: all three compile, link and refuse another region's disc in Azahar; JP and PAL untested in game for want of their discs; section 9.1) | JP (G8MJ01) and PAL (G8MP01) disc images through the converter; PAL at 50 and 60 Hz and in all its languages | Boot, the title, a new game and the 7.2 map sweep pass for each region |
| 7.9 Packaging | The CIA install path (title ID `000400000FD88600`, product code `KTR-N-TTYD`, ExHeader checked against a retail title), first-boot conversion messages, the HOME Menu banner and icon, the 3dsx fallback (FCRAM, no QTM) | The CIA installs, boots and uninstalls cleanly on Azahar and on the console; the 3dsx path still works |

The phase is done when 7.2 to 7.8 are all done. That is: the whole game completed once in Azahar, in story order, from a new file to the credits, with saves along the way.

**No hardware tests happen before phase 8** (decision, 2026-11-21). Until then:

- Azahar is the only target.
- Code is written to hardware rules anyway, because Azahar accepts things that hang or fault the real console:
  - every bounds check from section 5.4;
  - whole-register shader writes;
  - explicit cache flushes for GPU and DSP buffers;
  - Azahar's `UnmappedAccess` log lines treated as failures.
- Nothing depends on an unverified section 12 assumption:
  - `SystemModeExt` stays at 124MB;
  - QTM holds shared CPU state through the placement table (section 5.7), no GPU buffers are placed in QTM, and both the QTM path (CIA) and the FCRAM fallback (3dsx) get tested;
  - core 2 is used, with the core-0 fallback (`gxsync`) kept working.

Performance work inside these phases is limited to what the architecture needs: offline vertex conversion, zero-copy arenas, the core-2 render thread and VFP matrix code. Profiling-driven tuning (L1-sized loop splitting, QTM placement, Thumb/ARM per file, NDSP voices) gets its own pass afterwards, using the profiler from phase 3.

## 11. Open questions

These are asked in the conversation and recorded here with the answers once given.

1. O3DS target. **Answered: N3DS only.**
2. Asset converter. **Answered: both** — one C library, built as a PC tool and as a first-boot step on the 3DS.
3. Audio. **Answered: NDSP voices from the start**, with paper-cut's mixer as a host-side reference.
4. Effects with no direct PICA equivalent (indirect warps, z-textures, more than 6 TEV stages). **Answered: close equivalents are acceptable**, each verified scene by scene against Dolphin, none skipped.
5. Hardware. **Answered:** an N3DS that the agent reaches over the network, with 3dslink for netload and ftpd for pulling logs. Scripts read the console address from the `N3DS_IP` environment variable; there is no auto-discovery.
6. Regions. **Answered:** US first. The converter accepts all three regions from the start; JP and PAL are brought up in phase 7. All three are in the first release, as three builds (section 9.1).
7. Original game bugs. **Answered:** keep them, so behaviour matches the original.
8. CIA. **Answered:** unique ID `0xFD886` (title `000400000FD88600`) with product code `KTR-N-TTYD`, and plain original placeholder banner and icon art that can be replaced later. JP is `0xFD88A` / `KTR-N-TTYJ` and PAL `0xFD88B` / `KTR-N-TTYE` (`KTR-N-TTYP` is the probe's); section 9.1.
9. Git. **Answered:** work on a `port3ds` branch pushed to `SilentMystification/ttyd-3ds`, with PRs into `main` at phase boundaries.

## 12. Unverified assumptions (checked on hardware in phase 8)

- **QTM RAM on hardware:** Azahar maps 4 MiB minus 128 KiB through the ExHeader, and its GPU reads it. The probe confirms on the N3DS:
  - the CIA mapping;
  - what a 3dsx gets under the Homebrew Launcher;
  - QTM latency and bandwidth against FCRAM, under load from several CPU cores and with the GPU busy.
- **CIA memory:** whether `SystemModeExt: 178MB` is stable with the HOME Menu on retail N3DS; 124 MB is the fallback.
- **Core 2:** whether it is fully usable at 100% from a CIA and from a 3dsx started through an N3DS title. Your note says about 2.8 cores in total; the probe measures it.
- **ARM11 performance monitor:** whether its cache-miss counters are readable from userland under Luma. If not, cache work relies on timing.
