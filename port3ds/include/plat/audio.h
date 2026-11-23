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
void plat_audio_set_muted(int on); /* mute or restore the output (ramped over one buffer); the pause of the Options menu */
int plat_audio_muted(void);

/* Frames filled since the start, and the blocks the DSP ran out of (each
 * one an audible gap), for the HUD and the log. */
uint32_t plat_audio_frames(void);
uint32_t plat_audio_underruns(void);

/* ---- NDSP voices (switch ndspvoices=1) ----
 *
 * One sound voice per NDSP channel: the DSP decodes the DSP-ADPCM sample,
 * resamples and mixes it. Channel 0 stays the software mix (fill), so voice
 * channels are 1..plat_voice_channels(). Every call is made from inside
 * fill (the audio thread) and takes effect when the output of the quantum
 * being filled, delayed by the lead (plat_voice_set_lead), reaches the
 * speakers, so voices stay in time with the software mix. */

/* A sample to play, as DSP-ADPCM frames (8 bytes: a predictor/scale byte,
 * then 14 samples of 4 bits). Sample numbers count from the frame at adpcm. */
typedef struct PlatVoiceSample {
    const uint8_t* adpcm;  /* the frame holding sample 0, as on the disc */
    uint32_t key;          /* identity for the sample cache (its ARAM address) */
    uint32_t start;        /* first sample played; a multiple of 14 */
    uint32_t end;          /* one past the last sample: the sample's end, or the loop's */
    uint32_t loop_start;   /* when looping: the loop's first sample (any value) */
    uint8_t looping;
    uint8_t loop_reload;   /* the loop restarts the decoder from loop_yn1/yn2 (MusyX loopType 0) */
    uint16_t coefs[16];
    uint16_t ps;           /* decoder state at start */
    int16_t yn1, yn2;
    uint16_t loop_ps;      /* decoder state at the loop start */
    int16_t loop_yn1, loop_yn2;
} PlatVoiceSample;

typedef struct PlatVoiceMix {
    float rate;      /* the sample's rate in Hz */
    float l, r;      /* gains on the left and right outputs */
    uint8_t interp;  /* 0 polyphase, 1 linear, 2 none */
    uint8_t lpf;     /* one-pole low-pass: y = (a0 x + b0 y[-1]) / 32768 */
    int16_t lpf_a0, lpf_b0;
} PlatVoiceMix;

/* Voice channels available (channels 1..n); 0 when NDSP voices are off. */
int plat_voice_channels(void);
/* Output frames between a fill call and the speakers' hearing what it made
 * (the game's own buffering ahead of the output). */
void plat_voice_set_lead(uint32_t frames);
/* Starts a sample on a channel (replacing what it played). Returns 0 when the
 * sample cannot be played there (no memory, bad data): the caller keeps the
 * voice in software. */
int plat_voice_start(int ch, const PlatVoiceSample* smp, const PlatVoiceMix* mix);
/* New rate, gains or filter for a playing channel. */
void plat_voice_mix(int ch, const PlatVoiceMix* mix);
void plat_voice_stop(int ch);
/* A counter line for the log (switch ndspstats=N: every N seconds). */
void plat_voice_note(uint32_t software, uint32_t ndsp, uint32_t aux, uint32_t overflow, uint32_t aux_late);

/* ---- NDSP streams (switch ndspstreams=1, with ndspvoices=1) ----
 *
 * A MusyX stream is a ring of ADPCM frames (or PCM16 samples) the game refills
 * behind the voice's position. The ring is mirrored in linear memory and played
 * as a chain of short wave buffers (the decoder's history carries from one to
 * the next, and around the ring's end). */
typedef struct PlatStream {
    const uint8_t* ring;   /* the ring in ARAM */
    uint32_t bytes;        /* its size in bytes (ADPCM: 8 per 14 samples) */
    uint32_t samples;      /* its size in samples */
    uint8_t pcm16;         /* PCM16 (native order) instead of DSP-ADPCM */
    uint16_t coefs[16];
    uint16_t ps;           /* ADPCM: the first frame's predictor/scale */
} PlatStream;
/* Starts a stream on a channel from ring sample 0 (copies the ring now).
 * Returns 0 when it has to stay in software. */
int plat_stream_start(int ch, const PlatStream* st, const PlatVoiceMix* mix);
/* The game refilled bytes [off, off + len) of the channel's ring (in ARAM now):
 * the mirror takes them when the output reaches that point. */
void plat_stream_refill(int ch, uint32_t off, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
