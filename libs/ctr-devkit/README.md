# ctr-devkit

Developer tools for any libctru homebrew on the Nintendo 3DS. MIT licence (see `LICENSE`). The modules depend on libctru only. Each works alone, and they connect through one hook.

| Module | What it does |
|---|---|
| `crash` | Crash and hang handler: one versioned report for CPU exceptions, fatals, `svcBreak` and a hang watchdog. It writes to your log and a report file, shows a crash screen on both displays, dumps every thread's registers on a hang, and ends the process cleanly |
| `devupdate` | **Development only.** Installs a CIA over the network while the title runs, relaunches it with test switches, and keeps a crashed process reachable (recovery mode) |
| `devupdate/devupdate.py` | The PC side (Python 3, standard library only): `push`, `ping`, `restart`, `report`, `launch`, `bootstrap`, `files`, `queue`, `azahar-session` |
| `examples/crash`, `examples/devupdate` | Standalone example apps (3dsx and CIA). The devupdate example built as a 3dsx is the generic bootstrap `devupdate-boot.3dsx` |

## Building

You need only devkitPro (devkitARM, libctru, 3ds-tools). For CIAs you also need makerom and bannertool on `PATH`; without them you get the 3dsx only.

```sh
cmake -S libs/ctr-devkit -B build/ctr-devkit -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
cmake --build build/ctr-devkit
```

In this repository, `tools/port3ds/build.sh libs/ctr-devkit` does the same in the pinned Docker image, with the outputs in `build/3ds/ctr-devkit/examples/*/`. Each example also builds on its own (`cmake -S libs/ctr-devkit/examples/crash ...`).

In your own CMake project:

```cmake
add_subdirectory(path/to/ctr-devkit ctr-devkit)
target_link_libraries(app PRIVATE ctrdk_devupdate ctrdk_crash ctru m)
ctrdk_wrap_svcbreak(app)            # -Wl,--wrap=svcBreak: libctru and citro3d panics become crash reports
include(path/to/ctr-devkit/cmake/CtrDevkitApp.cmake)
ctrdk_add_app(app TITLE "My App" UNIQUE_ID 0xFD8xx PRODUCT_CODE CTR-N-XXXX)   # app.3dsx + app.cia
```

Pick a homebrew-range unique ID (`0xF8000`-`0xFFFFF`) that no other title on the console uses. The examples use `0xFD888` (crash) and `0xFD889` (devupdate). The TTYD port uses `0xFD886` and its probe `0xFD887`.

## crash

```c
#include "ctrdk/crash.h"
extern u32 __stacksize__;                 /* libctru's main thread stack */

CrashConfig cfg = {0};
cfg.app_name = "My App";
cfg.report_path = "sdmc:/3ds/myapp/crash.txt";
cfg.line_sink = my_log_line;              /* every report line, for your own log */
cfg.app_info = my_app_info;               /* build string, scene, phase, extra lines */
cfg.watchdog_seconds = 10;
cfg.wrap_svcbreak = true;
crash_init(&cfg);
crash_thread_register("main", __stacksize__);
/* other threads: crash_thread_register("audio", stack_bytes) at their entry */
for (;;) { crash_wd_progress(); /* one frame */ }
```

- **Report format v1.** A report is a run of `[CRASH]` lines. The first one carries the version (`[CRASH] v1 data abort thread=main pc=... lr=... sp=... far=... fsr=... cpsr=...`, or `[CRASH] v1 fatal: <message> (thread=... pc=...)`). The next lines carry the registers, the phase, scene, frames, uptime and build, and the memory. Then comes the stack scan: `[CRASH] stack <words>`, the candidate return addresses. A hang adds a dump of every registered thread (SVC `0x3B`). The build string in the report selects the ELF; `tools/port3ds/crash_report.py <report> --elf <app.elf>` resolves the addresses with addr2line.
- **Outputs.** Each line goes to `svcOutputDebugString`, to your `line_sink` and to `report_path`. The report file is appended with `open`/`write`, so no heap is used.
- **Shutdown hooks.** `crash_add_shutdown_hook(stage, fn, user, order)`:
  - `CRASH_STAGE_SCREEN` runs before the crash screen: take the GPU down without waiting for other threads;
  - `CRASH_STAGE_REPORT_END` runs at the end of a complete report: release audio;
  - `CRASH_STAGE_EXIT` runs on every exit the library makes: restore system settings and close files.
