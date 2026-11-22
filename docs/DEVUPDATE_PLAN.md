# Plan: `ctr-devupdate`, an over-the-air dev updater for any 3DS homebrew

Status: planned 2026-11-22, not started.

Decisions (owner):
- It is built in this repository as a self-contained folder, `libs/ctr-devupdate/`, and moves to its own repository once it has proved itself.
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

### PC tool (`libs/ctr-devupdate/devupdate.py`, Python 3, stdlib only)

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
3. **Example app** in `libs/ctr-devupdate/example/`: prints its build number, so a push visibly changes it.
4. **TTYD integration** and tool support.
5. **Done when:** a TTYD CIA build is pushed from the PC while the game runs, installs, relaunches into the new build (its log shows the new build string) and reattaches the live log, 10 times in a row with no console input. A deliberate corrupted push (bad CRC) is refused and leaves the old install working.

## Risks

- **Installing the running title:** AM may refuse or need the title's content to be closed first. FBI's self-update shows it can work, but how matters; step 1 settles it.
- **Network speed** while the game holds core 1 at 80% (the netload slowness seen 2026-11-22). The `on_begin` hook lowers the limit during transfers.
- **Sleep during a transfer:** cancel the install cleanly and report `error sleep`.
