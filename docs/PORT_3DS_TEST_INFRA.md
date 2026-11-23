# 3DS port: test infrastructure (backlog lane A)

Deterministic runs, the GameCube debug DOL, the per-frame comparison against Dolphin, and the gallery switches. Backlog items A2, A4, A7 and the groundwork for A5 ([PORT_3DS_BACKLOG.md](PORT_3DS_BACKLOG.md)); plan step 7.1 ([PORT_3DS_PLAN.md](PORT_3DS_PLAN.md) section 10). How to build and run is in [PORT_3DS_TESTING.md](PORT_3DS_TESTING.md).

## Files

| File | Build | What |
|---|---|---|
| `port3ds/game/test_mode.h`, `test_mode.c` | both | `determ` (virtual clock, DVD drain, virtual retrace), `gwdump` |
| `port3ds/game/test_gallery.c` | both | `fx=`, `battle=` |
| `port3ds/game/test_battle.c` | both | `btlauto=`, `btlmoves=` |
| `port3ds/game/test_plat_port.c` | port | the test mode's switches, log, RNG state and extra keys on the port |
| `port3ds/gcdebug/gcdebug.c` | GameCube debug | the same on the GameCube: switches from memory, a log buffer, the port's `map=`, `fastboot` and `padscript=` |
| `port3ds/game/vi.c`, `os.c`, `ai.c`, `audio.c`, `msl.c` | port | the `determ` hooks: callbacks once per frame, the clock source, frame-paced sound, the sound start delay on the real clock, rand's state |
| `configure.py --gcdebug`, `tools/project.py` | GameCube debug | the second build (own folder, own ninja file) |
| `tools/port3ds/run_dolphin.py --gcdebug` | | boots the debug DOL, writes the switches, reads the log |
| `tools/port3ds/gw_diff.py` | | first divergent frame between two `gwdump` streams |

The shared files are C89 so MWCC compiles them. They hold the game-specific logic (the DVD manager queue, `GlobalWork`, effects, battles). The SDK files (`vi.c`, `os.c`, `ai.c`) only get generic hooks: a replacement clock source, frame-paced audio, callbacks run once per frame (portability rule, plan section 1; each hook is listed below for GC3DS_LIBRARY_NOTE.md).

## Deterministic mode (`determ`, A2)

Game time advances exactly one retrace per game frame: 1/60 s (675000 ticks), or 1/50 s when `VIGetTvFormat` is PAL. Wall time does not enter game state:

