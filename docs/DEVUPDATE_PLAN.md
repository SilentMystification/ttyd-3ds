# Plan: `ctr-devupdate`, an over-the-air dev updater for any 3DS homebrew

Status: planned 2026-11-22, not started.

Decisions (owner):
- It is built in this repository as a self-contained folder, `libs/ctr-devkit/devupdate/` (part of the ctr-devkit family, see CTR_DEVKIT_PLAN.md), and moves to its own repository once it has proved itself.
- MIT licence.
- No dependency on TTYD or on the port's `plat/` layer. It uses only libctru, so any libctru app can drop it in.

## Goal

From a PC, one command installs a new build of a running homebrew title on the console and relaunches it, with no presses on the console:

```
devupdate.py push --ip 192.168.8.221 build/app.cia
```

This replaces the copy → FBI install → launch cycle. It gives fast iteration for anything that needs the installed CIA's ExHeader (QTM, CPU and core settings), which a 3dsx under the Homebrew Launcher cannot have.

## Pieces

### Device library (C, libctru)

```c
typedef struct {
    u16 port;                  /* default 5050 */
    const char* token;         /* shared secret; NULL = none (LAN dev use only) */
    const char* args_path;     /* the app's switch/args file, e.g. "sdmc:/3ds/app/args.txt"; NULL = no args command */
    int core;                  /* server thread core, default 1 */
    void (*on_begin)(void* user);           /* app quiesces: pause the game, stop audio, flush saves */
    void (*on_progress)(u32 done, u32 total, void* user);
    bool (*on_ready)(void* user);           /* return true to relaunch now; false = app relaunches later */
    void* user;
} DevUpdateConfig;

Result devupdate_start(const DevUpdateConfig* cfg);   /* starts the listener thread */
void   devupdate_stop(void);
void   devupdate_relaunch(void);                       /* aptSetChainloaderToSelf, then a normal exit */
bool   devupdate_pending(void);                        /* an update is installed and waits for relaunch */
```

- **Server.** A small TCP protocol on its own thread (no HTTP library). A header carries the magic, version, token, the payload kind (`cia`), the size and a CRC32. Then the body streams in chunks.
- **Install.** `AM_StartCiaInstall(MEDIATYPE_SD, &h)`, then a write per chunk as it arrives (no full copy in RAM), then `AM_FinishCiaInstall`. On any error, `AM_CancelCIAInstall` and an error code back to the PC. The CRC is checked before finishing.
  - Needs the `am:net` service, already in typical homebrew ExHeaders and in ours.
  - Installing over the running title is what FBI and Universal-Updater do when they update themselves. Verify that first on hardware.
- **Relaunch.**
  - `on_ready` lets the app save state first. Then `devupdate_relaunch()` calls `aptSetChainloaderToSelf()` and exits through the app's normal shutdown, so the new version starts.
  - For a 3dsx (under the Homebrew Launcher), CIA installs are refused with a clear message. 3dslink already covers that case.
- **Replies to the PC:** status lines (`receiving`, `installing`, `done`, `error <code> <text>`), so the PC tool shows progress and errors.
- **Arguments for the next launch:**
  - A CIA started from the HOME Menu or relaunched through APT gets no command line, so test switches travel through a file.
  - An `args <text>` command writes `args_path` and syncs it; `restart` then relaunches.
  - `args-once` marks the file for deletion after the next read, so a one-shot test (for example a crash test) does not repeat on a normal launch. The app deletes it, through a helper `devupdate_args_consume()`.
- **Extras, kept small:**
  - a `ping` command returning the title ID, the app's version string and free space;
  - a `restart` command that relaunches without installing.
- **Platform behaviour:**
  - the listener pauses itself during sleep and HOME (through an `aptHook`) and resumes after;
  - it doesn't hold the core-1 CPU budget, and blocks in `accept`/`recv`;
  - if the app runs core 1 at 80%, transfers are slower; `on_begin` is the place for the app to drop its limit during a transfer.
- **Security:** dev-only.
  - Off unless the app calls `devupdate_start`, and our games call it only in dev builds or behind a switch.
  - Optional token, LAN only, and nothing but CIA install and restart is exposed.

### PC tool (`libs/ctr-devkit/devupdate/devupdate.py`, Python 3, stdlib only)

- `push <cia>`: connect, send the header and stream the file, show progress, wait for `done`, and report the relaunch.
- `ping`, `restart`.
- `--ip` (default `$N3DS_IP`), `--port`, `--token`, `--timeout`.

