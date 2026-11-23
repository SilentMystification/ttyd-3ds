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