| Source of run-to-run difference | In `determ` |
|---|---|
| The retrace callbacks (`viPostCallback`: `retraceCount`, `renderTime`, `systemLv*Time`, `playTime`, the soft-reset timer, `psndMainInt`) ran on the vblank thread, 0 to 2 times per game frame | They run once per game frame, on the game thread, right after the retrace wait in `VIWaitForRetrace`, with a virtual retrace count (one per frame). The game-time budget hands out exactly one retrace |
| `OSGetTime`/`OSGetTick` in game code (the clocks above, `deltaGame`, the logo and kiosk idle timers, the lottery's date, save timestamps, effect animation start times) | A virtual clock: 2004-07-22 12:00 plus one retrace per virtual retrace, plus one tick per call within the retrace on the same OS thread (so a busy-wait on the clock, as in `sysWaitDrawSync`, still ends). The platform keeps the real clock (`port_os_real_time`) |
| DVD manager reads (`dvdmgr.c`) finished whenever the file thread got to them, so a load could end one frame or another | `VIWaitForRetrace` waits (more retraces, no game frame) until the DVD manager's queue has no read left, so every load ends before the frame after the one that queued it |
| Sound: MusyX's interrupt ran on the audio thread at the output rate; sound state (`psndSFXChk`, stream positions) moved with wall time | The AI DMA interrupt runs on the game thread, 32000/60 samples (3 or 4 quanta of 5 ms) per frame; the output plays silence |
| Port-side randomness | None found: the game's `rand` is MSL's generator seeded with 1 (`msl.c`), and nothing in the game calls `srand`. The port has no other random source that reaches game state |
| The RTC | Read once at boot for `OSGetTime`'s base; in `determ` the base is the fixed date |
| Input | `padscript=` steps count game frames; after the script, Azahar has no input |

### `gwdump=N`

Every N game frames (only from frame `gwdumpfrom=F` to `gwdumpto=T` when given), one record, written as three log lines with the same `f` (the port's log lines hold about 250 characters; `gw_diff.py` joins them):

```
gwdump f=900 wf=296 seq=2 map=gor_01 gsw0=30 gswf=... gsw=... lswf=... lsw=... evtgw=... evtgf=... evts=256
gwdump f=900 gwv=6:P,9:00000001
gwdump f=900 mario=<x>,<y>,<z> rng=... rc=899 lv=0 inbtl=0 btl=-1,-1 rt=... ft=... pt=... btn=... stick=0,0 gw=...
```

`f` game frame, `wf` frames since the first map change (the warp), `seq` sequence, GSWF/GSW/LSWF/LSW as FNV-1a hashes (word arrays hashed by value, so the hash is the same in both byte orders), `evtgw`/`evtgf` the event system's GW and GF (GW pointers counted as one value, since they differ between builds), `gwv` the non-zero GWs (pointers as `P`), `evts` event slots, Mario's position as float bits, `rng` MSL's generator state, `rc` retraceCount, `lv` systemLevelFlags, `btl` battle turn and active unit, `rt`/`ft`/`pt` renderTime, renderFieldTime and playTime, `btn`/`stick` pad 0, `gw` an FNV-1a hash of all of `GlobalWork` as raw bytes (build-specific: pointers and byte order).

### Proof

All in Azahar, two runs each with the same switches (2026-11-22):

| Run | Switches | Result |
|---|---|---|
| `gor_01` and a 1500-frame padscript (650 frames of boot, then walking with the stick, A and B) | `map=gor_01 story=30 determ gwdump=10 padscript=wait*650,cr*100,wait*20,cu*80,a*3,wait*40,cl*120,cd*60,a*3,wait*50,cr*90,b*2,wait*30,cu*70,wait*20,cl*50,a*2,wait*110`, to frame 1600 | 160 records equal, frames 10 to 1600, all 21 fields including the raw `GlobalWork` hash. Mario's position, the RNG and the event GWs change along the way |
| Map changes: the title, then `gor_01`, `gor_02`, `gor_00`, `gor_03` (`sweep.txt`, 200 frames each) | `sweep=200 determ gwdump=10` | 162 records equal, frames 10 to 1620, 23 fields |
| A battle | `map=hei_01 story=30 battle=4 btlauto=perfect determ gwdump=10`, to frame 3000 | 370 records equal, frames 10 to 3700, 23 fields: the warp, `battle: start 4 (hei01 Goomba)` at frame 704, four menu turns with perfect action commands, `battle: end result 1 after 2836 frames`, back in the field |

## GameCube debug DOL (A4)

```sh
python3 configure.py --version G8ME01 --gcdebug    # writes build_gcdebug.ninja; build.ninja is untouched
ninja -f build_gcdebug.ninja                       # build/gcdebug/G8ME01/MarioSt.dol, .elf and the 29 RELs
tools/port3ds/run_dolphin.py --gcdebug --switches "fastboot map=gor_01 story=30 determ gwdump=10 stopframe=900" \
    --break gcdbg_stop --read-log --json build/dolphin-last/gcdebug.json
```

- **Separate from the matching build.** Its own build folder (`build/gcdebug/`), its own ninja file, no `objdiff.json`, no progress or SHA-1 step; it uses the compilers and tools of `build/`. `tools/project.py` got two settings for that (`ninja_file`, `generate_objdiff`), with the old behaviour as default. The matching build reports the same numbers before and after (below).
- **How the switches get in.** The game code is compiled with `OSGetTime`, `OSGetTick`, `VIWaitForRetrace`, `VISetPre/PostRetraceCallback`, `PADRead`, `rand` and `srand` renamed (`-D`) to the `gcdbg_*` functions of `port3ds/gcdebug/gcdebug.c`, which is compiled without the renames and calls the real SDK. The SDK, MSL and MusyX keep the real functions. No source file in `src/` changed. The calls keep their size, so the game code is the same apart from branch targets.
- **Switches** are the text in `gcdbg_switches` (1 KB), which `run_dolphin.py` writes through the GDB stub before the first continue. The test log goes to `gcdbg_log` (192 KB; `gw_diff.py` warns when it filled), which it reads at the end (`--read-log`). `stopframe=N` calls `gcdbg_stop` at game frame N, the breakpoint to stop at, so the run goes at full speed until then.
- **Same behaviour as the port:** `map=` (with `story=`, `party=`), `fastboot`, `padscript=` (the port's parser, and the port's `pad.c` mapping into `PADStatus`: stick 104 at full tilt, analog A/B and triggers 0xFF), `determ`, `gwdump=`, `fx=`, `battle=`, `btlauto=`.
- **The RELs.** The disc's RELs are relocated against the retail DOL's addresses, so the debug DOL must load its own. `run_dolphin.py --gcdebug` extracts the image once with `dolphin-tool` into `build/gcdebug-disc/` (590 MB), copies the debug DOL and RELs over `sys/main.dol` and `files/rel/`, and boots that `sys/main.dol` (Dolphin boots the folder as a disc).

Checked (2026-11-22):

- The matching build before and after these changes: `30 files OK`, and the same progress report (All 99.89%, 1225 of 1226 files; DOL 629 of 630; Modules 596 of 596; Game Code 100%, 8283 of 8283 functions; SDK 100%; MusyX 30 of 31 files, 323 of 367 functions).
- The debug DOL builds (`build/gcdebug/G8ME01/MarioSt.dol`, 3.9 MB, and the 29 RELs) and boots in headless Dolphin through `run_dolphin.py`: GDB stub, switches written before the first continue, the log read at `gcdbg_stop`. A run to frame 1600 takes about 70 s at unlimited speed (one breakpoint, at the end).
- With `fastboot map=gor_01 story=30 determ gwdump=30` the GameCube and the port warp at the same game frame (604), with the same RNG state, retrace count and clocks in every record up to the warp and after it.

## Per-frame comparison (`gw_diff.py`, A7)

```sh
tools/port3ds/gw_diff.py --logs build/az-a/game_log.txt build/az-b/game_log.txt      # two existing logs
tools/port3ds/gw_diff.py --switches "map=gor_01 story=30" --padscript "wait*700,cr*60" --frames 1200 --every 10
```

The second form runs the port in Azahar and the debug DOL in Dolphin with the same switches plus `determ gwdump=<every>`, and compares the streams. It prints the first record whose fields differ, with both values of each differing field, and the last equal record. `--align warp` matches records on the frames since the warp instead of the frame number. Between builds the raw `gw` hash is not compared.

Checked (2026-11-22), `map=gor_01 story=30` and the A2 padscript, 1600 frames, a record every 10:

- **Port against Dolphin: 160 records equal**, frames 10 to 1600, 22 fields (all but `gw`), repeated with the final build. The boot, the warp, the map load and the walking agree frame for frame between the 3DS port and the GameCube code.
- On the way there the tool found one difference at frame 660, in `evtgw`: event GW 6 holds a pointer (`0x08974e60` on the port, `0x80d81ca0` on the GameCube). `gwdump` now writes GW pointers as `P` (`gwv` lists the non-zero GWs), and the streams agree.
- **A deliberate divergence** (a scratch debug DOL, not committed: `gcdbg_rand` flips a bit of the RNG state at frame 1000) against the same port log: `first difference at f=1010 ... last equal f=1000`, field `rng` (`d15ec385` against `2762c385`).
- **Through a battle** (`map=hei_01 story=30 battle=4 btlauto=perfect`, 3700 frames): equal up to frame 760, then `first difference at f=770`, field `rng`. A second pair of runs with `gwdump=1 gwdumpfrom=755 gwdumpto=772` puts it at **frame 767**, and the RNG states show what happened: both builds make the same 91 `rand` calls in frame 766 (the battle's set-up), then the GameCube makes the next call in frame 767 and two more in frame 769, the port the same calls two frames later. So the port runs this step of the battle start two frames late, with the same sequence of random numbers. From there the battle drifts (the port's third menu turn opens at frame 2070, the GameCube's at 2051; the port's battle ends after 2836 frames, the GameCube's after 2816). The cause is open (backlog lane B); the first suspects are the waits that the two builds pace differently, sound (the battle music and sound groups) and ARAM (see the risks below).

## Galleries (A5 groundwork)

| Switch | Effect |
|---|---|
| `fx=<name\|index>[,...]` | Spawns each effect through `evt_eff` (`n64:<name\|index>` through `evt_eff64`), as event scripts do, `fxwait=N` frames (default 60) after the warp's map is up, `fxgap=N` (default 120) apart. Names and indices are those of `eff_set_table` and `eff_set_n64` (`effdrv.c`, `eff_n64.c`). Default arguments: type 0, Mario's position 30 units up, then 1.0; `fxargs=a,b,...` gives the 13 script arguments (each passed as a script float, which reads as an int where the effect wants one). Logs the spawn and whether the effect is alive 2 and `fxgap` frames later |
| `battle=<id\|name>` | Starts the battle of entry `<id>` (or that name) of the current area's battle setup table (`areaDataPtr(area)->btlSetupTbl`, numbered as `npcSetBattleInfo` does) `battlewait=N` frames after the map is up, as `evt_npc_battle_start` does, with the first NPC on the map (an enemy if there is one) as the field enemy. An unknown id logs the table. Logs the start and `battle: end result R` |
| `btlauto=perfect\|fail` | Runs battles unattended. While the command menu is up it presses the turn's keys from `btlmoves=` (default A: the menu's first action, first move, first target), one every `btlstep=N` frames (default 10). `perfect` gives Mario's side the game's Auto Command badge (every action command and guard succeeds; Superguards where allowed); `fail` takes it away and presses nothing during action commands or enemy moves. During the end of the battle (star points, a level-up) and whenever a message window has its page out (msgdrv state 3, e.g. an enemy talking, also while a guard command is armed) it presses A. Every 600 frames without the menu it logs the active unit, the action-command state and the 7 window slots |
| `btlmoves=<turn>,<turn>,...` | Per command-menu opening, a `.`-separated key list (`a b x y l r z start up down left right`), e.g. `right.a.a`. After the list, A |

Checked (2026-11-22), the same switches in both builds:

- `fx=kemuri,1,n64:sweat_n64,nosuch fxgap=90` on `gor_01`: three spawns at frames 727, 817 and 907 (60, 150 and 240 frames after the map came up), events 66 to 68, the same frames and event ids in Azahar and Dolphin; all ended 90 frames later; in Dolphin (the 2-frame check came after the Azahar run) `kemuri` and `sweat_n64` were alive 2 frames after the spawn and `confetti` already gone; `nosuch` logged as not an effect.
- `battle=0` on `gor_01` (`GOR` test battle 0, against `gor_02_01_off_1`) and `battle=4` on `hei_01` (`hei01` Goomba): the battle starts at the same frame in both builds, the menu turns, action commands and enemy turns follow at the same frame offsets (`gor_01`: start 727, turn 1 at 913, its action command 958 to 1099 with result 2, in both builds). With the message-window fix the `hei_01` battle runs to `battle: end result 1` in both. `btlauto=fail` gives action command result 0 every time.

## Screenshot galleries (A5)

```sh
tools/port3ds/gallery.py fx --items 0-80,n64:0-102 --name fx_all            # every effect on gor_01
tools/port3ds/gallery.py battle --map hei_01 --items 0-3 --offsets 120,400 --name hei
tools/port3ds/gallery.py frames --switches "map=gon_02 story=30" --frames 648,664,688 --name gon02
tools/port3ds/gallery.py report --name fx_all                               # redo the report only
tools/port3ds/gallery.py battlelist --name btllist                          # every area's battle setup table
tools/port3ds/gallery.py battlesweep --name btl_perfect --mode perfect --per-area 2 --gwdump 20
tools/port3ds/gallery.py fx --items 0-80,n64:0-102 --name fx_all --gc-only  # Dolphin side ahead, then --reuse-gc
```

Output: `build/gallery/<name>/index.html` (items sorted by score, port | GameCube | difference x4), `results.json` (per item: the shots, mean/max difference, share of pixels over 32, the log lines: spawn, alive checks, `TEV:`/`GX:` warnings, crashes), `port/<batch>/`, `gc/<batch>/`, `diff/`, `logs/` (each run's game log; `hw_faults.json`: Azahar's unmapped accesses per PC, which are counted, not a failure).

**Score.** The plain port/GameCube difference of a shot is mostly the scene's (texture filtering, edges: about 4 to 7 of 255 on `gor_01`). Every `fx=` batch shoots the same frames of the same scene, so for each frame the median difference over all items shot there is the scene's, and an item's excess over it is the effect's. The score is the worst shot's mean excess, less the median score of its batch (a batch whose scene drifted, e.g. an NPC elsewhere, otherwise raises all its items alike). Items over 0.5 are marked for review.

**Shots in both builds** (shared test code, `test_gallery.c`):

| Switch | Effect |
|---|---|
| `shotat=F,F,...` | Shots of these game frames |
| `fxshots=O,O,...` | Shots O frames after each `fx=` spawn |
| `btlshots=O,O,...` | Shots O frames after each `battle=` start |
| `fx=0-80,n64:0-102` | `fx=` also takes index ranges (up to 128 effects per boot) |
| `battle=0-5,7` | Several battles per boot, one after another, HP and FP refilled before each |
| `btllist` | 30 frames after each map is up: `btllist: map M npcs N enemies E`, and once per area its battle setup table (with `sweep=`, every area in one run) |

A shot of frame F is the picture finished at F's frame hook (the start of `VIWaitForRetrace` in frame F, after `DEMODoneRender`'s draw sync and before the buffer flip). Both builds log `shot: f=F`.

- **Port:** at F-1 the test code waits for the render thread (`plat_gpu_sync`) and asks for a named shot (`plat_gpu_shot`), so the next present, frame F's, is saved: `gallery/fFFFFF.ppm`, the EFB, 608x480.
- **GameCube:** at F the test code calls `tst_shot_point(F)`. `run_dolphin.py --shots DIR` stops there, reads `DemoCurrentBuffer` and the XFB it points to (YUYV 4:2:2, 608x480) and saves `DIR/fFFFFF.png`.

**Dolphin without a window.** The Null backend draws nothing, so `--shots` uses `-v Vulkan` with `-p headless` (no surface, it renders offscreen; no Xvfb needed) and:
- XFB copies written to emulated memory (`XFBToTextureEnable=False`), not deferred (`DeferEFBCopies=False`), no copy filter (`DisableCopyFilter=True`, the vertical deflicker), native resolution, single core;
- these hacks also go into the profile's `GameSettings/G8ME01.ini`, because TTYD's own game INI is a higher config layer than `-C` and turned the XFB copies back into textures (the first try read an untouched XFB);
- the XFB (570 KB) is read with pipelined `m` packets parsed a buffer at a time (`RSP.read` parses byte by byte). A `gor_01` run to frame 900 with 6 shots takes 45 s.

**Alignment.** No cropping or scaling: `fbWidth` 608 and `efbHeight` = `xfbHeight` = 480 in the US modes, so the XFB is the EFB 1:1. The XFB is YUV 4:2:2, converted back with the inverse of the GameCube's copy (BT.601, studio range): about 1 to 2 levels of colour error and half the horizontal chroma resolution. Checked on `gor_01` at frame 700: best offset (0, 0) of all shifts within 2 pixels, mean difference 6.1 of 255 (4.3 inside a 4-pixel border), 2.8% of pixels over 32 (edges and texture filtering). PAL's 50 Hz mode stretches 480 lines to 568 (`--xfb-size 608x568`, then scale).

**Azahar's black shots.** With `shots=N`, Azahar sometimes saves a single-colour shot of a frame that drew normally. The gallery's shots (taken after `plat_gpu_sync`) showed none in 368; a single-colour shot would be marked and left out of the score.

**Speed.** Azahar (OpenGL on Xvfb, software GL) runs the game at about 20 to 40 game frames per second; its Vulkan renderer aborts at boot under Xvfb (status 134). An `fx=` batch of 24 effects, 60 frames apart, takes about 5 minutes in Azahar and 4 in Dolphin with 48 shots.

### First results (2026-11-22)

**Effects (B4)**, `tools/port3ds/gallery.py fx --items 0-80,n64:0-102 --name fx_all`: all 184 (81 of `eff_set_table`, 103 of `eff_set_n64`) on `gor_01`, story 30, default arguments, 8 boots of 24, shots 6 and 20 frames after each spawn.
- Every effect spawned in both builds, no crash, no hang, no unmapped access. The alive checks agree between the builds for all 184: 103 entries are gone 2 frames after the spawn with the default arguments (type 0 at Mario's position), 43 still alive after 60 frames (deleted then).
- `TEV: ... approximated` warnings, first seen at: `damage_star`, `fire`, `stageclear` (3 stages, TEV compare op), `bomb` (3 GX stages to 5 PICA stages, compare op), `fire` (4 to 6, compare op), `explosion_n64` (more than 6 combiner stages), `flame_n64` (combiner buffer unavailable for a two-step stage). No `GX:` warning after the map was up.
- Over the review threshold: `bomb` (score 5.1: the port draws its smoke puffs dark grey on black squares, the GameCube white), `flame_n64` (4.6: a black haze where the GameCube has a pale yellow one), `itemget` (1.6: the halftone background opaque white instead of tinted), `freeze_n64` (0.8), `stageclear` (0.8: its letters dark grey instead of green). The first two and the last are effects with a TEV approximation warning. Everything else is at most 0.3.

**`gon_02` struck-first frame** (`gallery.py frames`-style run, `map=gon_02 story=30`, shots every 8 frames): the bottom 57 rows (423 to 479) are black in frames 648 to 680 and half of them at 688 in **both** builds, the same picture (mean difference 5 to 10, all of it texture filtering); the bar is gone from 696 on. `gw_diff.py` over the two `gwdump=4` streams: 275 records equal, frames 4 to 1100 (Mario's position in both: 0, 0, 0 until the battle at 740). `gwdump` has no camera field.

**Battle tables**, `gallery.py battlelist` (a `sweep=40 btllist` run over the 282 maps that are not `stg_*`; 274 visited `ok`): 19 areas have battle setups, 382 in all: `aji` 23, `bom` 7, `dou` 27, `eki` 19, `gon` 21, `gor` 6, `gra` 13, `hei` 27, `jin` 25, `jon` 52, `las` 60, `moo` 16, `mri` 21, `muj` 15, `nok` 1, `pik` 10, `rsh` 2, `tik` 29, `win` 8. `aaa`, `hom`, `kpa`, `tou`, `usu`, `yuu` have an empty table; `bti`, `end`, `sys` no map with an NPC. That run also logged 163,862 unmapped 32-bit writes in Azahar, all in `animPoseMain` (`animdrv.c:4448` `pose->rawFrame`, and 4696; addresses 0x0 to 0x13C, so a NULL `AnimPose`), from about 100 s in to the end. The same sweep of the first ten maps (`aaa_00`, `aji_00` to `aji_08`) did not reproduce it, so the map is not known yet; the fx and battle runs had none. In each area the sweeps use the first map with an enemy NPC (`aji_01`, `bom_01`, `dou_02`, `eki_02`, `gon_04`, `gor_00`, `gra_03`, `hei_01`, `jin_01`, `jon_00`, `las_01`, `moo_01`, `mri_05`, `muj_02`, `nok_00`, `pik_00`, `rsh_02_a`, `tik_01`, `win_00`).

**Battles (B3)**, `gallery.py battlesweep`, port only, `determ`, the default move every turn (the menu's first action and target). Battle results: 1 won, 3 Mario's side defeated, 5 interrupted (`battle_seq_end.c`).

- `btlauto=perfect`, the first and last setup of each area (37 battles, `--per-area 2`, 2 per boot, `gwdump=20`): 28 ended (22 won; 5 lost: `gor` 0 and 5, `bom` 6, `jon` 51, `muj` 0; `nok` 0 interrupted after 364 frames, no turn), 9 did not end in the time given (about 10000 to 22000 frames), all still taking turns: `gon` 20 (Hooktail, 56 turns), `las` 0 and 59, `mri` 20, `muj` 14, `pik` 9, `rsh` 0 and 1, `tik` 28. Every action command result 2. No crash, no hang, no unmapped access, no `GX:` warning; one `TEV: 3 GX stages -> 3 PICA stages approximated: TEV compare op` per boot (the battle's first draw of that setup).
- `btlauto=fail`, the first setup of each area (19 battles, one per boot): 18 ended (7 won, 10 lost, `nok` 0 interrupted), `jin` 0 still taking turns after 24; every action command result 0. No crash, hang or unmapped access. `muj` adds the `fire` effect's two TEV approximations.
- Full tables: `build/gallery/btl_perfect/table.md`, `build/gallery/btl_fail/table.md`.

**Battle start against the GameCube**, the first battle of each area's perfect run against the same switches in the debug DOL (Dolphin, `gwdump=20` to frame 4500): the battle starts at the same frame in all 19 areas. The first differing record (always `rng` first):
- shortly after the start, frames 780 to 820, in 8 areas: `aji`, `gra`, `hei`, `jin`, `mri`, `moo`, `nok`, `win`;
- later in the battle in 6: `pik` 1160, `bom` 1220, `las` 1620, `dou`, `eki` and `jon` 2520;
- not at all up to frame 4500 in 5: `gon`, `gor`, `muj`, `rsh`, `tik` (the same menu turns, frame for frame).

Frame by frame (`gwdump=1`, frames 700 to 830): `hei` first differs at 768 (as before), `aji` at 794, `mri` at 803, `gor` and `tik` not at all. Only `hei` shows the clean pattern of the earlier analysis (the same states two frames later); in `aji` and `mri` some of the GameCube's states never appear in the port's stream, so the port makes a different number of `rand` calls there, not only later ones. So the battle-start divergence is not everywhere: it depends on the battle (the enemies' set-up), which points at an enemy- or stage-specific wait rather than at the battle start itself.

## Risks to determinism not removed

Found, not removed (the runs above show none of them in their streams, but each can move a stream):

- **Sound on the GameCube side.** The debug DOL keeps the real AI interrupt (Dolphin's emulated 32 kHz), while the port's `determ` runs 3 or 4 quanta per frame on the game thread. Game state that follows sound (an event waiting for a sound effect or a stream position) can differ between the two builds, though not between two runs of one build.
- **Loads end in the frame that queued them** (`tst_dvd_busy`) in both builds. Retail timing has longer loads, so `determ` timing differs from the original's around map changes. ARAM transfers (`ar.c`, owned elsewhere) and memory card operations are not drained; on the port both finish at once, on the GameCube they are asynchronous.
- **Retrace callbacks run at a fixed point** (after the retrace wait) rather than as an interrupt; another OS thread (the DVD check thread, the DVD manager) no longer sees a callback in the middle of its work.
- **Within-frame clock values** are the frame time plus one tick per call on the same OS thread. A build-specific clock read on the game thread shifts the game's later reads in that frame (the reason for the `audio.c` change); a new SDK function that reads `OSGetTime` on the game thread would do it again.
- **The port's log drops lines** when its ring is full (`plat_vlog`), and a dropped `gwdump` line is a missing record, which `gw_diff.py` skips instead of reporting. Records every 10 frames never hit it here.
- **Input after the padscript** is the real input: none in Azahar's headless runs, the buttons on a console.
- **Saves.** The port's SD save files and Dolphin's memory card start from different contents; a test that loads or saves will differ between builds.
- **The alarm thread** (`os_misc.c`, the kiosk idle timer of `seq_e3.c` only) compares virtual alarm times against the virtual clock from a host thread.

## Machine-wide limits (emulators and builds)

Agents share one PC, so the run and build scripts cap how much runs at once and keep the emulators from starving everything else:

| Variable | Default | Effect |
|---|---|---|
| `AZAHAR_MAX` | 3 | Azahar instances at once (`run_azahar.py`, flock slots in `$XDG_RUNTIME_DIR/ttyd-azahar`) |
| `DOLPHIN_MAX` | 2 | Dolphin instances at once (`run_dolphin.py`, flock slots in `$XDG_RUNTIME_DIR/ttyd-dolphin`) |
| `BUILD_MAX` | 2 | `build.sh` builds at once (flock slots in `${XDG_CACHE_HOME:-~/.cache}/ttyd3ds`); a waiting build prints a line |
| `BUILD_JOBS` | 8 | Ninja parallelism inside the build container (`cmake --build -j`) |

Both emulators start with core dumps disabled (`RLIMIT_CORE` 0, so systemd-coredump keeps nothing) and at `nice` 10. A waiting slot polls every 5 s.

## Library hooks (for GC3DS_LIBRARY_NOTE.md)

- `port_os_set_time_source(fn)` / `port_os_real_time()` (`os.c`): a replacement for the game's clock; the real clock for platform code.
- `port_ai_set_frame_paced(on)` / `port_ai_pump(quanta)` (`ai.c`): run the AI DMA interrupt from the caller's frame instead of the output clock.
- `VIWaitForRetrace` in a test mode (`vi.c`): the retrace callbacks once per game frame on the game thread, after a game-supplied "loads pending" check (`tst_dvd_busy`).
