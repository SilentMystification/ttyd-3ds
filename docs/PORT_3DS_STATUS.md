# 3DS port: status

Updated 2026-11-22. The plan is in [PORT_3DS_PLAN.md](PORT_3DS_PLAN.md) and the work queue in [PORT_3DS_BACKLOG.md](PORT_3DS_BACKLOG.md). The tools are in [PORT_3DS_TESTING.md](PORT_3DS_TESTING.md), [PORT_3DS_TEST_INFRA.md](PORT_3DS_TEST_INFRA.md) and [PORT_3DS_HW_CHECKLIST.md](PORT_3DS_HW_CHECKLIST.md).

## Where things stand

- **Phases:**
  - **0–5 done.** Phase 2's host decode tool decodes all 3204 US disc files and matches paper-cut. Phase 5's audio matches paper-cut within tolerance in Azahar.
  - **Phase 6 mostly done.**
  - **Phase 7 under way:**
    - test infrastructure is in;
    - the map sweep passes all reachable maps except the storybook `stg` maps (in progress);
    - galleries and the effect and battle sweeps are running.
  - **Phase 8 started on hardware:** the game boots and plays on the New 3DS as a 3dsx, and the CIA boots with QTM.
- **GameCube matching build:** 100% (Game Code, SDK, Modules, MusyX 31/31 since the `hardware.c` case fix).
- **Regions:** US, JP and PAL all build. JP and PAL show a clear message on the wrong disc. **In-game JP and PAL testing needs those discs.**
- **Performance** (Azahar game thread, `gor_01`): 267 → 184 ms per frame so far, through:
  - the display-list cache;
  - the content-stable memory registry;
  - the unlit fast path;
  - draw merging;
  - the word-at-a-time texture hash;
  - load-time jobs on core 2.
  - The vertex-shader path is in progress, gated by the no-z-fighting and no-seams tests.
  - The target is the GameCube's original frame rates on New 3DS.
- **Tooling:**
  - **`ctr-devkit`** (`libs/ctr-devkit/`, MIT): crash module, over-the-air CIA update, bootstrap, recovery. It works in Azahar. Its hardware checklist is the next console session.
  - **Deterministic mode** (`determ`), a GameCube debug DOL, and `gw_diff.py` for port-vs-GameCube comparison.
  - **Galleries, profilers** (code and data sampling), **headless slot-limited Azahar** (at most 3).
- **Saves:** big-endian, GameCube- and Dolphin-compatible, with old port saves migrated. Player options menu (SELECT, or touch) in `options.txt`.
- **Audio:** NDSP voices behind `ndspvoices=1`. Streams on NDSP in progress.
- **Movies:** a real THP decoder (from aurora, MIT). The US disc has no movie to test it on.

## Open items

See PORT_3DS_BACKLOG.md for the full queue.

1. **Hardware session** (PORT_3DS_HW_CHECKLIST.md): `ctr-devkit` checks, probe QTM bandwidth, Rosalina profiles, `cpulimit` 0/30/80, sleep and HOME (the GPU hold fix), the copy-target warning.
2. **JP and PAL discs** from the owner, for in-game region testing (plan section 9.1).
3. **Vertex-shader path:** finish, and pass the acceptance tests.
4. **Story replay, battles, effects, side content and systems** (phase 7 lanes B and A).
5. **GX translation on core 2, stereo 3D, frame pacing** (lane C).
6. **Known differences to check against the GameCube:**
   - the `gon_02` "struck first" black bar (map object `kuro1`);
   - the `hei_01` battle start, where the port is 2 frames late.

## Environment notes

- **Console:** reached by IP (`N3DS_IP`). It runs the Homebrew Launcher inside a takeover title.
- **Azahar:** runs headless through `run_azahar.py`'s machine-wide slot lock; never more than 3 instances.
- **Merging:** agents work in git worktrees with file ownership, and the planner merges into `port3ds`. Nothing is pushed without the owner.
