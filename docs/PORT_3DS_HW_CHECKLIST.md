# First hardware session with ctr-devkit

Set up once:

```
export N3DS_IP=<console address>
D=libs/ctr-devkit/devupdate/devupdate.py
EX=build/3ds/ctr-devkit/examples
tools/port3ds/build.sh && tools/port3ds/build.sh libs/ctr-devkit
```

Order matters: each step proves something the next one relies on.

1. **Crash handler, real exception path.** This uses the 3dsx through the Homebrew Launcher, so press Y first.
   - `tools/port3ds/run_hw.py --switches "crashtest=segv crashwait=5" --until "exiting the process" --timeout 120`
   - Repeat with `crashtest=hang wdog=5`. Expect real thread contexts from SVC 0x3B.
   - Repeat with `crashtest=break`, `fatal`, `null` and `undef`.
   - Then `tools/port3ds/crash_report.py --ftp $N3DS_IP`. This needs ftpd.
2. **Bootstrap.** This checks `am:net` access under Luma's homebrew loader, and the jump into an installed title.
   - Press Y, then `$D bootstrap --cia $EX/devupdate/ctrdk-devupdate.cia --switches hello=1`.
   - The example's screen should show the arguments `"hello=1"`.
3. **Self-install, relaunch and deliver arguments.**
   - Rebuild, then `$D push $EX/devupdate/ctrdk-devupdate.cia --switches hello=2 --wait 60`, three times. The build stamp should change each time.
   - Then `$D restart hello=3`.
   - Then a cold start from the HOME Menu. The screen should show the arguments `""`.
4. **Refused push.** `$D push $EX/devupdate/ctrdk-devupdate.cia --bad-crc` should give `error 5`, and the title should still start from the HOME Menu afterwards.
5. **Recovery.**
   - `$D restart crashtest=fatal`, then `$D report`, then `$D restart after=recovery`.
   - Repeat with `crashtest=segv`.
6. **TTYD.**
   - `$D bootstrap --cia build/3ds/game/ttyd.cia --switches devupdate`.
   - Push `build/3ds/game/ttyd.cia --wait 120` ten times, with `tools/port3ds/run_hw.py --no-send` attached for the live log.
   - Push once with `--bad-crc`. The game should keep running.
   - `$D restart crashtest=hang wdog=5`, then `$D report --out build/devupdate-last/report.txt`.
   - `sdmc:/3ds/ttyd/card` must be unchanged throughout.
7. **Sleep during a transfer.** Close the lid during a push. Expect `error 16 sleep`, with the install cancelled.

