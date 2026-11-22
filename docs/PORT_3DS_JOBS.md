# Fork-join jobs and load-time conversion

Status: 2026-11-22. This is step 2 and check E of [PORT_3DS_MULTICORE.md](PORT_3DS_MULTICORE.md). All timings come from Azahar, so only the shares mean anything; hardware timings come later.

## The job queue (`port3ds/plat/job.c`, `include/plat/job.h`)

It's a library mechanism: it has no game knowledge, and callers pass a function and an argument.

| Call | What it does |
|---|---|
| `plat_job_init(core, priority)` | Starts the worker thread. `-1` picks the defaults: core 2, priority 0x30 (below the render thread's 0x2E, so rendering always wins). The first submit calls it if nobody did. |
| `plat_job_submit(fn, arg)` | Queues a job and returns a handle. With `jobs=0`, or when the ring is full, the job runs at once and the handle is already done. |
| `plat_job_join(h)` | **Work stealing:** if the worker hasn't started the job, the caller runs it itself. It only waits for a job the worker is running. |
| `plat_job_join_all(hs, n)` | Steals every job not yet started (from the back, since the worker takes the front), then waits for the rest. |
| `plat_job_parallel_for(fn, arg, count, min_chunk, align)` | Splits `[0, count)` into up to 4 chunks whose edges are multiples of `align`. The caller runs chunk 0 itself, then joins the rest with `join_all`. A loop shorter than `2 * min_chunk` runs as one call. |
| `plat_job_stats(&s)` | Counters: queued, inline, run on the worker, stolen, waits, wait ticks, worker busy ticks. |

**How it works:**
- **The ring:** 64 fixed slots, with no allocation per job. A job's slot is its sequence number mod 64.
- **States:** each slot goes FREE → QUEUED → RUNNING → DONE → FREE. The move to RUNNING is a compare-and-swap, so the worker and a stealing joiner can never both run a job.
- **Locking:** submit takes a `LightLock` only to hand out the next sequence number.
- **Memory order:** release stores publish a job (before QUEUED) and its results (before DONE).
- **Waiting:** the worker sleeps on a `LightEvent` when the ring is empty. A joiner sleeps on a second event, with a 1 ms timeout as a safety net.

**Rules for job functions** (also in the header):
- pure computation on memory the caller owns until the join;
- no GPU or system service calls;
- no waits on other jobs;
- no two jobs of one batch may write the same bytes.

**APT suspend and sleep:** a job calls no services, so a suspend just stops the worker like any other thread, and the job finishes when the app runs again. Nothing needs hooking.

## Load-time conversion profile (switch `loadstats`)

`port_conv_begin` and `port_conv_end` (in `port3ds/game/port_swap.c`) bracket every converter: map, TPL, anim, font, camera road, MusyX (`musyx_swap.c`) and the word runs.
- **Per file:** each conversion logs `[LOADCONV] <kind> <file>: <bytes>, <us>`.
- **Per load:** a load is a burst of conversions, ended by a gap of more than a second. It logs a summary line: files, bytes, conversion time against the load's wall time, the DVD reads in between (count, bytes, wait time, now counted in `dvd.c`), and a breakdown per kind.
- **`convhash`** adds an FNV-1a hash of each converted file, so two runs can be compared.

### Measurements

Azahar, `sweep=60 story 30` over `gor_01`, `tik_01`, `hei_01`, `gon_02` (a battle on entry) and `gor_00`, with `jobs=0`:

| Converter | Files | Bytes | Time | Share of conversion time |
|---|---|---|---|---|
| map (`m/<map>/d`) | 7 | 4.34 MB | 208 ms | 50% |
| anim (`a/*`) | 69 | 3.55 MB | 163 ms | 39% |
| MusyX (boot: proj, pool, sdir, slib) | 4 | 0.51 MB | 26 ms | 6% |
| TPL (headers only) | 87 | 6.74 MB | 16 ms | 4% |
| camera road, font, word runs | 9 | 0.09 MB | 1.3 ms | <1% |

Per load (`jobs=0`):

| Load | Converted | Conversion | Load wall time | DVD wait |
|---|---|---|---|---|
| boot | 4.65 MB | 44 ms | 1.3 s | 5.5 s (boot reads before the first conversion) |
| `gor_01` | 2.91 MB | 113 ms (map 83 ms) | 11.2 s | 4.9 s |
| `tik_01` | 1.64 MB | 37 ms | 6.5 s | 4.3 s |
| `hei_01` | 0.82 MB | 34 ms | 0.85 s | 0.6 s |
| `gon_02` (and the battle) | 0.63 + 0.76 MB | 27 + 33 ms | 1.0 + 1.9 s | 0.8 + 4.7 s |
| `gor_00` | 1.80 MB | 67 ms (map 47 ms) | 8.4 s | 6.9 s |

**Conclusion:** the byte-swap converters are about 1% of a map load's wall time in Azahar. The rest is DVD reads (Azahar's SD emulation is slow) plus whatever runs between the reads. The game-thread profile points at texture hashing and conversion in the GX layer, which this profile doesn't cover. The map converter is the largest of the converters: a single `d` file reaches 1.7 MB and 83 ms.

