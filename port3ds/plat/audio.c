/* Audio output over NDSP.
 *
 * One NDSP channel plays interleaved stereo PCM16 at the game's rate (32 kHz,
 * MusyX's mixing rate; NDSP converts to the DSP's 32728 Hz). A ring of wave
 * buffers in linear memory is kept queued; the audio thread waits for the
 * DSP's frame callback and refills each buffer the DSP has finished with.
 *
 * Without NDSP (no sdmc:/3ds/dspfirm.cdc) the thread runs the same fill calls
 * on a timer and drops the samples, so the game's sound timing still runs
 * (the game waits on sounds ending). The log and the bottom console say so
 * once.
 *
 * The thread runs on core 1 at a priority above the game's threads, as the
 * GameCube's audio interrupt preempts them. It falls back to core 0 when
 * core 1 refuses it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/audio.h"
#include "plat_internal.h"

#define WAVEBUFS 5
#define QUANTA_PER_BUF 2 /* 2 x 5 ms: 50 ms queued at most */
#define THREAD_PRIO 0x1A

static PlatAudioFill s_fill;
static uint32_t s_rate, s_quantum;
static uint32_t s_buf_frames;
static ndspWaveBuf s_wbuf[WAVEBUFS];
static int16_t* s_samples;
static int16_t* s_scratch; /* null output: where the samples go */
static LightEvent s_frame_event;
static Thread s_thread;
static volatile int s_output;
static volatile uint32_t s_frames, s_underruns;

int plat_audio_has_output(void) { return s_output; }
uint32_t plat_audio_frames(void) { return s_frames; }
uint32_t plat_audio_underruns(void) { return s_underruns; }

static void dsp_frame_callback(void* data) {
    (void)data;
    LightEvent_Signal(&s_frame_event);
}

/* Switch wavdump=N: the first N seconds of output to audio.wav (16-bit
 * stereo), kept in memory and written once, so the file I/O cannot starve
 * the DSP while it records. */
static int16_t* s_wav;
static uint32_t s_wav_frames, s_wav_max;

static void wav_capture(const int16_t* out) {
    if (s_wav == NULL || s_wav_frames >= s_wav_max) {
        return;
    }
    uint32_t n = s_buf_frames;
    if (n > s_wav_max - s_wav_frames) {
        n = s_wav_max - s_wav_frames;
    }
    memcpy(s_wav + s_wav_frames * 2, out, n * 4);
    s_wav_frames += n;
    if (s_wav_frames < s_wav_max) {
        return;
    }
    FILE* f = fopen("audio.wav", "wb");
    if (f != NULL) {
        uint32_t bytes = s_wav_frames * 4;
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
        uint32_t riff = 36 + bytes, rate = s_rate, byte_rate = s_rate * 4;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &byte_rate, 4);
        h[32] = 4;
        h[34] = 16;
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &bytes, 4);
        fwrite(h, 1, sizeof(h), f);
        fwrite(s_wav, 4, s_wav_frames, f);
        fclose(f);
        PLOG_I("audio: wrote audio.wav (%lu frames)", (unsigned long)s_wav_frames);
    }
    free(s_wav);
    s_wav = NULL;
}

static void fill_block(int16_t* out) {
    s_fill(out, s_buf_frames);
    s_frames += s_buf_frames;
    /* the output's peak level, logged every 10 s: a sound check without ears */
    static int peak;
    static uint64_t since;
    for (uint32_t i = 0; i < s_buf_frames * 2; i++) {
        int v = out[i] < 0 ? -out[i] : out[i];
        if (v > peak) {
            peak = v;
        }
    }
    wav_capture(out);
    if (s_frames - since >= (uint64_t)s_rate * 10) {
        PLOG_I("audio: peak %d of 32767 over the last 10 s, %lu underruns", peak, (unsigned long)s_underruns);
        peak = 0;
        since = s_frames;
    }
}

static void ndsp_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("audio", 32 * 1024);
    /* Prime the ring: every buffer queued before the first wait. */
    for (int i = 0; i < WAVEBUFS; i++) {
        fill_block(s_wbuf[i].data_pcm16);
        DSP_FlushDataCache(s_wbuf[i].data_pcm16, s_buf_frames * 4);
        ndspChnWaveBufAdd(0, &s_wbuf[i]);
    }
    for (;;) {
        LightEvent_Wait(&s_frame_event);
        int done = 0;
        for (int i = 0; i < WAVEBUFS; i++) {
            if (s_wbuf[i].status == NDSP_WBUF_DONE) {
                done++;
            }
        }
        if (done == WAVEBUFS) {
            /* Every buffer played out before it was refilled: a gap. */
            s_underruns++;
        }
        /* Refill in the order the DSP played them (sequence ids). */
        while (done-- > 0) {
            int oldest = -1;
            for (int i = 0; i < WAVEBUFS; i++) {
                if (s_wbuf[i].status == NDSP_WBUF_DONE &&
                    (oldest < 0 || (int16_t)(s_wbuf[i].sequence_id - s_wbuf[oldest].sequence_id) < 0)) {
                    oldest = i;
                }
            }
            if (oldest < 0) {
                break;
            }
            fill_block(s_wbuf[oldest].data_pcm16);
            DSP_FlushDataCache(s_wbuf[oldest].data_pcm16, s_buf_frames * 4);
            ndspChnWaveBufAdd(0, &s_wbuf[oldest]);
        }
    }
}

