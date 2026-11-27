# Game subsystems across cores (gamepar)

Status: 2026-11-27, Azahar only. Owner decision of 2026-11-27: run self-contained game subsystems on more cores with byte-identical results; `src/` changes only under `TARGET_PORT`; the render thread may move to core 1 (`gpucore=1`), core 2 keeps the translator and the job worker. Steps 0, 1, 2b and 3 are built. Steps 4 (sort) and 5 (matrix subtrees) are open. Every switch is off by default.

## Switches

| Switch | Effect |
|---|---|
| `gamecnt=N` | Step 0. `[GAMECNT]` lines every N game frames, per-frame means: mapDisp objects walked and dispEntry calls, name lookups (calls, probes, strcmp, top callers as return addresses), qqsort calls by caller with n, hitCheckVecFilter and animPoseMain calls |
| `jobprobe=1` | With `gamecnt`: each job worker's wake-up round trip, once per frame |
| `mapnameidx=1` | Step 1. mapGetMapObj, hitNameToPtr(NoChk) and lightNameToPtr use a hash index (`port3ds/game/nameidx.c`) |
| `mapnameidx=2` | As 1, and the game's walk also runs; differing results are counted (`[NAMEIDX]` lines, `mismatch` warnings) and the walk's result is used |
| `jobw1=1` | Step 2b. Job worker 1 on core 1 (`jobcore1`, `jobprio1`, default 0x2F), batches only |
| `jobprio=P` | Worker 0 (core 2) priority; 0x2E puts it above the translator (0x2F) |
| `jobspin=US`, `jobtakeover=US` | Idle spin before a worker or a batch caller sleeps (default 30); a batch caller computes a chunk a worker still holds after this long (default 300) |
| `mapdispjob=1` | Step 3. mapDisp in phases with the per-object outcomes in parallel (`src/driver/mapdrv.c`) |
| `mapdispjob=2` | As 1, and the original loop also runs; its dispEntry records and screen rectangles are compared with phase B's (`[MAPDISPJOB]` lines) |
| `mapdispgrain=N` | Smallest chunk of objects (default 32; rounded up to a power of two, at most 64 chunks) |
| `dlcjob=1` | GX layer, not game logic: display-list cache entry builds as ring jobs on worker 0 (core 2), joined one frame later; `dlcjob=2` compares each with the old build (PORT_3DS_GX_PERF.md, "`dlcjob`"). With `gxthread=2` the translator holds core 2, so the jobs run stolen at the join |

## Step 0: counters

Dev build, `gamecnt=120`, means per frame over frames 722-841 (story 30; battle: gon_02 battle 1):

| Scene | mapDisp objects / dispEntry | mapGetMapObj calls x probes | hitNameToPtr | lightNameToPtr | qqsort | hitCheckVecFilter | animPoseMain |
|---|---|---|---|---|---|---|---|
| gor_01 | 1122 / 131 | 13.8 x 338 (4670 strcmp) | 2.0 x 274 | 0 | 25 (camDraw n=210; DVDMgrMain 24 x n=2) | 75 | 19 |
| pik_00 | 1303 / 297 | 7.0 x 670 (4690 strcmp) | 0 | 0 | 50 (camDraw n=350) | 72 | 8 |
| usu_00 | 485 / 122 | 0 | 0 | 145 x 2 | 66 (camDraw n=191) | 106 | 14 |
| battle | 254 / 103 | 4.0 x 127 | 0 | 0 | 118 (camDraw n=150; DVDMgrMain 117 x n=4) | 4 | 6.6 |

Top callers (addr2line on the dev ELF): gor_01 `evt_shop_main_func` 6/frame at 412 probes, `mapGrpFlagOn` 2/frame, `mapMain` 2/frame at 970 probes, `mapObjRotate`; pik_00 `mapObjRotate` 7/frame at 670 probes; hit: `hitMain`. The qqsort of n=150-350 is dispSort (`camDraw`); DVDMgrMain sorts its queue (n=2-4) on every idle pass.

Job round trip (`jobprobe=1`, wake to acknowledgement): before 2b (one worker, always signalled) gor_01 min 23 us, mean 280-460 us, max 1.5 ms. After 2b: worker 0 (core 2) mean 13-24 us, worker 1 (core 1) mean 125-210 us in Azahar.

`__malloc_update_mallinfo` in release builds: the caller is the bottom-screen HUD (`plat_hud_frame`, `port3ds/plat/menu.c` line ~608), `mallinfo()` every 500 ms on the game thread when the HUD is in its default "full" mode. Not changed here (menu.c is outside this task); a fix is to refresh the heap line every few seconds as the VRAM probe beside it does, or count heap use in the allocator.