- **Deadlines.** The watchdog thread ends the process if a report takes over 5 s before the screen is up, or if the exit takes over 4.5 s. A second fault inside the report ends the process at once. A fault on another thread while a report runs parks that thread.
- **Self tests.** `crash_test("segv" | "null" | "undef" | "fatal" | "break" | "hang" | "self")`. Set `cfg.test` and `cfg.test_frame` to run one at boot or at a frame. `self` calls the handler with made-up registers, because emulators such as Azahar never raise the guest's handler on a real fault.
- **Recovery.** `cfg.on_recovery` runs after the report and the crash screen. If it returns true, the screen says "recovery: waiting for the PC" and the crashed thread waits for START or for `crash_recovery_exit()`.

## devupdate

> **Development builds only.** Anyone on the same network can install a title on the console while devupdate runs. Start it only in dev builds or behind a switch. Set a `token` if the network is shared. It exposes CIA install, relaunch and the report, and nothing else.

```c
#include "ctrdk/devupdate.h"

const char* sw = devupdate_launch_args();  /* switches this launch received ("" from the HOME Menu) */
DevUpdateConfig dc = {0};
dc.version = "1.2 #345";
dc.on_begin = pause_app;                   /* push/restart starts: pause at a safe point, mute audio */
dc.on_ready = finish_saves;                /* installed: let saves finish; true = relaunch now */
dc.on_resume = resume_app;                 /* no relaunch after all (an error, or --no-relaunch) */
dc.on_relaunch = leave_main_loop;          /* or NULL and poll devupdate_relaunch_requested() */
devupdate_start(&dc);
```

- **Protocol.** TCP port 5050. The PC sends a 64-byte header: magic `CDU1`, version, command, flags, payload size, CRC32, argument length, title ID and a 32-byte token. Then come the arguments and the payload. The console answers with text lines and ends each request with `done` or `error <code> <text>`. The full layout is at the top of `devupdate/devupdate.c`.
- **Install.** `AM_StartCiaInstall` on the SD card, then one `FSFILE_Write` per chunk as it arrives, so nothing is kept in RAM. The CRC is checked before `AM_FinishCiaInstall`, and any error calls `AM_CancelCIAInstall`, so a bad push leaves the old install working. This needs `am:net` (the example RSF has it). A 3dsx refuses installs unless `allow_3dsx_install` is set, as the bootstrap does.
- **Writes to AM** are always whole 64 KiB chunks (the last one short), whatever the network's segments. A failed push replies `error <code> <what>: stage=<stage> at=<offset> rc=<AM result> (install cancelled, old install kept)`. `push --stage` writes the CIA to `<files_dir>/stage.cia` through the app's store first, checks its CRC from the file, then installs from there; `push` retries that way by itself after a failed direct install (`--no-stage-fallback` stops it). `devupdate.py selftest` checks the request framing against a host build of the device's decoder (`proto.c`). Details: `docs/CTR_DEVKIT_PLAN.md`, "protocol, `--stage` and failure handling".
- **Relaunch and switches.** `restart <switches>` and a push with relaunch store the switches as APT deliver arguments (`aptSetChainloaderArgs`, prefixed `ctrdk:`) and chainload the title itself. At boot, `devupdate_launch_args()` reads them with `APT_ReceiveDeliverArg`. They belong to that one launch only. `devupdate_launched()` tells whether a devupdate relaunch started this process.
- **HOME and sleep.** An `aptHook` stops taking requests while the HOME Menu is open or the console sleeps. Sleep during a transfer cancels the install (`error 16 sleep`).
- **Recovery mode.** `devupdate_start` prepares a dormant thread, a socket buffer, the AM session and the title ID while the app is still healthy. After a crash, `devupdate_enter_recovery(report)`, called from the crash module's `on_recovery`, wakes that thread. The thread serves `ping`, `report` (the crash report and the tail of `log_path`), `push` and `restart`. If the network is not up within `recovery_net_seconds`, it ends the process. `on_recovery_exit` ends the process through the app; with the crash module, that is `crash_recovery_exit(relaunch)`.

