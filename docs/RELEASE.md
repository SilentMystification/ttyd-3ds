# Release process

The version lives in one place: `port3ds/VERSION` (`MAJOR.MINOR.MICRO`, semver). The build embeds it in:

- the log banner, `TTYD 3DS v<version> build #<commits> <hash>` (and the error screen);
- the CIA title version (`makerom -major -minor -micro`; the CIA's major and minor are limited to 63, micro to 15);
- the SMDH description, which `3dsxtool` puts in the 3dsx (the SMDH has no version field, so the text carries it).

The log banner and the error screen show `<version>-dev+<shorthash>` for any build that is not a clean tree at the tag `v<version>`. The CIA and SMDH carry the plain `x.y.z`, because they are made when CMake configures and must not go stale.

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
   - `SHA256SUMS`;
   - `README.md`, a copy of USER_GUIDE.md.

   The folder name must be the plain `x.y.z`: if it has `-dev+`, the tree was not clean or the tag was missing.
7. **Check the artifacts:** boot the US CIA once in Azahar (`run_azahar.py`) and confirm the log banner shows the plain version; verify `SHA256SUMS` with `sha256sum -c`.
8. **Publish** the folder's files with the owner's go-ahead.

## What the artifacts contain

Code compiled from the decomp and the port, and generated placeholder banner, icon and sound (`make_placeholder_art.py`). No game data: the port reads textures, models, maps and sounds from the player's own `game.iso`. `release.py` checks only that an artifact is not a disc image and is not oversized, so do not add files to the CIA without reading PORT_3DS_PLAN.md section 6.1.

One number comes from the disc at build time: the GameCube arena's high end (`PORT_GAME_ARENA_HI`, from the disc's FST size, `gen_disc_data.py`). It is read from `iso/<id>.iso` when the build finds that disc, else a stand-in is used for JP and PAL. For a release build, have all three discs in `iso/` so the JP and PAL values are the real ones.
