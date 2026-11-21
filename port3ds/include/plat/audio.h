/* Audio output for the game side (implemented in plat/audio.c over NDSP).
 *
 * One stereo stream of 16-bit samples at a fixed rate. The platform's audio
 * thread asks for blocks of samples as the DSP plays them; the game's AI
 * emulation (game/ai.c) fills each block. */
#ifndef PLAT_AUDIO_H
#define PLAT_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the audio thread: write exactly `frames` interleaved stereo
 * frames (L, R) to out. */
typedef void (*PlatAudioFill)(int16_t* out, uint32_t frames);

/* Starts the audio thread. Every call to fill asks for a multiple of
 * quantum_frames frames.
 *
 * Returns 1 when the samples reach the speakers. Returns 0 when there is no
 * sound output (NDSP cannot start, usually because sdmc:/3ds/dspfirm.cdc is
 * missing): the thread still calls fill at the same real-time rate and drops
 * the samples, so the game's audio timing (sequencer, voice ends, the waits
 * on them) runs as with sound. */
int plat_audio_start(uint32_t rate, uint32_t quantum_frames, PlatAudioFill fill);

/* 1 while samples reach the speakers. */
int plat_audio_has_output(void);

/* Frames filled since the start, and the blocks the DSP ran out of (each
 * one an audible gap), for the HUD and the log. */
uint32_t plat_audio_frames(void);
uint32_t plat_audio_underruns(void);

#ifdef __cplusplus
}
#endif

#endif
