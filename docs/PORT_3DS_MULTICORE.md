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
| render (`plat/gpu.c`) | 2 | Replays the record into citro3d |
| log writer (`plat/log.c`) | 2, falling back to 1 | SD writes |
| audio (`plat/audio.c`) | 1 | MusyX software mix into one NDSP channel |
| dvd/io (`plat/disc.c`) | 1 | Disc image reads |
| arq (`game/ar.c`), card, alarm, vblank, watchdog | 1 | Small or waiting |

Azahar sampling (`gor_01`, shares of all busy time):

| Thread | Busy | Share of all busy time | Notes |
|---|---|---|---|
| game | 83% | 55% | — |
| render | 27% | 18% | about 73% of its time waiting |
| audio | 24% | 16% | — |
| one more core-1 thread | 16% | 11% | — |

**Core 0 is the wall, and core 2 has room.** Star3D found the same in its first hardware run: everything costly ran on core 0.

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
4. **A in stages:** the `gxthread=1` recording proof, then `=2`, then the fence and late-read audit, then verify mode on.
5. **Per-voice NDSP audio,** if core 1 turns out to matter.
