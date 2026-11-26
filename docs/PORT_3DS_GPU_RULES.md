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

## Command processor, transfer engine and QTM cases (GX native plan step 0)

Added 2026-11-27 for `docs/PORT_3DS_GX_NATIVE.md` step 0. Only CALL mode (step 5), the chunk placements and `texdt` (step 3b) depend on them. Azahar is no evidence for jump/call semantics, QTM or command-processor timing: only the New 3DS column decides.

- **Lists.** The case records the citro3d scene twice: the state with a draw that writes nothing (color write mask 0), then the draw packet alone (write mask on + draw). The harness builds the main list and the chunks from these words by hand. Every jump write is the last command of a 16-byte block; pads are `FACECULLING_CONFIG = 0` (the value every case sets). The main list goes to `gpuprobe/<case>.bin`, the chunk buffer to `gpuprobe/<case>.chunk` (not read by `gpu_cmd_diff.py --probe-dir`).
- **Call** (two channels, no return stack): the caller writes `CMDBUF_SIZE1/ADDR1` (chunk), `CMDBUF_SIZE0/ADDR0` (its own continuation, to the end of the main list), then `CMDBUF_JUMP1`; the chunk ends with `CMDBUF_JUMP0`. Sizes and addresses are bytes >> 3, physical.
- **cb_call500:** before each call the main list sets a 1x1 scissor at pixel (i & 63, i >> 6); OK = exactly 500 red pixels, the rest untouched.
- **cb_vram:** the chunk is written to VRAM by `GX_TextureCopy` (PPF timeout = HANG), read back by the CPU (note: "VRAM copy equal"), then called.
- **QTM** (`cb_qtm`, `vb_qtm`, `ib_qtm`, danger 3): the probe gets the free part of the application's QTM after the placements (`plat/gpuprobe_mode.c`; CPU 0x1E800000 + n = physical 0x1F000000 + n). The CPU writes the data; the note gives `GSPGPU_FlushDataCache` and `svcFlushProcessDataCache` result codes on the QTM range and what `osConvertVirtToPhys` returns for it. Buffer offsets are relative to citro3d's base 0x18000000 (0x07000000 + n); the chunk address is (0x1F000000 + n) >> 3. A 3dsx has no QTM mapping: SKIP.
- **cb_irq:** draw in the main list, then `JUMP0` to a chunk of flush + FINALIZE; the note gives the time to P3D and whether a second P3D comes within 20 ms.
- **cp_thru:** best of 3, from `GX_ProcessCommandList` to the P3D event: an empty list, 8192 words of single writes (to `TEXUNIT0_BORDER_COLOR`), 8192 words of 255-value bursts, and 8 calls of a 1024-word chunk of single writes. The log line `cp_thru:` gives words per us net of the empty list. OK = every list finished.
- **dt_tile_\*:** `GX_DisplayTransfer` linear to tiled, same format in and out, no scaling, sizes 8x8 to 256x256 and 64x32, 32x64, 128x16, 16x128, each with `FLIP_VERT` 0 and 1, compared byte for byte with the game's CPU tiler (`plat_probe_tile_ref` = `tile_level` in `game/gx/gx_tex.c`, row 0 to the last tile row; RGB8 has no game tiler and uses the probe's own). OK = every size equal with one flip setting (the note says which). One log line per size and flip.

| Case | Group | Expected | Azahar | Danger | New 3DS | What |
| --- | --- | --- | --- | --- | --- | --- |
| cp_thru | thru | OK (timing) | OK: 86 us per 8K words in all four modes (no timing evidence) | 1 | | command-processor time: 8 K words as single writes, bursts, 8 calls of a 1 K chunk |
| cb_jump0 | cmdlist | ? | OK, 86 us (no evidence for jump semantics) | 2 | | main list jumps (JUMP0) to a chunk holding state, draw and FINALIZE |
| cb_jump1 | cmdlist | ? | OK, 86 us (no evidence for jump semantics) | 2 | | main list jumps (JUMP1) to a chunk holding state, draw and FINALIZE |
| cb_call1 | cmdlist | ? | OK, 86 us | 2 | | call: channel 1 call of a draw chunk, channel 0 return, 1 call |
| cb_call500 | cmdlist | ? | OK, 500 red px, 86 us | 2 | | 500 calls of one static draw chunk, a 1x1 scissor per call: exactly 500 pixels |
| cb_call_nest | cmdlist | ? | OK, 86 us | 2 | | main calls A, A calls the draw chunk B (return into A's tail), A's tail jumps back to main |
| cb_call_a8 | cmdlist | ? | OK, 86 us | 2 | | call: chunk address 8 bytes past a 16-byte boundary |
| cb_call_a16 | cmdlist | ? | OK, 86 us | 2 | | call: chunk address 16 bytes past a 64-byte boundary |
| cb_call_size8 | cmdlist | ? | OK, 86 us | 2 | | call: chunk size 8 bytes past a multiple of 16 |
| cb_call_size16 | cmdlist | ? | OK, 86 us | 2 | | call: chunk size 16 bytes past a multiple of 32 |
| cb_vram | cmdlist | ? | OK, VRAM copy equal | 2 | | call: chunk in VRAM, written there by TextureCopy |
| cb_irq | cmdlist | ? | OK: P3D after 86 us, no second P3D in 20 ms | 2 | | draw in the main list, then JUMP0 to a chunk with only flush and FINALIZE: one P3D |
| dt_tile_rgba8 | dt | ? | OK: all 10 sizes equal with FLIP_VERT 1, none with 0 | 2 | | DisplayTransfer RGBA8 linear to tiled vs the CPU tiler |
| dt_tile_rgb8 | dt | ? | OK: all 10 sizes equal with FLIP_VERT 1, none with 0 | 2 | | DisplayTransfer RGB8 linear to tiled (probe's own tiler) |
| dt_tile_rgb565 | dt | ? | OK: all 10 sizes equal with FLIP_VERT 1, none with 0 | 2 | | DisplayTransfer RGB565 linear to tiled vs the CPU tiler |
| dt_tile_rgba5551 | dt | ? | OK: all 10 sizes equal with FLIP_VERT 1, none with 0 | 2 | | DisplayTransfer RGBA5551 linear to tiled vs the CPU tiler |
| dt_tile_rgba4 | dt | ? | OK: all 10 sizes equal with FLIP_VERT 1, none with 0 | 2 | | DisplayTransfer RGBA4 linear to tiled vs the CPU tiler |
| cb_qtm | qtm | ? | SKIP (3dsx: no QTM; no evidence) | 3 | | call: chunk in QTM ((0x1F000000 + n) >> 3), CPU written, GSP flush |
| vb_qtm | qtm | ? | SKIP (no evidence) | 3 | | vertex buffer in QTM |
| ib_qtm | qtm | ? | SKIP (no evidence) | 3 | | u16 index buffer in QTM |

Console run of these cases only (owner-gated): `$D restart "gpuprobe probe=cmdlist,thru,dt,qtm"` (no `probefresh`: earlier results stay in the journal), then `files get @/gpuprobe.txt` and the log lines `gpuprobe: dt ...` and `gpuprobe: cp_thru: ...`.
