# Paper Mario: The Thousand-Year Door on New 3DS: user guide

This guide is for players. It tells you how to install the port and play it.
Developers should read [BUILDING.md](BUILDING.md) and [PORT_3DS_TESTING.md](PORT_3DS_TESTING.md) instead.

Facts in this guide come from the port's source code. A fact the code does not prove is marked **(unverified)**.
At the time of writing the port had been run only in the Azahar emulator, not on a real console, so anything about
real hardware is unverified.

## What you need

- A **New 3DS, New 3DS XL or New 2DS XL**. An Old 3DS or Old 2DS cannot run the port. It shows a message and stops.
- **Custom firmware** (Luma3DS) on the console, and **FBI** to install the file.
- An SD card with free space for the disc image (about 1.4 GB).
- **Your own copy of the game**: a disc image you dumped yourself from your own disc.
  - The port contains **no game data**. It reads everything from your disc image.
  - The image must be a plain `.iso` (a full GameCube disc image). Compressed or trimmed formats such as RVZ, GCZ,
    CISO or NKit are not read. Convert them to `.iso` first (Dolphin can do this: right-click the game, Convert).
  - The port checks the game ID. At the moment only the **US release (G8ME01, revision 0)** is built.
    The JP (G8MJ01) and PAL (G8MP01) builds are planned but not available yet.
    A build only starts with the disc it was made for.

## Installing

1. Get the CIA for your disc's region, `ttyd-3ds-<version>-us.cia` (or `-jp`, `-pal`). Check it against `SHA256SUMS`.
2. Install it with FBI. Either:
   - copy `ttyd.cia` to the SD card, open FBI, choose SD, find the file, and choose Install CIA; or
   - in FBI choose Remote Install, then Receive URLs over the network, and send the file from a computer.
3. Make the folder `sdmc:/3ds/ttyd/` on the SD card if it is not there yet. The port also makes it on first start.
4. Copy your disc image to **`sdmc:/3ds/ttyd/game.iso`**. The name must be exactly `game.iso`.
5. Copy the **`res`** folder from the release package to **`sdmc:/3ds/ttyd/res/`** (so that `sdmc:/3ds/ttyd/res/sjis_font.bin` exists). The Japanese build needs it for its system font and stops with a message naming the file if it is missing; the US and PAL builds do not use it.
6. Start **TTYD 3DS** from the HOME Menu.

The CIA is the supported way to run the port. It sets the memory mode and the clock speed that the game needs.
The `.3dsx` file for the Homebrew Launcher may be refused for lack of memory. If you see "Not enough memory",
install the CIA.

## First boot

- The port opens `game.iso`, checks it, and reads the file table. It then starts the game.
- There is **no separate conversion step** and no progress bar. The port converts models, maps and textures
  from the disc while you play, as the game loads them.
- You see the game's own logos, then the title screen.
- The bottom screen shows, from the top: five HUD lines (frame rate and frame time; CPU, GPU and render times; RAM use; video RAM use;
  the New 3DS extra RAM "QTM"), a separator, the scrolling log, another separator and, on the last row, the version and build number.
  This is normal.
- How long the start takes on a real console is unverified. In Azahar the title screen appears
  after about 800 game frames.
- If something is wrong with the disc image or the SD card, you see a black text screen. See Troubleshooting.

## Controls

The game sees one GameCube controller. The 3DS maps to it like this:

| 3DS | GameCube |
| --- | --- |
| A, B, X, Y | A, B, X, Y |
| START | START |
| D-pad | D-pad |
| Circle Pad | Control stick |
| C-stick (New 3DS) | C-stick |
| L | L (fully pressed) |
| R | R (fully pressed) |
| ZR | Z |

- ZL does nothing by default (you can give it a job in the Options menu).
- In the game itself the touch screen is not used, except that touching the status lines on the bottom screen opens the Options menu (below).
- Rumble does not exist on the 3DS, so the game's rumble option does nothing.
- The game's own soft reset (hold B, X and START) is unverified.

### Options menu

Press **SELECT**, or touch the HUD lines at the top of the bottom screen (the first line shows an **[Options]** label on the right when it has room).
The game pauses while the menu is open and receives no input. The top screen shows the last frame, greyed and darker, and all sound is muted
(it fades out over a few milliseconds); both come back when you close the menu.