## Step 1: name index

One open-addressed table per map group (field, battle) and kind: 4 bytes a slot (hash tag 16 bits, item 16 bits), load factor at most 1/2, home slot = hash masked (no divide). Items go in in walk order with linear probing, so items of one name lie on one probe path in walk order and the first candidate that passes the flag test (hit DELETED, light ENABLED, read at lookup time) and the strcmp is the walk's answer. Rebuilt lazily at the next lookup after a map load or unload, a battle map load or unload, a mobj's hit entries, a light entry or the light reset, or when the group's key (entry count, object / hit arrays, their lengths, the map file pointer) changed.

Measured with `mapnameidx=2` (frames to 840): 0 mismatches in gor_01 (2712 map, 388 hit lookups), pik_00 (1600, 9), usu_00 (32421 light lookups), battle (532); map sweep tik (22 maps: 4651 map, 2048 hit) and jin (12 maps: 946, 246): 0. Index cost per lookup: about 1.1 probes and 1 strcmp instead of 130-970.

## Step 2b: jobs

See `port3ds/include/plat/job.h`. Frame batches (`plat_job_pfor_dyn`) are separate from the ring (long background jobs such as texasync). Each worker has its own wake event and a `sleeping` flag, so a submitter makes a system call only for a sleeping worker; idle workers and the batch caller spin `jobspin` us on the atomics first. Chunks are 2^k items (at most 64), taken with one atomic add. The caller spins, then sleeps until `jobtakeover` us, then takes over a chunk a worker still holds (a worker preempted by the render thread on core 1): a compare-and-swap on the chunk state picks the winner, the caller writes the alternate buffer, the returned mask says which chunks are there. A worker that lost a chunk may still run it; the next batch (and map unloads, through `plat_job_pfor_quiesce`) waits for it, so the job argument must not live on the caller's stack (found the hard way: a stale worker read a dead stack frame in gor_01). Jobs and batches run with the submitter's FPSCR (vmrs/vmsr); `jobcheck=1` asserts it (0 mismatches in every run).

Gates: `jobcheck=1` split conversion runs identical to inline in every run; determ shots `jobs=0` against the new code (C1, gor_01 and battle) and convhash: see the results table below.

## Step 3: mapDisp in phases

A: every entry's matrices (mapReCalcMatrix as the loop calls it; each entry's tree is separate, nothing reads another entry's matrices). P: each object's outcome through `plat_job_pfor_dyn`: the loop body's tests and float maths in the same order (the double-transformed corner kept), 8 bytes per object (what, mode, z), written sequentially per chunk. B: in object order, the screenRect clears and the dispEntry calls, so dispEntry order (fsort tie-breaks) is the loop's. Jobs call no GX, heap, rand or OS.