## The first jobs: long runs in `port_swap.c`

`port_swap_units` and `port_swap_array` cover the vertex arrays and the record arrays. Runs of 32 KB or more go through `plat_job_parallel_for`.
- **Why the data splits cleanly:** each item's swap depends only on its own bytes and its own bits in the "done" bitmap.
- **Chunk edges** fall on 8-byte file offsets, so no two chunks share a bitmap byte.
- **Leading items:** the ones before the first 8-byte edge run inline first. A run that never reaches an edge (an odd 2-byte phase) stays inline.

The pointer walks themselves (the relocation pass, the joint tree, the material graph) stay inline: they share the visited bitmap, and their order matters.

### Results (Azahar)

| | `jobs=0` | Jobs |
|---|---|---|
| `gor_01` map | 83.3 ms | 64.8 ms (−22%) |
| `gor_00` map | 46.7 ms | 38.7 ms (−17%) |
| All maps | 208 ms | 177 ms (−15%) |
| anim, TPL, MusyX | unchanged (their runs are mostly under 32 KB) | |
| Jobs over the sweep | — | 19 queued: 12 on the worker, 7 stolen, joiners waited 1.1 ms in total |

**Identical output:**
- `convhash`: 176 of 176 converted files hash the same with `jobs=0` and with jobs.
- `jobcheck=1` runs each split run with jobs and then inline from the same bytes, and compares the data and the bitmap: 12 of 12 runs were identical, none differed.

The saving per load is real but small (about 20 ms on the biggest map), because conversion is a small part of a load. The queue is ready for bigger loops: texture conversion at load and display-list decode (MULTICORE B), which belong to the GX layer.

## Waits that aren't work (check E)

`pc_sample.py --interval 200` on Azahar, `map=gor_01`: 45 s from boot (boot and map load), then 90 s of steady play.
- **Game thread:** 91% busy during the load window and 87% in steady play. The rest is `svcArbitrateAddress` under `LightEvent_Wait`, which is the baton and retrace wait (`VIWaitForRetrace` → `OSSleepThread`). That's a real block.
  - Busy time is mostly GX: `hash_bytes` (texture hashing) 22–27%, `draw_from_reader`, `gx_draw_vertices`.
  - **No samples landed in a spin loop:** none in `port_spin`, `OSYieldThread`, `DVDMgrRead`'s yield loop or `plat_gpu_sync`.
- **The other game host threads** (the DVD manager among them; one sample landed in `DVDMgrMain`): about 99% blocked on their batons.
- **Render thread:** 72% waiting.
- **DVD reads** block properly: `read_file` sleeps the OS thread on a queue that the file thread's completion wakes.

By reading the code, two loops poll instead of blocking, but neither shows in Azahar's profile, because the game thread is CPU-bound there.

1. **ARQ completion waits** (`PORT_SPIN` in `arammgr.c`: `aramMgrGarbage`, the ARAM uploads). `port_spin` used to call `svcSleepThread(0)`, which returns at once when no other thread at the same priority is ready on core 0. So the game thread polled for the whole simulated DMA time (80 MB/s, so a 32 KB chunk takes about 0.4 ms).
   - **Fixed in `ar.c`:** the DMA thread signals an event after each callback, and `port_spin` sleeps on it with a 0.5 ms timeout. The timeout bounds the wait if the flag comes from another interrupt.
   - The switch `arqspin` brings back the polling loop for A/B.
   - It's safe: the spinning thread never held the interrupt lock (the callback could not have run otherwise), and the timeout keeps the worst case at polling every 0.5 ms.
2. **The DVD manager's idle loop** (`dvdmgr.c` `proc_main`: `DVDMgrMain(); OSYieldThread();`). When the main thread sleeps in `VIWaitForRetrace` and no request is queued, this thread holds the baton and yields to itself, so core 0 spins until the retrace.
   - This is the GameCube's behaviour, and it costs the game nothing directly. On hardware it burns core 0 during every retrace wait, and the `s_lock` it takes on each pass is shared with the interrupt threads.
   - **Proposed** (not done, since `os_thread.c` belongs to the OS layer): when `OSYieldThread` finds no ready thread, sleep about 100 µs before returning. That's no change on the GameCube's terms, because nothing else could run anyway. Measure it on hardware with a switch.

## Switches

| Switch | Effect |
|---|---|
| `jobs=0` | Every job runs inline: the A/B baseline. |
| `jobstats=N` | `[JOBS]` counter line every N frames, while the counters change. |
| `jobcore=C`, `jobprio=P` | The worker's core and priority (defaults 2 and 0x30). |
| `jobcheck=1` | Each split conversion run is done with jobs and then inline, and compared (`[JOBCHECK]` lines). |
| `loadstats` | `[LOADCONV]` per conversion and per load. |
| `convhash` | Adds a hash of each converted file to the `loadstats` lines. |
| `arqspin` | ARQ waits poll with a yield, as before, instead of sleeping until the next ARQ callback. |