### PC tool

```sh
export N3DS_IP=192.168.1.50                     # or --ip; Azahar: --ip 127.0.0.1
devupdate.py ping
devupdate.py push build/app.cia --switches "level=3 wdog=5" --wait 60
devupdate.py push build/app.cia --no-relaunch   # install only
devupdate.py restart level=3 wdog=5
devupdate.py report --out report.txt            # after a crash: the report and the log tail
devupdate.py launch 000400000FD88600 --switches "..."
devupdate.py bootstrap --cia build/app.cia --switches "..."   # first install, no FBI needed
```

`bootstrap` sends `devupdate-boot.3dsx` to the Homebrew Launcher with 3dslink (press Y there first). It waits for the bootstrap to answer, pushes the CIA, and the bootstrap installs it and jumps into it with the switches as deliver arguments. From then on, the installed title's own devupdate takes over.

### Files and the test-run queue

Two more commands, for running a series of tests on the console with nobody there (no live scripting; everything is sent first). Both work in one folder the app chooses with `files_dir` (a path relative to its working directory, e.g. `queue` for `sdmc:/3ds/<app>/queue/`); the PC cannot reach anything outside it.

```sh
devupdate.py files put walk.tpad other.bin [--dest sub]   # upload, CRC32 checked; stored as <files_dir>/[sub/]name
devupdate.py files ls [dir] | get remote [-o local] | rm path...
devupdate.py queue add manifest.json [more files...]      # uploads the padfiles it names, then adds the entries
devupdate.py queue list | clear | start | status
devupdate.py queue pull build/walk                        # results.txt and everything the entries collected
```

