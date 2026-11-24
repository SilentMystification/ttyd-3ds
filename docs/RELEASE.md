# Release process

The version lives in one place: `port3ds/VERSION` (`MAJOR.MINOR.MICRO`, semver). The build embeds it in:

- the log banner, `TTYD 3DS v<version> build #<commits> <hash>` (and the error screen);
- the CIA title version (`makerom -major -minor -micro`; the CIA's major and minor are limited to 63, micro to 15);
- the SMDH description, which `3dsxtool` puts in the 3dsx (the SMDH has no version field, so the text carries it).

The log banner and the error screen show `<version>-dev+<shorthash>` for any build that is not a clean tree at the tag `v<version>`. The CIA and SMDH carry the plain `x.y.z`, because they are made when CMake configures and must not go stale.


## Standing rule: no game assets in the binaries

The CIA, the 3dsx and every release package contain no game assets. Models, textures, maps, sound, text and the executable's data arrays are read from the player's own disc image at run time. The binaries hold compiled code (the decompiled game, the SDK, MusyX and the port), the system metadata of the CIA (placeholder banner, icon and sound) and the per-region arena value. New data the port needs from the game is read from the disc image, never embedded.

- **Image arrays: the allowed list is empty.** The decomp's image arrays (`mapdrv.c` texture_0, `eff_torch.c` torch textures, `mobjdrv.c` cubeDN, `win_04.c` texture_0, the memory card art) are declared without contents under `TARGET_PORT` and filled at boot from the disc image (`include/port_disc_data.h`, `tools/port3ds/gen_disc_data.py`). `tools/port3ds/check_image_arrays.py` runs after every link and fails the build if an image array is initialised data in the ELF (its `ALLOWED` list is empty). A new one needs a `PORT_DISC_DATA` entry instead.
- **The port's own resources live outside the binary**, in a `res/` folder: the generated JP ROM font `res/sjis_font.bin` (about 250 KB). The build writes it to `build/3ds/<game dir>/res/`, `release.py` packages `res/` next to the CIA and 3dsx, and the player copies it to `sdmc:/3ds/ttyd/res/` (`port3ds/plat/res.c`; the loader also finds `sdmc:/3ds/ttyd/queue/res/`, where `devupdate.py files put sjis_font.bin --dest res` leaves it). If the JP build cannot find it, it stops with a message naming the file.
- `release.py` refuses artifacts that look like a disc image.

## Checklist

1. **Tests**, on the commit to release (nothing dirty):
   - map sweep, all maps: `tools/port3ds/map_sweep.py` (a full sweep is for milestones; see PORT_3DS_TESTING.md);
   - the galleries and the effect and battle sweeps;
   - the acceptance tests (vertex shader: no z-fighting, no seams; `vsh_accept.py`; `test_decode.py`; `test_gci.py`);
   - the wrong-disc check for JP and PAL (PORT_3DS_TESTING.md, "Regions");
   - the hardware checklist, PORT_3DS_HW_CHECKLIST.md, on the New 3DS, for the US build at least. Record what was not checked in the changelog.
2. **Version bump:** edit `port3ds/VERSION`.
3. **Changelog:** move the `Unreleased` entries of CHANGELOG.md under the new version heading with the date, and list known problems.
4. **Commit** the bump and the changelog on `port3ds` (or `main`, as agreed).
5. **Tag:** `git tag -a v<version> -m "TTYD 3DS <version>"`. Tags are made by the owner; nothing is pushed without them.
6. **Artifacts:** `python3 tools/port3ds/release.py`. It refuses a dirty tree. It builds US, JP and PAL (`build.sh . -DREGION=...`) into `build/release/<version>/`:
   - `ttyd-3ds-<version>-{us,jp,pal}.{cia,3dsx}`;
   - `res/sjis_font.bin`, the resource folder the player copies to `sdmc:/3ds/ttyd/res/` (needed by the JP build; harmless for the others);
   - `SHA256SUMS`;
   - `README.md`, a copy of USER_GUIDE.md.

   The folder name must be the plain `x.y.z`: if it has `-dev+`, the tree was not clean or the tag was missing.
7. **Check the artifacts:** boot the US CIA once in Azahar (`run_azahar.py`) and confirm the log banner shows the plain version; verify `SHA256SUMS` with `sha256sum -c`.
8. **Publish** the folder's files with the owner's go-ahead.

## What the artifacts contain

Code compiled from the decomp and the port, and generated placeholder banner, icon and sound (`make_placeholder_art.py`); the port's own resource files (the JP font) sit next to them in `res/`, not inside. No game data: the port reads textures, models, maps and sounds from the player's own `game.iso`. `release.py` checks only that an artifact is not a disc image and is not oversized, so do not add files to the CIA without reading PORT_3DS_PLAN.md section 6.1.

One number depends on the disc: the GameCube arena's high end (`PORT_GAME_ARENA_HI`, where the IPL puts the FST, from the disc's FST size). The build does not read a disc for it: it comes from the committed `ARENA_HI` table in `tools/port3ds/gen_disc_data.py`. The US value (`0x817EE1A0`) was computed from the US disc and equals the one measured in Dolphin; **JP and PAL are unknown** (`None`) until filled.

A dev build of JP or PAL uses the US value as a stand-in and prints a build warning. `release.py` refuses to package a JP or PAL release while its value is unknown (`--regions US` packages only the known ones). To fill the table, someone with that disc runs this once:

```
tools/port3ds/gen_disc_data.py --print-arena iso/G8MJ01.iso     # or iso/G8MP01.iso
```

and puts the printed line (`"G8MJ01": 0x...,`) in `ARENA_HI`, then commits. After that no disc is needed for any build.
