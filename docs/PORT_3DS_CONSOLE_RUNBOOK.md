# 3DS port: console session runbook (New 3DS)

Written 2026-11-27 for the first console session after the unattended work. One ordered session gives the most data. Order: (a) probe + calib + baseline, (b) GPU-hang survival, (c) gor_01 hang bisect, (d) new defaults and A/B pairs, (e) updater screen, deterministic CIA, supervisor. The order is the one the owner agreed (owner-questions item 2). Details of each tool are in the docs named at each step; this file only orders them and gives the exact commands.

## Rules for the whole session

- **Every console push needs the owner's explicit go.** `push`, `restart`, `files put`, `queue start`, `supervisor ...` and the Select-menu actions all change the console. Ask once per step block, not once per command; no automatic push loops. Building on the PC needs no go.
- **No sleep or poll loops.** Use one blocking call: `run_hw.py --no-send --until "<regex>" --timeout N` follows the live log and returns when the pattern shows (or on timeout). For a probe mode that has no live log, one `devupdate.py log` after the stated time.
- **Unattended sessions need Debug mode on** (one-time setup below). Debug mode off (`retail=1`, `debug=0`, the Options setting off, a tagged release under "auto", or a `TTYD_RETAIL` build) has no update server and no supervisor heartbeat, and ignores the `devupdate` switch and the "Update server" option. Only the test escape `retailkeep=...,devupdate*` keeps the server in such a run, so every `push` or `restart` of a retail build or a `retail=1` run carries `devupdate*` in its `retailkeep=` list; without it the remote session ends at that launch. A HOME Menu start in Debug mode off has no server: recover with a HOME start of a build in Debug mode on (the stored setting "on" covers every build but `TTYD_RETAIL`), FBI (`tools/port3ds/fbi_install.py`), or `debug=1` in `sdmc:/3ds/ttyd/debug3ds.txt`.
- The console is at `192.168.8.221`. The ARP entry needs about 20 s to warm up: retry a refused first call. Keep it on the charger, lid open.
- Add `fastboot` to every manual `restart` (it presses A through the title cards; `hw_baseline.py` and `run_hw.py` add it themselves, plain `devupdate.py restart` does not).
- No game assets in a CIA: the disc image is at `sdmc:/3ds/ttyd/game.iso`.
- A crash or hang that stops the game: the updater's recovery mode still answers. `$D report --out build/devupdate-last/report.txt`, then `$D restart "<next switches>"`. A frozen console: hold POWER, start the dev CIA from the HOME Menu (a dev build starts the server by itself), then go on. The probe journal and the hang files survive on the SD card.
- Time estimates are wall time on the console including boot and warp (about 1.5-2 min per game run).

## 0. Preparation on the PC (no console, no go needed)

```sh
export N3DS_IP=192.168.8.221
D="python3 libs/ctr-devkit/devupdate/devupdate.py --ip $N3DS_IP"
git checkout port3ds                        # the commit the numbers belong to; note `git rev-parse --short HEAD`
df -h .                                     # build/ dirs are 1-3 GB each
tools/port3ds/build.sh --dev                # build/3ds/game-dev/ttyd.cia   (probe, calib, hang tests: dev builds only)
tools/port3ds/build.sh                      # build/3ds/game/ttyd.cia        (release: LTO, timing)
tools/port3ds/build.sh --retail             # build/3ds/game-retail/ttyd.cia (TTYD_RETAIL, step D9)
tools/port3ds/build.sh libs/ctr-devkit/supervisor   # build/3ds/supervisor/ctrdk-supervisor.cia (step E3)
```

- Dev builds have LTO and `-O3` off: they are for the probe, calib, the hang tests and the updater/supervisor checks. **All timing (baseline, A/B, heap, retail) is taken with the release CIA.**
- The A/B configs are built into `tools/port3ds/hw_baseline.py` (`--configs list` prints the switches of each; names are case-insensitive; no edit needed): `D`, `C3`, `K`, `I`, `gpucore1`, `kg1`, `par`, `all`, `old`, `noqtm`, `recheap`, `vramtex`. The old names `default`, `gxdirty`, `gxdirect` are aliases of `D`.
- Config meaning: **D** = the defaults (`gxdirty gxdirect gximmdirect gxsdl gxrecfuse gxreclgroup texasync texfast` on, `dlspread=1500`, `tex16=0` exact textures; `gxthread=0`). **C3** = D + `gxthread=2 gxpipeline=1`. **K** = D + `gxone=1 gxchunk=1`. **I** = K + `gximmfl=1`. **gpucore1** = C3 + `gpucore=1`; **kg1** = K + `gpucore=1`; **par** = C3 + `jobw1=1 mapnameidx=1 mapdispjob=1`; **all** = gpucore1 + par; **old** = the explicit switch set from before the default flip (D2). If the planner flips `gxthread=2 gxpipeline=1` to the default before the session (owner-questions 15), D already is C3: then the "D vs C3" pair becomes `gxthread=0 gxpipeline=0` against the default.
- Also on the PC before the session: `tools/port3ds/hw_baseline.py --dry-run` to see the exact commands, and `$D --help` for flags.
- Check on the console once: `sdmc:/3ds/ttyd/game.iso`, `sdmc:/3ds/dspfirm.cdc`.

### One-time setup: Debug mode on (owner, by hand, once per SD card)

