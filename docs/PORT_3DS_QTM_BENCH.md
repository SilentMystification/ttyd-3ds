# qtmbench: QTM RAM against FCRAM and VRAM on the New 3DS

`libs/ctr-devkit/qtmbench/` is a small CIA (MIT, game-agnostic, part of ctr-devkit) that measures the New 3DS extra RAM ("QTM RAM", mapped at `0x1E800000`, first 2 MiB for the application) against FCRAM (the heap and the linear heap) and VRAM, from the CPU on each core and from the GPU. It checks that the GPU can read from and write into QTM, and what cache maintenance that needs. It also measures how CPU, GPU and DSP traffic slow each other down (the contention matrix), and reads the ARM11 performance counters.

Its numbers are for planning which game data goes into QTM (`port3ds/plat/memplace.c`). Azahar runs it, but Azahar does not model memory timing or caches, so only the console's numbers mean anything.

## The CIA

| | |
|---|---|
| Title | `000400000FD88C00` (`UNIQUE_ID 0xFD88C`, `CTR-N-CDKQ`), separate from the game (`0xFD886`/`0xFD88A`/`0xFD88B`), the probe (`0xFD887`) and the devkit examples (`0xFD888`, `0xFD889`) |
| ExHeader | `libs/ctr-devkit/qtmbench/qtmbench.rsf`: the game's New 3DS settings (124 MB, 804 MHz, L2, core 2, `MaxCpu 0xD0` = core 1 up to 80%) and the same QTM mapping, `1e800000-1e9fffff` read/write. It also allows `ControlPerformanceCounter` (0x3E), `Invalidate/Store/FlushProcessDataCache` (0x52-0x54) and `ControlProcessMemory` (0x70) |
| Built in | devupdate (push, restart with switches, `files get @/...` from `sdmc:/3ds/qtmbench/`), the crash module (crash report and recovery mode) |
| Build | `tools/port3ds/build.sh libs/ctr-devkit/qtmbench` gives `build/3ds/qtmbench/qtmbench.cia` (and `.3dsx`; the 3dsx has no QTM mapping and no New 3DS settings). `tools/port3ds/build.sh libs/ctr-devkit` builds it with the examples into `build/3ds/ctr-devkit/qtmbench/` |
| Output | `sdmc:/3ds/qtmbench/results.json` (every number), `results.txt` (tables), `log.txt` (one `[R] <key> <value>` line per result as it is produced, so a crash keeps what was done) |

It runs everything at launch, prints `QTMBENCH DONE`, and then idles with the devupdate server up until START.

### Switches

From devupdate (`push --switches`, `restart ...`), 3dslink arguments, or `sdmc:/3ds/qtmbench/switches.txt`:

| Switch | Effect |
|---|---|
| `quick` | short run for emulators: 5 sizes, 256 KiB per trial, 1 trial, 8 K chase loads, 30 ms windows |
| `only=a,b` | suites: `cpu_bw`, `cpu_lat`, `cpu_rand`, `cache`, `conc`, `gpu`, `coherency`, `contend`, `perf` |
| `budget=KiB` `trials=N` | bytes per bandwidth trial (2048) and trials per point (3) |
| `window=ms` | the measuring window of `conc` and `contend` (250) |
| `chase=N` | loads per latency point (262144) |
| `nocore1` `nocore2` | leave a core out |
| `nopte` | skip the page-table walk at the end |
| `norun` | only start (to pull old results) |
| `autoexit` | exit after the run (emulator tests) |

## Running it on the console

```sh
export N3DS_IP=<console address>
D=libs/ctr-devkit/devupdate/devupdate.py
tools/port3ds/build.sh libs/ctr-devkit/qtmbench
# first install: Homebrew Launcher, press Y, then
$D bootstrap --cia build/3ds/qtmbench/qtmbench.cia
# later builds (qtmbench, or any title with devupdate, running):
$D push build/3ds/qtmbench/qtmbench.cia --wait 90
# wait for QTMBENCH DONE on the top screen (about 1-2 minutes), then
tools/port3ds/qtm_bench.py pull                 # -> build/qtmbench/<time>/, prints the tables
# or all in one: push, wait for DONE in the log, pull, print
tools/port3ds/qtm_bench.py run [--switches "..."]
# again with other switches, without a push:
$D restart only=contend window=500
# compare two runs
tools/port3ds/qtm_bench.py diff build/qtmbench/<a>/results.json build/qtmbench/<b>/results.json
```

Leave the console alone during the run (HOME or sleep changes the numbers). The last step, the page-table walk, runs kernel code through `svcBackdoor`; it reads only through the kernel's own linear mappings and checks the address ranges first, but if the console stops at "page tables", reboot it: everything else is already saved. Rerun with `$D restart nopte`.

## The tests

Times come from `svcGetSystemTick` (268 MHz, 3.7 ns). 1 MB = 10^6 bytes. Regions: `heap` (malloc, 8 MiB), `linear` (linearAlloc, 8 MiB), `qtm` (2 MiB at `0x1E800000`), `vram` (vramAlloc, 2 MiB; the application mapping is read-only for the CPU, so only reads are measured, with data put in by the GPU).

