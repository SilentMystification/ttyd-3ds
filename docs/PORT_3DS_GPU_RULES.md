# PICA200 rules probe (gpuprobe)

`libs/ctr-devkit/gpuprobe/` (MIT, libctru and citro3d only) finds which PICA command patterns hang, fault or draw wrong. It runs inside the game CIA (development builds): switch `gpuprobe`, and the game does not start in that mode (`port3ds/plat/gpuprobe_mode.c`). A release build ignores the switch. The qtmbench CIA still has it as a mode too. Azahar accepts nearly everything below, so only the New 3DS run gives rules; the Azahar column documents what the emulator forgives.

## How it works

- Each case records one list (citro3d state plus one draw into a 64x64 RGBA8 target at +16 KiB of a 64 KiB VRAM block, sentinel `0x11223344` before and after it) and submits it with `GX_ProcessCommandList`, outside citro3d's frame queue.
- The probe waits for the P3D event (200 ms on hardware, 5000 ms in an emulator; `probetimeout=ms`).
- On a timeout the case is `HANG`. The probe calls `GSPGPU_ResetGpuCore`, runs a control draw, and if that fails it restarts gfx and citro3d. If the GPU still does not answer, the other cases are `SKIP`.
- After a finished case the CPU reads the block. `OK` = 8 sample pixels as expected and no sentinel word changed. `WRONG` = other pixels or a changed guard word (a write past the target). `ERR` = GSP refused the list.
- Journal `sdmc:/3ds/ttyd/gpuprobe.journal`: a `B <case>` line before each case and an `R <case> <result> <ms> <note>` line after. A case with `B` and no `R` at the next start is `FAULT` (the process died or froze) and is not run again. Finished cases are kept; `probefresh` starts over.
- Outputs: `sdmc:/3ds/ttyd/gpuprobe.txt` (table), the journal, and `sdmc:/3ds/ttyd/gpuprobe/<case>.bin` (the submitted PICA list, for `gpu_cmd_diff.py`).
- Order: danger 0 (controls), 1, 2 (hang likely), 3 (memory faults possible: heap buffers, 2048 texture, `rt_deleted`, which must stay last). Controls are the `ctl_*`, `*_full`, `*_off0/16`, `*_a256` cases.
- citro3d facts found while building it: it uploads a shader binary once, so later lists have no code; `ATTRIBBUFFERS_LOC` is the constant `0x18000000` and each buffer carries its offset; `C3D_BoolUnifSet` takes ids from 0x68; `GPU_SCISSOR_NORMAL` is 3.

## Switches (game CIA, development build)

| Switch | Meaning |
| --- | --- |
| `gpuprobe` | run the probe instead of the game (no OSInit, disc or GX), log `GPUPROBE DONE ...`, then idle with the devupdate server up (`restart` and `push` work) |
| `probe=a,b,grp,pre*` | only these case ids, group names or id prefixes |
| `probefresh` | ignore and delete the journal |
| `probeselftest` | add `self_timeout` (never submits; tests timeout and reset) |
| `probesafe` | skip danger-3 cases |
| `probetimeout=ms` | per-case wait |

## Console run (the planner runs it; the owner gates console pushes)

```sh
D="libs/ctr-devkit/devupdate/devupdate.py --ip $N3DS_IP"
$D restart "gpuprobe probefresh probeselftest"           # the game CIA (development build) is installed and running
```

