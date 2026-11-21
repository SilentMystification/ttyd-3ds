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
- **Debugger.** `--gdb` turns on Azahar's GDB stub (port 24689). The guest then waits for a debugger; use `tools/port3ds/gdbrsp.py` or `arm-none-eabi-gdb`.

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
| QTM | mapped read/write at `0x1E800000`, size `0x3E0000` |
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
| Map and story sweeps | 6–7 |
| Hardware push and log fetch (`N3DS_IP`) | 8 |
