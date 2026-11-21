# 3DS port: status and next steps

Handoff note, 2026-11-21. The plan is in [PORT_3DS_PLAN.md](PORT_3DS_PLAN.md), the tools in [PORT_3DS_TESTING.md](PORT_3DS_TESTING.md), and the questions for the owner in [PORT_3DS_QUESTIONS.md](PORT_3DS_QUESTIONS.md).

## Where things stand

Everything below runs in Azahar from the CIA, on the `port3ds` branch.

- **Boot to play.** The game runs from boot through the logos, title, file select and opening to field maps (Rogueport, Petal Meadows). Mario, partners and NPCs render, cutscene dialogs run, and a full first battle works (menus, action commands, enemy turns).
- **Sound.** The game mixes through NDSP, with real music levels and no underruns. That holds both with Azahar's HLE DSP and with `--lle-audio` (the console's own firmware, `build/dspfirm.cdc`, pulled from the owner's console).
- **Map sweep.** About 230 of 334 maps load with no fault. Of the rest:
  - about 45 start a battle on entry at story 30, which is expected;
  - the storybook's `stg` maps are not in the map table and are skipped;
  - the maps that crashed in the first sweep were fixed (copy-target delete, display lists recorded before their vertex setup, the ARAM callback race), and their reruns pass.

## Open items, in order

1. **First hardware boot.** The owner's N3DS runs ftpd at 192.168.1.223:5000.
   - The disc image is on the console: `/3ds/ttyd/game.iso`, 1459978240 bytes, size checked after the upload.
   - Then, in the Homebrew Launcher, press Y and run the probe first:
     `tools/port3ds/run_hw.py --ip 192.168.1.223 --build build/3ds/probe/ttyd_probe.3dsx`.
     The probe shows its results on screen and in `sdmc:/3ds/ttyd/probe.txt`.
   - Then the game: `run_hw.py --switches "wdog=30" ...`; see PORT_3DS_TESTING.md, "Running on the New 3DS".
   - If the boot memory check refuses the 3dsx, install the CIA with FBI.
2. **Remaining black frames in Azahar.** These are real flicker, not a screenshot artifact. Things already ruled out or fixed:
   - The state was lost after a mid-frame `plat_gpu_sync`. That is fixed.
   - The remaining ones are frames whose presented record holds only 20 to 3000 bytes with no draws: the frame's draws went into an earlier segment record, and the presented tail holds a clear. Each screenshot log line (`record of N bytes has ... draws, ... clears`) shows this.
   - Next: log every record (segments too) for a black frame, to find which call splits the frame and where that clear comes from. A `GXCopyTex(clear)` split across a sync is the main suspect.
3. **Map sweep speed.** A battle on entry makes the sweep wait 1800 frames and then restart Azahar, about 1.5 min each.
   - Treat sequence 4 (battle) as a pass at once, and warp on from the battle without restarting.
   - Lower `--frames`.
4. **Story-order testing** (phase 7): scenes that only run in story order.
5. **Vertex work on the PICA.** The owner expects it in the optimization pass. Decide after the hardware frame time is known; doing it before the story sweep avoids checking effects twice.

## Environment notes

- **Reaching the console.** Tailscale on this PC had a route for 192.168.1.0/24 that captured LAN traffic; the owner turned Tailscale off.
  - Network commands to the console need the command sandbox off.
  - The console is up when the owner says so: if it does not answer, the problem is on this PC's side. Retry the connection before anything else.
- **The rtk hook.** A hook rewrites some shell commands (`rtk ...`). Auto mode then asks for the exact rewritten command to be run again.
- **Fast-forward is expected.** Azahar runs fast-forwarded in the runner's profile (frame limiter off), so its sound plays fast. `--realtime` plays at normal speed.
- **Logging never blocks the game.** The log writer runs on core 2 at the lowest priority, and lines are dropped (and counted) when its queue is full.
