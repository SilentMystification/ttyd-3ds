# Plan: `ctr-devkit`, reusable developer tools for any 3DS homebrew

Status: planned 2026-11-22. Owner's decisions:
- MIT licence.
- Built in this repository as a self-contained folder (`libs/ctr-devkit/`) and moved to its own repository once proven.
- libctru only: no dependency on TTYD or on the port's `plat/` layer.

One small family of modules that work alone or together. An app links only the modules it wants.

| Module | What | Plan |
|---|---|---|
| `crash` | Crash and hang handler: report, crash screen, thread dump, `svcBreak` capture | this file |
| `devupdate` | Over-the-air CIA install and relaunch; recovery mode after a crash | [DEVUPDATE_PLAN.md](DEVUPDATE_PLAN.md), whose library moves to `libs/ctr-devkit/devupdate/` |
| PC tools | `crash_report.py`, `devupdate.py`, `fbi_install.py`, `pc_sample.py` (sampling profiler), `hw_harness.py` | moved from `tools/port3ds/` once generic |

## `crash` module

Source: today's `port3ds/plat/crash.c` (624 lines, commit 8591dbb), which is already game-agnostic in behaviour. What ties it to our platform layer, with the replacement for each:

| Today (`plat_*`) | In the library |
|---|---|
| `plat_crash_line`, `crash.txt` writing, `plat_log_crash_close`, `plat_console_trylock` | **Sinks:** the library writes the report to a callback the app provides (a log line sink), plus an optional file path it writes itself with plain `FILE*` and no heap. The app wires its live log or other channels to the line sink |
| `plat_build_string`, `plat_set_map_name`, `plat_set_phase`, `plat_qtm_available`, game frame | **An app-info callback** that fills a small struct (build, scene name, frame, free text lines). TTYD reports its map and phase there |
| `plat_switch_int` / `plat_switch_value` (`crashtest`, `crashwait`, `wdog`) | **Config fields** in `CrashConfig`; the app reads its own switches and fills them |
| `plat_crash_gpu_down` / `plat_set_gpu_abort`, `plat_set_audio_abort`, `plat_cpu_limit_restore` | **Shutdown hooks** registered by the app and run in order before the crash screen and before exit |
| `plat_thread_register`, `plat_stack_register`, `plat_crash_install_thread` | **The library's own thread registry:** `crash_thread_register(name)` called at each thread's entry; it installs the handler for that thread and records its handle and stack bounds |
| `plat_wd_progress`, `plat_wd_hold`, `plat_wd_quiesce`, `plat_wd_counter` | **The library's watchdog API:** `crash_wd_progress()`, `crash_wd_hold(bool)`, `crash_wd_quiesce()` |
| `plat_exit_process`, `plat_fatal` | `crash_fatal(fmt, ...)` and the exit path inside the library. The app keeps its own `plat_fatal` as a one-line wrapper |
| `plat_uptime_ms` | `svcGetSystemTick` directly |

API sketch:
```c
typedef struct {
    const char* report_path;          /* e.g. "sdmc:/3ds/app/crash.txt"; NULL = none */
    void (*line_sink)(const char* line, void* user);
    void (*app_info)(CrashAppInfo* out, void* user);
    int  watchdog_seconds;            /* 0 = off */
    int  screen_wait_seconds;         /* default 300 */
    bool wrap_svcbreak;               /* needs -Wl,--wrap=svcBreak at link time */
    void (*on_recovery)(const char* report, void* user); /* e.g. devupdate_enter_recovery; NULL = exit */
    void* user;
} CrashConfig;

void crash_init(const CrashConfig* cfg);
void crash_thread_register(const char* name);       /* call at each thread's entry */
void crash_add_shutdown_hook(void (*fn)(void* user), void* user, int order);
void crash_wd_progress(void);
void crash_wd_hold(bool hold);
void crash_wd_quiesce(void);
void crash_fatal(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void crash_test(const char* kind);                  /* "segv", "fatal", "break", "hang" */
```

What it keeps from today:
- one report path for exceptions, fatals, the watchdog and `svcBreak`;
- second-fault protection;
- report and exit deadlines enforced by the watchdog thread;
- the per-thread SVC 0x3B context dump;
- the crash screen on both displays;
- the CPU-limit restore as a shutdown hook.

**Report format.** It's line-based and versioned (`[CRASH] v1 ...`), so `crash_report.py` works for any app given that app's ELF. The build ID in the report selects the ELF.

## `devupdate` extras: `files` and `queue` (built 2026-11-22)

Owner request: no live scripting, but send input files to the console and queue several test runs, so a walk (a series of runs) goes by itself. Both are generic and live in `libs/ctr-devkit/devupdate/` (`files.c`, `queue.c`); TTYD only supplies glue (`port3ds/plat/devupdate.c`). Details and the manifest format are in `libs/ctr-devkit/README.md`.