### CPU (`cpu_bw`, `cpu_xcopy`, `cpu_lat`, `cpu_rand`)

Each point runs on a thread pinned to core 0, 1 and 2 (core 1 is time-sliced at the 80% limit, so its numbers are best-of-trials). The working set grows from 4 KiB to 2 MiB (8 MiB for FCRAM), which shows the L1 (16 KiB data per core), the shared 2 MiB L2 and memory.

| Test | What it measures |
|---|---|
| `cpu_bw read_ldm` | sequential read, `LDM` of 8 registers; MB/s best of the trials (aux: median) |
| `cpu_bw write_stm` | sequential write, `STM` |
| `cpu_bw copy_ldmstm`, `memcpy` | copy inside the region (MB/s of bytes copied) |
| `cpu_bw memset` | newlib memset |
| `cpu_xcopy` | 1 MiB memcpy and LDM/STM copy between regions (`heap>qtm`, `qtm>linear`, `vram>qtm`, ...) on core 0 |
| `cpu_lat chase` | dependent loads through a random single-cycle pointer chain, one node per 32-byte line: ns per load. Below 16 KiB it is the L1 latency, up to 2 MiB the L2, beyond it memory (plus TLB misses) |
| `cpu_rand rand_read/rand_write` | independent loads/stores at pseudo-random lines: ns per access (aux: million accesses per second); how much the core overlaps misses |

The cache attributes of a region show here: a cached region has L1-speed latency (a few ns) at 4-16 KiB; an uncached one has the same latency at every size.

### Cache maintenance (`cache`)

`gsp_flush`/`gsp_inval` (`GSPGPU_FlushDataCache`/`InvalidateDataCache`, an IPC to GSP) and `svc_flush`/`svc_store`/`svc_inval` (`svcFlush/Store/InvalidateProcessDataCache` on the process itself) over 4 KiB to 2 MiB, `_dirty` right after the range was written, `_clean` after it was only read. Value: us per call; aux: us per MiB. This is the price of every CPU-to-GPU hand-over of a QTM buffer.

### Two cores (`conc`)

LDM readers on two cores at once for the window: the same buffer, two FCRAM buffers, or one core in QTM and the other in FCRAM. Value: core 0 MB/s; aux: the other core's MB/s; note: the sum. Compare with the `single` rows.

### GPU (`gpu_read`, `gpu_write`, `gpu_check`)