While the menu is open it covers the whole bottom screen (the status lines and the log are hidden). The log keeps collecting in the
background; when you close the menu the status lines and the last 28 log lines are painted back exactly as they would have been.

- **L / R** change the page. **Up / Down** move. **Left / Right** or **A** change a value. **B** or **SELECT** closes it.
- With the touch screen, tap a line to change it, tap the title line (top row) to go to the next page, tap **[ Close ]** (top right) to close.
- Changes apply at once and are saved when you close the menu.

| Page | Option | Values |
| --- | --- | --- |
| Controls | GC A, B, X, Y, Z, L, R, Start | the 3DS button that presses it: A, B, X, Y, L, R, ZL, ZR, START or none. Several GameCube buttons may share one 3DS button |
| Controls | C-stick is | C-stick nub (default), or D-pad. With the D-pad as the C-stick the D-pad no longer sends GameCube D-pad presses, and the C-stick nub does nothing |
| Controls | Reset to defaults | puts the page back to A, B, X, Y, ZR = Z, L, R, START and the C-stick nub |
| Display | 3D effect | off or on (default on). Stereoscopic 3D is not in the port yet; the setting is stored for when it is |
| Display | Bottom screen HUD | off, FPS only (first line only), or full (default). Only the five HUD lines change; the separators, the log and the build row stay |
| Display | Texture filter | game (default), linear, mip or nearest. game: the filtering the game itself asks for. linear: smooth (bilinear) textures everywhere. mip: smooth textures plus smaller pre-shrunk copies (mipmaps) of each texture, which calms the shimmer of distant floors and walls on the small top screen; uses about a third more texture memory and makes a texture's first appearance a little slower. nearest: sharp blocky texels, for comparison. Screen effects, text-sized strips and lookup textures always keep the game's own filtering |
| Audio | Master volume | 0 to 100 percent in steps of 10 (default 100) |
| Performance | Quality | auto (default), high or fast. Stored only; later speed features will read it |
| Language | Game language | PAL version only: English, Deutsch, Francais, Espanol, Italiano. Stored only for now |
| Debug | Debug mode | auto (default), on or off. One switch for every developer function: the developer Select menu (ZL + SELECT), the log (`latest.log`), the update server, the crash watchdog and the test switches in `debug3ds.txt`. Off: none of them runs or uses memory (crashes still show the crash screen and write `crash.txt`). auto: on in a development build (`-dev` in the version), off in a release. Changes that stop something take effect at once where they can; the rest at the next start (the row then shows `relaunch`). This row is always there, so Debug mode can always be turned back on |

- The options are kept in **`sdmc:/3ds/ttyd/options.txt`**, a small text file, not in your save. Delete it to go back to the defaults.
  If it is damaged the port ignores it, writes a line to the log and uses the defaults.
- The Select menu of earlier builds was a developer menu. It is now only for developers (**ZL + SELECT**, while **Debug mode** in the Options menu is on, see above; a HOME Menu start of a release build needs Debug mode set to on):
  log level, warping to any map, return to title, exit, **Update server** (on or off, kept in `options.txt`; while it runs the row shows the
  console's IP address and port, for `libs/ctr-devkit/devupdate/devupdate.py`) and **Relaunch** (restarts the game at once with the current
  switches, so a changed option or the update server takes effect). Warping can break the story. Do not use it in a save you care about.
  The update server runs only in Debug mode (on by default there); its Debug tab has a **Debug mode** row too.

## Saves

- The port keeps your saves as files on the SD card, in the folder **`sdmc:/3ds/ttyd/card/`**.
- The game uses one memory card file. For the US release it is
  `01-G8ME-mariost_save_file.gci`. All four save slots are inside it.
- The folder acts as a memory card in slot A with 2043 free blocks.
  The game makes the file the first time you start a new file.
- The files use the same container as the GameCube memory card "GCI" format that Dolphin uses.
- **Back up** by copying the whole `sdmc:/3ds/ttyd/card/` folder to your computer. To restore, copy it back
  while the game is closed.