- **`files`** (`put`, `get`, `ls`, `rm`): one app-chosen SD folder (`files_dir`, TTYD: `sdmc:/3ds/ttyd/queue/`), CRC32-checked uploads written to `<name>.part` and renamed on success. The bytes go through a store callback set (`DevUpdateStore`: write, append, rename, remove, mkdir, flush, read). The port passes one that queues onto its SD service on core 1; the default is stdio; recovery mode always uses stdio.
- **`files` read-only root** (`files_ro_dir`, TTYD: `sdmc:/3ds/ttyd/`): paths starting `@/` (`files get @/previous.log`, `files ls @/`) read the game's own folder with stdio on the server thread; put and rm answer `error 20 read-only`. `devupdate.py log [--prev] [-o FILE]` prints the last 200 lines of `latest.log`.
- **`queue`** (`add`, `list`, `clear`, `start`, `status`, plus `pull` on the PC): entries `{name, switches, padfile, until, frames, timeout_s, collect}` stored on the card. `start` relaunches the title for entry 1 (APT deliver arguments, as `restart`), each entry ends on its `until` pattern, frame count or timeout, logs `[QUEUE] entry N <name> done result ...`, appends a line to `results.txt` and relaunches for the next; after the last it logs `[QUEUE] done`. A crash is recorded by the recovery thread and the queue goes on. The run state travels in the launch arguments (`ctrdkq=N`), so a cold start never continues a queue.
- **Choices:** the console has no JSON parser (the PC turns the manifest into tab-separated lines) and no regex library (a small matcher: literals, classes, `* + ?`, anchors, top-level `|`).
- **Azahar** has no APT relaunch; `azahar-session start --supervise` emulates it (marker file, arguments in a file, a supervisor that starts the title again). What stays untestable there is whether APT keeps the deliver arguments across a self-relaunch on a real console, and the relaunch timing.
- **Walks are for milestones.** The owner wants console walks rarely; the TTYD tool `tools/port3ds/hw_walk.py` takes an explicit list only, prints the entry count and estimate first, and the docs say so.

## `devupdate` protocol, `--stage` and failure handling (2026-11-23)

Owner requirement: **no state of the running game may prevent an update.** Hardware finding that started this: build #465 refused every push with `error 17 write failed at ~8-9 KiB: e0e083f2 (install cancelled)` when the PC sent the incoming build's version text; without the sidecar the same CIA installed.

**Wire format (v1, little-endian).** A 64-byte header (`proto.h`: magic `CDU1`, version, command, flags, payload size, payload CRC32, argument length, title ID, 32-byte token), then `args_len` argument bytes, then the payload. Flags: bit 0 relaunch after the push, bit 1 jump to the pushed title, bit 2 `VERSION` (the argument area starts with one length byte and that many text characters, then the switch text), bit 3 `STAGE` (see below). The device decodes the header and the argument area in `proto.c`, plain C with no libctru, so `devupdate.py selftest` builds the very same file with gcc (`selftest_host.c`) and feeds it the packets the PC tool really builds (`request_prefix`): version text of length 0, 1, 14, 47 and a raw 255-byte length, with and without switches, with `--bad-crc`, with and without `--stage`. Each decoded field, and the offset where the payload starts, must match. The decoder keeps at most 47 characters of a longer text but always consumes all of it, so the bytes that follow are never shifted; a length byte that promises more than the area holds is flagged and the area is kept as switch text.

**What the framing review found.** The parse (old code: `devupdate.c` `handle_client`, the `FLAG_VERSION` block) and the PC's `push()` agree byte for byte (the self-test shows the payload offset exactly `64 + args_len`), so the version text itself does not shift the stream. What differed with a version text is the shape of the traffic: the first segment carries header and arguments together and the payload's recv sizes then follow the network's segmentation, so AM was fed writes of arbitrary, odd sizes and offsets right across the CIA header (the failures were at 8-9 KiB, where the 0x2020-byte CIA header ends). The device now fills a 64 KiB buffer before each AM write: every write is a whole chunk at a 64 KiB multiple, the last one short, whatever the network does. This is the fix for the pattern; it is not proven on the console (the cause is not reproducible off hardware), which is why the failure path below reports AM's code and stage, and why `--stage` exists.

**`--stage`.** `push --stage` sends flag bit 3. The device writes the CIA to `<files_dir>/stage.cia.part` through the app's store callbacks (TTYD: core 1's SD service), renames it to `stage.cia` after the CRC matches, flushes, reads the file back and checks the CRC again, and only then starts the AM install and feeds AM from the file in 64 KiB chunks. A slow or interrupted network can then not break AM's stream, and what AM sees is a plain local file. The staging file is removed afterwards (also after a failure). It needs `files_dir`; the app's `on_begin`/`on_resume` still bracket the whole push.

**Automatic fallback.** `push` (no flag) tries the direct install. If the console answers `error 5, 13, 14, 15, 17 or 18`, or the connection breaks, the PC waits 4 s (the console shows the error for 3 s, then resumes the game) and sends the same CIA with `--stage` once. `--no-stage-fallback` and `--bad-crc` never retry.

**Failure handling.** Every failed push replies `error <code> <what>: stage=<stage> at=<offset> rc=<hex> (install cancelled, old install kept)` and logs the same: `13` AM_StartCiaInstall (`stage=am-start`), `14`/`15`/`16` connection closed, receive timeout, sleep, `17` AM or store write (`am-write`, `am-write-staged`, `stage-receive`, `stage-create`, `stage-rename`, `stage-open`), `5` CRC (`crc`, `stage-receive`, `stage-verify`), `18` AM_FinishCiaInstall (`am-commit`). `rc` is AM's (or the store's) result code, for a CRC error the CRC received. Before AM_FinishCiaInstall the install is cancelled with AM_CancelCIAInstall, so the old install stays; the commit moves the handle to AM (libctru), so there is nothing to cancel afterwards; that a failed commit keeps the old title is AM's behaviour and is a hardware check item. The app is resumed (`on_resume`) and the PC may retry at once.

