# 3DS port: building, running and testing

How to build the 3DS port and run it, the GameCube original and the paper-cut PC port, from scripts. Every command here is meant to be run exactly as written from the repository root, by a person or an agent, and returns a non-zero exit code on failure.

The plan is in [PORT_3DS_PLAN.md](PORT_3DS_PLAN.md).

## One-time setup

| What | How |
|---|---|
| Disc image | Copy your G8ME01 disc image to `iso/game.iso` (ignored by git). G8MJ01 and G8MP01 come later (plan phase 7) |
| Docker | Docker Engine on Linux, Docker Desktop on Windows. The build image is built automatically on first use |
| Azahar | Linux: Flatpak `org.azahar_emu.Azahar`. The runner uses its own profile and never changes your Azahar settings |
| Dolphin | `dolphin-emu-nogui` on `PATH` (Debian/Ubuntu package `dolphin-emu`) |
| GameCube decomp build | `python3 configure.py --version G8ME01 && ninja`, with `orig/G8ME01/game.iso` a symlink to `../../iso/game.iso`. Must end with `30 files OK`. Gives `build/G8ME01/MarioSt.elf`, whose symbols the Dolphin runner uses |
| paper-cut (reference) | Clone `https://github.com/LLCOOLT/paper-cut` next to this repo and build it as shown in `tools/port3ds/run_papercut.py --help` |

## Building

```sh
tools/port3ds/build.sh probe          # Linux: port3ds/probe -> build/3ds/probe/ttyd_probe.{3dsx,cia}
```

```powershell
powershell -NoProfile -File tools\port3ds\build.ps1 -Project probe   # Windows, same outputs
```

Both scripts build in the image from `tools/port3ds/docker/Dockerfile`:

- devkitARM pinned by digest (GCC 16.1.0, libctru 2.7.0, citro3d 1.7.1);
- makerom 0.19.0 and ctrtool 1.3.0, built from pinned source;
- bannertool 1.2.0.

The image tag is the first 12 hex digits of the Dockerfile's SHA-256, so editing the Dockerfile rebuilds the image.

On Windows the sources are copied into the Docker volume `ttyd3ds-work` with rsync and built there, because bind mounts are slow. `iso/`, `orig/`, `build/` and `.git/` are not copied.

Check a CIA's ExHeader (memory mode, clock, L2, core 2, QTM mapping):

```sh
docker run --rm -v "$PWD:/work" ttyd3ds-build:<tag> sh -c \
  'cd /tmp && ctrtool --contents=c /work/build/3ds/probe/ttyd_probe.cia' | grep -E 'Mapping|New3DS|L2|Core 2'
```

## Running in Azahar

```sh
tools/port3ds/run_azahar.py --build build/3ds/probe/ttyd_probe.cia \
    --switches autoexit --log 3ds/ttyd/probe.txt --until "PROBE DONE" --timeout 60
```

- **Profile.** Uses an isolated profile in `build/azahar-profile/` (config, NAND and SD card) with New 3DS mode on, no frame limit and no vsync.
  - Flatpak forces its own `XDG_*` variables, so the runner re-exports them inside the sandbox.
  - Builds must be under your home directory, because the Flatpak cannot see `/tmp`.
- **3dsx or CIA.** A `.3dsx` is booted directly. A `.cia` is installed with `azahar -i` and its installed `.app` is booted, so the CIA's ExHeader applies (QTM mapping, 804 MHz, core 2). A 3dsx gets no QTM mapping in Azahar.
- **Switches.** `--switches` is written to `sdmc:/3ds/ttyd/debug3ds.txt`.
- **When it stops.** It tails the game log (`--log`, relative to the SD card root). It stops on:
  - `--until` (pass);
  - a `[CRASH]` line;
  - `--stall` seconds without log growth;
  - `--timeout`;
  - Azahar exiting.
- **Results.** Azahar is always stopped afterwards. Logs are copied to `build/azahar-last/`. Azahar log lines that would fault on hardware (unmapped access, aborts, `svcBreak`) fail the run unless `--allow-hw-faults` is given.
- **Headless.** Azahar runs on a private Xvfb display (started and stopped by the run, software GL), so no window appears; `--show` uses the real display. `--profile DIR` and `--last DIR` let a second instance run beside another; the runner stops only the Azahar it started, and at most `AZAHAR_MAX` (default 3) instances run on the machine at once (slot files in `$XDG_RUNTIME_DIR/ttyd-azahar/`).
- **Debugger.** `--gdb` turns on Azahar's GDB stub (port 24689). The guest then waits for a debugger; use `tools/port3ds/gdbrsp.py` or `arm-none-eabi-gdb`.

### The game in Azahar

The game reads the disc image from the emulated SD card. Link it there once (a hard link: no second copy of 1.4 GB, and visible inside the Flatpak):

```sh
mkdir -p build/azahar-profile/data/azahar-emu/sdmc/3ds/ttyd
ln -f iso/game.iso build/azahar-profile/data/azahar-emu/sdmc/3ds/ttyd/game.iso
tools/port3ds/build.sh
tools/port3ds/run_azahar.py --build build/3ds/game/ttyd.cia --log 3ds/ttyd/log.txt \
    --switches "map=gor_01 shots=100 shotmax=40" \
    --until "shot shots/04000" --timeout 500 --stall 0
```

Switches the game reads (space-separated words in `--switches`):