The update server, the supervisor heartbeat, the log, the watchdog and the debug switches all belong to Debug mode (docs/PORT_3DS_TESTING.md, "Debug mode"). Development builds have it on by default ("auto"); set it to **on** so that a tagged release or a changed default cannot drop the console out of the remote session:

1. Start the game from the HOME Menu. Press SELECT (the Options menu), R to the **Debug** page, set **Debug mode** to `on`, B to close (the setting is saved to `sdmc:/3ds/ttyd/options.txt` as `debugmode=1`). The row shows `relaunch` when the change takes effect only at the next launch.
2. Leave the Select-menu (ZL+SELECT) General tab "Update server" at `on` (the default under Debug mode).
3. Optional fallback: `debug=1` as a word in `sdmc:/3ds/ttyd/debug3ds.txt` forces Debug mode on whatever `options.txt` says (a run's `restart` switches still come first). Do not put `devupdate` there: under Debug mode on the server starts by itself.
4. Check: `$D ping` answers; the boot log has `devupdate:` and `watchdog: 10 s, monitor thread started, guard thread started`.

To leave Debug mode for a measurement, use the run switches (`retail=1 retailkeep=...,devupdate*`), not the stored setting: the stored setting outlives the session.

## A. Probe, calibration, baseline (about 1 h 15 min)

### A1. Install the dev CIA (3 min)

```sh
$D push build/3ds/game-dev/ttyd.cia --switches "devupdate" --wait 120
```

Pass: the bottom screen shows the Updater screen (build line, status, 30-block bar), the game restarts, `$D log | tail` shows the new build number. Fail: `error N ...: stage=... rc=...` (the PC retries by itself with `--stage`; see CTR_DEVKIT_PLAN.md "devupdate protocol"). Record any error here: it is the updater check (E1) too.

### A2. `gpuprobe`: all groups, by danger (about 15-25 min)

Danger levels: 0 controls (`ctl_*`, `*_full`, `*_off0/16`, `*_a256`); 1 `cp_thru`; 2 hang likely (the scissor/viewport/draw/shader cases, all `cmdlist` `cb_*` call/jump cases, all `dt` `dt_tile_*`); 3 memory faults possible (`qtm` group `cb_qtm vb_qtm ib_qtm`, `tex_sz2048`, `vb_heap ib_heap tex_heap`, `rt_deleted`, which stays last and ends as FAULT). A HANG is reset by `GSPGPU_ResetGpuCore` and the run goes on; a FAULT (process died) is journaled and not run again.

```sh
# 1. controls .. danger 2, which includes the new groups cmdlist, thru, dt (about 1 min + 0.2 s per hang)
$D restart "gpuprobe probefresh probeselftest probesafe livelog"
python3 tools/port3ds/run_hw.py --no-send --until "GPUPROBE DONE" --timeout 600
# 2. the QTM cases (danger 3, new)
$D restart "gpuprobe probe=qtm livelog"
python3 tools/port3ds/run_hw.py --no-send --until "GPUPROBE DONE" --timeout 300
# 3. the rest of danger 3, resumes after a FAULT (no probefresh)
$D restart "gpuprobe livelog"
python3 tools/port3ds/run_hw.py --no-send --until "GPUPROBE DONE" --timeout 300
# 4. fetch
mkdir -p build/gpuprobe_hw
$D files get @/gpuprobe.txt -o build/gpuprobe_hw/gpuprobe.txt
$D files get @/gpuprobe.journal -o build/gpuprobe_hw/gpuprobe.journal
$D files ls @/gpuprobe                       # get every <case>.bin (and ctl_recover.bin, the cb_* .chunk files) into build/gpuprobe_hw/
python3 tools/port3ds/gpu_cmd_diff.py --probe-dir build/gpuprobe_hw
```

`probesafe` skips the danger-3 cases (`SKIP`, note "danger 3 skipped by request") and, since the fix in `libs/ctr-devkit/gpuprobe/gpuprobe.c` made with this runbook, does not write them to the journal (before it the journal held them as done, so steps 2 and 3 would have run nothing; a dev CIA built before that fix needs one `files put` of a journal without the `SKIP 0 danger 3` lines, or `probefresh` with `probe=qtm`). `livelog` works in probe mode (checked in the code: `boot.c` opens the log port from the `livelog` switch before the probe or calib mode is entered, and the probe mode logs through the same queue; the probe's `GPUPROBE DONE` line goes there). If a run does not end (freeze or no `GPUPROBE DONE`): hold POWER, start the dev CIA from HOME, `$D restart "gpuprobe"` (no `probefresh`: the journal marks the dead case FAULT and goes on). If step 1 shows the new `cb_*`, `dt_*`, `cp_thru` rows as skipped or missing, run `$D restart "gpuprobe probe=cmdlist,thru,dt"` (no `probefresh`).

What to look for, per row (result OK / WRONG / HANG / FAULT / ERR / SKIP):

| Group | Pass | Decides |
|---|---|---|
| `cp_thru` (log line `gpuprobe: cp_thru:`) | OK; words per us for single writes, bursts and 8 calls of a 1 K chunk | whether a frame list of 60-100 KB per frame is cheap; burst against single-write rate |
| `cmdlist`: `cb_jump0/1`, `cb_call1`, `cb_call500`, `cb_call_nest`, `cb_call_a8/a16/size8/size16`, `cb_vram`, `cb_irq` | OK. `cb_call500` must show exactly 500 red pixels. `cb_irq`: one P3D, no second one in 20 ms | CALL mode (step 5 of the native plan) and chunk placement; a WRONG or HANG on an alignment case gives a hardware rule |
| `dt`: `dt_tile_rgba8/rgb8/rgb565/rgba5551/rgba4` (log lines `gpuprobe: dt ...`) | OK with one flip setting for all 10 sizes (Azahar: all equal with `FLIP_VERT` 1, none with 0) | the `texdtflip` value and which sizes `texdt` may use |
| `qtm`: `cb_qtm`, `vb_qtm`, `ib_qtm` (note: flush result codes and `osConvertVirtToPhys`) | OK | whether the PICA fetches commands, vertices and indices from QTM: every QTM placement (`qtmgpu`, `gx_frame_list:qtm`, `gx_index:qtm`, step G) waits on it |
| everything else | compare with "Expected" and Azahar | each console result that differs from both becomes a rule |

Fill: **PORT_3DS_GPU_RULES.md** "New 3DS" column in both tables (cases, lines 60-172, and "Command processor, transfer engine and QTM cases", lines 186-207). Each row that differs from Azahar and from "Expected" becomes a function in `PROBE_RULES` (or a format in `TEX_ALIGN_PROBE`) in `tools/port3ds/gpu_cmd_diff.py`, tagged `[probe]`; keep the `gpu_cmd_diff.py --self-test` green. Copy `cp_thru` and `dt` log lines into the same doc.

Leave probe mode: the next `restart` with game switches.

### A3. `calib` and `calibpmu` (about 6 min)

```sh
$D restart "calib livelog"                    # 57 benchmarks, about 20-40 s on the console
python3 tools/port3ds/run_hw.py --no-send --until "CALIB DONE" --timeout 180
mkdir -p build/hw-calib
$D files get @/calib.txt -o build/hw-calib/calib.txt
$D restart "calib calibpmu livelog"           # second pass with the ARM11 counters
python3 tools/port3ds/run_hw.py --no-send --until "CALIB DONE" --timeout 240
$D files get @/calib_pmu.txt -o build/hw-calib/calib_pmu.txt
```

Pass: `CALIB DONE`; every row has a value. On the console the read and chase rows must **not** be flat (16 K / 256 K / 1 M / 16 M: L1, L2, streaming) and `branch_random_16K` must be slower than `branch_sorted_16K`; if they are flat, the timer or the cache is wrong. `CALIB pmu: not available` is a valid result (the SVC or backdoor is refused): note it and go on. `calibonly=text` runs only matching rows (for example `calibonly=exact`).

Fill: **PORT_3DS_60FPS.md** section 2a, table "New 3DS ns/op" and "MB/s" columns and the ratio (console ns / Azahar ns). The table has 47 rows; the **11 exact-math rows are not in it yet** (add them): `ldexpf_var`, `ldexpf_bits`, `fmod360_lib`, `fmod360_wrap`, `ticks2ms_div64`, `ticks2ms_mulhi`, `vdiv_f64`, `vsqrt_f64`, `vcmp_vmrs`, `texbufsize_old`, `texbufsize_new`. Read them as pairs: the second of each pair must be faster on the console (bit construction against `ldexpf`, the fast `fmod` path, multiply-high against the 64-bit divide, shifts against divides); `vdiv_f64` and `vsqrt_f64` show what the double paths cost (a decision for the f64 promotions and `sinf`/`cosf`: owner-questions 13). Put `calib_pmu.txt` cycles and miss counts per op beside it.

### A4. `hw_baseline`: ten scenes (about 20 min for one config)

Switch to the release CIA first. The `devupdate` switch keeps the server in this release build.

```sh
$D push build/3ds/game/ttyd.cia --switches "devupdate" --wait 120
tools/port3ds/hw_baseline.py --scenes gor_01,hei_01 --configs D      # 4 runs, check the table first
tools/port3ds/hw_baseline.py --configs D                             # the ten scenes: gor_01 gon_02 aaa_00 tik_01 hei_01 jin_00 battle eki_00 usu_00 pik_00
```

The tool restarts with the scene switches, `gxthreadstats=60 fastboot livelog hwb=<id>`, follows the live log and counts the first three windows that end at frame 781 or later (781, 841, 901; the warp is at about 595). Output `build/hw-baseline/baseline.md`, `results.json`, one log per run. The old stock configs (`default`, `gxdirty`, `gxdirect`) are now identical (those switches are default on; they are aliases of `D`), so run `--configs D` only: 10 runs, about 17-20 min. Pass: a row per scene with frame ms, core-0 busy ms, render ms (minus vblank wait). Compare with the Azahar columns in PORT_3DS_GX_PERF.md ("Timing" tables) and the Azahar-to-console ratio from A3. The headline question: how far from 16.7 ms is each scene (frame ms is a whole number of vblanks: 16.7, 33.3, 50, 66.7).

Fill: copy `baseline.md` into **PORT_3DS_GX_PERF.md** next to the Azahar numbers (section "Gate results of steps 0-2", and the "Defaults on" section), and the ratios into **PORT_3DS_60FPS.md** section 2c.

## B. GPU-hang survival (about 15 min, release CIA)

The merged code (`ResetGpuCore`, synthetic P3D interrupt, citro3d restore hook, start-up probe, crash guard thread) is proven in Azahar only; the real PICA is the question. See PORT_3DS_TESTING.md "GPU hang survival".

```sh
# B1. start-up check: look at the boot log of any run above
$D log | grep -E "gpu: start-up check"        # expect: "the GPU answered in N us"
# B2. pretend hang on a healthy GPU: the render thread stops before frame 700, a reset is requested
$D restart "fastboot map=gor_01 story=30 gxthreadstats=60 gpuhangtest=700 gpuhangrecover=1 wdog=30 livelog"
python3 tools/port3ds/run_hw.py --no-send --until "gxthread \d frame 901:" --timeout 300
# B3. give-up path: 4 hangs in a row
$D restart "fastboot map=gor_01 story=30 gpuhangtest=700 gpuhangrecover=1 gpuhangtimes=4 wdog=15 livelog"
python3 tools/port3ds/run_hw.py --no-send --until "no more resets|CRASH" --timeout 200
$D report --out build/devupdate-last/report_giveup.txt     # recovery mode answers; then B4
```

Pass B1: `GPU answered in N us` (on a hang left by an earlier process the log says `resetting it` and then `start-up reset (rc ...) worked`). Pass B2: `gpu: GPU hang (...): reset #1 done`, `citro3d state marked dirty after GPU reset #1`, frame counter and picture continue (look at the top screen: no black, no garbage, no second hang line). Fail: black or garbled picture after the reset, a second `fence wait` loop, or a crash: note `rc` values. Pass B3: after the third reset in 10 s the log says `no more resets`, the watchdog's crash screen appears, recovery mode answers (`report`, `restart` work). The real-hang test is the first bisect run (C1), which also needs survival on.

Fill: **PORT_3DS_TESTING.md** "GPU hang survival" (add a "Console, <date>" paragraph with the three results and the `rc` values) and owner-questions 4d.

## C. The gor_01 hang bisect (about 20-35 min, release CIA)

The hang: after a gor_01 warp the log repeats `gpu: fence wait for seq N ...` (PORT_3DS_TESTING.md "GPU hang (PICA stuck)"). Draws are numbered from 0 in the dumped record (`gpu_cmd_diff.py` prints the same index). `gpudrawcut=K` makes only draws 0..K-1; `gpudrawskip=A-B` leaves out draws A..B (inclusive). Both apply only to the records of game frame `gpudump=N`. **First check the frame number in an earlier hang's file name** (`$D files ls @/gpuhang`, `hang_f<N>_r<seq>.bin`). Frame 887 comes only from owner-questions 4b: no repo doc records the repro frame (PORT_3DS_TESTING.md "GPU hang (PICA stuck)" gives the log pattern, no frame), so treat 887 as unconfirmed; if `gpuhang/` is empty, C1 below is also the check: with `gpudump=887` and no hang, the run still dumps frame 887, and a hang later shows its own frame in the `hang_f<N>` name (then set `gpudump=N`). The base switches are fixed (batching and state cache off, so the draw order matches the dump): `gxbatch=0 gxstatecache=0`. `gxone/gxchunk` stay off (they turn themselves off with `gpudrawcut`).

```sh
BASE="fastboot map=gor_01 story=30 gxbatch=0 gxstatecache=0 gxthreadstats=60 gpudump=887 livelog"
run() {   # usage: run "<extra switches>"   result = HANG if the log has a hang line before frame 901, else OK
  $D restart "$BASE $1" && python3 tools/port3ds/run_hw.py --no-send --until "gxthread \d frame 901:|no more resets|CRASH" --timeout 240
  grep -c -E "GPU hang|fence wait for seq" build/hw-last/log.txt
}
```

The run helper prints the number of hang lines (0 = no hang). With survival on, a hang costs 2-3 s and `reset #N done`; after 3 resets in 10 s the game gives up and the crash path runs: count that as a hang, and `$D restart` from recovery mode.

1. **C1, repro (real hang, 2 min):** `run ""`. Expect hang lines and `reset #1 done`. This is also the **real-PICA recovery test** (B): does the picture come back after the reset, or does the hang repeat? If it does not hang, the repro needs the other switches of the original hang (ask the owner); do not bisect.
2. **C2, suspects first (about 6 min):** `run "gpudrawskip=535"`, then `run "gpudrawskip=207"`. Draw 535 reads the last vertex of the 441-vertex arena (0 bytes margin: hardware read-ahead?); draw 207 has 2 bytes margin. If a skip removes the hang, confirm with `run "gpudrawcut=535"` (draws 0..534: no hang) and `run "gpudrawcut=536"` (draws 0..535: hang). Done.
3. **C3, binary search, if no suspect (up to 10 runs, about 20 min):** K in 0..585, `run "gpudrawcut=K"`. Hang at K means the culprit is in draws 0..K-1. Start K=293; hang: next K = 146, else 439; and so on. The smallest K that hangs: the culprit is draw K-1. Confirm with `gpudrawskip=<K-1>` on the full record (no hang) and `gpudrawcut` K-1 and K. If the answer is not monotone (cut 200 hangs, cut 100 and 300 do not), the cause depends on timing or two draws: report the table.
4. **Collect:** `$D files get @/gpuhang/hang_f887_r<seq>.bin -o build/hw-hang/hang.bin` (and the `.txt`); then a run with `gpudump=887` and no cut gives the Azahar-comparable `dump_f887_r*.bin` (get it too): `python3 tools/port3ds/gpu_cmd_diff.py build/hw-hang/hang.bin build/hw-hang/dump.bin -n 20`. In Azahar, the same `gpudrawcut=K` list from `tools/port3ds/gpu_cmd_diff.py --full` shows the register state at draw K-1.

Fill: **PORT_3DS_TESTING.md** "GPU hang" section (the culprit draw, its vertex/index range and margins, the table of K against result) and owner-questions 4b. Keep the log of every run (`build/hw-last/log.txt` is overwritten: copy it to `build/hw-hang/c<N>_K.log`).

## D. New defaults, A/B pairs, retail, heap (about 2 h 15 min, release CIA)

Release CIA is on the console from A4 (push it again if the dev CIA was started in between). All pairs use `hw_baseline.py` with the configs built into the tool. Scenes: `gor_01,pik_00,usu_00,battle` (core 2 is the limit in gor_01 and pik_00; usu_00 is light; battle is the regression case: C3 frame was 25 ms before the D2 change and the fence wait 5.4 ms). Windows end at 781, 841, 901; Azahar windows were 722-841: same steady state. Run-to-run spread of one config was 0.3-0.5 ms in Azahar; on the console the first pair of identical runs gives the noise: **repeat the `D` config once at the start** (run the command twice; each run has its own id). Take the same table row for each config; a difference below the noise is "none".

### D1. One pass over all configs (32 runs, about 55 min)

```sh
S=gor_01,pik_00,usu_00,battle
tools/port3ds/hw_baseline.py --scenes $S --configs D,C3,K,I,gpucore1,kg1,par,all
```

(Three further configs, 12 runs, about 20 min, in D2.) Read each pair as `B - A` in frame, core-0 busy, translator, render (ms), and whether the frame stays at the same vblank count:

| Pair | A | B | Question and Azahar result | Fill |
|---|---|---|---|---|
| 1 | `D` | `C3` (`gxthread=2 gxpipeline=1`) | does the translator thread on core 2 pay on the console (Azahar: gor_01 50 -> 33.5, pik_00 50 -> 33.4, battle 27 -> 25) and is core 2 the limit; fence times | PORT_3DS_GX_PERF.md gate tables; owner-questions 15 (flip `gxthread=2` as default) |
| 2 | `C3` | `K` (`gxone=1 gxchunk=1`) | Azahar: no gain (K core 0 35.7 vs C3 game 22.5): does one `GX_ProcessCommandList` per run and the frame list written on core 0 pay on the real command processor; item list below | PORT_3DS_GX_PERF.md "Gate results of step 3" timing table (add a console row) |
| 3 | `K` | `I` (`+ gximmfl=1`) | Azahar: core 0 -0.2..-1.1 ms, frame unchanged | "Gate results of steps 3 and 4" table |
| 4 | `C3` | `gpucore1` (`gpucore=1`) | render thread on core 1 (Azahar: render 7.4 -> 10.9, suspect because core 1 shares the log thread and GSP completions go to core 0 only); look at core-1 load (sd, log, wdog threads): the budget is about 80 % | PORT_3DS_GAMEPAR.md "Timing" table; owner-questions 6 |
| 5 | `K` | `kg1` | same for K | same table |
| 6 | `C3` | `par` (`jobw1=1 mapnameidx=1 mapdispjob=1`) | Azahar: about 0 (name index -0.25..-0.4 ms, mapDisp net 0); job worker 1 on core 1 round trip (`jobprobe=1 jobstats=60 gamecnt=120` in one extra run: mean us per wake) | PORT_3DS_GAMEPAR.md "Timing" table and job round trip line |
| 7 | `gpucore1` | `all` | the three together with the render thread on core 1 | same table |

Pairs 2 and 3 console-test items (GX_PERF.md, steps 3 and 4; Azahar proves none): (1) the frame list is written on core 0 and flushed by `C3D_FrameEnd(0)` on the render thread's core: a stale word shows as a wrong state or a GPU hang (gpudump does not contain these runs); (2) GSP and P3D interrupt cost of a handful of lists per frame; (3) the framebuffer flush at the end of every run with draws; (4) `gximmfl`: the index arena's physical base taken once per record: `gximmfl=2` compares exactly that. Run the verify modes (`gxchunk=2`, `gximmfl=2`) in gor_01 once on the console: `restart "fastboot map=gor_01 story=30 gxone=1 gxchunk=2 gximmfl=2 gxthreadstats=60 livelog"` until `gxthread 0 frame 901:`; pass: `differ 0 records over 0`, `mismatches 0 missed 0 stale 0`, no `verify` warning, no hang.

### D2. New defaults against the old, and the 10-scene `c3` (about 35 min)

```sh
tools/port3ds/hw_baseline.py --scenes gor_01,pik_00,usu_00,battle --configs old,noqtm,recheap     # 12 runs
tools/port3ds/hw_baseline.py --configs C3                                                          # 10 scenes with gxthread=2 gxpipeline=1
```

`old` = `gxdirty gxdirect gximmdirect gxsdl gxrecfuse gxreclgroup texasync texfast dlspread` off and `tex16=1`: against `D` it shows what the new defaults and exact textures (`tex16=0`, +40-50 % texture memory) cost or gain on the console. Check for `tex16=0` memory trouble: no `out of memory` or heap line near zero in the logs (the pik_00 heap check is D4). `noqtm` and `recheap` are QTM questions 2 and 3 (step G); take them here to save a pass. Fill: PORT_3DS_GX_PERF.md "Defaults on, exact textures"; PORT_3DS_QTM.md section 9 (rows 2 and 3).

### D3. `texdt` (about 20 min)

```sh
# verify: expect "texdt: made .. verify queued .. ok .. BAD 0" (all sizes), no "texdt: verify MISMATCH"
$D restart "fastboot map=gor_01 story=30 texdt=2 texstats=60 gxthreadstats=60 livelog"
python3 tools/port3ds/run_hw.py --no-send --until "texdt: made" --timeout 300
#   repeat with map=pik_00, map=usu_00, map=gon_02 battle=1 btlauto=perfect (BAD must stay 0 in all)
# engine on: map-load frame cost, texdt=0 against texdt=1 (texasync stays 1)
$D restart "fastboot map=gor_01 story=30 texdt=0 texframe=1 loadprof gxthreadstats=60 livelog"   # about 90 s, then the same with texdt=1
python3 tools/port3ds/hw_baseline.py --scenes gor_01,pik_00 --configs D     # steady state does not change; shown for completeness
```

Pass: `BAD 0`. If every size is BAD: run once with `texdtflip=0` (the flip convention; the `dt_tile_*` probe from A2 shows the right one). The warp is at about frame 595: compare the `[LOADPROF]` line, the worst frame time after the warp and the `texframe` conv/create columns for `texdt=0` against `texdt=1`. Azahar: about -10 ms per big map-load frame, barely used with texasync (`reserve_half`). Also `texdt=1` once in each of the four scenes with a shot compared by eye, or `determ` (`texdt=0` against `texdt=1` must be identical; `determ` shots go to `shots/`, `$D files get @/shots/...`). Fill: PORT_3DS_GX_PERF.md "`texdt`" measurement plan item 4 (add "Console results"); decides whether `texdt` becomes default.

### D4. Heap check, pik_00 C3 (about 4 min)

```sh
$D restart "fastboot map=pik_00 story=30 gxthread=2 gxpipeline=1 heaplog=60 gxthreadstats=60 livelog"
python3 tools/port3ds/run_hw.py --no-send --until "gxthread 2 frame 901:|CRASH" --timeout 300
grep -E "heap until map change|heap:" build/hw-last/log.txt | tail -20
```

The lines are `heap until map change from <map>: peak arena ... at least N KiB free` (each map change) and the periodic `heap:` lines. Pass: no crash, no allocation failure, and least free heap well above the old 384 KiB (Azahar 3dsx: 11.3 MB with heap 83 100 KiB, linear peak 28.2 of 32 MB, 4.5 MB free). A CIA on the console has its own heap size: compare the *free* number and note the total. Also run once with `texdt=1` to see if the 512 KB staging arena costs the linear margin. Fill: PORT_3DS_GX_PERF.md "Heap margin", "Results: least free heap" table.

### D5. Retail (about 15 min)

Three cells per scene (gor_01, pik_00, usu_00, battle), each with `bench=721,841` (mean us per game frame, game busy, render over 120 frames; the line `bench 721-841:` goes to `latest.log`). Retail mode (Debug mode off) reads only the switches in `retailkeep=`: `bench` and `devupdate*` (the server, its port and token) must be in it.

```sh
KEEP="retailkeep=map,story,party,battle,btlauto,fastboot,gx*,tex*,bench,devupdate*"
SC="map=gor_01 story=30"                                  # then pik_00, usu_00, "map=gon_02 story=30 battle=1 btlauto=perfect"
# cell 1: base, release CIA, retail=0
$D restart "fastboot $SC gxthread=2 gxpipeline=1 bench=721,841 livelog"
python3 tools/port3ds/run_hw.py --no-send --until "bench 721-841:" --timeout 300
# cell 2: same CIA, runtime retail=1 (log off: read latest.log afterwards)
$D restart "fastboot $SC gxthread=2 gxpipeline=1 retail=1 $KEEP bench=721,841 devupdate"
$D files get @/latest.log -o build/hw-retail/gor_01_retail1.log ; grep "bench 721-841:" build/hw-retail/gor_01_retail1.log
# cell 3: the TTYD_RETAIL build with the same switches
$D push build/3ds/game-retail/ttyd.cia --switches "fastboot $SC gxthread=2 gxpipeline=1 $KEEP bench=721,841 devupdate" --wait 120
$D files get @/latest.log -o build/hw-retail/gor_01_rbuild.log
```

Do cell 3 last in the whole session block (retail has no server on a HOME start); to leave it, `$D push build/3ds/game/ttyd.cia --switches "retailkeep=devupdate*" --wait 120` while it is still running (the release CIA then boots in the stored Debug mode, on after the one-time setup). If the log is empty, wait for the `bench` frame (about 90 s after the restart) and fetch again. Pass: frame time equal or better; game busy within 0.7 ms (Azahar: no fps gain, 0.0 to 0.65 ms busy; what retail buys is memory and the removed periodic hitches: the 0.45 ms `mallinfo` walks every 500 ms and 30 s). Note the hitches: in cell 1 `slow frame` lines in the log show them; in retail look at feel and, if wanted, `pmu` is off. Fill: **PORT_3DS_TESTING.md** "Debug mode" results table (add a "console" block) and the retail line in owner-questions.

### D6. Efb608, vramtex (optional, only with time; about 10 min)

`efb608` stays off until a console test: `restart "fastboot map=gor_01 story=30 efb608 gxthreadstats=60 shots=300 shotmax=2"`, then `files get @/shots/*.ppm` and compare with a run without it (QTM.md test items: sampling the 608-row target through the 1024-row view stays in VRAM bounds). `vramtex`: config `vramtex` of `hw_baseline.py` (QTM.md tests 5-7, step G).

## E. Updater screen, deterministic CIA install, supervisor (about 1 h 15 min)

### E1. Updater screen (10 min; also covers A1)

Use a dev or release CIA, any push, and watch the bottom screen.

```sh
$D push build/3ds/game/ttyd.cia --switches "devupdate fastboot map=gor_01 story=30" --wait 120    # normal: all phases
$D push build/3ds/game/ttyd.cia --bad-crc --no-relaunch --wait 60                                # refused: the error shows 3 s, then the old screen returns
$D restart "fastboot map=gor_01 story=30 devupdate"                                              # restart without a push: the screen shows relaunching
```

Pass: the screen shows `Updater`, `build <running> -> <incoming>`, the status line (receiving N of M KiB and KiB/s, checking CRC, installing, relaunching), the 30-block bar and the log below, still scrolling; after the refused push the earlier screen is repainted exactly and the game goes on; the top screen is greyed (the pause); the relaunched log has `updated from #N <hash>`. Fail: stuck bar, torn bottom screen, a greyed top screen that stays, `pause: the game thread is not at a frame boundary` with a hung GPU should still allow the push. Also push once from inside the Options menu and once during a map load (not at a frame boundary): the update must still complete (2 s bound). Fill: PORT_3DS_TESTING.md "ctr-devkit: crash and devupdate" (Updater screen paragraph: add "Console, <date>").

### E2. Deterministic CIA install (10 min; PC build first)

On the PC (no ccache: a warm ccache build is not identical):

```sh
tools/port3ds/build.sh --dev . -DTTYD_CIA_DETERMINISTIC=ON
cp build/3ds/game-dev/ttyd.cia build/ttyd_det_a.cia && rm build/3ds/game-dev/ttyd.cia
tools/port3ds/build.sh --dev . -DTTYD_CIA_DETERMINISTIC=ON
cmp build/3ds/game-dev/ttyd.cia build/ttyd_det_a.cia && echo identical
```

On the console (the option is stated "console install untested"; makerom signatures never verify, AM accepts the CIA with signature checks patched out):

```sh
$D push build/ttyd_det_a.cia --switches "devupdate fastboot" --wait 120       # install: AM must accept
$D push build/ttyd_det_a.cia --switches "devupdate fastboot" --wait 120       # the same CIA again: AM must accept the reinstall (same ticket and content ID)
$D push build/3ds/game/ttyd.cia --switches "devupdate" --wait 120             # a normal CIA over it
```

Pass: all three installs finish, the game boots each time, HOME Menu shows one ttyd title (no duplicate), `cmp` printed `identical`. Fail: an `am-commit` or `am-write` error, a duplicate title, a boot failure: report the `error` text and keep the option OFF (it is off by default). Reset the build cache afterwards: `tools/port3ds/build.sh --dev . -DTTYD_CIA_DETERMINISTIC=OFF`. Fill: the option text in `port3ds/cmake/TtydCia.cmake` ("console install untested") and CTR_DEVKIT_PLAN.md.

### E3. Supervisor install and tests H1-H6 (45 min; the owner at the console; may need a reboot)

One-time install (CTR_DEVKIT_PLAN.md "One-time console install"). The release CIA from A4 or the dev CIA, both have the Select menu with Debug mode on. Defaults stay as built (owner answered 2026-11-27): on demand only (no boot ExHeader override), `ndm_hold` off, watchdog off until the PC arms it, the CIA is a separate SD file.

```sh
$D files put build/3ds/supervisor/ctrdk-supervisor.cia            # -> sdmc:/3ds/ttyd/queue/ctrdk-supervisor.cia
# on the console: ZL+SELECT, R for the Debug tab, row "Supervisor: off", press A
#   expect: the row shows "running ok 00000000"; the log has "menu: supervisor install: ok 00000000"
$D supervisor ping                                                 # expect: pong supervisor ... state=running
```

Then the test plan (each a pass/fail with the `rc` from the reply):

| Test | Command | Pass |
|---|---|---|
| H1 | `$D supervisor ping` and `$D supervisor status` while the game runs | answers; `heartbeat` and `frame` rise once per second |
| H2 | ping at the HOME Menu, after the game is closed, and while hung: `$D restart "fastboot map=gor_01 story=30 gpuhangtest=600 wdog=60"` | answers; `state=` right in each |
| H3 | `$D restart "crashtest=segv crashwait=60"` | the supervisor still answers on the crash screen |
| H4 | after `gpuhangtest=600` (as H2): `$D supervisor terminate`, then `$D supervisor launch "fastboot map=gor_01 story=30"` | terminate `pm=0`; each GPU-reset step (`ResetGpuCore`, `AcquireRight`, ...) has a known `rc`; the next game reaches frame 1 |
| H4b | `$D supervisor watch 20` then `gpuhangtest=600`: the watchdog relaunches by itself (at most `max_relaunches`=2) | relaunch without a PC command; then `$D supervisor watch 0` |
| H4c | `$D supervisor push build/3ds/game/ttyd.cia` | terminate, install, CRC, launch all pass; the game comes back |
| H5 | `$D supervisor reboot` (the console restarts; last, it costs 1-2 min) | `NS_RebootToTitle` returns in the game; the supervisor answers within 90 s |
| H6 | Select menu row "Supervisor", A (shows `off`); `$D supervisor ping` must fail; reboot clean | uninstall works |

Optionally `$D loop manifest.json` with two entries (one `until` pattern, one timeout) to see the unattended runner end with `results.tsv`. Fill: CTR_DEVKIT_PLAN.md "Test plan" (results next to H1-H6) and "Only the console can show" (F13, F15, F16 outcomes); owner-questions 3 and 4.

## G. QTM placement tests (after A2: `cb_qtm`, `vb_qtm`, `ib_qtm` pass; about 30 min, CIA only: a 3dsx has no QTM)

Skip every row whose probe failed. The method and test numbers are PORT_3DS_QTM.md section 9 (one switch per run, same scene and window). Use the same `hw_baseline.py` windows as D, scenes `gor_01,battle,aaa_00`; the first pair of identical runs (D1) is the noise. Keep a result only if it is outside the noise in at least two scenes.

| QTM.md test | Config / switches | Needs | Read |
|---|---|---|---|
| 2 is QTM worth it | `noqtm` (config `noqtm`, run in D2) | - | busy, render against default |
| 3 the record alone | `place=gx_record:heap` (config `recheap`, D2) | - | as 2 |
| 4 the log alone | `place=log:heap` | - | expected: no difference |
| 5, 6 textures in VRAM | `vramtex=1 vramstats=60`; then `vramtexreserve=256`, `vramtexmax=512` | - | render, the `vramstats` line (bound KB from VRAM against linear, pressure 0), picture |
| 7 copy targets under the tier | `vramtex=1 fx=indirect` and the START menu in gor_01 | - | no `gpu: no VRAM` line after the first; `target pressure` count |
| 8 index arenas in QTM | `qtmgpu place=log:heap,gx_state:heap,audio_state:heap,gx_index:qtm` | `ib_qtm` OK | the picture first (`shots=300 shotmax=2`), then busy and render |
| 9 the GX ring in QTM | `gxthread=2 gxpipeline=1 gxthreadkb=256`, against the same with `place=gx_ring:qtm` | - (CPU only) | busy, translator |
| native frame list | `gxone=1 gxchunk=1 qtmgpu place=gx_frame_list:qtm` against `K` | `cb_qtm` OK (GSP's address check of a QTM list) | picture, hang lines, busy |
| native chunks/immediate ring | placements of PORT_3DS_GX_NATIVE.md section 4b rows 1-3 (frame list, immediate ring, class blocks) | `cb_qtm`, `vb_qtm` | as above; priority rows 1-2 |

Also the CPU side: from A3, QTM rows (`memcpy_qtm_*`, `memset_qtm_*`) against heap and linear tell whether QTM is faster or slower for the CPU. Fill: PORT_3DS_QTM.md section 9 (a result column) and section 7 budget; PORT_3DS_GX_NATIVE.md section 4b (the `Depends on` column).

## Session timetable and cut lines

| Block | Min | Needs the owner at the console |
|---|---|---|
| 0 PC preparation (builds, `CONFIGS`) | 30 | no |
| A1-A4 dev CIA, probe, calib, baseline | 75 | go; restart after a freeze |
| B hang survival | 15 | go |
| C hang bisect | 20-35 | go |
| D1-D2 A/B pairs and old/new | 85 | go |
| D3-D6 texdt, heap, retail, efb608 | 50 | go |
| G QTM | 30 | go, only after A2 |
| E updater, deterministic CIA, supervisor | 65 | yes (menu, reboot, power) |

About 5 h 30 min in all. With 2 h: PC prep, A1-A3, A4 (`default`), B, C1-C2, D1 with `default,c3,k` only. With 3 h: add C3, the rest of D1, D4, E1. Put E3 and the retail build (D5 cell 3) last: the supervisor reboot and a retail build both end in states that need a person.

## Result tables to fill (index)

| Result | File and section |
|---|---|
| gpuprobe rows, new group rows | PORT_3DS_GPU_RULES.md "New 3DS" columns; `gpu_cmd_diff.py` `PROBE_RULES` |
| calib, exact-math rows, calibpmu | PORT_3DS_60FPS.md 2a table (add the 11 exact-math rows), ratios |
| hw_baseline, c3, K, I | PORT_3DS_GX_PERF.md gate tables (steps 0-2, 3, 4), "Defaults on"; PORT_3DS_60FPS.md 2c |
| gpucore, jobw1, mapnameidx, mapdispjob | PORT_3DS_GAMEPAR.md "Timing" and job round trip |
| hang survival, hang bisect | PORT_3DS_TESTING.md "GPU hang survival" and "GPU hang"; owner-questions 4b, 4d |
| texdt | PORT_3DS_GX_PERF.md "`texdt`" measurement plan |
| heap | PORT_3DS_GX_PERF.md "Heap margin" |
| retail | PORT_3DS_TESTING.md "Debug mode" results |
| QTM | PORT_3DS_QTM.md section 9, 7; PORT_3DS_GX_NATIVE.md 4b |
| updater, deterministic CIA, supervisor | PORT_3DS_TESTING.md "ctr-devkit"; `TtydCia.cmake`; CTR_DEVKIT_PLAN.md "Test plan" |

After the session: restore the normal game (`$D restart ""`, or push the release CIA with `devupdate` in `debug3ds.txt` removed if the owner wants a clean console), copy `build/gpuprobe_hw`, `build/hw-calib`, `build/hw-baseline`, `build/hw-hang`, `build/hw-retail` to a safe place (they are in `build/`, which is not committed) and commit the filled tables.