citro3d state and draws, but the harness records its own command lists and submits each one alone with `GX_ProcessCommandList` (not citro3d's frame queue). Times are CPU ticks from the submission to the GSP completion event (value, us) and GSP's own perf log, op start to IRQ (aux, us; `GSPGPU_SetPerfLogMode`). There is no GPU timestamp register; these are the closest. Each wait has a 2 s timeout, so a command GSP refuses shows as "failed or timed out" rather than a hang.

Every GPU result is read back by the CPU (after `GSPGPU_InvalidateDataCache`) and compared to a known pattern: `gpu_check` rows count the wrong pixels (0 = correct). The check accepts the image as-is or flipped (the note says which), since the comparison is about memory, not orientation.

| Test | Source / destination | Pattern |
|---|---|---|
| `vbo`, `vbo_x64` | vertex buffer in the region; 32x32 grid of flat-coloured quads into a 256x256 VRAM target; timing: 64 draws shrunk to 8x8 pixels (vertex fetch, not fill) | each 8x8 cell its colour, within 2 |
| `ibo`, `ibo_x128` | index buffer (u16) in the region, vertices in linear | same |
| `tex`, `tex_x16` | 256x256 RGBA8 texture in the region, one full-target quad, nearest; timing: 16 quads | each pixel equals its texel |
| `cmdlist` | the command list itself in the region: an immediate-mode grid (vertex data inside the list, ~300 KiB) recorded in linear and copied there | grid |
| `texcopy_src`, `dispxfer_src` | `GX_TextureCopy` / `GX_DisplayTransfer` reading the region | bytes / texels |
| `fill` | `GX_MemoryFill` of 1 MiB in the region | every word |
| `texcopy` | 1 MiB from linear into the region | bytes |
| `dispxfer` | 256x256 tiled to linear, into the region | texels |
| `rendertarget_x16` | colour buffer in the region (texture in VRAM) | texels |

### Coherency (`coherency`)

For linear and QTM, 16 KiB (fits in L1) and 1 MiB (L2):

- `cpu_wr_gpu_rd_noflush`: memory holds pattern B, the CPU writes A, the GPU copies the range out without a flush. Value: % of words the GPU saw as old (B). Non-zero = the CPU must flush before the GPU reads. `cpu_wr_flush_gpu_rd` is the control (expect 0).
- `gpu_wr_cpu_rd_noinval`: the CPU has read A (clean in its cache), the GPU fills the range with B, the CPU reads without an invalidate. Value: % still A. `gpu_wr_inval_cpu_rd` is the control (expect 0).

Note for the game: citro3d's `C3D_FrameEnd` flushes the whole linear heap (unless `GX_CMDLIST_FLUSH` is passed); it never flushes QTM. Data the CPU writes into QTM for the GPU needs its own `GSPGPU_FlushDataCache`.

### Contention (`contend`, `contend_raw`)

Owner question: can QTM add bandwidth by taking traffic off the FCRAM bus? Victims (rows) are measured for the window while an aggressor (column) runs:

| Victims | |
|---|---|
| `c0_read_heap4M`, `c0_lat_heap4M` | core 0 LDM read / pointer chase over 4 MiB of heap (beyond the L2) |
| `c0+2_read_heap` | cores 0 and 2 reading two 4 MiB heap buffers, summed |
| `c0_read_qtm2M`, `c0_lat_qtm1M` | the same in QTM |
| `gpu_tex_lin`, `gpu_vtx_lin`, `gpu_tex_qtm` | the GPU's texture (16 full-target quads) or vertex (64 grid draws) list, us per list |

| Aggressors | |
|---|---|
| `gpu_tex:lin/qtm/vram` | the GPU texture list looping, texture in that memory (fed from a thread on core 1) |
| `gpu_vtx:lin/qtm` | the vertex list looping |
| `dt:lin>lin` | `GX_DisplayTransfer` 512x512 RGBA8 (1 MiB) FCRAM to FCRAM looping |
| `ndsp` | NDSP playing 8 looping stereo PCM16 48 kHz channels from linear memory (needs `sdmc:/3ds/dspfirm.cdc`) |
| `c2:heap`, `c2:qtm` | core 2 LDM-reading 4 MiB of heap / 2 MiB of QTM |
| `c0+2:heap`, `c0+2:qtm` | cores 0 and 2 reading (GPU victims only) |

`contend` is the slowdown factor (1.00 = unaffected); `contend_raw` has the raw victim number (us, ns or MB/s), the aggressor's own achieved MB/s as aux, and the victim's performance counters in the note when they are available. GPU aggressors do not run against GPU victims (one GPU), nor CPU aggressors on a victim's own core.

How to read it: if `c0_read_heap4M` slows under `gpu_tex:lin` but not under `gpu_tex:qtm`, moving textures to QTM gives the CPU FCRAM bandwidth back. If `gpu_tex_lin` slows under `c0+2:heap` but not under `c0+2:qtm`, CPU data in QTM gives the GPU FCRAM bandwidth back.

### Performance counters (`perf`) and the counter API

User mode cannot read the ARM11 MPCore performance monitor (CP15 c15 is privileged), so the harness does not try. It uses the kernel's `svcControlPerformanceCounter` through the new devkit module `ctrdk/perf.h` (library `ctrdk_perf`): PMN0 = D-cache read misses, PMN1 = external requests, CCNT, and the SCU counters (external reads/writes, line-fill misses per core, port busy). `perf available` says whether the kernel allowed it; `cycles_per_tick` per core should be 3.0 at 804 MHz if CCNT counts that core; `pmn0/pmn1/scu0_per_line_*` are the events per 32-byte line of a 16 MiB read of heap and of QTM.

For the game, per frame:

```c
#include "ctrdk/perf.h"           /* link ctrdk_perf; RSF: ControlPerformanceCounter: 62 */
ctrdk_perf_init(NULL);            /* once */
CtrdkPerfSample a, b, d; char line[160];
ctrdk_perf_read(&a);              /* frame start, on the thread to measure */
/* ... frame ... */
ctrdk_perf_read(&b);
ctrdk_perf_diff(&a, &b, &d);
ctrdk_perf_format(&d, line, sizeof line);   /* "core0 cyc=.. drmiss=.. ext=.. scu_rd=.." */
```

The game's RSF (`port3ds/rsf/ttyd.rsf`) allows the SVC.

### Mapping (`map`)

- `perm`: `svcQueryMemory` for each region (value: permission, 3 = RW; aux: memory state; the note has base, size, state name and physical address). `qtm+2M` is the part the application must not map.
- `prot_to_r`: `svcControlMemory(MEMOP_PROT)` to read-only on one page and back (heap as the control). `alias_map`: `MEMOP_MAP` of a page to `0x13F00000`. These show whether the QTM mapping can be changed at all; there is no user-mode way to choose cache attributes.
- `pte` (console only): the page-table descriptor of each region through `svcBackdoor` (the RSF allows it, Luma implements it): section or page, memory type (`normal inner=WB-WA outer=WB-WA`, `non-cacheable`, `device`, ...), shareable, XN, AP. Also in `info` as `qtm_attrs`, `heap_attrs`, ... This is the authoritative answer to "what cache attributes does QTM have".

## What only the console can tell

Azahar runs every path (all `gpu_check` rows 0, the JSON and tables, devupdate), but:

- every timing (bandwidth, latency, cache maintenance, GPU times, slowdowns): Azahar does not model caches, buses or memory timing;
- coherency: Azahar has no CPU cache, so stale reads never happen there; the `%stale` rows are only meaningful on the console;
- `perf`: Azahar does not implement `svcControlPerformanceCounter`;
- `pte`: skipped in emulators;
- whether GSP accepts QTM addresses for command lists and transfers on real firmware;
- core 1's 80% time slicing, core 2, and the HOME Menu's share.
