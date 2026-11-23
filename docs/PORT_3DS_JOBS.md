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

## Load profile: where a map change goes (switch `loadprof`)

Status: 2026-11-22, Azahar on a busy machine (other agents' runs share the CPU, so wall times are rough; frame counts and SD times are not).

`loadprof` (`port3ds/game/loadprof.c`) measures a window from the request (`SEQ_MAP_CHANGE` or `SEQ_BATTLE` entered) to the first field frame (battle: `inBattle`). It logs one line per window and then the game thread's busy time for the next 90 frames. `loadstats` is not needed.

### What a map change spends

Azahar, `sweep=150`, story 30, one map after another, non-determ, no fast switches (frames are 70-100 ms in Azahar; GC frames are 16 ms):

| Load | Frames | Wall | Game-thread busy | Retrace wait | DVD reads (KB) | Caller wait for the reads | of which SD | of which baton wait | Conversion |
|---|---|---|---|---|---|---|---|---|---|
| `gor_01` (first) | 79 | 9.1 s | 8.2 s | 0.95 s | 68 (4215) | 2.4 s | 0.87 s | 1.6 s | 116 ms |
| `gor_00` (after `gor_01`) | 81 | 11.6 s | 10.9 s | 0.65 s | 57 (3775) | 4.2 s | 0.74 s | 3.4 s | 80 ms |
| `tik_01` | 42 | 1.6 s | 1.3 s | 0.28 s | 20 (1099) | 0.3 s | 0.22 s | 0.08 s | 22 ms |
| `gon_02` | 41 | 2.5 s | 2.1 s | 0.37 s | 22 (1280) | 0.5 s | 0.26 s | 0.21 s | 30 ms |
| `gon_02` battle start | 61 | 5.0 s | 4.3 s | 0.66 s | 24 (1429) | 0.7 s | 0.29 s | 0.39 s | 26 ms |

- **The 11.2 s that `loadstats` could not explain is frames.** A load runs about 80 game frames at 100 ms in Azahar. The SD reads are 0.9 s of that, conversion 0.1 s. The rest is the game thread building those frames (busy 8.2 s): steady-state frames in the new map cost 70 ms (`gor_01`) in Azahar, and the loading frames cost 100 ms.
- **ARAM is not a factor.** Map loads post no ARAM transfers at all; a battle start posts 2 (654 KB), which the 80 MB/s delay holds back for 7 ms in total. `aramfast=1` saves that 7 ms and nothing else.
- **Conversion is 0.1-1.5% of a load** (see above).
- **GX first use is not a load cost.** The first 6 frames of the new map cost the same as frame 61-90 (`gor_01`: 71.9, 70.9, 71.6, 72.0, 71.9, 72.0 ms against a steady 70.2; `tik_01` 22.8 then 21.2 against 22.9). The extra over the first 30 frames is 30 ms for `gor_01`, 0 for `tik_01` and 180-196 ms for `gor_00`: one-time texture hashing and conversion is small. What is large is the steady per-frame cost (the hashing at 22-27% of the game thread), which is not load time and belongs to the texture code. Nothing worth moving to the job queue at load time; if the texture owner wants to, only `gor_00` (about 0.2 s once) shows a first-use excess.

### Why the reads take so long to complete: the DVD manager only runs when the game thread sleeps

Of the caller wait for the reads (2.4 s for `gor_01`), the SD card itself is 0.87 s. The other 1.6 s is the **baton wait**: the read has finished, but the DVD manager thread is waiting for the game thread to give up the CPU. All of TTYD's threads run at one priority and switch only when the running one sleeps or yields (as on the GameCube), and the game thread only sleeps in `VIWaitForRetrace`, once per frame. So the DVD manager issues one read, the game thread wakes at the retrace and spends 70-100 ms on the frame, and the next read starts at the next frame. Frames by reads finished in them for `gor_01`: 36 frames with none, 30 with one, 12 with two, 1 with three or more. A load takes about one frame per read, not the frames the game logic needs.

`determ` shows the difference: its frames wait for the queued reads, and the same loads take exactly what the GameCube debug DOL takes in Dolphin (a title warp to each map, `determ`, `gwdump=1`; frames from the request to the first field frame, battle: to `inbtl=1`):

| Load | Dolphin (debug DOL, `determ`) | Port, `determ` (Azahar) | Port, real time (Azahar) | Port, real time, `dvdsettle=1` |
|---|---|---|---|---|
| `gor_01` | 63 | 63 | 79 | 63 |
| `gor_00` | 56 (from the title) | 52 (after `gor_01`) | 78-81 | 52 |
| `tik_01` | 39 (from the title) | 34 | 42-48 | 34 |
| `gon_02` | 34 (from the title) | 32 | 41-47 | 32 |
| `gon_02` battle start | 62 | 62 | 61 | 61 |

(Dolphin loads each map from the title, the port's sweep from the previous map, so the small differences for `gor_00`, `tik_01` and `gon_02` are the starting state. `gor_01`, which both load first, and the battle agree exactly.) The GameCube game logic needs 63 frames for `gor_01`. The port in real time needs 79-81 because the reads trickle in at one per frame; the original on a real drive is slower still (a read takes tens of milliseconds there), but its frames are short.

### Fix: `dvdsettle=1`

In `VIWaitForRetrace` (outside `determ`), while the sequence is `SEQ_MAP_CHANGE` or `SEQ_BATTLE` and the DVD manager has queued reads, the frame waits for them (yielding to the DVD manager thread, sleeping 0.2 ms between checks, at most 250 ms per frame). The field, where music and streaming reads are queued, never waits. Under `determ` the switch has no effect (determ already waits), so its `gwdump` streams are unchanged.

| Load (Azahar, real time) | Frames before | Frames with `dvdsettle=1` | Wall before | Wall with |
|---|---|---|---|---|
| `gor_01` | 79 | 63 (-20%) | 9.1 s | 7.5 s (-17%) |
| `gor_00` | 78-81 | 52 (-33%) | 11.2-11.6 s | 8.1 s (-29%) |
| `tik_01` | 42-48 | 34 | 1.6-1.9 s | 1.6 s |
| `gon_02` | 41-47 | 32 | 2.5-2.7 s | 2.2 s |
| battle start | 61 | 61 | 5.0 s | 5.0 s |

The sweep passed on all four maps. Wall times move with the load on this machine; the frame counts do not. On the console, frames are shorter than in Azahar, so the baton wait is shorter, but a load that issues one read per frame still pays one frame per read: judge `dvdsettle` on hardware (load time of `gor_01` from the title, with and without) before making it the default.

### The other quick wins: measured, no gain

- **`aramfast=1`:** map loads post no ARAM transfers, so it saves nothing there. In the battle start it saves 7 ms. The determ `gwdump` stream with it on is identical to the one with it off.
- **`discra=256` (read-ahead):** it **loses**. Of 68 reads in `gor_01`, 15 are over 64 KB (the game already reads big files whole, the DVD manager chunks them at 256 KB) and only 13 start where the last one ended; the small sequential ones are too few to pay for the read-ahead. In `gor_01` the SD calls went 69 to 67 but moved 5.3 MB for 4.2 MB asked (1.1 MB over-read), with 1 read-ahead hit; in `gor_00` 2.3 MB over-read, 2 hits; `tik_01` 0.95 MB over-read, 1 hit. SD time rose 0.87 to 1.04 s for `gor_01`. Off by default.
- **`discbatch=1` (merge queued requests):** merged 0 requests in every load. The queue is one deep: the DVD manager thread issues one read at a time and waits for it, so nothing is queued behind a read to merge. The code stays (it is correct and costs one compare), but there is nothing to gain unless a second reader (the THP movie player, `ndspstreams`) runs at the same time.
- **Determinism:** `determ gwdump=10` over the four-map sweep with `discra=256 discbatch=1 aramfast=1` against none: the 411 shared `gwdump` records are identical.

What needs the console: the SD card's real read times (Azahar's emulated card was 12.7 ms per read here; a Class 10 card reads 32 KB in about 2-3 ms, which shrinks the SD part of a load but not the one-read-per-frame pacing), the frame time during a load, and whether `dvdsettle`'s 250 ms cap ever shows as a hitch.

## Switches

| Switch | Effect |
|---|---|
| `jobs=0` | Every job runs inline: the A/B baseline. |
| `jobstats=N` | `[JOBS]` counter line every N frames, while the counters change. |
| `jobcore=C`, `jobprio=P` | The worker's core and priority (defaults 2 and 0x30). |
| `jobcheck=1` | Each split conversion run is done with jobs and then inline, and compared (`[JOBCHECK]` lines). |
| `loadstats` | `[LOADCONV]` per conversion and per load. |
| `convhash` | Adds a hash of each converted file to the `loadstats` lines. |
| `loadprof` | Map-change and battle-start load profile (see above) |
| `aramfast=1`, `discra=KB`, `discbatch=1`, `dvdsettle=0` (on by default now) | ARAM callback delay, disc read-ahead, queued request merging, wait for the DVD manager during loads (see above) |
| `arqspin` | ARQ waits poll with a yield, as before, instead of sleeping until the next ARQ callback. |