### TTYD integration (the only TTYD-specific part)

- In `port3ds/`, behind the switch `devupdate` (on in dev builds through `debug3ds.txt`):
  - call `devupdate_start` at boot;
  - `on_begin` pauses the game thread at a frame boundary, quiets audio and waits for the render thread;
  - `on_ready` flushes the card (save) files and returns true.
- `run_hw.py` / `fbi_install.py` gain a `--devupdate` path: push the CIA, wait for the relaunch, then attach to the live log.

## Steps

1. **Hardware check first:** a tiny test app that installs a CIA of *itself* from the SD card through AM while running, then relaunches with `aptSetChainloaderToSelf`. This proves the core assumption before anything else is built.
2. **Library and PC tool,** with a `README.md` (API, protocol, the "dev only" warning) and an MIT `LICENSE`.
3. **Example app** in `libs/ctr-devkit/examples/devupdate/`: prints its build number, so a push visibly changes it.
4. **TTYD integration** and tool support.
5. **Done when:** a TTYD CIA build is pushed from the PC while the game runs, installs, relaunches into the new build (its log shows the new build string) and reattaches the live log, 10 times in a row with no console input. A deliberate corrupted push (bad CRC) is refused and leaves the old install working.

## Recovery mode and hardware test harness (owner's idea, 2026-11-22)

When the app crashes, the crash handler hands over to the updater instead of ending the process. The console stays reachable, so a PC can collect the report, push a fix and restart the next test with no one at the console.

**Library part** (generic, in `ctr-devupdate`): `devupdate_enter_recovery(const char* report)`.
- **Prepared at `devupdate_start` time,** while the app is healthy:
  - a dormant recovery thread (core 1, waiting on an event);
  - a static buffer for `socInit`, so recovery never allocates from a possibly corrupted heap;
  - an open `am:net` handle;
  - a copy of the app's title ID.
- **Entering recovery:** the caller (the app's crash handler, after its report and crash screen) signals the recovery thread and parks itself.
- **What recovery offers:**
  - it restarts sockets on the reserved buffer if they are dead;
  - `report` (send the report text and log tail);
  - `push` (install a CIA, as normal);
  - `args <text>` plus `restart` (write the app's switch file, then relaunch; APT's deliver-argument mechanism is a fallback only if the file proves unreliable).
- **Relaunch:** `aptSetChainloaderToSelf()` plus the minimal APT exit needed for the chainload to take effect.
- **Its own deadline:** if recovery can't bring the network up in N seconds, it falls back to a plain process exit, as the crash handler does today.

**App part** (TTYD, in `port3ds/plat/crash.c`): with `devupdate` active, the crash screen says "recovery: waiting for the PC" instead of waiting for START, and the handler calls `devupdate_enter_recovery`.

**Harness** (`tools/port3ds/hw_harness.py`, generic apart from a test-list file):
- a test list (JSON) with entries of switches, a pass regex, a fail regex and a timeout. Examples: `map=hei_13`, `crashtest=hang wdog=5` (expects a crash), and later `battle=`/`fx=` gallery items;
- loop: push the build once, then per test `args <switches>` (one-shot) and `restart`, follow the live log, record pass, fail, timeout or crash;
- on a crash, pull the report, resolve it with `crash_report.py`, then restart into the next test;
- output: `build/hw-harness/<build>/results.json` and a summary;
- the Rosalina sampling profiler (`pc_sample.py`) can run inside the same loop for performance scenes.

**What stays manual:** faults that bring down the whole console rather than our process (kernel panics, a crash of a system service such as `qtm`, the sleep hang seen 2026-11-22), and anything needing the lid or HOME button. A power-cycling relay could cover some later; that is out of scope.

Steps (after steps 1 to 5 above):
6. Recovery mode in the library, plus the example app crashing on purpose and recovering.
7. TTYD crash handler integration; check with `crashtest=segv|fatal|break|hang`, all on hardware.
8. `hw_harness.py` with a first test list. **Done when** 20 tests, including 4 deliberate crashes, run unattended on the console twice in a row.

## Risks

- **Installing the running title:** AM may refuse or need the title's content to be closed first. FBI's self-update shows it can work, but how matters; step 1 settles it.
- **Network speed** while the game holds core 1 at 80% (the netload slowness seen 2026-11-22). The `on_begin` hook lowers the limit during transfers.
- **Sleep during a transfer:** cancel the install cleanly and report `error sleep`.