**Pause bounded, relaunch bounded.** `on_begin` (TTYD: `port3ds/plat/devupdate.c`) waits at most 2 s for the game thread to reach a frame boundary (`plat_frame_poll`, also polled inside the Options menu loop). Past that the update goes on with the game thread running, it holds at its next frame boundary, and the log line names what it was in: phase, map, Options menu open, save busy, stereo on, watchdog holds. `on_ready` waits for a card write only (up to 10 s); it no longer waits for the watchdog hold count, which the Options menu, a load and the pause itself all raise. After the install the library gives the process 8 s to end (the app's exit path through the game thread); if the game thread never reaches a frame boundary (a stuck load or loop), the server thread ends the process itself with the relaunch set (`recovery_exit(true)`). A failed or crashed state cannot hold an update: the recovery thread serves pushes with no pause at all.

## Steps

1. **Hardware check of today's handler:** `crashtest=segv` and `hang` on the console. This verifies SVC 0x3B and the real exception path before the code moves.
2. **Extract to `libs/ctr-devkit/crash/`,** with the replacements above. Nothing should change in behaviour for TTYD: the same report lines, the same screen. `port3ds/plat/crash.c` shrinks to glue: config, sinks, app info, shutdown hooks.
3. **Example app** `libs/ctr-devkit/examples/crash/`: a few lines of setup plus `crash_test` from buttons. It runs as a 3dsx and as a CIA.
4. **`devupdate`** follows DEVUPDATE_PLAN.md inside the same folder. The `on_recovery` hook connects the two.
5. **README** for the family: what each module does, the link flag for `svcBreak`, and the dev-only warning for `devupdate`.

**Done when:**
- TTYD's `crashtest` kinds behave exactly as before on Azahar and hardware, with the glue only;
- the example app builds standalone from `libs/ctr-devkit/` with only devkitPro installed.

## Supervisor (phase 1: design and skeleton, 2026-11-26)

**Owner goal.** The PC and the New 3DS stay unattended while the planner pushes CIA builds again and again. The console must never need a manual reboot. **Owner decisions:**
- a custom system module runs all the time, outside the game's process (development only, never in a release);
- no automatic rollback: a new build can always be pushed and launched, whatever the game's state (hung, crashed, ended, boot-looping, not running, at the HOME Menu);
- an optional ctr-devkit add-on (`libs/ctr-devkit/supervisor/`): an app opts in, and nothing breaks without it;
- installed and removed from the game's developer Select menu, through AM, not with FBI or PC steps;
- the supervisor can be updated remotely.

**Console fact (owner, 2026-11-26):** after a PICA hang, ending and relaunching the game does not reset the GPU. The next process hangs at its first frame.

### Facts

Sources: a Luma3DS master snapshot (13.x era: it has the plugin loader, the hb autoboot and the memory-mode override) at `~/Documents/GitHub/ftpd/_examples/Luma3DS-master`; libctru headers and `libctru.a` under `/opt/devkitpro`; the build in this repository. **V** = verified in that source or by a run here; **A** = assumed; **T** = test on the console. The console's Luma3DS version is not known here (Rosalina menu > System info): the facts hold for the version above.

| # | Fact | |
|---|---|---|
| F1 | PM launches any title through loader. For a `00040130` title, when the Luma3DS option "Enable loading external FIRMs and modules" is on, the loader first opens `/luma/sysmodules/<title ID>.cxi` from the SD card and uses it; otherwise the title comes from NAND (`loader.c` `RegisterProgram`, `patcher.c` `openSysmoduleCxi`). The title database is never checked. So a supervisor can be a NAND title (AM install) or an SD file (needs that option on) | V |
| F2 | Nothing in Luma3DS starts an extra system module at boot. Its `/luma/sysmodules/<name>.cxi` only replaces the 5 FIRM modules by name (`arm9 firm.c`). PM starts NS at boot with its dependencies (`pm launch.c` `autolaunchSysmodules`). Starting a module at boot would mean an ExHeader override of NS or of the HOME Menu (`/luma/titles/<tid>/exheader.bin`, it needs "game patching" and "external modules" on) that adds it as a dependency. If that dependency fails to load, NS or the HOME Menu does not start, so the console does not boot until a person changes the Luma config (SELECT at boot) or takes out the SD card | V (source), so this design does not use it |
| F3 | Launched with `PMAPP_LaunchTitle(..., 0)`, a module is not an application and not a dependency, so it runs until the console reboots: the app ending, crashing or being ended does not touch it. A launch of a title that already runs returns at once with success (`launch.c` `LaunchTitle`: `foundProcess`) | V (source) |
| F4 | PM panics (the console stops) when NATIVE_FIRM launches a `00040130` title whose low byte is not `02` (`launch.c`, `panic(4)`). The supervisor's title ID is `0004013000FD8A02` (RSF UniqueId `0xFD8A`, Category Base, Version 2, as Luma's `pm.rsf`). Checked in the built CIA and CXI | V (build) |
| F5 | Luma3DS's sm does not check the ExHeader service access list (`sm processes.c`: "Service access list checks removed"). Any process gets `pm:app`, `am:net`, `ns:s`, `ptm:sysm`, `soc:U`. But session counts still apply: `pm:app` has 4 sessions system-wide (`pm main.c`), Rosalina holds one all the time, so a client opens it only for one call | V |
| F6 | Luma3DS's kernel extension dispatches SVCs from its own table with no check of the process's SVC access mask (`k11_extension svcHandler.s`). `svcGetProcessInfo` type `0x10001` returns a process's title ID (`GetProcessInfo.c`). With `svcGetProcessList` and `svcOpenProcess`, any process sees whether the game runs | V (source); Azahar answers too (the app build reported `state=stopped`, so the calls succeeded there) |
| F7 | A thread with an exception handler in its TLS (libctru `threadOnException`) gets its own faults. Luma3DS shows its exception screen only when the handler is missing or invalid (`fatalExceptionHandlersMain.c` `isExceptionFatal`). The supervisor installs one on each thread, and the handler calls `svcExitProcess`. `svcBreak` from a process that is not debugged goes to Luma's own handler (`svc.c` `BreakHook`), so the module links with `--wrap=svcBreak`, and its wrapper ends the process | V (source); T (behaviour) |
| F8 | PM ends processes in two steps: notification `0x100`, then after the timeout `svcTerminateProcess` by force (`termination.c` `commitPendingTerminations`). Also at shutdown and reboot. So a hung game is ended by `PMAPP_TerminateTitle(tid, timeout)`, and a supervisor that does not answer `0x100` cannot hold up a reboot. On the app's end PM sends `0x110` to NS, the same path as an app that crashed | V (source); T (a game hung in the GPU wait ends) |
| F9 | Rosalina, a system module, uses `soc:U`. Its socket context comes from the SYSTEM region (`minisoc.c`, `MEMOP_REGION_SYSTEM`), it waits for `soc:U` to be registered, and it holds the NDM "infrastructure" exclusive state, so Wi-Fi does not change to StreetPass when the lid closes. `SOCU_Initialize` passes the caller's process ID, so each process has its own socket context: the game's socket context ending does not close the supervisor's sockets | V (source); A (independence); T |
| F10 | The supervisor's ExHeader has `MemoryType: System`, so its heap comes from the SYSTEM region. Its heap is fixed and small (0x51000 bytes with the 0x20000 socket context). libctru's default heap setup takes all free memory of the resource limit, which would starve the system | V (libctru `allocateHeaps.o` uses the resource limits); T (a 0x20000 socket context is enough; Rosalina uses 0x60000) |
| F11 | Reboot paths: Rosalina reboots with `APT_HardwareResetAsync()` with no `aptInit` (`menus.c`); libctru also has `PTMSYSM_RebootAsync` and `NS_RebootSystem`. `NS_RebootToTitle(mediatype, tid, memtype)` reboots straight into a title: NS writes the title into the deliver-argument block, which survives the warm reboot and is read again by Luma3DS (`arm9 deliver_arg.c`: the block at 0x20000000, CRC checked, title at +0x440, "run the title" flag at +0x460) | V (source and headers); T (`NS_RebootToTitle` from a module on Luma) |
| F12 | Launch arguments across a supervisor launch: `devupdate_launch_args()` reads `sdmc:/3ds/ctrdk-launch-args.txt` (prefix `ctrdk:`, deleted after reading) whenever APT has no deliver arguments, on a console too (`devupdate.c`). A module cannot set APT deliver arguments (no APT application state), so it writes that file, then launches the game | V (source) |
| F13 | `GSPGPU_ResetGpuCore` exists (libctru `gspgpu.h`). Whether GSP accepts it from a process that does not hold the GPU right, and whether it frees a stuck PICA, is not known | T |
| F14 | A warm reboot (any path in F11) resets the GPU with the rest of the SoC | A (it is a full reset; the deliver-argument register is the one thing kept) |
| F15 | AM installs a `00040130` CIA to NAND (`AM_StartCiaInstall(MEDIATYPE_NAND)`) on Luma3DS, the way FBI installs system titles | A; T |
| F16 | Azahar cannot start a custom module through PM (not tried; its PM is HLE). The app build of the same server runs there | V (app build: ping, status, an unknown command and a bad header answered as specified) |