6b. **Push during each game state (owner requirement: no state of the running game may prevent an update).** Run `$D selftest` first (no console; the request framing against a host build of the device's decoder). Then, with TTYD running under devupdate and `tools/port3ds/run_hw.py --no-send` attached for the log, push `build/3ds/game/ttyd.cia --wait 120` (the build's `ttyd.version` sidecar goes along as the version text) in each of these states, and check the bottom screen shows `build #N -> #M`, the push installs, and the new build starts:
   - Options menu open (greyed top screen); Select menu open;
   - during a map change (door, pipe, `warp`), and while Rogueport loads;
   - at a battle start and in the middle of a battle;
   - during a save (the save-point menu, or a card write in the log);
   - with the 3D slider up (stereo on), and in the pause-grey screen;
   - after a crash, in recovery mode;
   - with the console on a slow link (far from the access point): the push may fail direct. The PC must then retry by itself through the SD card (`--stage`); also try `--stage` on its own.
   For each: the log has `devupdate: the game thread paused after N ms (phase ...)`, or the warning `did not reach a frame boundary in 2000 ms; going on (phase ..., map ..., options menu ., save busy ., stereo ., ...)` followed by the install and the relaunch (`relaunching`, or `the relaunch did not end the process in 8 s; ending it`). Note any state that took the warning path and what the phase line said. A refused or failed push (`error 13..18 ...: stage=... rc=...`) must leave the old build installed and the game running; copy the `rc=` value (AM's result code) into the notes. If the console refuses a direct push (the old `error 17 write failed at ~8 KiB` pattern), record `rc` and whether `--stage` succeeded.

7b. **Console walk (a milestone check only, not routine).** Needs steps 3, 5 and 6 first (relaunch with deliver arguments, recovery, TTYD under devupdate).
   - Small first: `tools/port3ds/hw_walk.py --name first --maps gor_01,hei_00,gon_02 --frames 400 --shots 1 --yes`. It prints the entry count and estimate, uploads, starts, and follows the live log.
   - Check: three `[QUEUE] entry N ... done` lines, `[QUEUE] done`, `build/hw-walk/first/results.json`, a PNG for each entry in `build/hw-walk/first/shots/`, and no one touched the console.
   - A pad file: `--pad gor_01=<file.tpad>`; the entry ends on `[PAD] padfile end`. Check that the same file gives the same play as in Azahar.
   - A crash in the walk: use a manifest with a `crashtest=fatal crashframe=300` entry between two good ones (see "Console walk" in `PORT_3DS_TESTING.md`). Expect `result crash`, the report in `out/.../crash.txt`, and the next entry running.
   - Only on the console: that APT keeps the deliver arguments across each self-relaunch (entry switches and `ctrdkq=N` reach the new process), that a relaunch takes about 12 s, and that the live log reconnects each time.
   - Only then a longer walk (more than 12 entries needs `--yes`; keep these for milestones).

Then the measurement items from `PORT_3DS_BACKLOG.md` lane D, all through `devupdate`:

8. **The probe as a CIA:** QTM vs heap vs linear bandwidth from each core, and GPU reads from QTM.
9. **Rosalina profiles** (`pc_sample.py`) of Mario's house, Rogueport and a battle, plus a `cpulimit` 0 / 30 / 80 comparison.
10. **Sleep and HOME.** Expect the `gpu: suspend|sleep ... / gpu: resume` log lines and `cpu limit: 30% (HOME pressed, early ...)`.
11. **Copy targets:** open the START pause menu in `gor_01`. The background must show the captured field. Also run `vramfail=1` once, to check the strip fallback's `GX_TextureCopy` ordering on hardware.
12. **Shader path (`gxvsh=1`, default):** compare frame times and the render thread's time with `gxvsh=0`.
13. **NDSP audio:** `ndspvoices=1 ndspstreams=1 ndspstats=10` on the title screen, `gor_01` and a battle. Listen for a tick every 17.5 ms in the music (ADPCM history not carried across wave buffers on firmware). Note the `[NDSPV] flush ticks` line, with `audiobatch=1` and `audiobatch=2`.
14. **Stereo 3D:** the real slider, in `gor_01`, a battle and the pause menu. Judge depth and comfort at the default `stereosep=6` and median convergence, HUD crosstalk, and edge artifacts by near objects (try `stereogrid=80x48`). Compare `stereostats=60` against the slider at 0.
15. **GX translation thread:** `gxthreadstats=60` with `gxthread=0` and `gxthread=2` in `gor_01`, a battle and `aaa_00`. Keep mode 2 only if frames are faster on the console.
16. **Texture filter:** `texfilter=game` vs `texfilter=mip` (and `mip texlodbias=-0.5`) on a slow pan in `gor_01` and a battle. Judge distant-texture shimmer and softness; compare frame times.
17. **SD on core 1:** `sdstats=10` during play, a save and Options close. Expect 0 direct SD calls from cores 0/2.

What Azahar can't show, so these are the real tests:
- the APT jump or relaunch: Azahar aborts on it;
- the old install surviving a refused push;
- `am:net` under Luma's homebrew loader;
- the guest exception handler;
- real SVC 0x3B contexts;
- sleep.
