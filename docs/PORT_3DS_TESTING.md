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
| `shots=N` | Saves every Nth frame's EFB (608x480, as the game drew it) to `sdmc:/3ds/ttyd/shots/NNNNN.ppm` and logs that frame's draw counts. `shotmax=N` limits the number (default 200). In Azahar about one shot in seven comes out all black although its frame drew normally (the log line shows the frame's draws); take shots a few frames apart rather than relying on one |
| `gxdump=N` | Logs every draw of frame N: primitive, texture, first vertex in clip space, its matrix, TEV and blend state, and the code that began it (resolve with `addr2line -e build/3ds/game/ttyd.elf`). Runs of draws with the same matrix and texture are one line. EFB copies are logged too |
| `gxdumpcopy=N,K[,S]` | From game frame N, dumps (as `gxdump`) K frames, every S-th (default 1) of those that follow a frame with an EFB copy (`GXCopyTex`) and saves each one's picture as `shots/dumpFFFFF.ppm` (F is the game frame). Use it for effects that depend on a copy target when the frame number is not repeatable between runs. Draw lines end in `draw <n>` and copy lines in `after <n> draws`, so a draw that samples a copy target can be placed in the frame. If a shot is still pending the picture is skipped, but the dump is logged |
| `npctrace=<name>` | While the NPC with that name (`*` = any; `hex:<bytes>` = the name's bytes in hex, for the Japanese names, which the first-entry line gives) moves under `evt_npc_move_position`, logs one `npctrace` line per frame: position before, target, `dist`, `step`, `moveDir`, `moveSpeed`, `moveDt`, the offset handed to `npcMoveSide`, position after and `wallHit`. Logs Mario's position once at the start and each NPC name once when it first enters the function. At most 600 lines |
| `sweep=N` | Visits each map listed in `sdmc:/3ds/ttyd/sweep.txt` (`<map> [<story>]` per line), N game frames each, saving `sweep/<map>.ppm` and logging `sweep: <map> ok`. Used by `tools/port3ds/map_sweep.py` |
| `evtdump=N` | Logs the running event scripts at frame N, with the messages their windows show |
| `gputest` | Draws the orientation pattern at boot: red top left, green top right, blue bottom left of a grey screen |
| `loglevel=3` | Debug lines in the log (each converted disc file, primitives closed without `GXEnd`) |
| `noqtm` | Keeps the per-frame buffers in FCRAM even when QTM is mapped. QTM's application part (the first 2 MB) is used by default |
| `place=<name>:<tier>[,<name>:<tier>...]` | Forces a memory placement's tier (`qtm`, `linear` or `heap`) for A/B runs. Write spaces in names as `_` (`gx_record:linear`). Unknown names or tiers are logged as `[MEM] place=... ignored` and skipped; the GPU/DSP rules still apply |
| `qtmgpu` | Allows `GPU_READ`/`GPU_WRITE` placements in QTM. Off by default: GPU access to QTM is unverified on hardware. `DSP` placements never go in QTM |
| `texstats=N` | Every N frames, logs texture hashes, bytes hashed, lookups, hash mismatches and uploads per frame. The first change of each texture is logged with its address, size, format, memory region and the caller of the last `GXInitTexObj*` for that address |

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