- **Store.** The files and the queue are written through `DevUpdateConfig.store`: callbacks for write, append, rename, remove, mkdir, flush and read. An app that keeps SD access on one core passes callbacks that queue the work there (TTYD does, onto its SD service); with `NULL` the library uses stdio. Recovery mode always uses stdio, because the app's SD layer may be what crashed. Listing and sizes use stdio on the server thread.
- **Manifest** (JSON, a list or `{"defaults": {...}, "entries": [...]}`). An entry:

  | Field | Meaning |
  |---|---|
  | `name` | letters, digits, `. _ -`, up to 40; used in file names |
  | `switches` | the app's switches for this run (up to about 600 bytes) |
  | `padfile` | optional: a local file, uploaded; the run gets `<queue_padfile_key>=<files_dir>/<name>` (key default `padfile`) |
  | `until` | optional: a pattern matched against each of the app's log lines; a match ends the entry (result `pass`) |
  | `frames` | optional: ends the entry (result `frames`) after that many app frames |
  | `timeout_s` | seconds from process start; the entry ends with result `timeout` (default 300) |
  | `collect` | app paths (e.g. `shots`) moved to `out/<run>_<N>_<name>/` when the entry ends |
  | `shots` | optional shorthand for a screenshot setting; needs `--shots-switches "shots={n}"` (the app's own switch text) and collects `shots` |

  The `until` pattern is matched by a small built-in matcher (newlib for the 3DS has no `regcomp`): literals, `.`, `[set]`, `[^set]`, ranges, `\d \w \s`, `* + ?` on one of those, `^`, `$` and `|` between alternatives. Groups and counts are refused when queued.
- **Running it.** `queue start` relaunches the title (the restart path: APT deliver arguments) for entry 1 with its switches plus `padfile=...` and `ctrdkq=1`. The launch that finds `ctrdkq=N` runs entry N. The app feeds the library two things: `devupdate_queue_log_line(line)` for each log line and `devupdate_queue_frame()` once per frame. The server thread ends the entry when one of them, the timeout, or `devupdate_queue_finish(result)` says so: it records a line in `results.txt` (`N, name, result, frames, ms, outdir, detail`, tab-separated), moves the `collect` paths, logs `[QUEUE] entry N <name> done result <result> ...` and relaunches for entry N+1. After the last entry it logs `[QUEUE] done` and relaunches with no switches. State lives in the launch arguments, so a cold start from the HOME Menu never continues a queue; `state.txt` only serves `queue status`.
- **A crash** in an entry goes to recovery mode as always; instead of waiting for the PC, the recovery thread records the entry as `crash` (the report goes to `out/.../crash.txt`) and relaunches for the next entry. A hung relaunch (the app's exit path does not end the process within 8 s) is ended by the server thread.
- **Files in `files_dir`:** `queue.txt` (entries, tab-separated), `state.txt`, `results.txt`, `out/`, and the uploaded files.
- **Walks are for milestones.** A queue ties the console up for as long as its entries take; keep routine checks in an emulator.

### The same commands on Azahar

The tool and any harness built on it target hardware or Azahar alike; only `--ip` changes. Azahar's sockets are the host's, so the guest's port 5050 is `127.0.0.1:5050`. One long-lived headless instance serves many commands:

```sh
devupdate.py azahar-session start --profile ~/azahar-dev --boot build/app.cia   # Xvfb, waits for ping
devupdate.py --ip 127.0.0.1 push build/app.cia --no-relaunch
devupdate.py azahar-session stop --profile ~/azahar-dev                          # only this instance
```

Azahar cannot do the APT relaunch. `azahar-session start --supervise` emulates it for a test: it creates `sdmc:/3ds/ctrdk-emulate-relaunch`, and the library, which relaunches by writing the next launch's arguments to `sdmc:/3ds/ctrdk-launch-args.txt` when that marker exists (never on a console), leaves them there as it ends the process. A supervisor process sees the file, ends the instance if it is still around and starts Azahar on the same title again; the new process reads and deletes the file where `devupdate_launch_args()` would have had APT's deliver arguments. A restart, a push with relaunch and a queue's next entry then work in Azahar. What it cannot show is whether APT really keeps the arguments (hardware only).

`azahar-session` takes one of the machine's Azahar slots (flock'd slot files, `AZAHAR_MAX`, default 3; in this repository the lock of `tools/port3ds/run_azahar.py`), and the Azahar process holds the slot for as long as it runs. `stop` ends only the processes of that session.

What Azahar can and cannot show, as checked here (`tools/port3ds/devupdate_azahar.py`):
- **Works in Azahar:**
  - the sockets and the protocol;
  - the CRC refusal (the install is cancelled);
  - the AM streaming install, also over the running title (3 pushes in a row to one instance);
  - recovery mode: ping, report, a refused and a good push after a crash.
- **Does not work in Azahar:**
  - Any APT application jump (relaunch, `restart`, the bootstrap's jump). The Flatpak Azahar's Qt frontend aborts (`std::out_of_range`) or never starts the new title. So the deliver arguments of a relaunch can only be checked on hardware.
  - Keeping the old install after a cancelled push. Azahar's AM writes the CIA straight into the installed title's content folder and deletes it on cancel, so a refused push over an installed title removes that install there. The console's AM imports to a temporary area, but this has to be checked on hardware.

## TTYD integration notes

In this repository, `port3ds/plat/crash.c` and `port3ds/plat/devupdate.c` are the TTYD glue. The switch `devupdate` starts the server. A crash then goes to recovery mode instead of waiting for START.

**Saves are never touched by an update:** before a relaunch, TTYD only waits for a card write in progress to finish. It never deletes, truncates, reformats or recreates save files, and the CIA install leaves `sdmc:/3ds/ttyd/` untouched (`card/` and everything else).