`mapdispjob=2` (phase B's dispEntry records and rectangles saved and undone, the loop run, both compared field by field; chunks taken over included): 0 mismatches in gor_01 (838 calls, 37 chunks taken over with `jobtakeover=50`), pik_00 (838, 56), usu_00 (838, 27), battle (838, 32), with `gpucore=1 gxthread=2` too, and in the tik (3478 calls) and jin (2398) sweeps.

## Identity

| Gate | Result |
|---|---|
| determ shots 780-785, C1, A vs A + `mapnameidx=1 jobw1=1 mapdispjob=1` | gor_01, pik_00, usu_00, battle: 6/6 each, byte-identical |
| same, C3 | 6/6 each |
| same, C3 + `gpucore=1` | gor_01, pik_00, usu_00 6/6; battle 4/4 (both runs, base included, timed out at 200 s before the last two shots) |
| stereo (`stereoforce=1`, C3 + `gpucore=1`), EFB and both eyes | gor_01 14/14, pik_00 10/10 of the shots both runs saved (both runs timed out at 200 s; stereo is slow in Azahar) |
| gw_diff, gor_01 1600-frame padscript, C3 + `gpucore=1` vs + new switches | 160 of 160 records equal, 32 fields |
| map_sweep `--area tik`, `--area jin` in verify modes | 22/22, 12/12 maps ok, 0 mismatches |
| GameCube matching build | 100% after each src change |

Also: determ shots C1 `jobs=0` against C1 + new switches + `jobcheck=1`: gor_01 6/6, battle 6/6. convhash, gor_01 load, `jobs=0` against new switches + texasync: 85 of 85 converted files hash the same.

## Timing (Azahar, release 3dsx, `gx_perf_ab.py --repeat 2`, windows 722-841, ms per frame, best of 2)

C3 = `gxdirty=1 gxdirect=1 gximmdirect=1 texfast=1 texasync=1 gxthread=2 gxpipeline=1 gxsdl=1 gxrecfuse=1 gxreclgroup=1`. Core 0 = game thread busy, core 2 = translator, render = render thread minus its vblank wait (core 2 by default, core 1 with `gpucore=1`). Job worker time is not in these columns.

| Config | gor_01 frame / c0 / transl / render | pik_00 | usu_00 | battle |
|---|---|---|---|---|
| C3 | 33.5 / 22.5 / 13.2 / 7.4 | 33.4 / 28.1 / 15.5 / 7.5 | 32.2 / 18.9 / 13.5 / 6.3 | 28.5 / 15.6 / 13.4 / 3.8 |
| C3 `gpucore=1` (step 2a) | 33.5 / 22.9 / 13.4 / 10.9 | 33.4 / 28.8 / 15.6 / 10.5 | 33.2 / 19.4 / 13.6 / 9.0 | 29.1 / 15.6 / 14.9 / 7.2 |
| C3 `mapnameidx=1` | 33.4 / 22.1 / 13.2 / 7.4 | 33.4 / 27.9 / 15.5 / 7.5 | 32.8 / 19.2 / 13.4 / 6.4 | 27.5 / 15.6 / 13.7 / 3.8 |
| C3 `gpucore=1 mapnameidx=1` | 33.4 / 22.6 / 13.5 / 10.9 | 33.4 / 28.6 / 15.6 / 10.5 | 33.2 / 19.4 / 13.5 / 8.4 | 29.1 / 15.6 / 15.1 / 5.0 |
| + `jobw1=1 mapdispjob=1` | 33.5 / 22.9 / 13.4 / 10.9 | 33.4 / 28.3 / 15.6 / 10.4 | 32.6 / 19.4 / 13.8 / 8.8 | 29.6 / 15.6 / 14.1 / 5.7 |
| + `jobprio=0x2E` | 33.4 / 22.6 / 13.7 / 11.9 | 33.6 / 28.4 / 15.8 / 10.4 | 33.5 / 19.4 / 13.8 / 9.8 | 28.2 / 15.6 / 14.6 / 5.6 |
| C3 `mapnameidx=1 jobw1=1 mapdispjob=1` | 33.4 / 22.4 / 13.2 / 7.4 | 33.4 / 27.7 / 15.5 / 7.5 | 32.8 / 19.1 / 13.4 / 6.4 | 28.4 / 15.7 / 13.4 / 3.7 |

Run-to-run spread of one config is about 0.3-0.5 ms, so most differences above are noise. Direct timers (`gamecnt=120`, C3 `gpucore=1`, release, last two 120-frame windows):

| | gor_01 mapDisp / lookups (us per frame) | pik_00 |
|---|---|---|
| original | 930-1130 / 285-463 | 1530-1590 / 290-298 |
| `mapnameidx=1 jobw1=1 mapdispjob=1` | 1180-1230 / 44-50 | 1340-1430 / 19-46 |
| the same with `jobs=0` (phases, all on core 0) | 1250-1420 / 44-50 | 1870-1980 / 19-46 |

Findings: the name index saves what the lookups cost, 0.25-0.4 ms per frame in Azahar (gor_01, pik_00), not the 1.5-3 ms estimated: with the byte-loop strcmp a miss costs a few cycles, and the profile's `strcmp`/`mapGetMapObj`/`mapObjRotate` shares were too high for this build. mapDisp's whole per-object loop is 0.9-1.6 ms, not 2.5-3 ms; the parallel pass saves 0.1-0.6 ms against the phases run inline, but the phases themselves (table write, second walk, the dispEntry pass) cost 0.3-0.4 ms against the original loop, so the net is about 0 in gor_01 and -0.2 ms in pik_00. With C3 the translator (0x2F) keeps worker 0 (0x30) off core 2: worker 0 ran 0 chunks there; `jobprio=0x2E` lets it run but shows no gain. `gpucore=1` alone: render +3.5 ms (core 1 is slower in Azahar, or it shares core 1 with the log thread), core 0 +0.4 ms, frame unchanged. Azahar does not model caches or the real core speeds: judge on the console.

## Hardware fit (New 3DS)

- Name index: tables of 2-8 KB, read from one home slot sequentially; mask instead of modulo; FNV-1a (one multiply per byte); no syscalls, no doubles.
- Jobs: no system call per chunk or per batch while workers are awake (atomics only; a signal only for a sleeping worker); 64 chunk states in one array; worker 1 at 0x2F below render, vblank, ARQ and card, so it never delays rendering, and a preempted chunk is taken over instead of waited for.
- mapDisp: 8-byte outcomes in a sequential table; per chunk, a run of consecutive MapObjects (each 0x134 bytes) is read in order; floats only.