- **Moving saves between Dolphin and the port.** The port stores the save exactly as the GameCube does, so one
  file works in both. No conversion tool is needed.
  - To bring a save **into** the port: find the `.gci` file of the game in Dolphin and copy it to
    `sdmc:/3ds/ttyd/card/`. Dolphin keeps them in its user folder, `GC/USA/Card A/` for the US release
    (unverified: Dolphin paths). You can also export one with Tools, Memory Card Manager.
    The file name does not matter. The port finds the save by the name written inside the file.
    Close the game first, and start it again.
  - To take a save **out**: copy `01-G8ME-mariost_save_file.gci` from the card folder into Dolphin's `Card A`
    folder, or import it in the Memory Card Manager (unverified: Dolphin steps).
  - The save must be from the same region as the port. A save from another region is ignored (see below).
  - Saves made by earlier port builds are read and converted on load. They are rewritten in the GameCube format
    the next time you save. The log says "card: converted a save from the old little-endian layout".
  - Back up before you try. Copy over a save you want to keep only after a backup.
  - Tested: saves from the paper-cut PC port (a different program) load, and the game's own checks accept the
    port's saves in the GameCube byte order. A save from a real GameCube or Dolphin has not been tried
    (none was available).
- A save from another region is ignored. The log says, for example, "a JP save (G8MJ), this build reads G8ME saves".
  The game then shows no such save.
- A file that is shorter than it should be is ignored. The log says so.

## Troubleshooting

### Messages on a black screen

The port shows these and waits for START. Press START to leave.

| Message | What to do |
| --- | --- |
| This port needs a New 3DS, New 3DS XL or New 2DS XL. | Use a New 3DS family console. |
| The SD card cannot be used (missing or write-protected). | Put in the SD card. Make sure it is not write-protected. |
| No disc image found. Copy your Paper Mario: The Thousand-Year Door disc image (G8ME01) to sdmc:/3ds/ttyd/game.iso | Put the image at that exact path and name. |
| sdmc:/3ds/ttyd/game.iso cannot be read. | Check the file. Copy it again. |
| sdmc:/3ds/ttyd/game.iso is not the right game: found X, this build needs G8ME01. | Wrong game or wrong region. If X is not letters (shown as ?), the file is probably a compressed format. Convert it to a plain `.iso`. |
| This build supports revision 0 of G8ME01; the image is revision N. | Use a revision 0 disc image. |
| The disc image's file table cannot be read. / is damaged. | The image is damaged or cut short. Dump or copy it again. |
| Not enough memory: heap ... linear ... Install the CIA and start it from the HOME Menu. | Install the CIA instead of using the `.3dsx`. |

### The game stops or shows a crash screen

- The port has a crash handler. It shows a report on the screen. Press START to exit.
- It also saves the report to **`sdmc:/3ds/ttyd/crash.txt`**. The file keeps old reports, and new ones are added to the end.
- The normal log of the last run is **`sdmc:/3ds/ttyd/latest.log`**.
- A watchdog ends a run that stops responding for 10 seconds, and reports it as a crash.

### Saves

- A slot that says **File corrupted. This file must be erased.** failed the game's own check.
  The game keeps two copies of each slot. It shows the slot as corrupted only when both copies fail.
  Use Erase Data on that slot, then start a new game in it. Restore a backup if you have one.
- A save that does not match its checksums shows as corrupted. If a Dolphin save does this, copy it again
  and check that it is the same region.

### Making a bug report

Please include:

1. What you did, and what you expected.
2. The build string. It is the first line of `latest.log`, like `TTYD 3DS build #240 f4b3cc42`, and it is shown on the error screens.
3. Your console model (New 3DS, New 3DS XL, New 2DS XL) and your Luma3DS version.
4. Where in the game it happened (say what you were doing or where; the build number is on the last row of the bottom screen).
5. The files `sdmc:/3ds/ttyd/crash.txt` and `sdmc:/3ds/ttyd/latest.log`.
6. Do not send your disc image or your save files unless asked. They contain nothing the port needs for a bug report
   except when the bug is about saves.

## Legal

This port needs your own copy of the game. It contains no game data and does not help you get one.
It is a fan project. It is **not affiliated with or endorsed by Nintendo** or Intelligent Systems.
"Paper Mario" and "The Thousand-Year Door" belong to their owners.
