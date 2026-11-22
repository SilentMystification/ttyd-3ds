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

## Steps

1. **Hardware check of today's handler:** `crashtest=segv` and `hang` on the console. This verifies SVC 0x3B and the real exception path before the code moves.
2. **Extract to `libs/ctr-devkit/crash/`,** with the replacements above. Nothing should change in behaviour for TTYD: the same report lines, the same screen. `port3ds/plat/crash.c` shrinks to glue: config, sinks, app info, shutdown hooks.
3. **Example app** `libs/ctr-devkit/examples/crash/`: a few lines of setup plus `crash_test` from buttons. It runs as a 3dsx and as a CIA.
4. **`devupdate`** follows DEVUPDATE_PLAN.md inside the same folder. The `on_recovery` hook connects the two.
5. **README** for the family: what each module does, the link flag for `svcBreak`, and the dev-only warning for `devupdate`.

**Done when:**
- TTYD's `crashtest` kinds behave exactly as before on Azahar and hardware, with the glue only;
- the example app builds standalone from `libs/ctr-devkit/` with only devkitPro installed.
