# Changelog

The version is in `port3ds/VERSION`; the release steps are in [docs/RELEASE.md](docs/RELEASE.md).

## 0.1.0 (unreleased)

First release of the Paper Mario: The Thousand-Year Door port for the New 3DS. It runs the decompiled game code on the 3DS and reads all game data from the player's own GameCube disc image.

- **Regions:** US (G8ME01), JP (G8MJ01) and PAL (G8MP01) builds, each a CIA and a 3dsx. A build refuses another region's disc with a message. JP and PAL in-game testing needs those discs and is not done yet.
- **Install:** one CIA per region; the disc image goes to `sdmc:/3ds/ttyd/game.iso`. No conversion step: models, maps and textures are converted while playing.
- **Graphics:** GX translated to the PICA200 (display-list cache, draw merging, unlit fast path, vertex-shader path under test), a real THP movie decoder.
- **Audio:** MusyX on NDSP.
- **Saves:** big-endian, GameCube- and Dolphin-compatible; old port saves are migrated.
- **Options menu:** SELECT or the touch button, stored in `sdmc:/3ds/ttyd/options.txt`.
- **Tools:** crash reports and over-the-air CIA update (`ctr-devkit`), deterministic mode, a GameCube debug DOL and `gw_diff.py` for comparing with Dolphin, map sweep, galleries, profilers.
- **Tested** in the Azahar emulator; the first hardware session is still to come, so real-console behaviour is unverified.
- **Known problems:** the `gon_02` "struck first" black bar; the `hei_01` battle start is 2 frames late; frame rate is below the GameCube's while the performance work continues; stereo 3D is not in yet.