| Switch | Effect |
|---|---|
| `map=<map>[,<bero>]` | Starts a new game on that map once the title or the opening is up (`fastboot` presses A through the warning screen). `story=N` sets the story so far (GSW 0, default 0); `party=N` the partner (default 1, Goombella; 0 none) |
| `fastboot` | Presses A every 6 game frames (held 2) through the boot sequences before the title (the warning screen, the logos), and stops for good once the title sequence starts or a `map=`/`sweep=` warp begins. `run_azahar.py`, `run_hw.py` and `map_sweep.py` add it by default; pass `nofastboot` (`--nofastboot` for `map_sweep.py`) to leave it out |
| `crashtest=<kind>` | Tests the crash handler (`crashwait=N` seconds on the crash screen, default 300; `crashframe=N` game frame of the test, default 60; `wdog=N` hang limit in seconds). Kinds, run on the game thread at that frame: `segv` (write to address 0), `fatal` (`plat_fatal`), `break` (`svcBreak(USERBREAK_PANIC)`), `hang` (endless loop, the watchdog fires after `wdog`). At boot: `self` (the handler called with made-up registers), `null` (write to `crashaddr`, default 4), `undef`, `boot-fatal`. Azahar never raises a fault for an unmapped access, so `segv` only logs `UnmappedAccess` there; use `self` for the exception path |
| `padscript=<steps>` | Scripted input: comma-separated `KEYS*frames` steps (`a*3`, `wait*40`, `start*3`, `a+b*2`). Frames are game frames (one per VIWaitForRetrace). Keys: `a b x y l r zl zr start select up down left right`; `cl cr cu cd` push the circle pad (the game's control stick) fully left, right, up or down. TTYD's menus and dialogs move with the stick, not the D-pad, so use `cu`/`cd` there. Up to 2048 steps |
| `padrec=<path>` | Records the pad state the game reads, once per game frame (the point where `padscript` advances), into a TPAD file at `<path>` (relative to `sdmc:/3ds/ttyd/`, or `sdmc:/...`). Flushed every 600 frames, when the `padscript` ends and at exit. Format and use under "Recorded inputs" |
| `padfile=<path>` | Plays a TPAD file back, one record per game frame, replacing live input, `fastboot` and `padscript`. After the last frame the input is neutral and `[PAD] padfile end at frame N` is logged. Works under `determ`. The GC debug DOL takes `padfile=/name.tpad` from the disc folder |
| `shots=N` | Saves every Nth frame's EFB (608x480, as the game drew it) to `sdmc:/3ds/ttyd/shots/NNNNN.ppm` and logs that frame's draw counts. `shotmax=N` limits the number (default 200). In Azahar about one shot in seven comes out all black although its frame drew normally (the log line shows the frame's draws); take shots a few frames apart rather than relying on one |
| `gxdump=N` | Logs every draw of frame N: primitive, texture, first vertex in clip space, its matrix, TEV and blend state, and the code that began it (resolve with `addr2line -e build/3ds/game/ttyd.elf`). Runs of draws with the same matrix and texture are one line. EFB copies are logged too |
| `gxdumpcopy=N,K[,S]` | From game frame N, dumps (as `gxdump`) K frames, every S-th (default 1) of those that follow a frame with an EFB copy (`GXCopyTex`) and saves each one's picture as `shots/dumpFFFFF.ppm` (F is the game frame). Use it for effects that depend on a copy target when the frame number is not repeatable between runs. Draw lines end in `draw <n>` and copy lines in `after <n> draws`, so a draw that samples a copy target can be placed in the frame. If a shot is still pending the picture is skipped, but the dump is logged |
| `npctrace=<name>` | While the NPC with that name (`*` = any; `hex:<bytes>` = the name's bytes in hex, for the Japanese names, which the first-entry line gives) moves under `evt_npc_move_position`, logs one `npctrace` line per frame: position before, target, `dist`, `step`, `moveDir`, `moveSpeed`, `moveDt`, the offset handed to `npcMoveSide`, position after and `wallHit`. Logs Mario's position once at the start and each NPC name once when it first enters the function. At most 600 lines |
| `sweep=N` | Visits each map listed in `sdmc:/3ds/ttyd/sweep.txt` (`<map> [<story>]` per line), N game frames each, saving `sweep/<map>.ppm` and logging `sweep: <map> ok`. Used by `tools/port3ds/map_sweep.py` |
| `evtdump=N` | Logs the running event scripts at frame N, with the messages their windows show |
| `vramfail=1` | Test only: copy targets (`GXCopyTex`) never get VRAM, so they take the linear-memory path: the copy is rendered into the shared VRAM scratch target in strips and each strip is copied (`GX_TextureCopy`) into the texture. The picture must equal the VRAM path's (checked: START menu in `gor_01` and `fx=indirect`, 0 differing bytes). The log says `gpu: no VRAM for a WxH copy target, format F (KB): free ..., largest block ...` once per target |
| `gputest` | Draws the orientation pattern at boot: red top left, green top right, blue bottom left of a grey screen |
| `loglevel=3` | Debug lines in the log (each converted disc file, primitives closed without `GXEnd`) |
| `noqtm` | Keeps the per-frame buffers in FCRAM even when QTM is mapped. QTM's application part (the first 2 MB) is used by default |
| `place=<name>:<tier>[,<name>:<tier>...]` | Forces a memory placement's tier (`qtm`, `linear` or `heap`) for A/B runs. Write spaces in names as `_` (`gx_record:linear`). Unknown names or tiers are logged as `[MEM] place=... ignored` and skipped; the GPU/DSP rules still apply |
| `qtmgpu` | Allows `GPU_READ`/`GPU_WRITE` placements in QTM. Off by default: GPU access to QTM is unverified on hardware. `DSP` placements never go in QTM |
| `texstats=N` | Every N frames, logs texture hashes, bytes hashed, lookups, hash mismatches and uploads per frame. The first change of each texture is logged with its address, size, format, memory region and the caller of the last `GXInitTexObj*` for that address |
| `texfilter=<game\|linear\|mip\|nearest>` | Forces the texture filter mode (the `texfilter` option, which it overrides): `game` GX's filters as set (the port ignores GX mip levels), `linear` bilinear min and mag, `mip` bilinear plus a mip chain (GX's own levels where the texture has them, else a box filter at upload; trilinear), `nearest` point sampling. Never applied to `GXCopyTex` targets, textures a TEV stage reads as an indirect offset map, or textures with a side of 4 or less; one-bit-alpha cut-outs get no chain. A mode change drops every converted texture |
| `texlodbias=<float>` | Added to the level-of-detail bias of every sampler in `linear`, `mip` and `nearest` modes (positive blurs, negative sharpens; default 0). `game` mode ignores it (no chains there) |
| `texcensus=N` | Every N frames logs what the game asks for: distinct uploaded textures and sampler setups by GX min filter, mag filter and mip flag, GX formats, how many are copy targets, indirect maps or tiny, and the `texmem:` line (textures live, KB, KB of them mip levels). `texstats=N` also logs `texmem:` every N frames |
| `jobs=0` | Every `plat_job` runs inline: the A/B baseline for the core-2 job queue |
| `sdstats=N` | Every N seconds, an `sdstats` line: SD operations by kind (write, append, writeat, remove, rename, mkdir, read, disc) and the core that asked, queue depth now and at most, bytes written and read, and the **direct SD calls**: from core 1, and from cores 0/2 after the game loop began (must be 0; a warning gives the first caller's address and core). See [PORT_3DS_MULTICORE.md](PORT_3DS_MULTICORE.md) |
| `jobstats=N` | A `[JOBS]` counter line every N frames while the counters change |
| `jobcore=C`, `jobprio=P` | Job worker core and priority (defaults 2 and 0x30, below the render thread) |
| `jobcheck=1` | Each split conversion runs with jobs and again inline from the same bytes, and the two are compared (`[JOBCHECK]` lines) |
| `loadstats` | A `[LOADCONV]` line per load-time conversion, and a summary per load: per-kind time, DVD reads and wait |
| `convhash` | Adds an FNV-1a hash of each converted file to the `loadstats` lines |
| `arqspin` | ARQ waits poll with a yield (the old behaviour) instead of sleeping until the next ARQ callback |
| `loadprof` | Map-change and battle-start load profile: from the request (`SEQ_MAP_CHANGE`/`SEQ_BATTLE`) to the first field frame, one `[LOADPROF]` line (frames, wall, game-thread busy, retrace waits, DVD reads with caller wait, SD time and baton wake, ARAM transfers, conversion), the file-system calls and over-read, frames by reads finished in them, a DVD histogram by size, and then the game thread's busy time per frame over the first 90 frames of the new map. Turns on the `[LOADCONV]` lines too. See [PORT_3DS_JOBS.md](PORT_3DS_JOBS.md), "Load profile" |
| `aramfast=1` | ARAM DMA callbacks come 20 us after the post whatever the size, instead of after the 80 MB/s transfer time |
| `discra=KB` | Disc read-ahead on the file thread: a read of at most 64 KB that starts where the previous read ended reads KB kilobytes from there in one SD call, and the next reads that fall inside come from memory. Measured as a loss (over-read, no time saved); off by default |
| `discbatch=1` | Requests queued behind the one the file thread is reading that continue it on disc and in memory are read in the same SD call; each still completes in order with its own result. The queue is almost always one deep, so it merges nothing in the measured loads |
| `dvdsettle=1` | While a map change or a battle start loads (outside `determ`), a frame does not end until the DVD manager's queued reads have finished (at most 250 ms). A load then takes the frames the game's own logic needs (what `determ` and Dolphin show) instead of about one frame per read: `gor_01` 79 to 63 frames, `gor_00` 78 to 52 in Azahar. Default off until judged on hardware |
| `padlog` | Logs `padlog: 3ds=<held hex> gc=<GameCube button bits>` whenever the GameCube button state changes, to verify remaps |
| `lang=<en\|de\|fr\|es\|it\|nl>` | PAL: the game language. The default comes from the 3DS system language; any other language falls back to English |
| `hz=50` | PAL: preselects 50 Hz at the 50/60 Hz prompt (default 60 Hz) |
| `gxtrust=0/1` | 1 (default): display-list cache entries in unchanged, covered memory hit without hashing; 0 hashes every hit. `gxcache=2` still hashes and decodes trusted hits and counts `stale` / `mismatched` |
| `gxscribble=N` | Test only: at frame N, flips a byte of a cached list's array with no report. Verify mode must count it; normal mode keeps drawing the old vertices |
| `gxstatever=0/1` | 0 (default). 1: matrix loads don't bump the state version, and unchanged state skips `build_state`. Fewer rebuilds, but no time saved yet |
| `gxfogcache=0/1` | 1 (default): the fog table is recomputed only when the fog parameters change, and sent with an id instead of a hash |
| `gxunlit=0/1` | 1 (default): unlit colour channels skip `channel_color` and the eye-space normal |
| `gxmerge=0/1` | 1 (default): consecutive draws with nothing recorded between them share one draw command. `gxstats` shows `gpu draws N (of M calls)` |
| `gxvsh=0/1` | 1 (default): the vertex-shader path. Display-list cache entries get PICA streams, immediate-mode draws and decoded lists go through a per-frame ring, and a pass group fully eligible for `gxvshk` frames is transformed on the GPU. 0: every vertex is transformed on the CPU (A/B, and the hardware fallback). Acceptance: `tools/port3ds/vsh_accept.py` |
| `gxvshk=N` | Hysteresis: eligible frames in a row before a group moves to the shader (default 30) |
| `gxvshdecal=0/1` | 1 (default): a decal-like draw on the CPU path links the earlier shader-capable groups in its viewport, so they take one path. 0 only counts the links; it breaks the offload rules |
| `gxvshimm=0/1` | 1 (default): immediate-mode draws and decoded lists may take the shader path; 0 keeps them on the CPU |
| `gxvsh_check=N` | Logs every pass group of frame N (path, eligibility, streak, reasons, decal links) |
| `gxvshs16=0/1`, `gxvshkb=N` | s16 stream attributes where exact (default 1); stream memory in KB (default 6144) |
| `gxdepthshot` | Each shot also saves the depth buffer (`shots/NNNNN.dep`). Debug only |
| `gxclearrgb=RRGGBB` | Test only: the frame clear colour (the seam test uses `FF00FF`) |
| `shotfrom=N` | The first presented frame a shot may be taken on; with `shots=1 shotmax=K`, K consecutive frames |
| `determ` | Deterministic test mode: game time advances exactly one retrace per frame (1/60 s, or 1/50 s on PAL) from a fixed date. The retrace callbacks run once per frame on the game thread, each frame waits for queued DVD reads, sound interrupts follow the frame, and the output is silent. Two runs with the same switches and padscript give identical `gwdump` streams. Details in [PORT_3DS_TEST_INFRA.md](PORT_3DS_TEST_INFRA.md) |
| `gwdump=N` | A `gwdump` record every N frames: sequence, map, story flags (GSW), event values, Mario's position, the RNG state, battle state, clocks, pad, and a `GlobalWork` hash. Limit the range with `gwdumpfrom=`/`gwdumpto=`. Compare runs, or the port against Dolphin, with `tools/port3ds/gw_diff.py` |
| `fx=<name\|index>[,...]` | Spawns effects through `evt_eff` (`n64:` through `evt_eff64`) after the map is up. Options: `fxwait`, `fxgap`, `fxargs` |
| `battle=<id\|name>` | Starts the given entry of the current area's battle setup table after the map is up (`battlewait=N`) |
| `fonttest=sjis` | Draws fixed Shift-JIS strings (katakana, full-width digits, hiragana, half-width, kanji) over every frame in any region build, through the port's SJIS ROM font: `OSInitFont`, `OSGetFontTexture` and one GX quad per glyph (port3ds/game/test_font.c). Logs `fonttest: SJIS font loaded ...`. The font is rendered at build time by `tools/port3ds/gen_sjis_font.py` (Noto Sans CJK JP, SIL OFL); `build.sh` runs it on the host (Python Pillow; the font is downloaded once into `build/fonts/`). The JP build uses the same font for its system messages |
| `btlauto=perfect\|fail` | Runs battles unattended. `btlmoves=` gives the menu keys per turn and `btlstep=` the frames between presses. `perfect` forces every action command to succeed and `fail` presses nothing during them |
| `stopframe=N` | GameCube debug DOL only: stops at game frame N, for `run_dolphin.py --break gcdbg_stop` |
| `ndspvoices=1` | Plays MusyX's dry DSP-ADPCM voices on NDSP channels 1–23, timed to the software mix on channel 0. Streams, PCM, voices with an aux send, and starts beyond the free channels stay in software, and a voice that later gets an aux send moves back. Loops that start inside an ADPCM frame are re-aligned at load. Default off. See [PORT_3DS_AUDIO.md](PORT_3DS_AUDIO.md) |
| `ndspstats=N` | Every N seconds, `[NDSPV]` lines: voice-frames in software, on NDSP and with aux, overflows, starts, cache hits, re-aligned loops, late events, and the share of time in the mixer. Also channel 0 flushes, `flush ticks`, and with streams on: starts, refills, underruns, waits and dropped events |
| `ndspcachekb=N` | Size of the NDSP voices' sample cache in linear memory, in KB (default 4096) |
| `ndspstreams=1` | With `ndspvoices=1`, music streams play on NDSP channels: the ARAM ring (DSP-ADPCM or PCM16) is mirrored into linear memory and played as a chain of short wave buffers, refilled in step with channel 0. Default off. See PORT_3DS_AUDIO.md, "NDSP streams" |
| `audiobatch=N` | Channel 0's 10 ms buffers are refilled once N are free and flushed in one call over the whole ring. N=1 (default) flushes each; N=2 halves the flushes and adds up to 10 ms latency |
| `stereoforce=<0..1>` | Simulates the 3D slider (Azahar has none headless). With the `stereo3d` option on and a value above 0, presented frames are drawn in stereo by depth reprojection (render once, warp per eye; [PORT_3DS_GPU_OFFLOAD.md](PORT_3DS_GPU_OFFLOAD.md), "Stereo 3D"). With shots, each shot also saves the left and right screen targets as `shots/N.L.ppm` and `N.R.ppm` (400x240). 0 is identical to mono |
| `stereosep=<px>` | Total parallax at infinity at the full slider, in top-screen pixels (default 6, max 20) |
| `stereoconv=<1..99>` | Convergence: that percentile of the depth grid (default 50, the median) |
| `stereogrid=<W>x<H>` | Depth grid and warp mesh size (default 40x24, up to 80x48) |
| `stereostats=N` | Every N frames, a `stereostats` line: stereo and mono render-thread and GPU time, mesh time, 2D tail size and its depth writes, convergence |
| `stereolog=N` | Logs the draw order of stereo frames N and N+300 (S/s scene draw on the CPU/shader path, L/l screen-layer draw, C copy, X clear) and where the 2D tail starts |
| `scrshot` | Mono shots also save the top screen target as `shots/N.M.ppm` (400x240), for comparing against stereo eye shots |
| `gxthread=0/1/2/3` | Where GX is translated (`port3ds/game/gx/gx_rec.c`). 0 (default): on the game thread as each call is made. 1: recorded by value into a ring and translated by the game thread at fences (draw sync, `GXDrawDone`, read-backs, a memory release while translation is pending, a full ring). 2: a translator thread on core 2 follows the ring; the game thread translates the first part of each frame itself (see `gxthreadsplit`). 3: verify, as 1 plus getter, list-byte and late-read checks (`gxthread verify` warnings, `verify (total)` line). See PORT_3DS_MULTICORE.md "A, built" |
| `gxthreadstats=N` | Every N frames, `gxthread` lines: frame, game busy, fence and idle times, translator and render busy and core-2 share, record words, commands, lists, primitives, split point, fences by cause, array snapshot bytes. Cheap, unlike `gxstats` |
| `gxthreadsplit=0/1` | `gxthread=2`: 1 (default) translates each frame's first display-list calls on the game thread while core 2 replays the previous frame (hill-climbed split); 0 records everything |
| `gxthreadsplitat=N` | `gxthread=2`: fixed split of N display-list calls, for measuring |
| `gxthreadrel=0/1` | 1 (default): a memory release reported while translation is pending is a fence. 0 is for verify experiments only |
| `gxthreadkb=N` | Record ring size in KB (default 4096) |
| `gpucore=N` | Render thread core (default 2 on New 3DS, 1 on old) |
| `yieldsleep[=us]` | When `OSYieldThread` finds no other ready OS thread, sleeps `us` µs (default 100) instead of returning at once. This stops the DVD manager's `proc_main` spinning on core 0. Default off until measured on hardware |
| `yieldstats=N` | Every N seconds, a `[THREADS]` line: each OS thread's share of baton-holding time, idle yields, and time slept by `yieldsleep` |
| `shotbottom=N` | Saves the bottom screen as `shots/bNNNNN.ppm` (320x240, at most 60) every N ticks. A tick is a game frame, or a ~16 ms menu poll while the Options menu is open, so it also catches the open menu (the log line says `menu=1` or `menu=0`). Layout: rows 1-2 HUD, rows 3-30 log; with a menu open, rows 3-14 menu and rows 16-30 log (`plat/menu.c`) |
| `devmenu` | Enables the developer menu, opened with ZL+SELECT. SELECT alone opens the player Options menu, so a `padscript` that pressed `select` for the old developer menu now opens Options. While the Options menu is open, `padscript` steps advance once per ~16 ms menu poll, not per game frame. Example: `wait*900,select*3,wait*5,right*2,wait*5,select*3` |
| `devupdate` | Development only: starts the ctr-devkit updater at boot (TCP 5050, `devupdateport=N`, `devupdatetoken=TEXT`), and a crash then goes to recovery mode instead of waiting for START. See "ctr-devkit: crash and devupdate" below |

Sound needs `sdmc:/3ds/dspfirm.cdc` (dumped from a console with DSP1), in the profile's SD card as on the console. Without it the game runs silently with the same sound timing. `--lle-audio` makes Azahar run that firmware itself (its LLE DSP) instead of its own model of it, which is slower but closer to the console. `wavdump=N` saves the first N seconds of the game's mix as `sdmc:/3ds/ttyd/audio.wav`, and the log has the output's peak level every 10 s.

### Map sweep

```sh
tools/port3ds/map_sweep.py --story 30 --frames 120        # every map on the disc
tools/port3ds/map_sweep.py --area tik --story 30          # one area
tools/port3ds/map_sweep.py --maps gor_01,hei_00
```

A battle that a map starts counts as a pass (`ok (battle on entry)`): the game raises `BATTLE_SEQ_END`, the flag `battleMain` sets itself, so `seq_battleMain` exits through `battle_exit` to the field and the sweep goes on (victory screens skipped).
It writes the map list to the SD card, runs the game with `sweep=N`, and reads the log. A run that ends early (a crash, a stall, a battle a map starts on entry) is followed by one that resumes after the map being visited. Results are in `build/map-sweep/`: `results.json` (each map `ok`, `timeout`, `stopped`, `crash`, `stall` or `not in map table`: the storybook's `stg` maps, which the map change cannot enter), `shots/<map>.png`, and for a crash or stall the whole game log in `logs/<map>.txt`. Maps are entered without an entrance, so Mario starts where the map's default position puts him (often below the ground, from where the game's fall recovery lifts him).

### Crashes, svcBreak and hangs

Exceptions, `plat_fatal`, the watchdog (`wdog=N`, default 10 s without a frame) and every `svcBreak` (the game links with `--wrap=svcBreak`, so libctru and citro3d panics count) go through one report path in `port3ds/plat/crash.c`.
It writes `[CRASH]` lines to the log, `crash.txt` (appended, kept across boots) and the live log: registers, the stack scan, and for a hang the registers and top stack words of every port thread (SVC `0x3B`; Azahar returns an empty context, which the log says).
Then it shuts citro3d and gfx down without waiting for other threads and shows a crash screen on both screens (top: what happened, thread, PC/LR, FAR/FSR, map, frame, build, whether `crash.txt` was written; bottom: the call chain, two addresses per line) until START or 5 minutes, releases GSP and the DSP and ends the process. The watchdog thread ends the process if the report takes over 5 s before the screen is up.

`tools/port3ds/build.sh` archives each game ELF as `build/elfs/<build number>_<hash>.elf`. Resolve a report with it:

```sh
tools/port3ds/crash_report.py build/azahar-last/game_log.txt      # or a crash.txt, or --ftp <console ip>
tools/port3ds/run_azahar.py --build build/3ds/game/ttyd.cia --switches "crashtest=break crashwait=3" \
    --until "exiting the process" --expect-crash --timeout 90 --stall 60
```

### Finding a hang

```sh
tools/port3ds/hang_probe.py --switches "map=jin_09 story=30 wdog=400" --after "OSLink jin" --wait 6 --samples 4 \
    --peek 0090e208:2
```

Runs the game under Azahar's GDB stub, stops it `--wait` seconds after the log matches `--after`, and prints each thread's PC, LR and registers with their functions, the game thread's stack (code addresses found on it), and any `--peek ADDR:SIZE` memory, for each of `--samples` stops 0.5 s apart. `wdog=N` (switch) raises the watchdog's 10 s limit so the probe has time. When a hang shows the game thread not moving at all, look at `build/azahar-last/azahar_log.txt` for `UnmappedAccess` lines first: Azahar lets reads of unmapped memory return zero, where the console would crash.

## Running on the New 3DS

Setup on the console (once):

- Luma3DS with the Homebrew Launcher.
- The disc image at `sdmc:/3ds/ttyd/game.iso` (1.4 GB: an SD card reader is much faster than ftpd).
- For sound, `sdmc:/3ds/dspfirm.cdc` from the DSP1 homebrew. Without it the game runs silently and says so in the log.

```sh
export N3DS_IP=192.168.1.50      # the console's address (System Settings or the Homebrew Launcher's Y screen)
# Homebrew Launcher: press Y to wait for 3dslink, then:
tools/port3ds/run_hw.py --switches "wdog=30" --until "warp:|title" --timeout 300
tools/port3ds/run_hw.py --switches "map=gor_01 wdog=30" --until "warp: gor_01" --timeout 300
```

`run_hw.py` sends the `.3dsx` with 3dslink, passing the switches as arguments, and then connects to the game's live log on TCP port 17492. A netloaded game opens that port by itself. It prints and saves every line to `build/hw-last/log.txt` and stops like the Azahar runner. The game also writes `sdmc:/3ds/ttyd/log.txt` and keeps the previous run as `log.old`, so the report of a crash that cut the connection can be fetched with ftpd.

A 3dsx gets the memory of the title the Homebrew Launcher runs in, and it does not get the QTM mapping. If the game reports at boot that the heap or linear heap is too small, install the CIA instead (copy `build/3ds/game/ttyd.cia` to the SD card and install it with FBI) and start it from the HOME Menu. A CIA's switches come from `sdmc:/3ds/ttyd/debug3ds.txt`. Its live log needs the `livelog` switch.
Put `livelog` in `sdmc:/3ds/ttyd/debug3ds.txt`, start the CIA, then run `run_hw.py --no-send --until ...` to attach (`run_hw.py` adds `livelog` itself when it sends a 3dsx).
`--connect-timeout N` (default 60) is how long it retries the connection.

Installing the CIA without an SD card reader: open FBI > Remote Install > Receive URLs over the network, then run `N3DS_IP=<ip> tools/port3ds/fbi_install.py` (`--cia`, `--port 8080`).
It serves a copy of the CIA over HTTP from this PC's address toward the console and sends FBI the URL on TCP 5000.
It ends when FBI closes the connection (a reset counts) and prints whether the file was fully requested.

The probe (`build/3ds/probe/ttyd_probe.3dsx`) shows its results on the top screen and writes `sdmc:/3ds/ttyd/probe.txt`.

### Profiling (PC sampling)

```sh
tools/port3ds/pc_sample.py --elf build/3ds/game/ttyd.elf --duration 60 --interval 200 --threads game=1,render=8
```

`pc_sample.py` attaches to a running process through a GDB stub, repeatedly interrupts it, reads the PC and LR of every thread, continues, and prints a flat profile per thread (self functions with their top 3 callers from LR, an inclusive list, busy shares) and an overall top 30. `--json FILE` writes the same data. The tool takes the ELF and the process as arguments and knows nothing about the game. With no `--pid` it talks to Azahar's stub (port 24689).

- **Azahar.** Start the game with `run_azahar.py --gdb` (the guest waits for a debugger), connect once, continue, and detach after the game reaches the scene (`hang_probe.py` shows the connect-and-continue steps); then run `pc_sample.py`. Azahar's stub costs about 150 ms per stop, so the rate is about 6 Hz at `--interval 20` and the game barely advances between stops: with a 20 ms interval the game thread showed the same PC in every sample. Use `--interval 200` or more there. Results are a relative guide only, because Azahar's timing is not the console's.
- **Safety.** The target is detached and continued on exit, Ctrl-C and errors (verified in Azahar: the game's log kept growing after detach and after Ctrl-C).
- **Samples slower than 200 ms** print a warning and double the interval (up to 1 s).

## Profiling on hardware

1. Luma3DS: open the Rosalina menu (L+Down+Select), choose Debugger options, and enable the debugger. It listens on TCP ports 4000 and up (one per attached process). Enabling it freezes the console until a client connects, so do it, start the CIA from the HOME Menu, then open the Rosalina menu again, Process list, and note the game's process ID (PID). Luma's GDB server is only up while the debugger is enabled and a client has not attached yet.
2. Make sure the CIA's ELF is the one that was built into the installed CIA (`build/elfs/<build>.elf`, or `build/3ds/game/ttyd.elf`).
3. Run, from this PC (the console must be reachable):

```sh
N3DS_IP=192.168.1.223 tools/port3ds/pc_sample.py --pid <PID> --port 4000 \
    --elf build/3ds/game/ttyd.elf --duration 30 --interval 20 \
    --threads game=<tid>,render=<tid> --json build/pc_sample.json
```

   Thread ids are printed in the output's first table; run once without `--threads` to see them.
4. `--title <hex title id>` instead of `--pid` asks Rosalina for its process list over the protocol (`qXfer:osdata`). That is written from the GDB specification, not checked against Rosalina; if it fails, use `--pid`.

Not verified (written from Luma3DS's documentation and the GDB protocol, tested only against Azahar): attaching with `vAttach;<pid>`, the 20 ms rate Rosalina can sustain, and whether `D` resumes the process (the tool also sends `c`, and Rosalina detaches when the socket closes). Expect the achieved rate to be printed on the first line of the output; if samples are slower than 200 ms the tool backs off by itself.

## ctr-devkit: crash and devupdate

The crash handler and the updater are the libctru-only libraries in `libs/ctr-devkit/` (MIT, see its README). `port3ds/plat/crash.c` and `port3ds/plat/devupdate.c` are TTYD's glue. Reports now start with the format version (`[CRASH] v1 ...`); `crash_report.py` reads both forms.

```sh
tools/port3ds/build.sh libs/ctr-devkit          # examples: build/3ds/ctr-devkit/examples/{crash,devupdate}/
D=libs/ctr-devkit/devupdate/devupdate.py
$D --ip $N3DS_IP push build/3ds/game/ttyd.cia --switches "map=gor_01 wdog=30" --wait 120   # needs devupdate on
$D --ip $N3DS_IP restart map=hei_13 wdog=5
$D --ip $N3DS_IP report --out build/devupdate-last/report.txt   # after a crash (recovery mode)
$D --ip $N3DS_IP bootstrap --cia build/3ds/game/ttyd.cia --switches devupdate   # first install, no FBI
```

- **Switches.** `restart` and `push` switches reach the relaunched game as APT deliver arguments, which `plat_switches_load` puts first. They hold for that one launch; a start from the HOME Menu gets only `debug3ds.txt`. A devupdate relaunch keeps the server on.
- **Saves.** Before a relaunch the game only waits for a card write in progress. It never touches the files in `sdmc:/3ds/ttyd/`.
- **Azahar.** The same commands work with `--ip 127.0.0.1` against one long-lived headless instance:

```sh
$D azahar-session start --profile build/azahar-profile --boot build/3ds/game/ttyd.cia   # takes an Azahar slot
tools/port3ds/devupdate_azahar.py [--app ttyd|ttyd-crash]   # the scripted check, one instance per run
```

Azahar runs:
- the protocol and the CRC refusal;
- the AM install, also over the running game, which keeps running;
- the pause at a frame boundary and the resume;
- recovery mode after `crashtest=fatal`.

It cannot do an APT application jump: the Flatpak Azahar aborts, or never starts the new title, on any relaunch (`restart`, a push with relaunch, the bootstrap's jump). In Azahar a cancelled push over an installed title also removes that install, because its AM writes into the title's folder. Relaunches, their deliver arguments, and the old install surviving a refused push are therefore checked on the console only.

## Running the GameCube game in Dolphin

```sh
tools/port3ds/run_dolphin.py --frames 600 --deref gp:0x40 --read marioStMain:16 --addr 0x80000000:8 \
    --json build/dolphin-last/f600.json
```

- **How it runs.** Headless (`-p headless -v Null`) in an isolated user folder `build/dolphin-profile/`, with the GDB stub on port 55555, unlimited speed, and debugging enabled so the JIT honours breakpoints.
- **Frames.** `--frames N` stops at the Nth call of `marioStMain`, which runs once per game frame. Expect about 10 frames per second of wall time in this mode, because every frame stops at the breakpoint.
- **Symbols.** These come from `build/<version>/MarioSt.elf`. The decomp is byte-identical to the retail DOL, so its addresses are valid for the retail disc.
- **Reads.**
  - `--read NAME[:SIZE]` reads at a symbol.
  - `--deref NAME:SIZE` follows a pointer variable (for example `gp` to `GlobalWork`).
  - `--addr 0xADDR:SIZE` reads raw memory.
- `--dol FILE` boots a built DOL with `iso/game.iso` as the disc, for an instrumented decomp build.

## Running paper-cut

```sh
tools/port3ds/run_papercut.py --map gor_01 --frames 450 --shot 400        # screenshots in build/papercut-last/
tools/port3ds/run_papercut.py --frames 300 --print "gp->retraceCount"     # stop at frame 300 under gdb, print
tools/port3ds/run_papercut.py --ptype GlobalWork                          # layout with offsets (64-bit)
```

The runs use:

- an isolated user folder (`build/papercut-profile/`) with the intro skipped;
- paper-cut's deterministic 60 Hz clock (`TTYD_FAST`);
- an offscreen window;
- no Discord connection.

## Hardware probe

`port3ds/probe` reports what the process was granted and measures a few costs. It writes them to the top screen and `sdmc:/3ds/ttyd/probe.txt`.

Results in Azahar (2026-11-21, CIA):

| Item | Result |
|---|---|
| Model | New 3DS, with a 124 MB application region |
| QTM | mapped read/write at `0x1E800000`, size `0x3E0000` (before the change to 2 MB). The game now maps only the first 2 MB (`0x1E800000-0x1E9FFFFF`) |
| QTM physical address | `osConvertVirtToPhys` gives `0x1F000000` |
| CPU use of QTM | write and read back: ok |
| GPU copy linear → linear | ok |
| GPU copy QTM ↔ linear | fails in both directions. The GPU does not use QTM; plan section 5.7 |
| Core 1 limit | `APT_SetAppCpuTimeLimit(80)` accepted |
| Threads | threads start on cores 0, 1 and 2, but `svcGetProcessorID` returns garbage in Azahar, so core placement cannot be checked there |
| Timings | not meaningful in Azahar; measured on hardware in plan phase 8 |

## Still to come

| Tool | Phase |
|---|---|
| `tools/port3ds/memcompare.py`: the same structures read from Dolphin, paper-cut and Azahar and compared field by field. GameCube and 3DS layouts come from the 3DS ELF's debug info (the two layouts are identical apart from byte order); paper-cut values come from its own debug info | 1 |
| `tools/port3ds/golden.py` | 4 |
| Story sweeps | 6–7 |

### Host decode (no console, no emulator)

`tools/port3ds/test_decode.py [--disc iso/game.iso] [--papercut ../paper-cut] [--full]` builds `tools/port3ds/host_decode` (the port's own converters compiled for the PC) and checks it against paper-cut's decoders:
- swap tables applied twice give back the original;
- the disc's file table matches `gcdisc.py`;
- every disc file converts, and converting again changes nothing;
- the disc-data arrays match an independent `main.dol`/REL read;
- TPL headers and pixels, anim keyframes, map vertex counts and bounds, and MusyX all match paper-cut;
- paper-cut's `disc_sweep` accepts the same files.

It takes about 1 to 3 minutes and exits 1 on any failure. Without the disc image or paper-cut, the checks that need them are skipped. Manual use: `host_decode --disc iso/game.iso list|decode PATH|all|discdata`. Details are in [PORT_3DS_HOST_DECODE.md](PORT_3DS_HOST_DECODE.md).

### Saves

- `tools/port3ds/test_gci.py selftest` needs only `gcc`, no emulator. Run it after any change to the save structs, `card.c` or `save_swap.c`. It builds the real `card.c` and `save_swap.c` for all three releases. It checks:
  - the GCI header;
  - that the data on disk is big-endian (the GameCube layout);
  - the migration of old little-endian port saves;
  - the game's own checksums;
  - paper-cut's genuine saves.
- `tools/port3ds/test_gci.py check FILE...` validates any GCI file.
- If a save struct changes, the build regenerates the swap tables, and a `_Static_assert` fails the build if the layout drifts from the probe.
- **Azahar save check:**
  1. Put a `card1.gci` in the profile's `sdmc:/3ds/ttyd/card/`.
  2. Reach file select with `wait*850` followed by repeated `a*3,wait*150`, with `shots=100` to see it.
  3. Rogueport has a save block, so saving mid-game is a cheap test.

### Options

Player settings live in `sdmc:/3ds/ttyd/options.txt` as `key=value` lines ending in `#end`. Deleting the file resets everything to defaults.

### Regions

- **Building:**
  - `tools/port3ds/build.sh . -DREGION=JP` (or `PAL`) writes to `build/3ds/game_jp` (or `build/3ds/game_pal`).
  - The discs go in `iso/G8MJ01.iso` and `iso/G8MP01.iso`, or are given with `-DTTYD_DISC=`.
- **Wrong-disc check:** a build refuses a disc from another region. With the US disc, `run_azahar.py --build build/3ds/game_pal/ttyd.cia --switches "" --until "This build is for"` should pass.
- **JP and PAL in-game testing needs the JP and PAL discs.** The steps are in PORT_3DS_PLAN.md section 9.1.

### GameCube debug DOL and Dolphin comparison

- **Build:** `python3 configure.py --version G8ME01 --gcdebug && ninja -f build_gcdebug.ninja` builds a non-matching DOL with the same test switches.
- **Run:** `run_dolphin.py --gcdebug` boots it headless.
- **Compare:** `tools/port3ds/gw_diff.py` runs the same switches and padscript in Azahar (with `determ`) and in Dolphin, and reports the first frame and the fields that differ. Both runs start with the same memory card, no save files by default (`--card empty`) or exactly one GCI (`--card PATH.gci`); the port's `sdmc:/3ds/ttyd/card/` and Dolphin's `GC/<region>/Card A` folder are emptied and filled to match, and the state is printed. Save data changes the file-select text and so the RNG, so a run on a card with saves differs from one without.
- Details are in [PORT_3DS_TEST_INFRA.md](PORT_3DS_TEST_INFRA.md).

### Recorded inputs

`padrec=` writes the pad state the game saw into a TPAD file, and `padfile=` plays it back, so a run (a hand-played session, or a `padscript` run) can be repeated exactly, or a Dolphin movie can drive the port.

- **File format** (little-endian): a 16-byte header, the magic `TPAD`, version u32 (1), frame count u32, flags u32 (bit 0 = recorded under `determ`); then 8 bytes per game frame, the GameCube `PADStatus` fields the game reads: buttons u16, stickX s8, stickY s8, substickX s8, substickY s8, triggerL u8, triggerR u8. Buttons are the `PAD_BUTTON_*` bits.
- **Frame index:** record `n` is what `PADRead` returns during the `n`th game frame, counting from the first `VIWaitForRetrace`. Recording and playback both ignore frames in which the game never reads the pad (recorded as neutral).
- **Record:** `determ gwdump=10 padscript=... padrec=run.tpad`. Add `determ` if the file is to be replayed under it; the header notes it.
- **Replay:** `determ gwdump=10 padfile=run.tpad`, with no `padscript`. The `gwdump` stream matches the recorded run's record for record, apart from the lines after the file's end in a longer run. Compare with `tools/port3ds/gw_diff.py --logs A B`.
- **Dolphin movies:** `tools/port3ds/dtm2pad.py dtm2pad movie.dtm out.tpad` converts controller 1 of a GameCube DTM; `pad2txt out.tpad` lists the frames; `selftest` checks the converter on a synthetic movie. The conversion is tested on a synthetic movie only; whether a real movie lines up with the port's boot frames is untested.
- **GC debug DOL:** `padfile=/name.tpad` reads the file through `DVDOpen` from the extracted disc folder (`build/<disc>/files/name.tpad`, put there by hand, for example `build/gcdebug-disc/files/`; at most 8192 frames) and logs the same `[PAD]` lines. It has no `padrec=`. Checked in Dolphin: a 1888-frame file recorded in the port under `determ` loads and ends at frame 1888.
- **Limits:** the Select menu and the Options menu still take the live input while open. A first run after a fresh Azahar profile can differ from later runs in `rng` (seen once at game frame 1160; two later runs of the same switches matched each other and the replay).

### THP movies

The US disc has no THP files, so the tests make their own.

1. Make a test movie: `tools/port3ds/thp_test/make_thp.py OUT.thp [--size WxH] [--frames N]`.
2. Build `thp_dump` with gcc from `port3ds/third_party/aurora_thp/*.c`. Add `-DTHP_SDK_QUARTER_IDCT_BUG=0` for an exact decode.
3. Compare against ffmpeg: `tools/port3ds/thp_test/compare.py OUT.thp --dump ./thp_dump`.
4. On-device timing: the `tools/port3ds/thp_test/arm_bench` CIA (`--until "BENCH DONE"`).

### Battle-background (`stg`) maps in the sweep

- **What they are:** the `stg_*` maps are battle backgrounds (`bmapLoad`'s `specificStageDataDir`). Only `stg_01_0` to `stg_01_4` are in the map table.
- **How the sweep reaches the others:** through a battle on `hei_01` at story 30. It waits `--frames` frames after the battle is set up, then logs `sweep: <map> ok (battle stage)`.
- **What counts as a pass:** both battle maps load, shown as `stage: <map> up: 2 maps, stage map joints N`.
- **Result:** all 52 pass (2026-11-22).

