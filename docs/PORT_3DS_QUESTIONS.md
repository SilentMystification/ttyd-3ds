# 3DS port: open questions

Questions that came up during work that the user should decide, collected while the user was away. Each one says what is assumed until there is an answer.

## 1. Japanese system font (G8MJ01)

The GameCube's system font (IPL ROM) is not on the disc and cannot be shipped. The port builds the same in-memory font from its own 5x7 ASCII glyphs (`port3ds/game/os_font.c`). TTYD draws this font only for system messages that the port never shows (disc errors, the progressive-scan prompt, the fatal error screen). `romfont.c` still builds its glyph table from it at boot.

The Japanese release needs an SJIS font for the same table.

- **Assumption until answered:** for G8MJ01 (phase 7), the table is built from a freely licensed Japanese bitmap font, chosen then.
- **Question:** is there a font you prefer or want avoided?

## 2. Display layout

The game's 608x480 picture is shown at half size (304x240), centred on the 400x240 top screen with black side borders.

- **Assumption until answered:** this layout. The bottom screen is the log, HUD and Select menu.
- **Question:** do you want an option to stretch the picture to the full 400 pixels of width (wrong aspect ratio), or anything else on the borders?

## 3. Textures converted at first use

The plan said to convert textures to PICA formats offline. The port now converts each texture the first time it is drawn and caches it, as the earlier 3DS ports did, because textures the game builds at runtime (screen copies, fonts) need that path anyway.

- **Assumption until answered:** first-use conversion. An offline pre-converted cache remains an option for the optimization pass, if map loads show conversion time.
- **Question:** none needed; this is noted here because it changes the plan.

## 4. Audio: one NDSP channel for the mix, not one per voice (for now)

You asked for NDSP voices from the start. The port currently mixes MusyX's voices on the CPU and plays the finished 32 kHz stereo mix on one NDSP channel. It runs MusyX's own DSP command lists through a software mixer (paper-cut's `hw_pcmixer.c`, MIT). This was the shortest route to correct sound: the mixer already implements MusyX's DSP behaviour (ADPCM, sample-rate conversion, envelopes, ITD, aux buses for reverb and chorus, surround folding). Those effects are also why a direct voice-per-NDSP-channel mapping is not exact: the aux reverb/chorus buses and the matrix surround have no NDSP equivalent.

- **Assumption until answered:** the CPU mixer stays for now (audio thread on core 1). The bug and optimization pass measures its cost on hardware. If the cost is too high, voices move to NDSP channels (24 available) with the aux effects kept on the CPU, compared against the CPU mix for equivalence.
- **Question:** is that order acceptable, or do you want the NDSP-voice design done before anything else?

## 5. Sound without the DSP firmware

NDSP needs `sdmc:/3ds/dspfirm.cdc`, which the user dumps with the DSP1 homebrew. Without it the port runs silently: the audio clock still runs so the game's sound timing is unchanged, and the log says why.

- **Assumption until answered:** silent with a log line, plus a one-time note on the bottom console.
- **Question:** would you rather it stop at boot with a message, as for a missing disc image?
