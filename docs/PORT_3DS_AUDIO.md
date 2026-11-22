# 3DS port: audio

How the port makes sound, how it is measured against paper-cut, and the NDSP voice path (switch `ndspvoices=1`). The plan is in [PORT_3DS_PLAN.md](PORT_3DS_PLAN.md) section 5.5; the switches are in [PORT_3DS_TESTING.md](PORT_3DS_TESTING.md).

## The two paths

- **Software mix (default).** MusyX (`libs/musyx`, its PC target) builds its DSP command list every 5 ms as on the GameCube, and paper-cut's mixer (`hw_pcmixer.c`) runs it on the CPU: ADPCM decode, resampling, envelopes, the aux buses with reverb and chorus, the compressor. The result is one stereo 32 kHz stream on NDSP channel 0 (`port3ds/plat/audio.c`). MusyX's frame runs on the audio thread (core 1) from the AI emulation (`port3ds/game/ai.c`), as it ran from the AI DMA interrupt.
- **NDSP voices (`ndspvoices=1`).** A voice that plays a DSP-ADPCM sample dry (no aux send) gets an NDSP channel of its own (channels 1 to 23); the DSP decodes, resamples and mixes it. Everything else stays in the software mix on channel 0. See "NDSP voices" below.

## Measuring

`tools/port3ds/audio_compare.py` makes the captures and compares them. Captures go to `build/audio/`.

| Command | What |
|---|---|
| `capture-papercut --scene S` | paper-cut's headless build (`build/papercut-headless/ttyd-us`, built from `../paper-cut` with `-DTTYD_BACKEND=headless`; the command prints the build lines when it is missing), `$TTYD_AUDIO_WAV`, its deterministic 60 Hz clock. `pc.wav`, 32 kHz. About a second per scene |
| `capture-port --scene S [--switches ...] [--lle-audio] [--no-sink]` | The port in Azahar through `run_azahar.py`. `wavdump.wav`: the game's software mix (switch `wavdump=N`), 32 kHz; with `ndspvoices=1` it holds only what is still mixed in software. `azahar.wav`: Azahar's sound output, every NDSP channel after the DSP (below) |
| `compare A.wav B.wav [--json F]` | B against the reference A, with tolerances (below) |
| `cpu --scene S [--switches ...]` | `pc_sample.py --interval 200` on the scene, under Azahar's GDB stub (with `wdog=900`: the stops slow the frames) |
| `loops` | The loop points of every sample on the disc |
| `loopcheck` | The port's loop re-alignment code (from `port3ds/plat/audio.c`, compiled with the host's gcc) on every looping sample of the disc, against MusyX's software path |
| `selftest` | The comparison against synthetic signals (a delayed, resampled copy passes; a pitch shift and gaps fail) |

Scenes (`audio_compare.py scenes`): `title` (boot to the title music), `gor_01` (Rogueport square at story 30: walk right, jump twice, walk left, jump), `battle` (`gon_02` at story 30, which starts a battle on entry), `win_00` (an ambient loop that needs the loop re-alignment), `tou_03` (effects whose reverb send comes after their start).

### Recording what NDSP plays

With NDSP voices the game no longer has the whole mix, so the capture has to come from the emulator. `capture-port` points Azahar's SDL2 sink at SDL's disk driver (`output_type=4`, the device `System audio playback device`, `SDL_AUDIO_DRIVER=disk`, `SDL_AUDIO_DISK_OUTPUT_FILE`; Azahar hands its own "auto" to SDL as a device name, which SDL refuses). SDL3 (the Flatpak's SDL2 is sdl2-compat) runs that device at 44.1 kHz whatever is asked. The sink takes samples at a wall-clock pace, and Azahar repeats its last sample or leaves silence when emulation falls behind, so the capture holds emulation at 34% (`frame_limit`) and slows the disk driver to match (`SDL_AUDIO_DISK_TIMESCALE`). Azahar still stalls at times: `compare --azahar` (automatic for `azahar.wav`) cuts those held and silent runs inside loud audio and reports them, and the gap count of an Azahar capture is not part of its verdict (the game's own underruns are in its log). A tap inside the DSP (every channel also sent to aux bus 0, read by an `ndspAuxSetCallback` callback) was tried first: libctru never called the callback in Azahar.

### The comparison

