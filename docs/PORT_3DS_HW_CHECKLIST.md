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
11. **The hardware-only warning** `gpu: copy target 2 cannot be rendered to`.

What Azahar can't show, so these are the real tests:
- the APT jump or relaunch: Azahar aborts on it;
- the old install surviving a refused push;
- `am:net` under Luma's homebrew loader;
- the guest exception handler;
- real SVC 0x3B contexts;
- sleep.