The run takes about 1 minute plus 200 ms per hang. A freeze or crash: restart the console app (or power cycle), then `$D restart "gpuprobe"` (no `probefresh`; after a crash the updater's recovery mode accepts the restart too): the journal marks the case that died as FAULT and goes on. Repeat until `GPUPROBE DONE` is in the log. Then:

```sh
$D files get @/gpuprobe.txt -o build/gpuprobe_hw.txt         # the table
mkdir -p build/gpuprobe_hw
$D files ls @/gpuprobe                                       # one .bin per case; get each into build/gpuprobe_hw/ (ctl_recover.bin too)
python3 tools/port3ds/gpu_cmd_diff.py --probe-dir build/gpuprobe_hw
```

The probe writes to `sdmc:/3ds/ttyd/` (devupdate's read-only folder, `@/`): `gpuprobe.txt`, `gpuprobe.journal`, `gpuprobe/<case>.bin`. Leave probe mode with `$D restart "<game switches>"`. The Azahar table and lists below were produced by the qtmbench build (`sdmc:/3ds/qtmbench/`); the game CIA gives the same results (checked 2026-11-26 in Azahar: `gpuprobe probefresh probeselftest`, then `gpuprobe` resumed `rt_deleted` as FAULT, then a normal restart booted the game).

Fill the "New 3DS" column below. Every row where the console differs from the Azahar result and from "Expected" becomes a rule: add a function to `PROBE_RULES` (or a format to `TEX_ALIGN_PROBE`) in `tools/port3ds/gpu_cmd_diff.py`, tagged `[probe]`.

## gpu_cmd_diff.py rules

| Tag | Rule | Source |
| --- | --- | --- |
| documented | empty scissor registers at a draw (mode on, right < left, bottom < top, or right/bottom wrapped from 0) | PORT_3DS_PLAN.md "Hardware hangs" |
| documented | empty viewport (width/2 or height/2 not above 0, or an infinite inverse) | same |
| documented | draw with `NUMVERTICES` = 0 | same |
| documented | RGB565 texture base not 128-byte aligned (alignment assumed; fix it from the `tex_rgb565_a*` rows) | STAR3D_NOTES.md |
| documented | vertex shader output written in part (not .xyzw), or in the output map but never written (the checker scans the code from the entry point to the first END; needs a list with the shader upload: `--shader`) | PORT_3DS_PLAN.md, STAR3D_NOTES.md |
| probe | none yet: `PROBE_RULES`, `TEX_ALIGN_PROBE` | this probe |

`gpu_cmd_diff.py --self-test` tests each rule on built lists. `--probe-dir DIR` checks every list the probe saved. On the Azahar lists it flags exactly the documented cases: `sc_w0/h0/inv/zero`, `vp_w0/h0/zero`, `dr_zero_arr/elem`, `sh_part_*`, `tex_rgb565_a8..a64`; no control is flagged. It also shows that the old attribute-offset rule fires on `vb_off2/4/8`.

## Cases and results

"Expected" is the prediction (documented rule, or `?` = unknown). "Azahar" is the result from the run of 2026-11-26 (software-free GL renderer, `probeselftest probefresh`, then a second run that took `rt_deleted` from the journal as FAULT). Azahar timeouts are 5000 ms. "New 3DS" is empty until the console run.

Results seen in Azahar worth knowing:

- Every documented hang (`sc_w0/h0/inv`, `vp_*0`, `dr_zero_*`, `sh_part_*`) is OK in Azahar; `sh_b15` is OK too (the hardware rule says b15 fails).
- `sc_zero` (scissor 0,0,0,0 through citro3d) draws the whole target: right-1 wraps to 0xFFFF.
- `cb_trunc4` (list 4 bytes short) and `cb_nofinal` (no FINALIZE) hang in Azahar too. The GPU recovered with `GSPGPU_ResetGpuCore` each time.
- `rt_dim80` is WRONG as designed: it shows the guard check works.
- `vb_base8` and `vb_off2/4/8` behave as the register values say; the heap cases (`vb_heap`, `ib_heap`, `tex_heap`: physical address 0, citro3d adds no buffer) and `tex_sz2048` draw nothing.
- `rt_deleted`: citro3d panics (svcBreak) when a render target is deleted inside a frame. The process dies; the next run records FAULT from the journal.

| Case | Group | Expected | Azahar | Danger | New 3DS | What |
| --- | --- | --- | --- | --- | --- | --- |
| self_timeout | selftest | HANG | HANG | 0 | |  |
| ctl_arrays | control | OK | OK | 0 | | DrawArrays 3 vertices, full-target triangle |
| ctl_elems16 | control | OK | OK | 0 | | DrawElements, u16 indices |
| ctl_elems8 | control | OK | OK | 0 | | DrawElements, u8 indices |
| dr_arr6 | control | OK | OK | 0 | | DrawArrays 6 vertices (two triangles) |
| dr_strip3 | control | OK | OK | 0 | | triangle strip, 3 vertices |
| sc_full | control | OK | OK | 0 | | scissor on, whole target |
| sc_left32 | control | OK | OK | 0 | | scissor on, left half |
| vp_full | control | OK | OK | 0 | | viewport 0,0,64,64 |
| sh_full | control | OK | OK | 0 | | shader writes every output in full |
| sh_b14 | control | OK | OK | 0 | | shader branches on bool uniform b14 (true) |
| vb_off0 | control | OK | OK | 0 | | vertex data at attribute offset 0 |
| vb_off16 | control | OK | OK | 0 | | vertex data at attribute offset 16 |
| vb_base16 | control | OK | OK | 0 | | vertex buffer base 16-byte aligned |
| ib_off0 | control | OK | OK | 0 | | u16 indices at offset 0 |
| ib_off16 | control | OK | OK | 0 | | u16 indices at offset 16 |
| cb_aligned16 | control | OK | OK | 0 | | list size a multiple of 16 bytes |
| rg_ctl | control | OK | OK | 0 | | write to a defined register (FACECULLING_CONFIG = 0) |
| rg_mask0 | control | OK | OK | 0 | | register write with byte mask 0 (does nothing) |
| tex_rgba8_a256 | texture | OK | OK | 0 | | rgba8 8x8 at a base aligned to exactly 256 bytes |
| tex_rgb565_a256 | texture | OK | OK | 0 | | rgb565 8x8 at a base aligned to exactly 256 bytes |
| tex_etc1_a256 | texture | OK | OK | 0 | | etc1 8x8 at a base aligned to exactly 256 bytes |
| tex_sz8 | control | OK | OK | 0 | | RGBA8 8x8 |
| sc_past | scissor | OK? | OK | 1 | | scissor larger than the target |
| sc_off | scissor | ? | OK | 1 | | scissor entirely beside the target |
| vp_right | viewport | OK? | OK | 1 | | viewport starts inside and ends past the target |
| vp_big | viewport | OK? | OK | 1 | | viewport 1024x1024 on a 64x64 target |
| dr_arr4 | draw | OK? | OK | 1 | | DrawArrays, 4 vertices (not a multiple of 3) |
| dr_arr5 | draw | OK? | OK | 1 | | DrawArrays, 5 vertices |
| dr_elem4 | draw | OK? | OK | 1 | | DrawElements, index count 4 |
| dr_elem5 | draw | OK? | OK | 1 | | DrawElements, index count 5 |
| dr_elem8_4 | draw | OK? | OK | 1 | | DrawElements u8, index count 4 |
| sh_b15 | shader | WRONG (documented) | OK | 1 | | shader branches on bool uniform b15 (true) |
| ib8_off1 | buffer | OK? | OK | 1 | | u8 indices at offset 1 |
| tex_np24 | texture | ? | OK | 1 | | RGBA8 24x24 (not a power of two) |
| tex_sz1024 | texture | OK? | OK | 1 | | RGB565 1024x1024 (largest documented) |
| rt_linear | target | ? | OK | 1 | | render target in linear memory, not VRAM |
| rt_dim80 | target | WRONG (writes past the target) | WRONG | 1 | | frame buffer 80x80 on a 64x64 allocation: must show the guard check works |
| sc_w0 | scissor | HANG (documented) | OK | 2 | | scissor width 0 (left == right) |
| sc_h0 | scissor | HANG (documented) | OK | 2 | | scissor height 0 (top == bottom) |
| sc_inv | scissor | HANG (documented) | OK | 2 | | scissor inverted (right < left) |
| sc_zero | scissor | HANG (documented) | WRONG | 2 | | scissor 0,0,0,0 |
| vp_off | viewport | ? | OK | 2 | | viewport entirely right of the target |
| vp_w0 | viewport | HANG (documented) | OK | 2 | | viewport width 0 |
| vp_h0 | viewport | HANG (documented) | OK | 2 | | viewport height 0 |
| vp_zero | viewport | HANG (documented) | OK | 2 | | viewport 0x0 |
| dr_1v | draw | ? (documented: zero-count) | OK | 2 | | DrawArrays, 1 vertex |
| dr_2v | draw | ? | OK | 2 | | DrawArrays, 2 vertices |
| dr_elem1 | draw | ? | OK | 2 | | DrawElements, 1 index |
| dr_elem2 | draw | ? | OK | 2 | | DrawElements, 2 indices |
| dr_strip2 | draw | ? | OK | 2 | | triangle strip, 2 vertices |
| dr_fan2 | draw | ? | OK | 2 | | triangle fan, 2 vertices |
| dr_zero_arr | draw | HANG (documented) | OK | 2 | | DrawArrays, 0 vertices |
| dr_zero_elem | draw | HANG (documented) | OK | 2 | | DrawElements, 0 indices |
| sh_part_clr | shader | HANG (documented) | OK | 2 | | colour output written as .xyz only |
| sh_part_tc | shader | HANG (documented) | OK | 2 | | texcoord output written as .xy only |
| sh_part_pos | shader | HANG (documented) | OK | 2 | | position output written as .xyz only |
| vb_off2 | buffer | ? | OK | 2 | | attribute buffer offset 2 |
| vb_off4 | buffer | ? | OK | 2 | | attribute buffer offset 4 |
| vb_off8 | buffer | ? | OK | 2 | | attribute buffer offset 8 |
| vb_base8 | buffer | ? | WRONG | 2 | | vertex buffer base only 8-byte aligned |
| ib_off1 | buffer | ? | OK | 2 | | u16 indices at offset 1 (odd) |
| ib_off2 | buffer | ? | OK | 2 | | u16 indices at offset 2 |
| ib_off4 | buffer | ? | OK | 2 | | u16 indices at offset 4 |
| ib_off8 | buffer | ? | OK | 2 | | u16 indices at offset 8 |
| at_13 | buffer | ? | OK | 2 | | attribute count field 13 (more than 12 attributes) |
| tex_rgba8_a128 | texture | ? | OK | 2 | | rgba8 8x8 at a base aligned to exactly 128 bytes |
| tex_rgba8_a64 | texture | ? | OK | 2 | | rgba8 8x8 at a base aligned to exactly 64 bytes |
| tex_rgba8_a32 | texture | ? | OK | 2 | | rgba8 8x8 at a base aligned to exactly 32 bytes |
| tex_rgba8_a16 | texture | ? | OK | 2 | | rgba8 8x8 at a base aligned to exactly 16 bytes |
| tex_rgba8_a8 | texture | ? | OK | 2 | | rgba8 8x8 at a base aligned to exactly 8 bytes |
| tex_rgb565_a128 | texture | ? | OK | 2 | | rgb565 8x8 at a base aligned to exactly 128 bytes |
| tex_rgb565_a64 | texture | ? (Star3D: unaligned fail) | OK | 2 | | rgb565 8x8 at a base aligned to exactly 64 bytes |
| tex_rgb565_a32 | texture | ? (Star3D: unaligned fail) | OK | 2 | | rgb565 8x8 at a base aligned to exactly 32 bytes |
| tex_rgb565_a16 | texture | ? (Star3D: unaligned fail) | OK | 2 | | rgb565 8x8 at a base aligned to exactly 16 bytes |
| tex_rgb565_a8 | texture | ? (Star3D: unaligned fail) | OK | 2 | | rgb565 8x8 at a base aligned to exactly 8 bytes |
| tex_etc1_a128 | texture | ? | OK | 2 | | etc1 8x8 at a base aligned to exactly 128 bytes |
| tex_etc1_a64 | texture | ? | OK | 2 | | etc1 8x8 at a base aligned to exactly 64 bytes |
| tex_etc1_a32 | texture | ? | OK | 2 | | etc1 8x8 at a base aligned to exactly 32 bytes |
| tex_etc1_a16 | texture | ? | OK | 2 | | etc1 8x8 at a base aligned to exactly 16 bytes |
| tex_etc1_a8 | texture | ? | OK | 2 | | etc1 8x8 at a base aligned to exactly 8 bytes |
| tex_sz4 | texture | ? | OK | 2 | | RGBA8 4x4 |
| tex_np100 | texture | ? | OK | 2 | | RGBA8 100x100 (not a multiple of 8) |
| cb_size8 | cmdlist | ? | OK | 2 | | list size 8 bytes past a multiple of 16 |
| cb_trunc4 | cmdlist | ? | HANG | 2 | | list size 4 bytes short (no word padding) |
| cb_nofinal | cmdlist | HANG (no FINALIZE) | HANG | 2 | | FINALIZE write cut off the end |
| rg_undef06 | register | ? | OK | 2 | | write to register 0x0006 (not defined) |
| rg_undef2e0 | register | ? | OK | 2 | | write to register 0x02E0 (not defined) |
| rg_past | register | ? | OK | 2 | | write to register 0x03F0 (past the file) |
| rg_span | register | ? | OK | 2 | | consecutive write from 0x02FE across the end of the file |
| rt_dim60 | target | ? | OK | 2 | | frame buffer 60x60 (not a multiple of 8) |
| sh_none | shader | ? | OK | 2 | | no vertex shader set up: every vertex-shader register write removed from the list |
| tex_sz2048 | texture | ? | WRONG | 3 | | ETC1 2048x2048 (past the 1024 limit) |
| vb_heap | buffer | ? | WRONG | 3 | | attribute buffer in the normal heap (no physical address) |
| ib_heap | buffer | ? | WRONG | 3 | | index buffer in the normal heap |
| tex_heap | texture | ? | WRONG | 3 | | texture in the normal heap |
| rt_deleted | frame | ? | FAULT | 3 | | citro3d: render target deleted inside the frame (must stay last) |