The two sides never match sample for sample (different resamplers, and NDSP's 32728 Hz), and the game's timing differs between runs (loads, the emulator's frame rate), so `compare` does not hash:

- **Alignment.** The best lag of B's RMS envelope (20 ms frames, dB) against A's, by Pearson correlation; then each 4 s window of A is aligned on its own (within 8 s), because the runs drift apart at loads.
- **Envelope.** Per-channel envelope correlation in each window (the median is the verdict's number), the level difference and the L/R balance.
- **Spectrum.** The long-term 1/3-octave spectrum (50 Hz up to 90% of the lower Nyquist frequency), level-normalised; the RMS of the band differences.
- **Pitch.** A shift search (1/24 octave steps, up to 3 semitones) on a fine log-frequency spectrum, and the chroma per frame (cosine similarity, per window).
- **Clicks and gaps.** Clicks: second-difference spikes ten times above their 50 ms neighbourhood and above -30 dBFS. Gaps: 10 ms or more of digital silence or one held sample with loud audio on both sides. A bad loop point shows up as either.

Tolerances: envelope correlation at least 0.75, level within 4 dB, spectrum RMS difference at most 4 dB, pitch within 0.25 semitone, chroma cosine at least 0.75, at most 6 more clicks and 2 more gaps a minute than A.

paper-cut's golden audio hashes (`tests/port/golden/audio_G8ME01.json`, from its `map_sweep.py --golden`) are SHA-1s of the samples of each map's headless WAV dump. They only say "bit-identical to paper-cut's last run": the port's game timing differs (map loads, Azahar's frame rate), so even the same mixer never produces the same samples, and the plan's "within tolerance" needs the comparison above instead. paper-cut's own captures (`capture-papercut`) are the reference.

## Baseline (phase 5's open item)

US disc, Azahar 2126.2, HLE DSP. `pc` is paper-cut's capture, `wavdump` the port's software mix, `azahar` Azahar's output (recorded at 34% speed, except the title's at 100%, before the speed limit existed). Envelope is the median per-window correlation (left/right; the 10th percentile in brackets).

| A | B | env L/R (p10) | level dB | spectrum dB | pitch | chroma | clicks/min A/B | verdict |
|---|---|---|---|---|---|---|---|---|
| title pc | title wavdump | 0.94/0.92 (0.75) | -2.0/-2.3 | 0.30 | 0 | 0.99 | 1/2 | ok |
| title wavdump | title azahar | 0.80/0.78 (0.58) | +0.1/+0.1 | 1.77 | 0 | 0.91 | 2/0 | ok |
| title pc | title azahar | 0.75/0.76 (0.53) | -2.0/-2.1 | 1.76 | 0 | 0.90 | 1/0 | ok |
| gor_01 pc | gor_01 wavdump | 0.79/0.81 (0.71) | -1.9/-2.3 | 0.39 | 0 | 0.97 | 13/28 | clicks |
| gor_01 wavdump | gor_01 azahar | 0.63/0.53 (0.28) | -0.1/-0.1 | 1.30 | 0 | 0.89 | 28/59 | envelope, clicks |
| battle pc | battle wavdump | 0.91/0.87 (0.69) | -2.0/-2.5 | 0.89 | 0 | 0.97 | 0/0 | ok |
| battle wavdump | battle azahar | 0.54/0.53 (0.32) | -0.1/-0.1 | 1.15 | 0 | 0.80 | 0/3 | envelope |
| tou_03 pc | tou_03 wavdump | 0.84/0.92 (0.79) | -1.4/-0.8 | 0.16 | 0 | 0.96 | 0/0 | ok |
| win_00 pc | win_00 wavdump | 0.56/0.54 (0.34) | -4.6/-3.8 | 2.27 | 0 | 0.57 | 21/13 | envelope, level, chroma |

- **The port's mix against paper-cut is within tolerance** on the title, the battle and `tou_03`. The spectrum matches to under 1 dB and the pitch exactly (both run the same mixer code); the music lines up once each window is aligned (the title's second piece starts 4 s later on the port, after a longer load). The port is 2 dB quieter than paper-cut in every scene, left and right alike; this is not a mix gain (see "Level against paper-cut" below: the scale of the waveforms agrees, the content differs).
- `win_00` does not match: the chroma (0.57) says the two runs play different music or ambience there at story 30 (a different scene state between the two warps), not a mixing difference; not investigated. It is used below only for the A/B, port against port.
- `gor_01` fails on clicks only: the two runs do not walk and jump at the same moments (the port's padscript and paper-cut's input script differ in timing), and footsteps and jump sounds are the transients the click detector counts. Paper-cut's 13 a minute are its own footsteps.
- **The Azahar recordings are the noisy instrument.** Azahar stalls even at 34% speed (`gor_01` 5.6 s and the battle 7.5 s of held runs cut, 130 to 170 gaps a minute in loud audio before cutting), and every stall shifts the rest of the window, which costs envelope correlation (0.5 to 0.8 against the game's own mix of the same run). Spectrum, level and pitch survive it (within 0.1 dB of level, under 2 dB of spectrum). So NDSP voices are judged against an Azahar recording of the software path, made the same way (below), not against `wavdump`.
- The game's own underrun counter (`audio: peak ... underruns`) was 0 in every run.
- The `--lle-audio` run is under "A/B" below.

## NDSP voices

`ndspvoices=1` (default off). Code: `port3ds/game/audio.c` (the decision, per MusyX frame), `port3ds/plat/audio.c` (channels, sample cache, loop re-alignment, timing), and one hook in `libs/musyx/src/musyx/runtime/hw_pcmixer.c` (`salPcPbOffload`, `TARGET_PORT` only).

### Voice mapping

- **Which voices.** At a voice's start (its state is 1 when MusyX's frame begins), the voice gets a free NDSP channel when it plays a plain DSP-ADPCM sample (`compType` 0 or 1, loop type 0, outside the stream buffers at the top of ARAM), in studio 0, with no aux send. It keeps the channel until it ends, MusyX stops it, or a new note starts on it. Everything else stays in the software mix: streams (TTYD's background music is streamed, so its music mostly stays there), PCM samples, voices with an aux send, and starts when all 23 channels are busy (counted as overflow; MusyX already caps and steals voices itself, 64 of them, so nothing is dropped, the extra voice is mixed in software).
- **MusyX is unchanged.** The command list is built as before. For a voice on NDSP, the mixer steps its parameter block as its null mix does (addresses, loops and a one-shot's end, the envelope and volume ramps, the parameter updates) without decoding or mixing it, so everything MusyX reads back (state, positions, depop values) is as with the software mix, and MusyX's own voice logic, priorities and stealing run as they always did.
- **Per frame (5 ms).** From the block after the frame: rate `ratio * 32000 / 65536` Hz (NDSP resamples to its 32728 Hz), gains `env * vol / 32768^2` with the envelope and volumes averaged over the frame (start and end), surround as `L - S`, `R + S` (MusyX's matrix), the interpolation (polyphase, linear, none, as `srcSelect`) and the one-pole low-pass (`ndspChnIirMonoSetParamsCustomFilter`). NDSP applies them at its next frame; there is no ramp inside a frame.
- **Streams** (`ndspstreams=1`, with `ndspvoices=1`) go to NDSP channels too: see "NDSP streams" below.
- **Not carried over.** The interaural time delay (a few samples between left and right), the category gains (paper-cut's settings sliders; 1.0 in the port), the compressor (off in TTYD), and the depop ramp of a voice cut while loud.

### Timing

MusyX's frame renders into the AI block that plays three quanta later (15 ms), and channel 0 has up to 50 ms queued, so a voice set up at once would sound up to 65 ms early against the software mix. Every start, mix and stop is an event stamped with the output frame its quantum reaches the speakers at, and the audio thread applies the events once per DSP frame, as channel 0's playback position (its buffer's sequence id and the DSP's sample position) reaches them. Voices stay within about one DSP frame of the software mix. A stalled channel 0 does not hold voices back: events more than half a second ahead, or a queue more than half full, are applied at once (counted as "late").

### Sample memory

The DSP reads linear memory and ARAM is a heap buffer, so each sample is copied to a cache in linear memory (4 MB by default, `ndspcachekb=N`, at most half of the free linear memory), least recently used first out, never while a channel plays it. The game reloads sound groups at the same ARAM addresses, so a cached copy is compared with ARAM at every start (`memcmp`, about the cost of the copy) and replaced when it differs.

### Loops

NDSP loops a whole wave buffer, and an ADPCM buffer must start at a frame (14 samples, 8 bytes). A voice plays as buffer A (the start up to the loop) and buffer B (the loop, repeated, its decoder history reloaded at each pass as MusyX's loop type 0 does).

`audio_compare.py loops` on the US disc: one sample directory (`sound/proj/pmario.sdir`), 1247 samples, all DSP-ADPCM. 212 loop; **151 of them (71%) start inside a frame**, and 197 end inside one (an end inside a frame is fine: a buffer may stop at any sample). Loops are 1052 to 100345 samples long (median 14722), none shorter than two frames. So the re-alignment is needed, and since the port reads the player's disc and ships no data, it is done at load time.

Checked offline against the disc: the re-aligned copy, played as NDSP plays it (A, then B with its history reloaded each pass), against MusyX's software path (decode to the end, jump to the loop with the loop history), over the first pass and three loop passes of every looping sample. The test is the plat code itself (`adpcm_decode`, `adpcm_encode_frame`, `realign_loop` compiled for the host). All 151 re-align, none refused; the disc's loop predictor/scale always equals the loop frame's header. Worst SNR 52.5 dB over a whole sample; the error is confined to the re-encoded frames at the loop seam, worst single sample 2039 of 32767 (sample id 1078, at a loud seam). That is the quantisation of re-encoding already-quantised data in one frame per pass, the same order as DSP-ADPCM's own error.

The re-alignment is done when a sample enters the cache. For a loop from `ls` to `end`: the loop start moves forward to the next frame boundary `nls` (`d = nls - ls`, 1 to 13 samples), the end moves by the same `d`, and the waveform past the old end continues with the loop's first `d` samples. Only the one or two frames holding the old end are re-encoded (each with all 8 predictors and 12 scales of the sample's own coefficient table, a beam search over the nibbles, run through the decoder exactly); every frame before them is the disc's own. The new loop's history is the decoded value of the two samples before `nls`, which is what the loop pass reaches there.

### Aux and reverb

TTYD sets up studio 0 with ReverbHI on aux A and Chorus on aux B (`sound.c`), and sends to them per sound effect (`psndSetReverb`: 0, 20, 40 or 60 by map; `SoundEfxSetAux1` sets it on an effect after it starts) and per MIDI channel. Measured with `ndspstats` (voice-frames: one voice in one 5 ms frame; a send counts when its volume or ramp is non-zero):

| Scene | Voice-frames | With an aux send |
|---|---|---|
| title | 100832 | 2306 (2.3%) |
| `gor_01` | 46311 | 0 |
| battle | 97870 | 840 (0.9%) |
| `win_00` | 55354 | 5716 (10%) |
| 27-map sweep, 240 frames each | 673853 | 76403 (11%); none in 9 maps, over a third in `tou_01`, `tou_03`, `eki_00`, `nok_00` |

**Decision: a voice with an aux send plays entirely in the software mix**, dry part included. That is MusyX's own aux path (the buses, `salHandleAuxProcessing`, the reverb and chorus callbacks), unchanged, so reverb sounds as before. The cost: the aux callbacks and the synth logic together take 1.7% of the time in `gor_01` (fill minus command list and mixer), and an aux voice costs what any software voice costs. The alternative (dry on NDSP, only the aux sends mixed in software into one more channel) still decodes and resamples the voice on the CPU, so it would save only the dry mix, and it would put the dry and wet halves of one voice on two clocks. NDSP's own aux buses would need libctru's aux callback, which never ran in Azahar (see "Recording what NDSP plays").

Most sends come **after** the start (the game sets an effect's reverb just after starting it): in the first sweep, 55053 of the 76403 aux voice-frames were on voices already on NDSP, whose send was then lost. So a voice on NDSP that gets an aux send moves back to the software mix (`nv_to_software`): its channel stops at that frame, and since the null mix kept the block's addresses, loop count, ramps and resampler phase but decoded nothing, the decoder state (predictor/scale, two history samples) and the resampler's last four source samples are rebuilt by fetching the sample with the mixer's own fetch (`salPcPbFetch`) from a copy of the block as the voice started, up to where it is now (at most the sample plus one loop; the send comes within a frame or two, so it is a few hundred samples). The handover is at a frame boundary on both sides (the stop is an event at that frame's output time). `ndspstats` counts the moves ("moved to software for a late aux send") and any position not reached ("history not rebuilt", then the voice plays on from a cleared history).

Checked with a sweep of the eight maps with the most late sends (`gor_02`, `nok_00`, `mri_01`, `tou_01`, `tou_03`, `usu_00`, `dou_00`, `eki_00`, 240 frames each): 231 starts, 11 voices moved back to software, every history rebuilt, no aux send left on an NDSP voice (it was 55053 voice-frames, a few long ambient voices, before the move existed) and no history left unrebuilt (`norebuild 0`), no crash; 40% of the voice-frames still ran on NDSP. The `tou_03` row of the A/B table is a recording with two such moves.

### A/B: `ndspvoices=1` against the software mix

Same scene, same build, both recorded from Azahar at 34% (LLE at 15%):

| Scene | A (`ndspvoices=0`) | B (`ndspvoices=1`) | env L/R (p10) | level dB | spectrum dB | pitch | chroma | clicks/min A/B | verdict |
|---|---|---|---|---|---|---|---|---|---|
| `gor_01` | azahar | azahar | 0.80/0.78 (0.69) | 0.0/0.0 | 0.04 | 0 | 0.95 | 59/57 | ok |
| battle | azahar | azahar | 0.86/0.84 (0.74) | +0.1/+0.1 | 0.05 | 0 | 0.91 | 3/2 | ok |
| `tou_03` (late reverb sends: 2 voices moved back to software) | azahar | azahar | 0.90/0.94 (0.86) | -0.1/-0.1 | 0.06 | 0 | 0.94 | 2/1 | ok |
| `win_00` (a re-aligned loop) | azahar | azahar | 0.88/0.89 (0.72) | +0.1/+0.2 | 0.85 | 0 | 0.89 | 14/16 | ok |
| `gor_01`, LLE | azahar | azahar | 0.82/0.79 (0.70) | 0.0/0.0 | 0.03 | 0 | 0.95 | 51/58 | clicks (+7; see below) |

- Level agrees to 0.2 dB and the spectrum to 0.1 dB (`win_00`: 0.85, its top band, below): the NDSP voices come out at the gains, rates and filter the software mix gives them. The envelope correlation is the instrument's own: two `ndspvoices=0` recordings of `gor_01` against each other give 0.84/0.81, spectrum 0.03 dB, chroma 0.95, and a click count 11 a minute apart (Azahar's stalls), while the game's own mix of those two runs is identical (envelope 1.00). No more clicks with NDSP voices than without.
- Voices moved: `gor_01` 118 starts on NDSP, 4 kept in software (streams), 0 overflow, 0 refused; 46% of the voice-frames ran on NDSP. Battle 80 starts on NDSP, 7 kept (6 streams, 1 aux); 36% of the voice-frames. At most 3 channels played at once in these scenes (TTYD's 64 MusyX voices are rarely all busy), so stealing never happened; when the 23 channels are full the extra start stays in software ("no channel").
- No sample in `gor_01` or the battle needed the loop re-alignment at run time (`realigned 0`): their NDSP voices were one-shots and frame-aligned loops. Besides the offline check of every loop (`loopcheck`, above), a 27-map sweep (`sweep=240`, story 30, `ndspvoices=1`) re-aligned 15 loops at run time (in `gor_03`, `win_00`, `mri_00`, `tou_01`, `usu_00`, `muj_00`, `bom_00`, `aji_00`; ambient loops, most of them stereo pairs) with no refusal, no overflow and no crash, and `win_00`, whose ambient loop pair (loop 2382..37908, moved 12 samples) is re-aligned, is the `win_00` row above: the same envelope, level and pitch as the software mix. Its spectrum differs at the top band only (4 dB at 16 kHz, where NDSP's 32728 Hz output and its polyphase filter reach a little further than the 32 kHz mix).
- The title is unchanged by the switch: its music is a stream, and only 2 voices started in the capture.
- **LLE (`--lle-audio`, the console's DSP firmware run by Azahar, at 15%):** `gor_01` `ndspvoices=0` against `=1`: envelope 0.82/0.79 (p10 0.70), level 0.0 dB, spectrum 0.03 dB, pitch 0, chroma 0.95, clicks 51 against 58 a minute (one over the tolerance, inside the 11 a minute the instrument shows between two identical runs). The same 118 starts went to NDSP, so the firmware plays the ADPCM buffers, loops, rates, gains and filter as the HLE does. LLE's recording against HLE's (both `ndspvoices=0`) differs by 0.6 dB of spectrum: the firmware's own resampling and mixing, the same with and without NDSP voices.

### Threading

All of it runs on the existing audio thread. The decision and the parameter updates run inside MusyX's frame (`salCtrlDsp`, under `hwIRQEnterCritical`), so the game's sound calls cannot run meanwhile; the events are produced and consumed on the same thread, so they need no lock; the NDSP calls happen when the thread applies them, once per DSP frame.

### CPU

`gor_01` at story 30 after its padscript, Azahar, HLE DSP. Two measures:

| | `ndspvoices=0` | `ndspvoices=1` |
|---|---|---|
| Audio thread "fill busy" (the game's own tick count around the fill calls, share of wall time, `audio: peak` lines) | 14.3% | 10.6% |
| of which MusyX's mixer (`[NDSPV] ... mixer`, `ndspstats`) | 12.3% | 8.6% |
| of which the command list | 0.3% | 0.3% |
| `pc_sample.py --interval 200`, 60 s (175 stops): audio thread busy | 25.0% | 30.3% |
| of which `DSP_FlushDataCache` (an IPC to the DSP service, blocked) | 17.6% of its samples | 21.1% |
| of which game code (mixer, MusyX, `sound.c`) | 7.4% | 9.1% |

Other scenes, in-game measure (fill busy; mixer): battle 19.7% to 16.5% (16.9% to 13.8%); `tou_03` 14.4% to 14.6% (most of its voices have reverb sends, so they stay in software).

- The in-game measure is the reliable one: the mixer's share drops by 30% and the whole fill by 26%. The drop is not larger because TTYD streams its background music: the stream voices (and their resampling and the bus work) stay in the software mix, and they are most of what is left. Moving the streams to NDSP channels is `ndspstreams=1`, below.
- `pc_sample` cannot see the difference: at 2.9 Hz the audio thread gets about 175 samples a minute (one standard deviation is about 3.3 points), and most of its busy samples are the thread blocked in `DSP_FlushDataCache`, the cache flush of each 10 ms channel-0 buffer before NDSP reads it (100 a second). In Azahar that IPC is expensive; on the console it is a service round trip. It is the same in both modes; flushing fewer, larger buffers (or once per refill pass) would cut it, at the cost of latency.
- The NDSP side adds the null stepping of each offloaded block (no decode), one `memcmp` of the sample per voice start, and one `ndspChnSetMix`/`ndspChnSetRate` per playing voice per frame.

## NDSP streams

`ndspstreams=1` (default off; needs `ndspvoices=1`). TTYD's music is MusyX streams: a ring of 0xE00 samples (3584, 112 ms; 2 KB of ADPCM) per mono stream, two streams for a stereo piece, refilled by the game's update callback behind the voice's position. Code: `nv_start_stream` and `nv_stream_upload` in `port3ds/game/audio.c`; the stream slots, mirror, segment chain and refill events in `port3ds/plat/audio.c`; one hook in `libs/musyx/src/musyx/runtime/hw_aramdma.c` (`salPcStreamUploadHook`, `TARGET_PORT` only, called on the high-priority ARAM uploads that `hwFlushStream` makes).

- **Which voices.** A voice whose sample is at the top of ARAM (the stream area) with `compType` 4 (DSP-ADPCM stream) or 2 (PCM16 stream), looping the whole ring, in studio 0, no aux send. Everything else about the voice (rate, envelope, gains, pan) goes through the same per-frame mix update as the other voices, so a stereo pair is two mono channels with their own pans. MusyX still steps the stream's block with the null mix, so `streamHandle` sees the position it always saw and runs the game's update callback as before.
- **The mirror.** At the start the whole ring is copied (synchronously, from ARAM) into linear memory. Each later refill is seen at the ARAM upload; MusyX's position runs ahead of what is heard by the lead (15 ms plus up to 50 ms queued), and the refill overwrites the part of the ring behind MusyX's position, which the output has not reached yet. So the mirror takes a refill as an event at the output time of the end of its block, plus four quanta (a DSP frame of slack; the apply test is one quantum early). These events have a queue of their own so they hold no voice event back. The part of the ring the mirror still holds correctly is the ring minus what the refills lag by (the callback's 20 ms period, the mirror's 15 to 20 ms).
- **Playing it.** The ring is played as a chain of wave buffers of 560 samples (40 ADPCM frames, 17.5 ms), three queued, topped up once per DSP frame. A segment is queued only if it ends within 60% of a ring after what is playing now (`ndspChnGetSamplePos` of the playing buffer; otherwise it waits, counted as `waits`). The first buffer carries the stream's decoder state (predictor/scale and zero history); the others carry no ADPCM context, so the DSP carries its history on across buffers and around the ring's end, as MusyX's stream loop type does (`loopType` 1; the ring's first frame header equals the loop predictor/scale MusyX reloads). PCM16 streams (the game's `SOUND_SS_FLAG_ADPCM` clear) use the same chain with PCM16 buffers.
- **Not done.** A stream that gets an aux send after it started keeps playing on NDSP and loses the send (its decoder history cannot be rebuilt as a sample's can; TTYD gives streams no aux send, and none was seen). `sndStreamARAMUpdate` refills made on the game's thread (TTYD does not use it) are ignored.

### A/B: `ndspvoices=1 ndspstreams=1` against `ndspvoices=0`

Azahar, HLE DSP, 34% speed, both sides recorded the same way (the machine was loaded by other runs; the title's `ndspvoices=0` recording went silent after 40 s, an Azahar stall, so the title is compared over its first 38 s).

| Scene | env L/R (median per 4 s; whole run at one lag) | level dB | spectrum dB (worst) | pitch | chroma | clicks/min A/B | verdict |
|---|---|---|---|---|---|---|---|
| title (a stream; 4 s to 38 s) | 0.86/0.92; 0.99/0.99 | +0.10/+0.06 | 1.26 (4.1 at 16 kHz) | 0 | 0.96 | 0.0/0.0 | ok |
| battle (6 streams) | 0.93/0.92; 0.82/0.81 | +0.19/+0.16 | 2.27 (7.4 at 16 kHz) | 0 | 0.92 | 0.0/1.2 | ok |
| `gor_01` | 0.83/0.83; 0.72/0.75 | +0.05/+0.03 | 1.29 (4.1 at 16 kHz) | 0 | 0.94 | 53/85 | clicks |

- The 16 kHz band is NDSP's 32728 Hz output and polyphase filter against the 32 kHz mix (as for `win_00` above). The envelope numbers are the instrument's (Azahar stalls). Level and pitch agree to 0.2 dB and exactly.
- `gor_01` clicks: voices alone (`ndspvoices=1`) against `ndspvoices=0`: 67/53 a minute, voices plus streams against voices alone 85/67. Footsteps and jumps are the clicks (the doc's earlier 11 a minute between identical runs); +18 is above that, and the title and battle, which are all music, show none, so the segment joins are not the cause, but the cause is not pinned.
- Streams on NDSP in the runs: title 4 starts (two stereo pieces), battle 6, `gor_01` 4; 0 underruns, 0 waits, 0 dropped refill events; refills about 600 a minute per stereo pair.

### CPU, streams

In-game "fill busy" (the audio thread's fill calls, share of wall time, `audio: peak` lines), Azahar HLE:

| Scene | `ndspvoices=0` | `ndspvoices=1` (before streams, from above) | `ndspvoices=1 ndspstreams=1` |
|---|---|---|---|
| title | 8.9 to 9.9% | 8.0 to 9.9% | 6.5 to 6.8% |
| `gor_01` | 14.3 to 14.6% | 10.6% | 8.2% |
| battle | 20.1 to 20.2% | 16.5% | 11.1 to 11.3% (MusyX mixer 6.5% of the time; it was 17.2%) |

Music was most of what stayed in the software mix: voices and streams together take the audio thread's fill from 14.4% to 8.2% (`gor_01`) and from 20.2% to 11.2% (battle) of the time, about 43% less. The rest is the command list, the aux voices, the software voices left (PCM, no-channel) and the output bookkeeping.

## Level against paper-cut

The baseline's "2 dB quieter in every scene" is not a gain in the port.

- **No gain stage differs.** `hw_pcmixer.c` differs from paper-cut's only by the offload hook and the pointer handles; `hw_pc.c`'s render copies the quantum; `portAudioMixExtra` (the DVD stream, the player's music files, PM64 effects) adds nothing without those inputs; the category gains default to 1.0 and `salPcSetVolume` (the master volume) is Aurora-only, so the headless capture has none. The game's volume calls are the same (`sound.c`: `sndVolume(0x7F, 0, 0xFF)`). On the port's side the AI block is copied to channel 0 unscaled and `set_mix(0, 1.0, 1.0)`.
- **The waveforms agree in scale.** Title, paper-cut's `pc.wav` against the port's `wavdump.wav`, aligned by cross-correlation (a constant lag) and 5 s windows from 12 s: the least-squares gain of one on the other is 1.02 (+0.17 dB), twice. The RMS of the same aligned 4 s windows differs by 1.6 dB (paper-cut louder) at four times out of four. Both hold at once because paper-cut's render has more energy that the port's does not: per 1/3-octave band, +0.6 dB at 50 Hz, +1.0 at 200 to 400, +1.7 at 400 to 800, +3.8 at 800 to 1600, +3.0 at 1600 to 3200, then -0.7 and -1.3 above 6400, and its left/right balance is 0.0 against the port's L 1.0 dB below R. A level offset would be flat over the bands and the balance. So the gap is a difference in what is playing (a layer, a MIDI channel's volume or pan, an effect's wet level), in the title, a scene of one music piece. The software mixer is the same code and gives the same scale to what it is given.
- **Not found.** Which voice differs needs a per-voice dump on both sides at the same point (not in the audio files' remit: MusyX's voice state is the same code, so the difference is in what the game sets, e.g. through the sequencer, the stream fades or pan, or the reverb and chorus). The scenes' `level` figure in the baseline table is therefore not a mix gain to fix; with it open, treat "within 4 dB" as the tolerance for a music-heavy scene.

## Channel 0's cache flush

Each 10 ms channel-0 buffer is flushed with `DSP_FlushDataCache` before NDSP reads it (100 a second). libctru's version is a service call on the DSP session; the linear heap is cached memory and the DSP reads physical memory, so a buffer written by the CPU has to be written back; there is no uncached linear allocation to avoid it, and NDSP offers no flush-free buffer. What can change is the number of calls: `audiobatch=N` (default 1) refills the ring's buffers only once N are free and flushes the whole 16 KB ring once, before queueing them. In Azahar (`gor_01`, voices and streams on) `audiobatch=1` made 7495 flushes taking 0.57 s in 80 s and `audiobatch=2` 3748 taking 0.28 s (`[NDSPV] flush ticks` line); the output is the same (against `audiobatch=1`: level +0.01 dB, spectrum 0.05 dB, pitch 0, clicks 85 and 82 a minute, no underruns). So the "17 to 21% of the audio thread's samples" the CPU table above shows for `pc_sample` is the sampler's own bias (the stop lands in the service call) and not a cost to remove. On the console this is a number to measure: compare `flush ticks` with `audiobatch=1` and `2`.

## Risks and checks on the console

- These numbers come from Azahar, HLE and (one scene) LLE with the console's firmware. On the console, check: the ADPCM loop history reload of a looping buffer (both HLE and the firmware are expected to reload it, as `adpcm_data` marks the buffer), negative mix gains (surround; TTYD runs stereo, so `vS` is normally 0), and the monopole filter coefficients.
- A voice whose aux send starts after it started on NDSP moves back to the software mix at a frame boundary (see "Aux and reverb"): one 5 ms frame of the send is missed, and the handover is only as exact as the event timing (about one DSP frame).
- Volume changes land once per NDSP frame, without a ramp: a fast attack is a 5 ms step.
- A stream with `ndspstreams=1`: the DSP must carry the ADPCM decoder history from one wave buffer to the next and around the ring (buffers after the first carry no ADPCM context); Azahar's HLE does, the title's long music is clean there, and the console's firmware is the thing to check (a tick at each 17.5 ms join would be it; if so, give each buffer the right history by decoding the previous 2 samples on the CPU).
- The mirror takes a refill about 15 to 20 ms after MusyX makes it; a game callback that returns less than it was asked to leaves less data in the ring (as on a GameCube), and the segment chain waits (`waits`) before it plays a stale sample. A stalled audio thread long enough for the ring to lap is as bad as on the GameCube.
- `audiobatch=2` adds up to one buffer (10 ms) of queue latency.