static void null_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("audio2", 32 * 1024);
    const uint64_t ticks_per_block = (uint64_t)SYSCLOCK_ARM11 * s_buf_frames / s_rate;
    uint64_t due = svcGetSystemTick();
    for (;;) {
        fill_block(s_scratch);
        due += ticks_per_block;
        uint64_t now = svcGetSystemTick();
        if (now < due) {
            svcSleepThread((int64_t)((due - now) * 1000000000ull / SYSCLOCK_ARM11));
        } else if (now - due > ticks_per_block * WAVEBUFS) {
            /* Far behind (a long stall): drop the backlog instead of catching up. */
            s_underruns++;
            due = now;
        }
    }
}

/* exit(): silence the channel and release the DSP for the HOME Menu. */
static void ndsp_shutdown(void) {
    ndspSetCallback(NULL, NULL);
    ndspChnReset(0);
    ndspExit();
}

static int start_ndsp(void) {
    Result r = ndspInit();
    if (R_FAILED(r)) {
        PLOG_W("audio: ndspInit failed (%08lx); no sound. Dump the DSP firmware to sdmc:/3ds/dspfirm.cdc "
               "(the DSP1 homebrew) for sound.",
               (unsigned long)r);
        return 0;
    }
    s_samples = linearAlloc(WAVEBUFS * s_buf_frames * 4);
    if (s_samples == NULL) {
        ndspExit();
        plat_fatal("audio: no linear memory for %u bytes of wave buffers", (unsigned)(WAVEBUFS * s_buf_frames * 4));
    }
    memset(s_samples, 0, WAVEBUFS * s_buf_frames * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetOutputCount(1);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)s_rate);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);
    memset(s_wbuf, 0, sizeof(s_wbuf));
    for (int i = 0; i < WAVEBUFS; i++) {
        s_wbuf[i].data_pcm16 = s_samples + i * s_buf_frames * 2;
        s_wbuf[i].nsamples = s_buf_frames;
        s_wbuf[i].looping = false;
        s_wbuf[i].status = NDSP_WBUF_FREE;
    }
    LightEvent_Init(&s_frame_event, RESET_ONESHOT);
    ndspSetCallback(dsp_frame_callback, NULL);
    atexit(ndsp_shutdown);
    plat_set_audio_abort(ndsp_shutdown);
    return 1;
}

static Thread make_thread(ThreadFunc fn) {
    Thread t = threadCreate(fn, NULL, 32 * 1024, THREAD_PRIO, 1, false);
    if (t == NULL) {
        t = threadCreate(fn, NULL, 32 * 1024, THREAD_PRIO, 0, false);
    }
    if (t == NULL) {
        plat_fatal("audio: no thread");
    }
    return t;
}

int plat_audio_start(uint32_t rate, uint32_t quantum_frames, PlatAudioFill fill) {
    if (s_thread != NULL) {
        return s_output;
    }
    if (fill == NULL || rate == 0 || quantum_frames == 0) {
        plat_fatal("audio: bad start (rate %lu, quantum %lu)", (unsigned long)rate, (unsigned long)quantum_frames);
    }
    s_fill = fill;
    s_rate = rate;
    s_quantum = quantum_frames;
    s_buf_frames = quantum_frames * QUANTA_PER_BUF;
    int wav_seconds = plat_switch_int("wavdump", 0);
    if (wav_seconds > 0 && wav_seconds <= 120) {
        s_wav_max = (uint32_t)wav_seconds * rate;
        s_wav = malloc(s_wav_max * 4);
    }
    s_output = start_ndsp();
    if (s_output) {
        s_thread = make_thread(ndsp_thread);
        PLOG_I("audio: NDSP, %lu Hz stereo, %d buffers of %lu frames", (unsigned long)rate, WAVEBUFS,
               (unsigned long)s_buf_frames);
    } else {
        s_scratch = malloc(s_buf_frames * 4);
        if (s_scratch == NULL) {
            plat_fatal("audio: no memory");
        }
        s_thread = make_thread(null_thread);
        PLOG_I("audio: no output, timing only (%lu Hz)", (unsigned long)rate);
    }
    return s_output;
}