### What runs where

| Piece | Where | Does |
|---|---|---|
| `ctrdk-supervisor.cia` (`supervisor/main.c`, `server.c`, `supervisor.rsf`) | its own process, core 1, priority 48, from start until reboot | TCP 5051 (protocol below); watches the game's title ID; phase 2 terminates, installs, launches and resets |
| `ctrdk_supervisor_client` (`supervisor/client.c`, `include/ctrdk/supervisor.h`) | in the app, development builds | `ctrdk_supervisor_status()`, `install(cia, game_tid, port)`, `uninstall()`, `ensure()` |
| `ctrdk-supervisor-app.3dsx` (`supervisor/app_main.c`) | Azahar | the same server as an ordinary app, for protocol tests |
| `devupdate.py supervisor ping|status` | PC | talks to port 5051 (`--sv-port`, `$CTRDK_SV_PORT`) |

**How it gets started.** On demand, never at boot (F2):
- `ctrdk_supervisor_install()` starts it at once after the install, so no reboot is needed (`reboot_needed` is set only if the start fails);
- the app calls `ctrdk_supervisor_ensure()` as the first thing in `main()`, before the GPU, audio or the disc. After any reboot, the first launch of the game starts it again;
- a boot-looping game still starts it, unless the build fails before that line.

**What is not covered:** a cold boot or a power loss with nobody there. Until someone launches the game once, nothing listens. (Open question Q1.)

