# Work queue to the finish line

Updated 2026-11-22. Owner goals: original GameCube frame rates, stereo 3D, all three regions in the first release, an options menu, Dolphin save import, a user README. Agents take items from the top of each lane.

**Limits:**
- at most 3 Azahar instances at once (`run_azahar.py` slot lock), so Azahar-heavy items queue behind each other;
- hardware items wait until `ctr-devkit` is in.

## Running now

| Item | Agent |
|---|---|
| `ctr-devkit`: crash module, devupdate, bootstrap, recovery, persistent Azahar sessions | Opus |
| GX display-list cache: (a) of `PORT_3DS_GX_PERF.md` | Opus |
| Job queue with work stealing, load-time jobs, busy-wait check | Opus |
| Curtain black geometry: data | Sonnet |
| Phase 2 host decode tool and round-trip tests | Sonnet (no Azahar) |
| MusyX GameCube build mismatch: which file, since when | Sonnet (no Azahar) |

## Lane A: tooling (Azahar)

1. `hw_harness.py` on top of `devupdate`, with Azahar and hardware targets: test lists, results JSON. *Needs `ctr-devkit`.*
2. **Deterministic test mode:** game time advances exactly 1/60 s per frame (1/50 for PAL), so the same inputs replay the same way.
3. **Recorded inputs:** Dolphin DTM movies or a TTYD TAS converted to padscripts, played back in the port.
4. **GameCube debug DOL:** the same source with the same test switches, non-matching and separate from the matching build, so `fx=`, `battle=` and story jumps behave alike in Dolphin and the port.
5. **Galleries:** `fx=<effect>` and `battle=<id>`, screenshotted in both builds and diffed automatically.
6. **GX feature census:** runtime hashes of TEV, blend, texture-format and indirect configurations, compared with a static scan of `src/`.
7. **Per-frame `GlobalWork` comparison against Dolphin** (story flags, sequence, positions), stopping at the first divergence.

## Lane B: correctness (Azahar, after the A items they need)

1. Storybook `stg` maps reachable and swept (55 maps).
2. Story playthrough by replay, chapter by chapter, including the Peach and Bowser interludes and Bowser's side-scrolling stages, through the ending and credits.
3. Battles: every action command, partner and special move, boss, hazard and audience event, through `battle=` plus a test hook that picks moves and forces action-command results.
4. Effects: all of `src/effect/` and `src/effect/n64/`, plus the screen-copy, indirect-texture and z-texture scenes, with per-game GX overrides where the PICA can't. This includes the curtain fix and closing out the prologue-cabin black frames.
5. Side content: the Pit of 100 Trials, Trouble Center, Pianta Parlor, quizzes, cooking, shops, the transformations.
6. Systems: save/load in every slot, copy and delete, game over, soft reset, every pause-menu tab, mail, journal.
7. Regions: JP and PAL through the converter. PAL at 50 and 60 Hz, all languages. Each passes boot, a new game, and the map sweep.

## Lane C: performance (to the original frame rates)

1. GX display-list cache (running).
2. The cheap state fixes:
   - a state version separate from matrix loads;
   - fog table caching;
   - an unlit fast path;
   - draw merging on the render thread.
3. Vertex shader transform, gated by the no-z-fighting, no-seams tests (`PORT_3DS_GPU_OFFLOAD.md`). Then immediate mode, the matrix palette, and lighting.
4. Game arena in linear memory (Star3D's idea), so the GPU reads game data without copies.
5. GX translation moved to core 2 (`PORT_3DS_MULTICORE.md` A): `gxthread=0/1/2`, the late-read audit, the render fence, verify mode.
6. Per-voice NDSP audio: MusyX voices on NDSP channels, loops re-aligned at build time.
7. **Stereo 3D:** two-eye rendering in the record/replay, the slider-driven quality profile, and the GPU budget for both eyes.
8. QTM placement and the core-1 limit, decided by hardware numbers (Lane D).
9. Frame pacing: no VBlank wait after a long frame; PAL at 50 Hz.

## Lane D: hardware (after `ctr-devkit`)

1. The `ctr-devkit` hardware checklist:
   - a running title installing its own CIA, then relaunching;
   - deliver arguments surviving the relaunch;
   - `am:net` under Luma's homebrew loader;
   - SVC 0x3B;
   - `crashtest=segv`.
2. Probe as a CIA: QTM vs heap vs linear bandwidth from each core, and GPU reads from QTM.
3. Rosalina profiles of Mario's house, Rogueport and a battle. A `cpulimit` 0/30/80 comparison.
4. Sleep and HOME: the GPU hold fix and the early CPU-limit drop. Live log from the CIA.
5. The hardware-only warning `gpu: copy target 2 cannot be rendered to`.
6. Multi-hour stability: memory, log volume, saves.

## Lane F: found along the way

1. **THP movie decoder in C: done (2026-11-22, from aurora, MIT).** Untested in game: the US disc has no movie. Check the timing on hardware. The JP and PAL `dig` map's `demo.thp` plays black and silent today, because the SDK's THP decoder is PowerPC assembly and the port stubs it. The plan forbids stubs. Write a C Motion-JPEG plus THP audio decoder (portable, in the library layer).
2. **JP ROM font (SJIS).** The port's ROM font is ASCII-only, and JP needs SJIS glyphs. This is the likeliest JP boot problem.
3. **JP and PAL discs from the owner,** to run plan section 9.1's steps.
4. **Battle "struck first" black band (gon_02):**
   - Traced to the map object `kuro1`, a black frame around the room. It is identical on disc and in the port, drawn every frame, and nothing hides it. Its bottom bar shows below the floor at the camera position of that moment.
   - Likely the original game's behaviour, but confirm against a GameCube frame through the gallery tool (Dolphin screenshot of `gon_02` with `story=30`, frames 592-616), and compare the camera with `gw_diff`.
5. **The DVD manager's idle loop** polls instead of blocking (`os_thread.c`'s `OSYieldThread`). Measure on hardware behind a switch.

## Lane E: leftovers and release

1. **Phase 5:** audio golden-hash comparison against paper-cut.
2. **Phase PRs:** `port3ds` into `main` at the phase boundaries. *Pushing needs the owner's OK each time.*
3. **Options menu** (controls, quality, 3D), separate from the debug Select menu.
4. **Dolphin `.gci` save import** into the port's SD save files.
5. **User README/guide:** dumping your own disc, installing, controls.
6. **Versioning and release process.** After release: the `libgc3ds` and `ctr-devkit` repository splits.