### Protocol (port 5051)

devupdate's 64-byte `CDU1` header and text replies, ended by `done` or `error <code> <text>`, so `devupdate.py`'s `Link` works unchanged. Command 1 (`ping`) is shared with devupdate. The supervisor's own commands are `0x100` and up, so they never collide with devupdate's. The optional `token=` in `sdmc:/3ds/ctrdk/supervisor.cfg` is checked as devupdate checks its token.

| Cmd | Name | Reply | Phase |
|---|---|---|---|
| 1 | ping | `pong supervisor version=<stamp> kind=sysmodule uptime=<s> game=<tid> state=running|stopped|unknown pid=<n> proto=1` | 1 (built) |
| 0x101 | status | `status key=value` lines: kind, version, uptime, ip and port, game title, state, pid, last heartbeat, commands | 1 (built; heartbeat says `none`) |
| 0x102 | terminate | `PMAPP_TerminateTitle(game, 3 s)`: notification `0x100`, then by force (F8). Then a GPU reset attempt (F13) | 2 |
| 0x103 | launch `<switches>` | writes `ctrdk:<switches> ctrdk_from=supervisor` to `sdmc:/3ds/ctrdk-launch-args.txt` (F12), then launches the game through NS (`NS_LaunchTitle`, so NS and the HOME Menu know it is the application; T) | 2 |
| 2 | push (CIA) | **always** terminate, then install through AM (`MEDIATYPE_SD`, 64 KiB writes, CRC before the commit, cancel on error, as devupdate), then launch with the switches. A CIA with the supervisor's own title goes to NAND (update, below) | 2 |
| 6 | files get/ls | read-only: the app's folder (`sdmc:/3ds/<app>/`: `latest.log`, `previous.log`, `crash.txt`, `gpuhang/`), so logs and dumps come back even when the game is gone | 2 |
| 0x104 | watch `<N>` / `0` | arms or disarms the watchdog (policy below) | 2 |
| 0x105 | reboot `[title]` | last resort: `NS_RebootToTitle(SD, game)`, else `APT_HardwareResetAsync()` (F11) | 2 |
| 0x106 | exit | the supervisor ends (for its own update and for uninstall) | 2 |

Every request has a time limit (10 s per receive, as devupdate). No request waits for the game. One client at a time. A stuck PC is dropped by the receive timeout.

### Heartbeat

The supervisor knows the game is alive from two signals:
- **the process** (F6): it runs, or it does not;
- **a heartbeat** (phase 2): the supervisor registers a service `ctrdk:sv` (one IPC port, 2 sessions). The game calls `ctrdk_supervisor_heartbeat(frame, flags)` once per second at a frame boundary: one IPC call, a few microseconds. `flags` are `paused` (Options menu, developer menu, an update in progress), `loading`, `recovery` (the crash screen is waiting for the PC). A heartbeat that does not change means the game loop is stuck, even when devupdate's server thread on port 5050 still answers.

Without the supervisor, the client's heartbeat does nothing. It tries the service again once every 10 s and does `ensure()` then.

Why not the other signals:
- a TCP ping to port 5050 only shows that devupdate's thread is alive, not the game loop;
- a counter in an SD file wears the card and is slow;
- shared memory needs the same IPC to exchange the handle.

### Policy

The supervisor acts only when the PC asks, or after the PC armed the watchdog. A paused or recovering game is never "hung".

- **Push:** terminate (3 s) -> GPU reset attempt -> install -> launch with switches -> reply `done` after the launch call. The PC then waits for the first heartbeat.
- **Watchdog** (armed with `watch N`): no heartbeat change for N s while the process runs and the flags are neither `paused` nor `recovery` -> terminate -> GPU reset attempt -> relaunch with the last switches -> one line in `sdmc:/3ds/ctrdk/supervisor.log`. At most 2 relaunches per push. After that it disarms and waits for the PC.
- **Boot loop:** nothing automatic beyond staying reachable (owner decision). The game starts it before it can crash, so the PC can always push a fixed build.
- It never blocks on the game: all calls into PM, NS and AM have timeouts or return at once (F3, F8), and the server thread never waits on the game.

### GPU hang: which side does what

1. **The game, at start-up** (another agent adds this): it resets the GPU before its first frame. It holds the GPU right after `gfxInit`, which is the natural place for `GSPGPU_ResetGpuCore`.
2. **The supervisor, between terminate and launch:** it opens `gsp::Gpu` and calls `GSPGPU_ResetGpuCore`, first without the right, then with `GSPGPU_AcquireRight` (the game is gone; the HOME Menu may hold the right), then it releases the session. This is a fallback for a hang that stops the game before its own reset (for example in `gfxInit`, waiting for the GX queue). Whether GSP accepts each step is test item H4.
3. **Neither works** (the relaunched game gives no heartbeat within 30 s, and its log shows it stuck at frame 0): the PC sends `reboot`. The supervisor calls `NS_RebootToTitle(MEDIATYPE_SD, game, 0)`. The reboot resets the GPU (F14) and comes back in the game, whose `ensure()` starts the supervisor again. So the console stays reachable with no person. Only if that call fails, it uses `APT_HardwareResetAsync()`. That comes back at the HOME Menu with nothing listening, so the PC runner stops with a report.

### Install, update, uninstall

- **Never in a release.** The supervisor CIA is never inside the game's CIA, and `tools/port3ds/release.py` packages only `ttyd.cia`, `ttyd.3dsx` and `res/`. The game only installs it from a file that the developer puts on the SD card. In TTYD that file is `sdmc:/3ds/ttyd/queue/ctrdk-supervisor.cia`, sent with `devupdate.py files put`. The Select-menu row is in the developer menu (ZL+SELECT), which a release player does not see by default.
- **Install:** `ctrdk_supervisor_install()` installs it to NAND through AM. It refuses any CIA whose title ID is not `0004013000FD8A02`, so it never writes another title to NAND. Then it writes `supervisor.cfg` (port, the game's title ID) and starts it.
- **Update** (phase 1): put the new CIA, then Select menu row "Supervisor" A (remove), then A (install). Install ends a running old supervisor first (`PMAPP_TerminateTitle`, 3 s).
- **Update** (phase 2, remote): `devupdate.py supervisor push ctrdk-supervisor.cia`. The supervisor installs its own title to NAND (its code is already loaded; F15), launches the game if it is not running, replies, and exits. Then the game's `ensure()` starts the new one within 10 s.
- **Uninstall:** `ctrdk_supervisor_uninstall()` ends it, deletes the NAND title (the fixed title ID only) and removes `supervisor.cfg`.
- **If the supervisor crashes:** it ends itself quietly (F7) and the console goes on. Nothing starts it at boot (F2), so a broken supervisor can never stop the console from booting. The worst case is that the next `ensure()` starts it again, and it crashes again, once per game launch.

### What still needs a person

These cannot be fixed from software:
- a kernel panic, or an ARM9 or ARM11 kernel exception (the Luma3DS exception screen);
- a PM panic (F4 is why the title ID is fixed);
- an exception screen of another system module;
- a power loss;
- a cold boot (Q1);
- a game build that crashes before `ensure()`.

How to keep them rare:
- the supervisor itself never reaches an exception screen (F7);
- it uses no GPU, APT or HID;
- it starts nothing at boot;
- keep the console on the charger with the lid open: no sleep, Wi-Fi stays on (F9).

### PC side: the unattended runner (phase 2 contract)

`devupdate.py loop manifest.json`. Each entry: `{cia, switches, until, timeout_s, collect}`. For each entry:

| Step | Talks to | Time limit | On failure |
|---|---|---|---|
| 1. `supervisor ping` | supervisor | 10 s, 3 tries | stop, report "supervisor gone" (the only stop besides the end) |
| 2. push the CIA (terminate, install, launch) | supervisor | 120 s | retry once; then fall back to devupdate on 5050 (`push`, with its `--stage` fallback); then go to step 6 for this entry |
| 3. wait for the first heartbeat (`status`) | supervisor | 60 s | `terminate` + `launch` once; then `reboot` (wait 90 s for the supervisor's ping after it); then record "no start" |
| 4. run: poll `status` every 5 s until the `until` pattern (in `latest.log`) or `timeout_s` | supervisor (+ `files get @/latest.log`) | `timeout_s` | record "timeout" |
| 5. collect `latest.log`, `crash.txt`, `gpuhang/*` | supervisor `files` (devupdate if the game still answers) | 60 s | record "no logs" |
| 6. one line in `build/loop/<run>/results.tsv`: entry, result, times, the paths it collected | - | - | - |

- After the last entry the runner prints the table and exits 0.
- It exits 2 only when the supervisor does not answer in step 1. Then the report says which entry was running and the last status the runner saw.
- The runner never waits without a time limit. It polls only the console, never an agent.
- The planner starts the runner as one background job.

### Test plan (console, owner-gated)

- **H1** Install from the Select menu. Then `devupdate.py supervisor ping` from the PC.
- **H2** Ping while the game is at the HOME Menu, after the game is closed, and while the game is hung (`gpuhangtest=600`). Expect: it answers, `state=` is right.
- **H3** In the game's crash screen (`crashtest=segv`): the supervisor still answers.
- **H4** After `gpuhangtest`: terminate from the supervisor (phase 2 build). Then test each GPU-reset step of "which side does what", and relaunch. Expect: the next game reaches frame 1, or the step fails with a known `rc`.
- **H5** `reboot`: `NS_RebootToTitle` comes back in the game, and the supervisor answers within 90 s.
- **H6** Uninstall from the Select menu. Then the ping fails, and a reboot is clean.

### Open questions for the owner

1. **Q1, cold boot:** does "always" include the console after a power loss or a manual reboot with nobody there? If yes, the only way found is a boot-time dependency through a Luma ExHeader override (F2), with the boot risk given there. Recommendation: no; keep the start on demand.
2. **Q2, Wi-Fi:** should the supervisor hold the NDM exclusive infrastructure state like Rosalina (F9)? Then Wi-Fi does not change to StreetPass when the lid closes, but StreetPass and SpotPass stay off while it runs.
3. **Q3, the watchdog default:** off until the PC arms it (this design), or on at a fixed N after each supervisor launch?
4. **Q4, carrying the CIA:** keep the supervisor CIA as a separate file put on the SD card (this design; a release can never carry it), or embed it in the romfs of `--dev` builds only? Embedding needs a CMake option that `release.py` refuses.

### State of phase 1

- **Built:**
  - `ctrdk-supervisor.cia` (73,984 bytes) and `.cxi`, title `0004013000FD8A02` (checked in both files);
  - `ctrdk-supervisor-app.3dsx`;
  - `ctrdk_supervisor_client`;
  - the game dev build with the Select-menu row.
- **Run in Azahar (the app build):** `ping` and `status` answered, an unknown command got `error 4`, a bad header got `error 1`.
- **Not run:** the module itself (Azahar cannot start it, F16); nothing on the console.
- **Phase 2:** built; see the next section.

### Phase 2 (built 2026-11-26)

**Owner questions still open; each choice is a switch** (`sdmc:/3ds/ctrdk/supervisor.cfg`, `include/ctrdk/supervisor.h` lists every key):

| Question | Default built | Switch |
|---|---|---|
| Q1 cold boot | on demand only: no ExHeader override at boot | none in code; an override would be an owner step on the SD card (F2) |
| Q2 NDM hold | off | `ndm_hold=1` (Rosalina's calls: `NDMU_EnterExclusiveState(INFRASTRUCTURE)`, `NDMU_LockState`) |
| Q3 watchdog | off until the PC sends `watch N` | `watch=N` arms it at every supervisor start |
| Q4 carrying the CIA | a separate SD file (`files put`, Select menu), never in the game CIA | remote update: `supervisor self-update` |

Other switches: `gpureset=0|1|2` (default 2), `launch=ns|pm` (default ns), `launch_flags=` (default 1), `terminate_ms=` (3000), `max_relaunches=` (2), `log=0|1` (1, `sdmc:/3ds/ctrdk/supervisor.log`, moved to `supervisor.old.log` past 256 KiB).

**Commands as built** (`supervisor/server.c`):

| Cmd | What it does |
|---|---|
| 0x102 terminate `[gpureset=N]` | `PMAPP_TerminateTitle(game, terminate_ms)` (0x100, then PM's forced end); if the process still runs 1 s later, `svcTerminateProcess` on it. Then the GPU reset attempt. Reply: `terminate pm=<rc> pid= ms= forced= kill= state=` and `gpureset ...` |
| 0x103 launch `<switches>` | ends the game first when it runs (a launch of a running title does nothing, F3), GPU reset, writes `ctrdk:<switches> ctrdk_from=supervisor` to `sdmc:/3ds/ctrdk-launch-args.txt`, `NS_LaunchTitle(game, launch_flags)` (or `PMAPP_LaunchTitle`). A failed launch removes the file, so a later manual start does not pick up the switches |
| 2 push | terminate, `AM_StartCiaInstall(SD)`, 64 KiB writes at 64 KiB offsets, CRC before `AM_FinishCiaInstall`, cancel on error (devupdate's error codes and `stage=` words), GPU reset, launch. On a failed install the old build is launched again, so the PC can retry here or on 5050. A title other than `game=` is refused (`error 24`); with `game=` empty the pushed title becomes the game. The supervisor's own title: NAND install, launch the game if it is not running, reply, exit |
| 6 files `get|ls` | read-only, `dir=` (written by the app's `ensure()`: its working directory); `@sv/...` is `sdmc:/3ds/ctrdk/`. Two passes over the first N bytes, so a log the game appends to still matches its CRC |
| 0x104 watch `N` | arms (0 disarms); the relaunch count starts at 0 |
| 0x105 reboot `[hard|dry]` | replies first, then `NS_RebootToTitle(SD, game, 0)`; if it fails, or nothing happens for 15 s, `APT_HardwareResetAsync` |
| 0x106 exit | replies, then the module ends |

**Bounded waits.** The module sets `srvSetBlockingPolicy(true)`: a full service port (pm:app has 4 sessions) fails at once instead of waiting. PM's terminate has its own timeout. `GSPGPU_AcquireRight` can wait while another process holds the right, so the GPU reset runs on its own thread; the server waits 2 s at most, and while an earlier attempt still waits in GSP the next one is skipped. Receives time out after 15 s (push) or 10 s (header). Nothing waits for the game.

**GPU reset** (F13, H4): opens its own `gsp::Gpu` session with raw IPC (no `gspInit`, which would register an interrupt queue and take the right): `ResetGpuCore` (0x001B0000), then `AcquireRight(0)` (0x00160042), `ResetGpuCore`, `ReleaseRight` (0x00170000). Each result is in the reply and the log. Needs only the `gsp::Gpu` session (Luma's sm does not check the RSF list, F5; the RSF lists it anyway).

**Heartbeat** (`supervisor/hb.c`, `client.c`): service `ctrdk:sv`, 3 sessions. Command `0x00010080` (frame, flags) -> `0x00010080` (0, watch seconds). `ctrdk_supervisor_heartbeat(frame, flags)` only stores two words; a client thread (priority 0x3F, the app's core) sends them once per second, so the game's frame loop never waits on IPC. Without the service it looks again every 10 s and calls `ensure()` (every 60 s when the supervisor is not installed). With devupdate linked, the thread adds `paused` while an update holds the app (`devupdate_busy()`) and `recovery` in recovery mode. TTYD (`port3ds/plat/boot.c`, dev builds only): `ensure()` after the log starts, before the GPU, audio and the disc; one heartbeat per `plat_frame_poll`, `loading` while a watchdog hold is up.

**Watchdog** (`supervisor/policy.c`, plain C, tested on the PC by `devupdate.py selftest`): off, or the game stopped -> nothing; `paused`/`recovery` -> the timer restarts; no change of the frame value for N s (4 N with `loading`) -> terminate, GPU reset, relaunch with the last switches, at most `max_relaunches` per push or arm, then it disarms itself. A game that hangs before its first heartbeat counts from the launch. "Unknown" process state (emulators) counts as running.

**PC side** (`devupdate.py`): `supervisor ping|status|terminate|launch|push|files|log|watch|reboot|exit|self-update`, and `loop manifest.json` as in the runner contract above. Manifest entries: `{name, cia, switches, until, frames, timeout_s, watch, collect}`; an entry without `cia` launches the installed build. Per-step limits are manifest fields (`push_timeout_s` 120, `start_timeout_s` 60, `reboot_wait_s` 90, `poll_s` 5, `log_poll_s` 10). Results: `build/loop/<run>/results.tsv`, `loop.log`, and `NN_<name>/` with `latest.log`, `crash.txt`, `gpuhang/*`. Exit 2 (with the entry and the last status in `loop.log`) only when the supervisor stops answering.

**Tested off the console:**
- `devupdate.py selftest`: 80 of 80 (the push framing as before; each supervisor request through the device's decoder; 29 cases of the watchdog policy and the text helpers, `policy.c` built with gcc; the runner against mock consoles: pattern end, frame-count end, push fallback to 5050 after two refused supervisor pushes, exit 2 when the supervisor is gone).
- Azahar, the app build (`ctrdk-supervisor-app.3dsx`, which sends itself heartbeats through the real client and service), two sessions, 2026-11-26:
  - `ctrdk:sv` registered, 1 session, the heartbeat count and frame value rose once per second (status `heartbeat=23 frame=1198`), then froze at `app_hb_stop`;
  - `files get @/latest.log` (70,890 bytes) and `gpuhang/dump_1.bin` (76,800 bytes) byte-identical; `ls @/`, `ls @/gpuhang` right; `@/../ctrdk/supervisor.cfg` -> `error 20 bad path`; a missing file -> `error 21`; `put` -> `error 20 read-only`; an unknown command -> `error 4`; `log --sv` returned the module's log;
  - `reboot --dry` answered; `terminate` with the game not running: `state=stopped (nothing to end)`;
  - `terminate` with `gpureset=2`: Azahar logs `ResetGpuCore` as unimplemented and its `AcquireRight` handler fails an assertion, which ends the emulator. Hence the app build's `gpureset=0` default;
  - `watch 5` armed; it did not fire, as specified: Azahar's process list reports the game `stopped` (it is not running), and a stopped game is never relaunched. The relaunch decision itself is covered by the policy self-test;
  - `launch "map=gor_01 wdog=9"`: args file `ctrdk:map=gor_01 wdog=9 ctrdk_from=supervisor` written, `NS_LaunchTitle` rc 0 (Azahar's NS: nothing starts; the supervisor kept answering);
  - `push build/3ds/game/ttyd.cia` (4,365,760 bytes, 3.3 s): `ok receiving`, `installing`, `installed 000400000fd88600`, launch rc 0;
  - `self-update ctrdk-supervisor.cia`: installed `0004013000fd8a02` to NAND (the title folder appeared in the profile's NAND), launched the game because it was not running, replied `exiting`, and the app ended.

**Only the console can show:** that PM starts the module and the module runs beside the game (F16); `PMAPP_TerminateTitle` on a hung game (H4); whether GSP accepts `ResetGpuCore` from this process and whether it frees a stuck PICA (H4; Azahar does not implement it, and its `AcquireRight` from the holder of the right fails an assertion that ends the emulator, so the app build defaults to `gpureset=0`); `NS_LaunchTitle` from a module with the HOME Menu in front, and the flags it needs; AM writing the module's own NAND title while it runs (F15); `NS_RebootToTitle` coming back in the game (H5); the heartbeat across two processes; NDM.

### One-time console install (owner)

Luma3DS needs no option change: the module is a NAND title (F1).

1. Build the supervisor: `tools/port3ds/build.sh libs/ctr-devkit/supervisor`. It gives `build/3ds/supervisor/ctrdk-supervisor.cia`.
2. Push a game build that has the Select-menu row (this commit or later), as usual.
3. Send the supervisor to the SD card: `devupdate.py --ip $N3DS_IP files put build/3ds/supervisor/ctrdk-supervisor.cia`. It goes to `sdmc:/3ds/ttyd/queue/`.
4. On the console: ZL+SELECT opens the developer menu. Press R for the Debug tab, go to "Supervisor: off" and press A. The row then shows `running ok 00000000`, and the log has `menu: supervisor install: ok 00000000`.
5. From the PC: `devupdate.py --ip $N3DS_IP supervisor ping`. Expect: `pong supervisor ... state=running`.
6. To remove it: the same row, A (it shows `off`).
